#include "CctvReceiver.h"
#include "MjpegBuffer.h"
#include <winhttp.h>
#include <chrono>
#include <exception>
#include <utility>
#pragma comment(lib, "winhttp.lib")

namespace {
struct InternetCloser {
  void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); }
};
using InternetHandle = std::unique_ptr<void, InternetCloser>;
bool Valid(const CctvSettings& s) {
  return !s.host.empty() && s.host.find_first_of(L"/\\\r\n") == std::wstring::npos &&
      s.port >= 1 && s.port <= 65535 && !s.path.empty() && s.path.front() == L'/' &&
      s.path.find_first_of(L"\r\n") == std::wstring::npos;
}
}

CctvReceiver::~CctvReceiver() { Close(); }

void CctvReceiver::NotifyLocked() {
  if (!notificationPending_ && target_ && message_) {
    notificationPending_ = ::PostMessageW(target_, message_, 0, 0) != FALSE;
  }
}

void CctvReceiver::Configure(const CctvSettings& settings, HWND target, UINT message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_) return;
  settings_ = settings;
  target_ = target;
  message_ = message;
  ++generation_;
  snapshot_ = {};
  snapshot_.generation = generation_;
  active_ = settings.enabled && Valid(settings);
  snapshot_.status = !settings.enabled ? CctvStatus::Disabled :
      active_ ? CctvStatus::Connecting : CctvStatus::Invalid;
  snapshot_.message = !settings.enabled ? L"CCTV 사용이 꺼져 있습니다" :
      active_ ? L"라즈베리파이 CCTV에 연결 중..." : L"CCTV 주소 설정을 확인하세요";
  if (active_ && workerStopped_ && thread_.joinable()) thread_.join();
  if (active_ && !thread_.joinable()) {
    workerStopped_ = false;
    try { thread_ = std::thread(&CctvReceiver::Run, this); }
    catch (const std::exception&) {
      active_ = false;
      snapshot_.status = CctvStatus::Invalid;
      snapshot_.message = L"CCTV 수신 스레드를 시작하지 못했습니다";
    }
  }
  NotifyLocked();
  wake_.notify_all();
}

void CctvReceiver::Pause() {
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = false;
  ++generation_;
  snapshot_ = {};
  snapshot_.generation = generation_;
  snapshot_.status = CctvStatus::Paused;
  snapshot_.message = L"관리자 탭에서 CCTV를 연결합니다";
  NotifyLocked();
  wake_.notify_all();
}

void CctvReceiver::Close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
    active_ = false;
    ++generation_;
    target_ = nullptr;
    wake_.notify_all();
  }
  if (thread_.joinable()) thread_.join();
}

CctvSnapshot CctvReceiver::Consume() {
  std::lock_guard<std::mutex> lock(mutex_);
  notificationPending_ = false;
  return snapshot_;
}

bool CctvReceiver::IsCurrent(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return !closing_ && active_ && generation_ == generation;
}

void CctvReceiver::Publish(std::uint64_t generation, CctvStatus status,
                           const std::wstring& text,
                           std::shared_ptr<const std::vector<std::uint8_t>> frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_ || !active_ || generation_ != generation) return;
  snapshot_.generation = generation;
  snapshot_.status = status;
  snapshot_.message = text;
  snapshot_.jpeg = std::move(frame);
  snapshot_.receivedAt = snapshot_.jpeg ? GetTickCount64() : 0;
  NotifyLocked();
}

void CctvReceiver::Run() {
  try {
    for (;;) {
      CctvSettings settings;
      std::uint64_t generation = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return closing_ || active_; });
        if (closing_) return;
        settings = settings_;
        generation = generation_;
      }
      Publish(generation, CctvStatus::Connecting, L"CCTV 영상 연결 중...");
      std::wstring failure = L"CCTV 연결이 끊겼습니다. 자동으로 다시 연결합니다";
      {
        // Synchronous network work is confined to this worker. Tab changes only
        // invalidate its generation and never wait on a socket or thread join.
        InternetHandle session(WinHttpOpen(L"MobleKurly-CCTV/1.0",
            WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0));
        if (session) WinHttpSetTimeouts(session.get(), 1500, 1500, 1500, 1200);
        InternetHandle connection(session ? WinHttpConnect(session.get(),
            settings.host.c_str(), static_cast<INTERNET_PORT>(settings.port), 0) : nullptr);
        InternetHandle request(connection ? WinHttpOpenRequest(connection.get(), L"GET",
            settings.path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr);
        if (request && IsCurrent(generation) &&
            WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request.get(), nullptr)) {
          DWORD status = 0, size = sizeof(status);
          if (WinHttpQueryHeaders(request.get(),
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                WINHTTP_NO_HEADER_INDEX) && status == 200) {
            MjpegBuffer parser;
            ULONGLONG lastPublished = 0;
            std::uint8_t buffer[8192];
            while (IsCurrent(generation)) {
              DWORD count = 0;
              if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &count) || !count)
                break;
              std::vector<std::uint8_t> latest;
              if (parser.Push(buffer, count, latest)) {
                const auto now = GetTickCount64();
                if (lastPublished == 0 || now - lastPublished >= 100) {
                  Publish(generation, CctvStatus::Streaming, L"CCTV 영상 수신 중",
                      std::make_shared<const std::vector<std::uint8_t>>(std::move(latest)));
                  lastPublished = now;
                }
              }
            }
          } else {
            failure = status == 503 ? L"CCTV 카메라 준비 중 · 자동 재연결" :
                L"CCTV 영상 주소에 응답이 없습니다 · 자동 재연결";
          }
        }
      }
      Publish(generation, CctvStatus::Reconnecting, failure);
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(1200), [this, generation] {
        return closing_ || !active_ || generation_ != generation;
      });
      if (closing_) return;
    }
  } catch (const std::exception&) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    workerStopped_ = true;
    snapshot_ = {};
    snapshot_.generation = ++generation_;
    snapshot_.status = CctvStatus::Invalid;
    snapshot_.message = L"CCTV 수신 오류 · 다시 연결을 눌러주세요";
    NotifyLocked();
  }
}
