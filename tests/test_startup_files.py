#!/usr/bin/env python3
"""Files the server creates at startup are private, and leftovers are cleaned."""

from pathlib import Path
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time

PORT = 6379


def start(binary: str, cwd: str) -> subprocess.Popen:
    proc = subprocess.Popen([binary], cwd=cwd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    deadline = time.time() + 5
    while time.time() < deadline:
        assert proc.poll() is None, "server exited during startup"
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.05)
    raise AssertionError("server did not start")


def run(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix="kvstore-startup-files-") as cwd:
        d = Path(cwd)
        (d / "kvstore.snapshot.tmp").write_bytes(b"interrupted checkpoint")

        proc = start(binary, cwd)
        try:
            assert not (d / "kvstore.snapshot.tmp").exists(), "stale temporary snapshot kept"
            for name in ("kvstore.wal", "kvstore.lock"):
                mode = stat.S_IMODE((d / name).stat().st_mode)
                assert mode == 0o600, f"{name} has mode {oct(mode)}"
        finally:
            proc.send_signal(signal.SIGINT)
            proc.communicate(timeout=10)
    print("stale temporary snapshot removed; WAL and lock file are mode 0600")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
