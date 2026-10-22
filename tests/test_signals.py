#!/usr/bin/env python3
"""Signal handling: SIGTERM/SIGINT shut down cleanly, SIGPIPE is ignored."""

from pathlib import Path
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

PORT = 6379
WAL_HEADER_SIZE = 8


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
        assert proc.poll() is None, "server exited during startup"
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.05)
    raise AssertionError("server did not start")


def clean_shutdown(binary: str, sig: signal.Signals) -> None:
    with tempfile.TemporaryDirectory(prefix="kvstore-signals-") as cwd:
        proc = start(binary, cwd)
        try:
            assert command(resp("SET", "k", "v") + resp("SET", "k2", "v2")) == b"+OK\r\n+OK\r\n"
            began = time.time()
            proc.send_signal(sig)
            _, err = proc.communicate(timeout=10)
            elapsed = time.time() - began
        finally:
            if proc.poll() is None:
                proc.kill()
        assert proc.returncode == 0, f"{sig.name}: exit status {proc.returncode}\n{err}"
        assert elapsed < 3, f"{sig.name}: shutdown took {elapsed:.1f}s"

        # The final checkpoint moved everything into the snapshot.
        wal = Path(cwd) / "kvstore.wal"
        assert wal.stat().st_size == WAL_HEADER_SIZE, f"WAL has {wal.stat().st_size} bytes"
        assert (Path(cwd) / "kvstore.snapshot").stat().st_size > WAL_HEADER_SIZE

        again = start(binary, cwd)
        try:
            assert command(resp("GET", "k")) == b"$1\r\nv\r\n"
            assert command(resp("GET", "k2")) == b"$2\r\nv2\r\n"
        finally:
            again.send_signal(signal.SIGTERM)
            again.communicate(timeout=10)
    print(f"{sig.name}: exit 0 in {elapsed:.2f}s, WAL emptied into the snapshot, data survives restart")


def sigpipe_is_ignored(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix="kvstore-sigpipe-") as cwd:
        proc = start(binary, cwd)
        try:
            os.kill(proc.pid, signal.SIGPIPE)
            time.sleep(0.3)
            assert proc.poll() is None, "SIGPIPE killed the server"
            assert command(resp("SET", "k", "v")) == b"+OK\r\n"
        finally:
            proc.send_signal(signal.SIGTERM)
            proc.communicate(timeout=10)
    print("SIGPIPE is ignored")


def run(binary: str) -> None:
    clean_shutdown(binary, signal.SIGTERM)
    clean_shutdown(binary, signal.SIGINT)
    sigpipe_is_ignored(binary)


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
