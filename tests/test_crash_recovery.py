#!/usr/bin/env python3

import argparse
import os
import random
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

HOST = "127.0.0.1"
PORT = 6379


def encode_command(*parts: str) -> bytes:
    encoded = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        data = part.encode()
        encoded.append(f"\x24{len(data)}\r\n".encode())
        encoded.append(data)
        encoded.append(b"\r\n")
    return b"".join(encoded)


def read_exact(sock: socket.socket, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise RuntimeError("connection closed while reading response")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_line(sock: socket.socket) -> bytes:
    data = bytearray()
    while True:
        data.extend(read_exact(sock, 1))
        if data.endswith(b"\r\n"):
            return bytes(data[:-2])


def read_response(sock: socket.socket):
    prefix = read_exact(sock, 1)
    if prefix == b"+":
        return read_line(sock).decode()
    if prefix == b"-":
        raise RuntimeError(read_line(sock).decode())
    if prefix == b":":
        return int(read_line(sock))
    if prefix == b"$":
        size = int(read_line(sock))
        if size == -1:
            return None
        data = read_exact(sock, size)
        if read_exact(sock, 2) != b"\r\n":
            raise RuntimeError("invalid bulk-string terminator")
        return data.decode()
    if prefix == b"*":
        count = int(read_line(sock))
        if count == -1:
            return None
        return [read_response(sock) for _ in range(count)]
    raise RuntimeError(f"unknown RESP prefix: {prefix!r}")


def command(*parts: str):
    with socket.create_connection((HOST, PORT), timeout=2.0) as sock:
        sock.sendall(encode_command(*parts))
        return read_response(sock)


def wait_for_server(proc: subprocess.Popen, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            stderr = proc.stderr.read() if proc.stderr else ""
            raise RuntimeError(f"server exited during startup:\n{stderr}")
        try:
            with socket.create_connection((HOST, PORT), timeout=0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("server did not start listening in time")


def start_server(binary: str, data_dir: str) -> subprocess.Popen:
    proc = subprocess.Popen(
        [binary],
        cwd=data_dir,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    wait_for_server(proc)
    return proc


def kill_server(proc: subprocess.Popen) -> None:
    if proc.poll() is None:
        os.kill(proc.pid, signal.SIGKILL)
    proc.wait(timeout=5)


def verify_state(expected: dict[str, str]) -> None:
    size = command("SIZE")
    if size != len(expected):
        raise AssertionError(f"SIZE mismatch: expected {len(expected)}, got {size}")

    keys = command("KEYS")
    if set(keys) != set(expected):
        raise AssertionError(f"KEYS mismatch: expected {set(expected)}, got {set(keys)}")

    for key, value in expected.items():
        actual = command("GET", key)
        if actual != value:
            raise AssertionError(f"GET {key!r}: expected {value!r}, got {actual!r}")


def run_acknowledged_crashes(binary: str, data_dir: str, rng: random.Random) -> None:
    expected: dict[str, str] = {}
    keys = [f"key-{i}" for i in range(8)]

    for cycle in range(5):
        proc = start_server(binary, data_dir)
        try:
            for operation in range(20):
                choice = rng.randrange(10)
                key = rng.choice(keys)

                if choice < 6:
                    value = f"{cycle}:{operation}:{rng.getrandbits(32):08x}"
                    assert command("SET", key, value) == "OK"
                    expected[key] = value
                elif choice < 9:
                    result = command("DEL", key)
                    assert result in (0, 1)
                    expected.pop(key, None)
                else:
                    assert command("CLEAR") == "OK"
                    expected.clear()
        finally:
            kill_server(proc)

        proc = start_server(binary, data_dir)
        try:
            verify_state(expected)
        finally:
            kill_server(proc)


def run_inflight_crashes(binary: str, data_dir: str, rng: random.Random) -> None:
    probe_key = "crash-probe"

    for iteration in range(12):
        proc = start_server(binary, data_dir)
        base_value = f"base-{iteration}"
        candidate_value = f"candidate-{iteration}-" + ("x" * 65536)

        try:
            assert command("SET", probe_key, base_value) == "OK"

            sock = socket.create_connection((HOST, PORT), timeout=2.0)
            try:
                sock.sendall(encode_command("SET", probe_key, candidate_value))
                time.sleep(rng.uniform(0.0, 0.003))
                kill_server(proc)
            finally:
                sock.close()
        finally:
            if proc.poll() is None:
                kill_server(proc)

        proc = start_server(binary, data_dir)
        try:
            recovered = command("GET", probe_key)
            if recovered not in (base_value, candidate_value):
                raise AssertionError(
                    f"in-flight SET recovered impossible value: {recovered!r}"
                )
        finally:
            kill_server(proc)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", help="path to the kvstore server binary")
    args = parser.parse_args()

    binary = str(Path(args.binary).resolve())
    rng = random.Random(0xC0FFEE)

    with tempfile.TemporaryDirectory(prefix="kvstore-crash-") as data_dir:
        run_acknowledged_crashes(binary, data_dir, rng)
        run_inflight_crashes(binary, data_dir, rng)

    print("crash/restart recovery checks passed")


if __name__ == "__main__":
    main()
