#!/usr/bin/env python3
"""Unusable data files must stop startup with a message and exit status 1.

The server must not abort (SIGABRT), must not hang, and must leave the files it
refused to load exactly as they were.
"""

from pathlib import Path
import os
import subprocess
import sys
import tempfile

WAL_MAGIC = b"KVWL"


def wal_header(version: int = 1) -> bytes:
    return WAL_MAGIC + version.to_bytes(4, "little")


def record(key: bytes, value: bytes) -> bytes:
    payload = (b"\x00" + len(key).to_bytes(4, "little") + key +
               len(value).to_bytes(4, "little") + value)
    return len(payload).to_bytes(4, "little") + bytes(4) + payload  # checksum deliberately wrong


def run_server(binary: str, cwd: str) -> subprocess.CompletedProcess:
    return subprocess.run([binary], cwd=cwd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True, timeout=10)


def expect_failed_start(binary: str, label: str, setup, expect_text: str) -> None:
    with tempfile.TemporaryDirectory(prefix="kvstore-startup-errors-") as cwd:
        setup(Path(cwd))
        before = {p.name: p.read_bytes() for p in Path(cwd).iterdir() if p.is_file()}
        proc = run_server(binary, cwd)
        assert proc.returncode == 1, (
            f"{label}: expected exit status 1, got {proc.returncode}\n{proc.stderr}")
        assert expect_text in proc.stderr, f"{label}: stderr was:\n{proc.stderr}"
        after = {name: (Path(cwd) / name).read_bytes() for name in before}
        assert before == after, f"{label}: refused start modified data files"
    print(f"{label}: exit 1 with a message")


def corrupt_middle(d: Path) -> None:
    # Two records with wrong checksums: the first is followed by more data, so
    # this is corruption rather than a torn tail.
    (d / "kvstore.wal").write_bytes(wal_header() + record(b"a", b"1") + record(b"b", b"2"))


def bad_magic(d: Path) -> None:
    (d / "kvstore.wal").write_bytes(b"NOPE" + (1).to_bytes(4, "little"))


def future_version(d: Path) -> None:
    (d / "kvstore.wal").write_bytes(wal_header(version=99))


def wal_is_directory(d: Path) -> None:
    (d / "kvstore.wal").mkdir()


def garbage_snapshot(d: Path) -> None:
    (d / "kvstore.snapshot").write_bytes(b"this is not a snapshot")


def run(binary: str) -> None:
    expect_failed_start(binary, "corruption in the middle of the WAL", corrupt_middle,
                        "WAL recovery failed")
    expect_failed_start(binary, "WAL with wrong magic", bad_magic, "WAL recovery failed")
    expect_failed_start(binary, "WAL with unsupported version", future_version,
                        "unsupported format version")
    expect_failed_start(binary, "WAL path is a directory", wal_is_directory,
                        "WAL recovery failed")
    expect_failed_start(binary, "garbage snapshot", garbage_snapshot,
                        "Snapshot recovery failed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
