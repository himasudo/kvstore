#!/usr/bin/env python3
"""Concurrent TCP streams, protocol/EOF edges, and slow-reader memory bounds."""

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

from test_tcp_protocol import connect, encode, response, wait_ready


def rss_kib(pid: int) -> int:
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    raise AssertionError("VmRSS not reported by Linux")


def parallel_clients():
    workers = 12
    barrier = threading.Barrier(workers, timeout=15)

    def client(i: int):
        with connect() as sock:
            sock.settimeout(25)
            wire = bytearray()
            expected = []
            for j in range(12):
                key = f"stress-{i}-{j}".encode()
                val = (f"data-{i}-{j}".encode() + bytes([i, j, 0, 13, 10]))
                wire += encode(b"SET", key, val) + encode(b"GET", key)
                expected.extend([b"OK", val])
            barrier.wait()
            if i % 2:
                sock.sendall(wire[:3])
                time.sleep(0.006)
                sock.sendall(wire[3:])
            else:
                sock.sendall(wire)
            if i % 3 == 0:
                sock.shutdown(socket.SHUT_WR)
            for want in expected:
                got = response(sock)
                assert got == want, (i, want, got)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(client, i) for i in range(workers)]
        for future in futures:
            future.result(timeout=45)

    with connect() as sock:
        for i in range(workers):
            for j in range(12):
                key = f"stress-{i}-{j}".encode()
                expected = f"data-{i}-{j}".encode() + bytes([i, j, 0, 13, 10])
                sock.sendall(encode(b"GET", key))
                assert response(sock) == expected


def protocol_edges():
    with connect() as sock:
        sock.sendall(encode(b"SET", b"valid-prefix", b"preserved") +
                     b"*1\r\n$3\r\nBAD\r\n")
        assert response(sock) == b"OK"
        assert response(sock)[0] == b"ERR"
        assert sock.recv(1) == b""

    with connect() as sock:
        sock.sendall(encode(b"GET", b"valid-prefix") + b"*2\r\n$3\r\nGET\r\n$6\r\nabc")
        sock.shutdown(socket.SHUT_WR)
        assert response(sock) == b"preserved"
        assert response(sock)[0] == b"ERR"
        assert sock.recv(1) == b""

    with connect() as sock:
        sock.sendall(b"*1\r\n$3\r\nGE")
        sock.shutdown(socket.SHUT_WR)
        assert response(sock)[0] == b"ERR"
        assert sock.recv(1) == b""

    for _ in range(40):
        with connect() as sock:
            sock.sendall(b"*2\r\n$3\r\nGET\r\n$4\r\nabcx\r\n")
        with connect() as sock:
            sock.sendall(encode(b"GET", b"valid-prefix"))
            assert response(sock) == b"preserved"


def slow_reader_bounds(pid: int):
    with connect() as sock:
        payload = b"z" * (256 * 1024)
        sock.sendall(encode(b"SET", b"slow-reader", payload))
        assert response(sock) == b"OK"

    before = rss_kib(pid)
    with connect() as blocked:
        blocked.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
        # Responses exceed 700 MiB without backpressure; do not read them.
        blocked.sendall(encode(b"GET", b"slow-reader") * 3000)
        time.sleep(0.5)
        during = rss_kib(pid)
        assert during - before < 64 * 1024, (before, during)

        started = time.monotonic()
        with connect() as fast:
            fast.sendall(encode(b"SET", b"fast", b"still-alive") +
                         encode(b"GET", b"fast"))
            assert response(fast) == b"OK"
            assert response(fast) == b"still-alive"
        assert time.monotonic() - started < 5.0

    with connect() as sock:
        sock.sendall(encode(b"GET", b"fast"))
        assert response(sock) == b"still-alive"


def main(binary: str):
    with tempfile.TemporaryDirectory(prefix="kvstore-net-stress-") as cwd:
        proc = subprocess.Popen([binary], cwd=cwd, stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, text=True)
        try:
            wait_ready(proc)
            parallel_clients()
            protocol_edges()
            slow_reader_bounds(proc.pid)
            assert proc.poll() is None, "server crashed during network stress"
            print("Network concurrency, protocol EOF and backpressure stress passed")
        finally:
            if proc.poll() is None:
                os.kill(proc.pid, signal.SIGKILL)
            proc.wait(timeout=5)


if __name__ == "__main__":
    main(str(Path(sys.argv[1]).resolve()))
