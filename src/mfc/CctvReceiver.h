#pragma once
#include <windows.h>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct CctvSettings {
  bool enabled = true;
  std::wstring host;
  int port = 8081;
  std::wstring path = L"/video_feed";
};

enum class CctvStatus { Paused, Disabled, Connecting, Streaming, Reconnecting, Invalid };
struct CctvSnapshot {
  CctvStatus status = CctvStatus::Paused;
  std::wstring message = L"CCTV 연결 대기";
  std::shared_ptr<const std::vector<std::uint8_t>> jpeg;
  ULONGLONG receivedAt = 0;
  std::uint64_t generation = 0;
};

// Separate from the inspection camera and conveyor control. UI notifications
// carry no owning pointers: one latest frame and one pending message are enough.
class CctvReceiver final {
public:
  CctvReceiver() = default;
  ~CctvReceiver();
  CctvReceiver(const CctvReceiver&) = delete;
  CctvReceiver& operator=(const CctvReceiver&) = delete;
  void Configure(const CctvSettings& settings, HWND target, UINT message);
  void Pause();
  void Close();
  CctvSnapshot Consume();
private:
  void Run();
  bool IsCurrent(std::uint64_t generation);
  void Publish(std::uint64_t generation, CctvStatus status,
               const std::wstring& text,
               std::shared_ptr<const std::vector<std::uint8_t>> frame = {});
  void NotifyLocked();
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;
  CctvSettings settings_;
  CctvSnapshot snapshot_;
  HWND target_ = nullptr;
  UINT message_ = 0;
  bool active_ = false;
  bool closing_ = false;
  bool notificationPending_ = false;
  bool workerStopped_ = false;
  std::uint64_t generation_ = 0;
};
