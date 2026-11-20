#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// One bounded JPEG accumulator. Retains a trailing FF across TCP read boundaries.
// Push returns the newest complete frame; a false return leaves 'latest' alone.
class MjpegBuffer {
public:
  explicit MjpegBuffer(std::size_t maximum = 2U * 1024U * 1024U)
      : maximum_(maximum < 4 ? 4 : maximum) {}

  bool Push(const std::uint8_t* data, std::size_t size,
            std::vector<std::uint8_t>& latest) {
    bool ready = false;
    for (std::size_t i = 0; i < size; ++i) {
      const auto value = data[i];
      if (bytes_.empty()) {
        if (value == 0xff) bytes_.push_back(value);
        continue;
      }
      if (bytes_.size() == 1) {
        if (value == 0xd8) bytes_.push_back(value);
        else if (value != 0xff) bytes_.clear();
        continue;
      }
      bytes_.push_back(value);
      if (bytes_.size() > maximum_) {
        bytes_.clear();
        if (value == 0xff) bytes_.push_back(value);
        continue;
      }
      if (bytes_[bytes_.size()-2] == 0xff && value == 0xd9) {
        latest.assign(bytes_.begin(), bytes_.end());
        bytes_.clear();
        ready = true;
      }
    }
    return ready;
  }
  std::size_t BufferedBytes() const { return bytes_.size(); }
private:
  std::size_t maximum_;
  std::vector<std::uint8_t> bytes_;
};
