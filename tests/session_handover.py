"""Hand the display over to a fake greetd only once the shell has drawn its background, not just its bar."""

import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile


STUB = "#!/bin/sh\nexit 0\n"

SHELL = """#!/bin/sh
"$HANDOVER_LAYER_CLIENT" HEADLESS-1 30 &
exec 3<>"$HANDOVER_ROOT/shell-gate"
read -r _ <&3
exec "$HANDOVER_LAYER_CLIENT" HEADLESS-1 0
"""


def receive(greetd, timeout):
    greetd.settimeout(timeout)
    try:
        return greetd.recv(256)
    except socket.timeout:
        return None


def check(binary, layer_client):
    with tempfile.TemporaryDirectory(prefix="um-handover.") as directory:
        root = Path(directory)
        (root / "bin").mkdir()
        for name in ("systemctl", "dbus-update-activation-environment"):
            stub = root / "bin" / name
            stub.write_text(STUB)
            stub.chmod(0o700)
        shell = root / "shell"
        shell.write_text(SHELL)
        shell.chmod(0o700)
        os.mkfifo(root / "shell-gate")
        config = root / "config.toml"
        config.write_text(f'[general]\nxwayland = false\nshow_cheatsheet = false\nautostart = ["{shell}"]\n')

        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as greetd:
            greetd.bind(str(root / "greetd"))
            environment = os.environ.copy()
            for name in ("WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY", "UMBRIEL_SOCKET", "NOTIFY_SOCKET",
                         "SYSTEMD_EXEC_PID", "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE"):
                environment.pop(name, None)
            environment.update(
                PATH=str(root / "bin") + ":" + os.environ["PATH"],
                XDG_RUNTIME_DIR=str(root),
                XDG_CACHE_HOME=str(root / "cache"),
                DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-session-bus"),
                DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(root / "no-system-bus"),
                GREETD_HANDOVER_SOCKET=str(root / "greetd"),
                HANDOVER_ROOT=str(root),
                HANDOVER_LAYER_CLIENT=str(layer_client),
                WLR_BACKENDS="headless",
                WLR_HEADLESS_OUTPUTS="1",
                WLR_LIBINPUT_NO_DEVICES="1",
            )
            with (root / "compositor.log").open("w+") as log:
                process = subprocess.Popen(
                    [str(binary), "-c", str(config)],
                    env=environment, stdout=log, stderr=log, start_new_session=True,
                )
                try:
                    early = receive(greetd, 1.5)
                    assert early is None, f"asked greetd to switch before the shell drew its background: {early!r}"
                    assert process.poll() is None, "compositor exited while waiting for the shell"

                    with (root / "shell-gate").open("w") as gate:
                        gate.write("draw\n")

                    assert receive(greetd, 5.0) == b"READY=1", "no READY=1 after the shell drew its background"
                    assert receive(greetd, 0.5) is None, "unexpected extra message to greetd"
                except BaseException:
                    log.seek(0)
                    print(log.read(), file=sys.stderr)
                    raise
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait(timeout=5)


if __name__ == "__main__":
    check(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
    print("greetd is asked to switch only after the shell's background layer maps, not its bar")
