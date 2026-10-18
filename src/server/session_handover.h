#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <wayland-server-core.h>
}

struct wlr_session;

namespace umbriel {

  class Output;

  struct SessionHandoverHost {
    wl_event_loop* loop = nullptr;
    wlr_session* session = nullptr;
    std::function<std::span<const std::unique_ptr<Output>>()> outputs;
    std::function<void()> requestSessionExit;
  };

  struct SessionHandoverTimeouts {
    int presentedFallbackMs = 1000;
    int logoutMs = 20000;
  };

  // Hands the display over from a display manager that keeps its greeter on screen while this session starts on
  // another VT. The shell's READY=1 (sd_notify) is relayed to the display manager, which switches VTs; once the
  // outputs have committed their first frame, PRESENTED=1 tells it the greeter can go.
  class SessionHandover {
  public:
    SessionHandover(SessionHandoverHost host, std::string socketPath, SessionHandoverTimeouts timeouts = {});
    ~SessionHandover();

    SessionHandover(const SessionHandover&) = delete;
    SessionHandover& operator=(const SessionHandover&) = delete;

    // Exposes NOTIFY_SOCKET to the processes spawned between these two calls.
    void beginShellSpawn();
    void endShellSpawn();

    // Asks the display manager to bring up the greeter behind this session and switch to it once it is drawn; this
    // session quits when told to (QUIT=1) or after a timeout. Returns false when no display manager is listening.
    [[nodiscard]] bool requestLogout();

  private:
    static int onNotifyReadable(int fd, uint32_t mask, void* data);
    static int onFrameTimer(void* data);
    static int onPresentedTimer(void* data);
    static int onLogoutTimeout(void* data);
    static void onSessionActive(wl_listener* listener, void* data);

    struct PresentWatch {
      SessionHandover* owner = nullptr;
      wl_listener present{};
      bool presented = false;
    };

    static void onOutputPresent(wl_listener* listener, void* data);
    void send(std::string_view message) const;
    void sendFrameDone();
    void sendPresented();
    void clearPresentWatches();

    SessionHandoverHost m_host;
    std::string m_socketPath;
    SessionHandoverTimeouts m_timeouts;
    std::string m_notifyPath;
    int m_notifyFd = -1;
    wl_event_source* m_notifySource = nullptr;
    wl_event_source* m_frameTimer = nullptr;
    wl_event_source* m_presentedTimer = nullptr;
    wl_event_source* m_logoutTimer = nullptr;
    wl_listener m_sessionActive{};
    std::vector<std::unique_ptr<PresentWatch>> m_presentWatches;
    bool m_readySent = false;
    bool m_presentedSent = false;
    bool m_logoutPending = false;
    bool m_logoutReleased = false;
  };

} // namespace umbriel
