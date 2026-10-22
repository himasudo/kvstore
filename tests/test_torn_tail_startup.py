#!/usr/bin/env python3
"""A crash during the last WAL write must not stop the server from starting.

Writes two keys, kills the server with SIGKILL, damages the end of the WAL the
way an interrupted write can, and restarts. Damage with real data after it
must still refuse to start.
"""

from pathlib import Path
import os
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


def start(binary: str, cwd: str) -> subprocess.Popen:
    proc = subprocess.Popen([binary], cwd=cwd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
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


def command(payload: bytes) -> bytes:
    with socket.create_connection(("127.0.0.1", PORT), timeout=3) as sock:
        sock.sendall(payload)
        sock.settimeout(3)
        return sock.recv(4096)


def stop(proc: subprocess.Popen, sig: int) -> None:
    if proc.poll() is None:
        proc.send_signal(sig)
    proc.wait(timeout=10)
    if proc.stderr:
        proc.stderr.close()


def seed(binary: str, cwd: str) -> None:
    proc = start(binary, cwd)
    try:
        assert command(resp("SET", "a", "1") + resp("SET", "b", "2")) == b"+OK\r\n+OK\r\n"
    finally:
        stop(proc, signal.SIGKILL)


def flip_last_byte(wal: Path) -> None:
    data = bytearray(wal.read_bytes())
    data[-1] ^= 0xFF
    wal.write_bytes(data)


def zero_last_record(wal: Path) -> None:
    # Record "b" = 8 byte header + 1 + 4 + 1 + 4 + 1 byte payload = 19 bytes.
    data = bytearray(wal.read_bytes())
    data[-19:] = bytes(19)
    wal.write_bytes(data)


def append_zeros(wal: Path) -> None:
    with wal.open("ab") as f:
        f.write(bytes(64))


def flip_first_record(wal: Path) -> None:
    data = bytearray(wal.read_bytes())
    data[8 + 8 + 2] ^= 0xFF  # inside record "a", which has record "b" after it
    wal.write_bytes(data)


def expect_start(binary: str, damage, label: str, expected_b: bytes) -> None:
    with tempfile.TemporaryDirectory(prefix="kvstore-torn-tail-") as cwd:
        seed(binary, cwd)
        damage(Path(cwd) / "kvstore.wal")
        proc = start(binary, cwd)
        try:
            assert proc.poll() is None, f"{label}: server refused to start"
            assert command(resp("GET", "a")) == b"$1\r\n1\r\n", label
            assert command(resp("GET", "b")) == expected_b, label
        finally:
            stop(proc, signal.SIGINT)
    print(f"{label}: starts and recovers the intact records")


def expect_refusal(binary: str, damage, label: str) -> None:
    with tempfile.TemporaryDirectory(prefix="kvstore-corrupt-") as cwd:
        seed(binary, cwd)
        damage(Path(cwd) / "kvstore.wal")
        proc = start(binary, cwd)
        try:
            assert proc.poll() is not None, f"{label}: server started on a corrupt WAL"
            assert proc.returncode != 0, label
        finally:
            stop(proc, signal.SIGKILL)
    print(f"{label}: refuses to start")


def run(binary: str) -> None:
    missing_b = b"$-1\r\n"
    expect_start(binary, flip_last_byte, "final record bit flip", missing_b)
    expect_start(binary, zero_last_record, "final record zero-filled", missing_b)
    expect_start(binary, append_zeros, "zero padding after records", b"$1\r\n2\r\n")
    expect_refusal(binary, flip_first_record, "damaged record followed by data")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
