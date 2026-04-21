# kvstore

A networked, concurrent, persistent key-value storage engine written in C++23.

Designed from scratch as a systems programming project, kvstore implements
a Redis-comparable architecture: a custom wire protocol, in-memory hash map
storage, TCP networking, concurrent client handling, and crash-resistant
persistence via a write-ahead log.

## Status

Active development. Persistence layer complete (write-ahead log, binary record format, crash recovery).
Hardening layer next.

## Architecture

- **Storage engine** — `std::unordered_map` with heterogeneous lookup, protected by `std::shared_mutex`
- **Protocol** — RESP (Redis Serialization Protocol) — compatible with redis-cli and any RESP client
- **Parser** — converts raw wire bytes into typed `Command` structs
- **Dispatcher** — executes commands against the store, returns typed results via `std::variant`
- **Encoder** — serializes results back to RESP wire format
- **Networking** — multi-threaded TCP server; each thread runs its own epoll event loop with `SO_REUSEPORT`
- **Concurrency** — reader-writer locking via `std::shared_mutex`; concurrent reads, exclusive writes; TSan verified
- **Persistence** — append-only write-ahead log with binary record format and crash recovery on startup

## Persistence

kvstore uses a write-ahead log (WAL) for crash-resistant persistence. Every mutating command (SET, DEL) is serialized to disk and fsynced before being applied to the in-memory store. On startup, the WAL is replayed in full to reconstruct state.

### Record Format

Each WAL record uses a fixed binary layout:

```
| length (4B) | checksum (4B) | opcode (1B) | key_len (4B) | key | value_len (4B) | value |
```

- **length** — total payload size in bytes; used to detect partial writes on recovery
- **checksum** — reserved for future integrity validation
- **opcode** — `0x00` for SET, `0x01` for DEL
- All multi-byte integers are little-endian

Partial records at the end of the WAL (caused by a crash mid-write) are detected via length mismatch and discarded. The WAL file is opened with `O_APPEND` to guarantee all writes go to the end of the file.

## Commands

| Command | Description |
|---------|-------------|
| `SET key value` | Store a key-value pair |
| `GET key` | Retrieve a value by key |
| `DEL key` | Delete a key |
| `EXISTS key` | Check if a key exists |
| `KEYS` | List all keys |
| `SIZE` | Return number of keys |
| `CLEAR` | Delete all keys |

## Requirements

- GCC 13 or later (C++23 required)
- Linux (epoll — Linux only)
- GNU Make

## Build

```bash
git clone https://github.com/yourusername/kvstore.git
cd kvstore
make
```

## Run

```bash
./kvstore
```

Server spawns one thread per logical core via `std::thread::hardware_concurrency()`.
Each thread binds its own `SO_REUSEPORT` socket and runs an independent epoll loop.
All threads share a single `KVStore` instance protected by a reader-writer lock.
WAL is written to `kvstore.wal` in the working directory.
Listens on port 6379 by default.

## Connect

Compatible with any RESP client including redis-cli:

```bash
redis-cli -p 6379 SET foo bar
redis-cli -p 6379 GET foo
redis-cli -p 6379 KEYS
```

Or with netcat using raw RESP:

```bash
printf "*3\r\n\$3\r\nSET\r\n\$3\r\nfoo\r\n\$3\r\nbar\r\n" | nc localhost 6379
```

## Testing

```bash
make test
./test_pipeline
```

## License

MIT — see [LICENSE](LICENSE) for details.