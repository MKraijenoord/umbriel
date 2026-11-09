#include "server/session_handover.h"

#include "core/log.h"
#include "output/output.h"
#include "wlr.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace umbriel {

  namespace {
    constexpr Logger kLog("handover");
    constexpr int kInactiveFrameIntervalMs = 16;

    [[nodiscard]] bool fillAddress(sockaddr_un& addr, std::string_view path) {
      addr.sun_family = AF_UNIX;
      if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        return false;
      }
      std::memcpy(addr.sun_path, path.data(), path.size());
      return true;
    }
  } // namespace

  SessionHandover::SessionHandover(SessionHandoverHost host, std::string socketPath, SessionHandoverTimeouts timeouts)
      : m_host(std::move(host)), m_socketPath(std::move(socketPath)), m_timeouts(timeouts) {
    wl_event_loop* loop = m_host.loop;
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    m_replyPath = std::format("{}/umbriel-ready-{}.sock", runtimeDir != nullptr ? runtimeDir : "/tmp", getpid());
    ::unlink(m_replyPath.c_str());
    sockaddr_un addr{};
    m_replyFd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (m_replyFd < 0
        || !fillAddress(addr, m_replyPath)
        || ::bind(m_replyFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      kLog.error("failed to create the reply socket {}: {}", m_replyPath, std::strerror(errno));
      if (m_replyFd >= 0) {
        ::close(m_replyFd);
        m_replyFd = -1;
      }
    } else {
      m_replySource = wl_event_loop_add_fd(loop, m_replyFd, WL_EVENT_READABLE, onReplyReadable, this);
    }

    m_shellSettleTimer = wl_event_loop_add_timer(loop, onShellSettled, this);
    m_shellFallbackTimer = wl_event_loop_add_timer(loop, onShellFallback, this);
    wl_event_source_timer_update(m_shellFallbackTimer, m_timeouts.shellFallbackMs);

    wlr_session* session = m_host.session;
    if (session != nullptr && !session->active) {
      m_frameTimer = wl_event_loop_add_timer(loop, onFrameTimer, this);
      wl_event_source_timer_update(m_frameTimer, kInactiveFrameIntervalMs);
      m_sessionActive.notify = onSessionActive;
      wl_signal_add(&session->events.active, &m_sessionActive);
    }
    kLog.info(
        "waiting for the shell to draw (session {})", session != nullptr && session->active ? "active" : "inactive"
    );
  }

  SessionHandover::~SessionHandover() {
    clearPresentWatches();
    if (m_sessionActive.link.next != nullptr) {
      wl_list_remove(&m_sessionActive.link);
    }
    for (wl_event_source* source :
         {m_replySource, m_shellSettleTimer, m_shellFallbackTimer, m_frameTimer, m_presentedTimer, m_logoutTimer}) {
      if (source != nullptr) {
        wl_event_source_remove(source);
      }
    }
    if (m_replyFd >= 0) {
      ::close(m_replyFd);
      ::unlink(m_replyPath.c_str());
    }
  }

  void SessionHandover::layerSurfaceMapped() {
    if (m_readySent || !m_host.shellDrawn()) {
      return;
    }
    wl_event_source_timer_update(m_shellSettleTimer, m_timeouts.shellSettleMs);
  }

  int SessionHandover::onShellSettled(void* data) {
    auto* self = static_cast<SessionHandover*>(data);
    kLog.info("shell has drawn; asking the display manager to switch");
    self->sendReady();
    return 0;
  }

  int SessionHandover::onShellFallback(void* data) {
    auto* self = static_cast<SessionHandover*>(data);
    kLog.warn("shell did not draw in time; asking the display manager to switch anyway");
    self->sendReady();
    return 0;
  }

  void SessionHandover::sendReady() {
    if (std::exchange(m_readySent, true)) {
      return;
    }
    wl_event_source_timer_update(m_shellSettleTimer, 0);
    wl_event_source_timer_update(m_shellFallbackTimer, 0);
    send("READY=1");
    wlr_session* session = m_host.session;
    if (session != nullptr && session->active && m_presentedTimer == nullptr) {
      // Not a handover after all (the session was already active): there is no greeter frame to replace.
      sendPresented();
    }
  }

  bool SessionHandover::requestLogout() {
    if (m_logoutReleased) {
      return false;
    }
    if (m_logoutPending) {
      return true;
    }
    if (m_replyFd < 0) {
      return false;
    }
    m_logoutPending = true;
    kLog.info("asking the display manager for a seamless logout");
    send(std::format("LOGOUT=1\nREPLY={}", m_replyPath));
    m_logoutTimer = wl_event_loop_add_timer(m_host.loop, onLogoutTimeout, this);
    wl_event_source_timer_update(m_logoutTimer, m_timeouts.logoutMs);
    return true;
  }

  int SessionHandover::onLogoutTimeout(void* data) {
    auto* self = static_cast<SessionHandover*>(data);
    kLog.warn("display manager did not take over in time; quitting");
    self->m_logoutReleased = true;
    self->m_host.requestSessionExit();
    return 0;
  }

  int SessionHandover::onReplyReadable(int fd, uint32_t /*mask*/, void* data) {
    auto* self = static_cast<SessionHandover*>(data);
    std::array<char, 256> buffer{};
    const ssize_t len = ::recv(fd, buffer.data(), buffer.size() - 1, 0);
    if (len <= 0) {
      return 0;
    }
    const std::string_view message(buffer.data(), static_cast<size_t>(len));
    if (self->m_logoutPending && message.contains("QUIT=1")) {
      kLog.info("greeter is on screen; quitting");
      self->m_logoutReleased = true;
      self->m_host.requestSessionExit();
    }
    return 0;
  }

  int SessionHandover::onFrameTimer(void* data) {
    auto* self = static_cast<SessionHandover*>(data);
    self->sendFrameDone();
    wl_event_source_timer_update(self->m_frameTimer, kInactiveFrameIntervalMs);
    return 0;
  }

  void SessionHandover::sendFrameDone() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (const auto& output : m_host.outputs()) {
      if (output->sceneOutput() != nullptr) {
        wlr_scene_output_send_frame_done(output->sceneOutput(), &now);
      }
    }
  }

  void SessionHandover::onSessionActive(wl_listener* listener, void* /*data*/) {
    SessionHandover* self;
    self = wl_container_of(listener, self, m_sessionActive);
    wlr_session* session = self->m_host.session;
    if (session == nullptr || !session->active) {
      return;
    }
    wl_list_remove(&self->m_sessionActive.link);
    self->m_sessionActive.link.next = nullptr;
    if (self->m_frameTimer != nullptr) {
      wl_event_source_remove(self->m_frameTimer);
      self->m_frameTimer = nullptr;
    }

    kLog.info("session active; committing outputs");
    for (const auto& output : self->m_host.outputs()) {
      wlr_output* wlrOutput = output->sceneOutput() != nullptr ? output->sceneOutput()->output : nullptr;
      if (wlrOutput == nullptr || !wlrOutput->enabled) {
        continue;
      }
      auto watch = std::make_unique<PresentWatch>();
      watch->owner = self;
      watch->present.notify = onOutputPresent;
      wl_signal_add(&wlrOutput->events.present, &watch->present);
      self->m_presentWatches.push_back(std::move(watch));
    }
    for (const auto& output : self->m_host.outputs()) {
      output->commitPendingInitialState();
    }
    self->m_presentedTimer = wl_event_loop_add_timer(self->m_host.loop, onPresentedTimer, self);
    wl_event_source_timer_update(self->m_presentedTimer, self->m_timeouts.presentedFallbackMs);
  }

  void SessionHandover::onOutputPresent(wl_listener* listener, void* data) {
    PresentWatch* watch;
    watch = wl_container_of(listener, watch, present);
    const auto* event = static_cast<const wlr_output_event_present*>(data);
    if (event == nullptr || !event->presented) {
      return;
    }
    watch->presented = true;
    SessionHandover* self = watch->owner;
    for (const auto& other : self->m_presentWatches) {
      if (!other->presented) {
        return;
      }
    }
    kLog.info("every output presented its first frame");
    self->sendPresented();
  }

  int SessionHandover::onPresentedTimer(void* data) {
    auto* self = static_cast<SessionHandover*>(data);
    kLog.warn("not every output reported a presented frame; releasing the greeter anyway");
    self->sendPresented();
    return 0;
  }

  void SessionHandover::sendPresented() {
    if (std::exchange(m_presentedSent, true)) {
      return;
    }
    send("PRESENTED=1");
    clearPresentWatches();
    if (m_presentedTimer != nullptr) {
      wl_event_source_remove(m_presentedTimer);
      m_presentedTimer = nullptr;
    }
  }

  void SessionHandover::clearPresentWatches() {
    for (const auto& watch : m_presentWatches) {
      wl_list_remove(&watch->present.link);
    }
    m_presentWatches.clear();
  }

  void SessionHandover::send(std::string_view message) const {
    sockaddr_un addr{};
    if (!fillAddress(addr, m_socketPath)) {
      kLog.error("invalid handover socket path {}", m_socketPath);
      return;
    }
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return;
    }
    if (::sendto(
            fd, message.data(), message.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)
        )
        < 0) {
      kLog.error("failed to send {} to {}: {}", message, m_socketPath, std::strerror(errno));
    }
    ::close(fd);
  }

} // namespace umbriel
