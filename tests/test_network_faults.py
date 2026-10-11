#!/usr/bin/env python3
"""Single-use test-only syscall failures in the real epoll networking path."""

import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile

from test_tcp_protocol import connect, encode, response, wait_ready


def check_fault(binary: str, fault: str) -> None:
    with tempfile.TemporaryDirectory(prefix="kvstore-net-fault-") as cwd:
        env = os.environ.copy()
        env["KVSTORE_NET_FAULT"] = fault
        proc = subprocess.Popen([binary], cwd=cwd, env=env,
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, text=True)
        try:
            wait_ready(proc)
            with connect() as sock:
                sock.sendall(encode(b"SIZE") + encode(b"SIZE"))
                if fault == "send_eio":
                    try:
                        data = sock.recv(64)
                        assert data == b"", data
                    except (ConnectionResetError, BrokenPipeError):
                        pass
                else:
                    assert response(sock) == 0
                    assert response(sock) == 0

            # Hard connection failures must not kill the listening process.
            with connect() as sock:
                sock.sendall(encode(b"SIZE"))
                assert response(sock) == 0

            assert proc.poll() is None, (fault, proc.returncode)
        finally:
            if proc.poll() is None:
                os.kill(proc.pid, signal.SIGKILL)
            _, stderr = proc.communicate(timeout=5)
        assert f"NET_FAULT_TRIGGERED:{fault}" in stderr, (fault, stderr)


def main(binary: str) -> None:
    for fault in (
        "accept_eintr", "accept_eagain",
        "read_eintr", "read_eagain",
        "send_eintr", "send_eagain", "send_short", "send_eio",
    ):
        check_fault(binary, fault)
    print("Network syscall error-path tests passed")


if __name__ == "__main__":
    main(str(Path(sys.argv[1]).resolve()))
