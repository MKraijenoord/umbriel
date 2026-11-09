#include "server/session_handover.h"

#include "check.h"
#include "wlr.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

using umbriel::Output;
using umbriel::SessionHandover;
using umbriel::SessionHandoverHost;
using umbriel::SessionHandoverTimeouts;

namespace {
  constexpr int kDispatchLimitMs = 2000;

  class ScopedEnvironment {
  public:
    ScopedEnvironment(const char* name, const std::string& value) : m_name(name) {
      if (const char* previous = std::getenv(name)) {
        m_previous = previous;
      }
      setenv(name, value.c_str(), 1);
    }

    ~ScopedEnvironment() {
      if (m_previous) {
        setenv(m_name.c_str(), m_previous->c_str(), 1);
      } else {
        unsetenv(m_name.c_str());
      }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

  private:
    std::string m_name;
    std::optional<std::string> m_previous;
  };

  sockaddr_un socketAddress(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.data(), path.size());
    return addr;
  }

  void sendDatagram(const std::string& path, std::string_view message) {
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    const sockaddr_un addr = socketAddress(path);
    const ssize_t sent =
        ::sendto(fd, message.data(), message.size(), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    CHECK_EQ(sent, static_cast<ssize_t>(message.size()));
    ::close(fd);
  }

  class FakeGreetd {
  public:
    explicit FakeGreetd(std::string path) : m_path(std::move(path)) {
      m_fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      const sockaddr_un addr = socketAddress(m_path);
      CHECK_EQ(::bind(m_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
    }

    ~FakeGreetd() { ::close(m_fd); }

    FakeGreetd(const FakeGreetd&) = delete;
    FakeGreetd& operator=(const FakeGreetd&) = delete;

    const std::string& path() const { return m_path; }

    [[nodiscard]] std::string receive() const {
      std::array<char, 512> buffer{};
      const ssize_t len = ::recv(m_fd, buffer.data(), buffer.size(), 0);
      if (len < 0) {
        CHECK_EQ(errno, EAGAIN);
        return {};
      }
      return std::string(buffer.data(), static_cast<size_t>(len));
    }

  private:
    std::string m_path;
    int m_fd = -1;
  };

  class FakeSession {
  public:
    explicit FakeSession(bool active) {
      m_session.active = active;
      wl_list_init(&m_session.devices);
      wl_signal_init(&m_session.events.active);
      wl_signal_init(&m_session.events.add_drm_card);
      wl_signal_init(&m_session.events.destroy);
    }

    wlr_session* get() { return &m_session; }

    void activate() {
      m_session.active = true;
      wl_signal_emit_mutable(&m_session.events.active, nullptr);
    }

  private:
    wlr_session m_session{};
  };

  std::filesystem::path freshDirectory() {
    static int count = 0;
    auto dir = std::filesystem::temp_directory_path() / std::format("umbriel-handover-{}-{}", getpid(), ++count);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
  }

  struct Fixture {
    Fixture() = default;

    ~Fixture() {
      handover.reset();
      wl_event_loop_destroy(loop);
      std::filesystem::remove_all(dir);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void start(wlr_session* session, SessionHandoverTimeouts timeouts = {}) {
      handover = std::make_unique<SessionHandover>(
          SessionHandoverHost{
              .loop = loop,
              .session = session,
              .outputs = [] { return std::span<const std::unique_ptr<Output>>{}; },
              .shellDrawn = [this] { return shellDrawn; },
              .requestSessionExit = [this] { ++exitRequests; },
          },
          greetd.path(), timeouts
      );
    }

    std::string replySocket() const { return std::format("{}/umbriel-ready-{}.sock", dir.string(), getpid()); }

    void greetdReplies(std::string_view message) const { sendDatagram(replySocket(), message); }

    void shellDraws() {
      shellDrawn = true;
      handover->layerSurfaceMapped();
    }

    void dispatchFor(int milliseconds) const {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
      while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        wl_event_loop_dispatch(loop, static_cast<int>(remaining.count()) + 1);
      }
    }

    void dispatchUntil(const std::function<bool()>& done) const {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDispatchLimitMs);
      while (!done() && std::chrono::steady_clock::now() < deadline) {
        wl_event_loop_dispatch(loop, 10);
      }
    }

    std::filesystem::path dir = freshDirectory();
    ScopedEnvironment runtimeEnvironment{"XDG_RUNTIME_DIR", dir.string()};
    FakeGreetd greetd{(dir / "greetd.sock").string()};
    wl_event_loop* loop = wl_event_loop_create();
    std::unique_ptr<SessionHandover> handover;
    bool shellDrawn = false;
    int exitRequests = 0;
  };
} // namespace

UMBRIEL_TEST(drawnShellOnActiveSessionAsksForTheSwitchOnceTheShellSettles) {
  Fixture fixture;
  FakeSession session(true);
  fixture.start(session.get(), {.shellSettleMs = 20});

  fixture.shellDraws();
  CHECK_EQ(fixture.greetd.receive(), std::string());
  fixture.dispatchFor(60);

  CHECK_EQ(fixture.greetd.receive(), std::string("READY=1"));
  CHECK_EQ(fixture.greetd.receive(), std::string("PRESENTED=1"));
  CHECK_EQ(fixture.greetd.receive(), std::string());
}

UMBRIEL_TEST(layerSurfacesMappedBeforeTheShellIsDrawnDoNotAskForTheSwitch) {
  Fixture fixture;
  FakeSession session(true);
  fixture.start(session.get(), {.shellSettleMs = 20});

  fixture.handover->layerSurfaceMapped();
  fixture.dispatchFor(60);

  CHECK_EQ(fixture.greetd.receive(), std::string());
}

UMBRIEL_TEST(layerSurfaceMappedWhileTheShellSettlesWaitsForItToo) {
  Fixture fixture;
  FakeSession session(true);
  fixture.start(session.get(), {.shellSettleMs = 150});

  fixture.shellDraws();
  fixture.dispatchFor(100);
  fixture.handover->layerSurfaceMapped();
  fixture.dispatchFor(100);
  CHECK_EQ(fixture.greetd.receive(), std::string());

  fixture.dispatchFor(100);
  CHECK_EQ(fixture.greetd.receive(), std::string("READY=1"));
}

UMBRIEL_TEST(shellThatNeverDrawsGetsTheSwitchAfterTheFallback) {
  Fixture fixture;
  FakeSession session(true);
  fixture.start(session.get(), {.shellFallbackMs = 20});

  fixture.dispatchFor(60);

  CHECK_EQ(fixture.greetd.receive(), std::string("READY=1"));
  CHECK_EQ(fixture.greetd.receive(), std::string("PRESENTED=1"));
}

UMBRIEL_TEST(switchIsRequestedOnce) {
  Fixture fixture;
  FakeSession session(true);
  fixture.start(session.get(), {.shellSettleMs = 20, .shellFallbackMs = 80});

  fixture.shellDraws();
  fixture.dispatchFor(50);
  fixture.handover->layerSurfaceMapped();
  fixture.dispatchFor(100);

  CHECK_EQ(fixture.greetd.receive(), std::string("READY=1"));
  CHECK_EQ(fixture.greetd.receive(), std::string("PRESENTED=1"));
  CHECK_EQ(fixture.greetd.receive(), std::string());
}

UMBRIEL_TEST(drawnShellOnInactiveSessionKeepsTheGreeterUntilTheSessionIsActive) {
  Fixture fixture;
  FakeSession session(false);
  fixture.start(session.get(), {.shellSettleMs = 20});

  fixture.shellDraws();
  fixture.dispatchFor(60);

  CHECK_EQ(fixture.greetd.receive(), std::string("READY=1"));
  CHECK_EQ(fixture.greetd.receive(), std::string());
}

UMBRIEL_TEST(activatedSessionReleasesTheGreeterAfterTheFallbackWhenNoOutputPresents) {
  Fixture fixture;
  FakeSession session(false);
  fixture.start(session.get(), {.presentedFallbackMs = 20, .shellSettleMs = 20});
  fixture.shellDraws();
  fixture.dispatchFor(60);
  CHECK_EQ(fixture.greetd.receive(), std::string("READY=1"));

  session.activate();
  fixture.dispatchFor(100);

  CHECK_EQ(fixture.greetd.receive(), std::string("PRESENTED=1"));
  CHECK_EQ(fixture.greetd.receive(), std::string());
}

UMBRIEL_TEST(logoutAsksGreetdToTakeOverAndReplyToTheReplySocket) {
  Fixture fixture;
  FakeSession session(true);
  fixture.start(session.get());

  CHECK(fixture.handover->requestLogout());
  fixture.dispatchFor(50);

  CHECK_EQ(fixture.greetd.receive(), std::format("LOGOUT=1\nREPLY={}", fixture.replySocket()));
  CHECK_EQ(fixture.exitRequests, 0);
}

UMBRIEL_TEST(repeatedLogoutWhileGreetdTakesOverIsSwallowedWithoutAskingAgain) {
  Fixture fixture;
  FakeSession session(true);
  fixture.start(session.get());

  CHECK(fixture.handover->requestLogout());
  CHECK(fixture.handover->requestLogout());

  CHECK_EQ(fixture.greetd.receive(), std::format("LOGOUT=1\nREPLY={}", fixture.replySocket()));
  CHECK_EQ(fixture.greetd.receive(), std::string());
}

UMBRIEL_TEST(greetdQuitEndsTheSessionAndLetsTheNextExitThrough) {
  Fixture fixture;
  FakeSession session(true);
  fixture.start(session.get());
  CHECK(fixture.handover->requestLogout());

  fixture.greetdReplies("QUIT=1");
  fixture.dispatchUntil([&] { return fixture.exitRequests != 0; });

  CHECK_EQ(fixture.exitRequests, 1);
  CHECK(!fixture.handover->requestLogout());
}

UMBRIEL_TEST(greetdQuitWithoutAPendingLogoutIsIgnored) {
  Fixture fixture;
  FakeSession session(true);
  fixture.start(session.get());

  fixture.greetdReplies("QUIT=1");
  fixture.dispatchFor(50);

  CHECK_EQ(fixture.exitRequests, 0);
  CHECK(fixture.handover->requestLogout());
}

UMBRIEL_TEST(logoutIsNotDeferredWhenTheReplySocketCouldNotBeCreated) {
  Fixture fixture;
  FakeSession session(true);
  const ScopedEnvironment missingRuntimeDir("XDG_RUNTIME_DIR", (fixture.dir / "missing").string());
  fixture.start(session.get());

  CHECK(!fixture.handover->requestLogout());
  CHECK_EQ(fixture.greetd.receive(), std::string());
}

UMBRIEL_TEST(logoutGivesGreetdTwentySecondsByDefault) { CHECK_EQ(SessionHandoverTimeouts{}.logoutMs, 20000); }

UMBRIEL_TEST(logoutWaitsForGreetdWithinTheTimeout) {
  Fixture fixture;
  FakeSession session(true);
  fixture.start(session.get());

  CHECK(fixture.handover->requestLogout());
  fixture.dispatchFor(200);

  CHECK_EQ(fixture.exitRequests, 0);
}

UMBRIEL_TEST(logoutEndsTheSessionByItselfWhenGreetdNeverAnswers) {
  Fixture fixture;
  FakeSession session(true);
  fixture.start(session.get(), {.logoutMs = 20});

  CHECK(fixture.handover->requestLogout());
  fixture.dispatchUntil([&] { return fixture.exitRequests != 0; });

  CHECK_EQ(fixture.exitRequests, 1);
  CHECK(!fixture.handover->requestLogout());
}

int main() { return RUN_TESTS(); }
