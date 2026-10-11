#!/usr/bin/env python3
"""A failed listening socket must fail startup instead of hanging forever."""

from pathlib import Path
import os
import signal
import socket
import subprocess
import sys
import tempfile


def run(binary: str) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as blocker:
        # Prior test connections can leave TIME_WAIT entries on port 6379.
        blocker.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        blocker.bind(("127.0.0.1", 6379))
        blocker.listen(1)

        with tempfile.TemporaryDirectory(prefix="kvstore-bind-failure-") as cwd:
            proc = subprocess.Popen([binary], cwd=cwd, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.PIPE, text=True)
            try:
                try:
                    rc = proc.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    raise AssertionError("server hung after all listeners failed to bind")
                assert rc != 0, f"failed listener unexpectedly exited with {rc}"
            finally:
                if proc.poll() is None:
                    os.kill(proc.pid, signal.SIGKILL)
                proc.communicate(timeout=5)
    print("Listener startup failure exits with nonzero status")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
