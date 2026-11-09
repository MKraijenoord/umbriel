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
    std::function<bool()> shellDrawn;
    std::function<void()> requestSessionExit;
  };

  struct SessionHandoverTimeouts {
    int presentedFallbackMs = 1000;
    int logoutMs = 20000;
    int shellSettleMs = 150;
    int shellFallbackMs = 10000;
  };

  // Hands the display over from a display manager that keeps its greeter on screen while this session starts on
  // another VT. Once the shell has drawn and its layer surfaces settle, READY=1 asks the display manager to switch
  // VTs; once the outputs have committed their first frame, PRESENTED=1 tells it the greeter can go.
  class SessionHandover {
  public:
    SessionHandover(SessionHandoverHost host, std::string socketPath, SessionHandoverTimeouts timeouts = {});
    ~SessionHandover();

    SessionHandover(const SessionHandover&) = delete;
    SessionHandover& operator=(const SessionHandover&) = delete;

    void layerSurfaceMapped(std::string_view surface);

    // Asks the display manager to bring up the greeter behind this session and switch to it once it is drawn; this
    // session quits when told to (QUIT=1) or after a timeout. Returns false when no display manager is listening.
    [[nodiscard]] bool requestLogout();

  private:
    static int onReplyReadable(int fd, uint32_t mask, void* data);
    static int onShellSettled(void* data);
    static int onShellFallback(void* data);
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
    void sendReady();
    void sendFrameDone();
    void sendPresented();
    void clearPresentWatches();

    SessionHandoverHost m_host;
    std::string m_socketPath;
    SessionHandoverTimeouts m_timeouts;
    std::string m_replyPath;
    int m_replyFd = -1;
    wl_event_source* m_replySource = nullptr;
    wl_event_source* m_shellSettleTimer = nullptr;
    wl_event_source* m_shellFallbackTimer = nullptr;
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
