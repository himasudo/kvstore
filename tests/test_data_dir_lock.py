#!/usr/bin/env python3
"""Only one server may use a data directory at a time."""

from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

PORT = 6379


def resp(*args: str) -> bytes:
    out = b"*%d\r\n" % len(args)
    for arg in args:
        data = arg.encode()
        out += b"$%d\r\n%s\r\n" % (len(data), data)
    return out


def command(payload: bytes) -> bytes:
    with socket.create_connection(("127.0.0.1", PORT), timeout=3) as sock:
        sock.sendall(payload)
        sock.settimeout(3)
        return sock.recv(4096)


def start(binary: str, cwd: str) -> subprocess.Popen:
    proc = subprocess.Popen([binary], cwd=cwd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)
    deadline = time.time() + 5
    while time.time() < deadline:
        if proc.poll() is not None:
            return proc
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.05)
    raise AssertionError("server neither started nor exited")


def stop(proc: subprocess.Popen, sig: int) -> str:
    if proc.poll() is None:
        proc.send_signal(sig)
    _, err = proc.communicate(timeout=10)
    return err


def run(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix="kvstore-lock-") as cwd:
        first = start(binary, cwd)
        try:
            assert first.poll() is None, "first instance failed to start"
            assert command(resp("SET", "k", "v")) == b"+OK\r\n"

            # The port is already open (first instance), so wait for the exit
            # itself instead of for a listener.
            second = subprocess.Popen([binary], cwd=cwd, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.PIPE, text=True)
            try:
                _, err = second.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                second.kill()
                raise AssertionError("second instance started in the same directory")
            assert second.returncode == 1, f"exit status {second.returncode}"
            assert "already using this data directory" in err, err
            print("second instance in the same directory exits 1 with a message")

            assert first.poll() is None, "first instance died"
            assert command(resp("GET", "k")) == b"$1\r\nv\r\n"
            print("first instance keeps serving")
        finally:
            stop(first, signal.SIGKILL)  # no clean shutdown: the lock must not go stale

        third = start(binary, cwd)
        try:
            assert third.poll() is None, "restart after SIGKILL was refused by a stale lock"
            assert command(resp("GET", "k")) == b"$1\r\nv\r\n"
            print("restart after SIGKILL succeeds and recovers the data")
        finally:
            stop(third, signal.SIGINT)


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
