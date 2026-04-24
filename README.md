# kvstore

A networked, concurrent, persistent key-value storage engine written in C++23.

Designed from scratch as a systems programming project, kvstore implements
a Redis-comparable architecture: a custom wire protocol, in-memory hash map
storage, TCP networking, concurrent client handling, crash-resistant
persistence via a write-ahead log and periodic snapshotting, and hardened
input validation with clean shutdown and idle connection reaping.

## Architecture

- **Storage engine** — `std::unordered_map` with heterogeneous lookup, protected by `std::shared_mutex`
- **Protocol** — RESP (Redis Serialization Protocol) — compatible with redis-cli and any RESP client
- **Parser** — converts raw wire bytes into typed `Command` structs; enforces size limits on all length prefixes
- **Dispatcher** — executes commands against the store, returns typed results via `std::variant`
- **Encoder** — serializes results back to RESP wire format
- **Networking** — multi-threaded TCP server; each thread runs its own epoll event loop with `SO_REUSEPORT`
- **Concurrency** — reader-writer locking via `std::shared_mutex`; concurrent reads, exclusive writes; TSan verified
- **Persistence** — append-only write-ahead log with binary record format and crash recovery on startup
- **Snapshotting** — periodic full-state snapshot with atomic file replacement and WAL truncation
- **Robustness** — per-connection buffer cap, idle timeout reaping, clean shutdown via eventfd

## Persistence

kvstore uses a two-tier persistence strategy: a write-ahead log (WAL) for durability on every mutation,
and periodic snapshots to bound recovery time.

### Write-Ahead Log

Every mutating command (SET, DEL) is serialized to disk and fsynced before being applied to the in-memory
store. On startup, the WAL is replayed in full to reconstruct any mutations since the last snapshot.

### Snapshotting

A background thread writes a full snapshot of the current store state to disk every 60 seconds.
The snapshot uses an atomic write sequence to guarantee no crash window:

1. Write all live key-value pairs as SET records to `kvstore.snapshot.tmp`
2. `fsync` the tmp file guarantee it reaches physical storage
3. `rename` tmp to `kvstore.snapshot` atomic directory entry swap, no half-states possible
4. Truncate `kvstore.wal` to zero all prior mutations are now covered by the snapshot

### Recovery Sequence

On startup, kvstore recovers state in two phases:

1. Load `kvstore.snapshot` if it exists, reconstructs state at the time of the last snapshot
2. Replay `kvstore.wal` on top applies any mutations that occurred after the snapshot

WAL replay is idempotent: if the server crashed between the snapshot rename and the WAL truncation,
the WAL still replays cleanly. `insert_or_assign` on an already-present key is a no-op in terms of correctness.

### Record Format

Both the WAL and snapshot use the same binary record layout:

```
| length (4B) | checksum (4B) | opcode (1B) | key_len (4B) | key | value_len (4B) | value |
```

- **length** — total payload size in bytes; used to detect partial writes on recovery
- **checksum** — reserved for future integrity validation
- **opcode** — `0x00` for SET, `0x01` for DEL
- All multi-byte integers are little-endian

Partial records at the end of the WAL (caused by a crash mid-write) are detected via length mismatch and discarded.

## Robustness

### Input Validation

- All RESP length prefixes are bounds-checked against `MAX_KEY_SIZE`, `MAX_VALUE_SIZE`, and `MAX_ARG_COUNT`
- `std::stoi` calls are wrapped in try-catch to handle overflow on malformed length fields
- Per-connection accumulation buffer is capped at `MAX_BUFFER_SIZE` (1MB); connections exceeding the limit are closed

### Slow Client Defence

Connections that send no data for 30 seconds are reaped by a background heartbeat running on every
`epoll_wait()` timeout. This prevents a Slowloris-style attack from exhausting connection slots.

### Clean Shutdown

A global `eventfd` is registered with every thread's epoll instance on startup. On `SIGINT`, the signal
handler writes to the eventfd, waking all threads simultaneously. Each thread checks `keep_running`,
finds it false, and exits. The signal handler contains only async-signal-safe calls.

## Performance

Benchmarked with `redis-benchmark` on a local machine, 50 concurrent clients, 100k requests:

| Operation | Throughput | p50 latency | p99 latency |
|-----------|-----------|-------------|-------------|
| GET       | ~50,000 req/s | 0.5ms | 1.1ms |
| SET       | ~5,000 req/s  | 9.5ms | 25ms  |

The GET/SET gap is intentional and expected. Every SET fsyncs the WAL to physical storage before
sending a response. This is the same durability guarantee PostgreSQL provides with
`synchronous_commit = on`. No acknowledged write is ever lost, even on a hard kill (`kill -9`).

GET operations acquire a shared reader lock and never touch disk, which is why they are roughly
10x faster.

To reproduce:

```bash
redis-benchmark -p 6379 -t set,get -n 100000 -c 50
```

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

To build with Thread Sanitizer for race detection during development:

```bash
make kvstore-tsan
```

## Run

```bash
./kvstore
```

Server spawns one thread per logical core via `std::thread::hardware_concurrency()`.
Each thread binds its own `SO_REUSEPORT` socket and runs an independent epoll loop.
All threads share a single `KVStore` instance protected by a reader-writer lock.
WAL is written to `kvstore.wal` in the working directory.
Snapshot is written to `kvstore.snapshot` every 60 seconds.
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