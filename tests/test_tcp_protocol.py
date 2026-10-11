#!/usr/bin/env python3
"""End-to-end RESP stream tests against the real kvstore TCP server."""

import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HOST = "127.0.0.1"
PORT = 6379


def encode(*args: bytes) -> bytes:
    parts = [f"*{len(args)}\r\n".encode()]
    for arg in args:
        parts.extend((f"\x24{len(arg)}\r\n".encode(), arg, b"\r\n"))
    return b"".join(parts)


def exact(sock: socket.socket, size: int) -> bytes:
    chunks = []
    while size:
        chunk = sock.recv(size)
        if not chunk:
            raise AssertionError("connection closed before complete response")
        chunks.append(chunk)
        size -= len(chunk)
    return b"".join(chunks)


def line(sock: socket.socket) -> bytes:
    buf = bytearray()
    while not buf.endswith(b"\r\n"):
        buf.extend(exact(sock, 1))
        if len(buf) > 64:
            raise AssertionError("invalid RESP response line")
    return bytes(buf[:-2])


def response(sock: socket.socket):
    typ = exact(sock, 1)
    if typ == b"+":
        return line(sock)
    if typ == b"-":
        return (b"ERR", line(sock))
    if typ == b":":
        return int(line(sock))
    if typ == b"$":
        count = int(line(sock))
        if count == -1:
            return None
        payload = exact(sock, count)
        assert exact(sock, 2) == b"\r\n"
        return payload
    if typ == b"*":
        count = int(line(sock))
        return [response(sock) for _ in range(count)]
    raise AssertionError(f"unexpected response prefix: {typ!r}")


def connect() -> socket.socket:
    sock = socket.create_connection((HOST, PORT), timeout=4)
    sock.settimeout(8)
    return sock


def wait_ready(proc: subprocess.Popen) -> None:
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            err = proc.stderr.read() if proc.stderr else ""
            raise AssertionError(f"server exited during startup: {err}")
        try:
            with socket.create_connection((HOST, PORT), timeout=0.1):
                return
        except OSError:
            time.sleep(0.03)
    raise AssertionError("server did not start listening")


def run(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix="kvstore-network-") as cwd:
        proc = subprocess.Popen([binary], cwd=cwd, stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, text=True)
        try:
            wait_ready(proc)

            # Fragment the array count, bulk lengths, payload and CRLFs.
            with connect() as sock:
                for fragment in (b"*3\r", b"\n$3\r\nSE", b"T\r\n$4\r\nf",
                                 b"rag\r\n$5\r\nval", b"ue\r", b"\n"):
                    sock.sendall(fragment)
                    time.sleep(0.004)
                assert response(sock) == b"OK"
                sock.sendall(encode(b"GET", b"frag"))
                assert response(sock) == b"value"

            # Multiple commands in one buffer, followed by an incomplete suffix.
            with connect() as sock:
                first = encode(b"SET", b"pipe", b"one")
                second = encode(b"GET", b"pipe")
                third = encode(b"SET", b"pipe", b"two")
                fourth = encode(b"GET", b"pipe")
                sock.sendall(first + second + third + fourth[:-2])
                assert response(sock) == b"OK"
                assert response(sock) == b"one"
                assert response(sock) == b"OK"
                sock.sendall(fourth[-2:])
                assert response(sock) == b"two"

            binary = b"a\r\n\x00b\xff"
            with connect() as sock:
                sock.sendall(encode(b"SET", b"binary", binary) +
                             encode(b"GET", b"binary"))
                assert response(sock) == b"OK"
                assert response(sock) == binary

            # Half-close must not discard replies for already received commands.
            with connect() as sock:
                sock.sendall(encode(b"SET", b"half", b"closed") +
                             encode(b"GET", b"half"))
                sock.shutdown(socket.SHUT_WR)
                assert response(sock) == b"OK"
                assert response(sock) == b"closed"
                assert sock.recv(1) == b""

            with connect() as sock:
                sock.sendall(b"*1\r\n$3\r\nBAD\r\n")
                err = response(sock)
                assert err[0] == b"ERR", err
                assert sock.recv(1) == b""

            # A paused reader forces multiple nonblocking output flushes.
            large = b"x" * 65536
            with connect() as sock:
                sock.sendall(encode(b"SET", b"large", large))
                assert response(sock) == b"OK"
                sock.sendall(encode(b"GET", b"large") * 80)
                time.sleep(0.15)
                for _ in range(80):
                    assert response(sock) == large
                sock.sendall(encode(b"SIZE"))
                assert response(sock) >= 1

            print("TCP fragmentation, pipelining, half-close and slow-reader tests passed")
        finally:
            if proc.poll() is None:
                os.kill(proc.pid, signal.SIGKILL)
            proc.wait(timeout=5)


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
