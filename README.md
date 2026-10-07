# kvstore

A networked, concurrent, persistent key-value store written in C++23 for Linux.

kvstore uses an in-memory hash table, a RESP command interface, epoll-based networking, a write-ahead log, and periodic snapshots. The storage path is built around a simple rule: persistent mutation order must match the order recovered after restart.

## Architecture

- **Storage** — `std::unordered_map` protected by `std::shared_mutex`
- **Protocol** — RESP command subset used by the supported commands
- **Networking** — one epoll loop per worker thread with `SO_REUSEPORT`
- **Reads** — use the store's shared reader lock
- **Mutations** — `SET`, `DEL`, and `CLEAR` are serialized through the dispatcher
- **Persistence** — each mutation is written and synced to the WAL before it is applied in memory
- **Snapshots** — full-state checkpoint every 60 seconds
- **Shutdown** — `eventfd` wakes the worker loops on `SIGINT`

## Persistence

### Write-ahead log

Persistent mutations follow this order:

```text
WAL append -> fsync -> in-memory mutation -> response
```

The dispatcher serializes this sequence so WAL order and in-memory mutation order cannot diverge under concurrent writers.

The WAL records `SET`, `DEL`, and `CLEAR`.

### Checkpoints

Snapshot creation uses the same mutation lock as normal writes:

1. Copy the current store into `kvstore.snapshot.tmp`
2. `fsync` the snapshot file
3. Rename it to `kvstore.snapshot`
4. `fsync` the parent directory
5. Reset the WAL

Writes wait while a checkpoint is created. Reads can continue.

### Recovery

Startup recovery loads the snapshot first, then replays the WAL.

Snapshot recovery validates the entire file before applying any entries; truncated or malformed published snapshots are rejected without partially mutating the store.\n\nWAL recovery validates record sizes and field boundaries before using them. A partial final WAL record is treated as a torn tail and truncated back to the last complete record. Structurally invalid complete records and unknown opcodes are rejected.

### Persistence format

WAL and snapshot files use the same versioned record codec:

```text
WAL:      | KVWL (4B) | version (4B) | records... |
snapshot: | KVSS (4B) | version (4B) | records... |
record:   | length (4B) | CRC32C (4B) | opcode (1B) | key_len (4B) | key | value_len (4B) | value |
```

All integer fields are little-endian. Record payloads are checksummed before replay, and unsupported format versions are rejected. WAL opcodes are `0x00 SET`, `0x01 DEL`, and `0x02 CLEAR`; snapshots contain SET records.

## Networking

The server starts one worker per logical CPU. Each worker owns an epoll loop and its own listening socket using `SO_REUSEPORT`.

Connections have a bounded input buffer and are closed after 30 seconds of inactivity.

The server listens on port `6379`.

## Commands

| Command | Description |
| --- | --- |
| `SET key value` | Store a key-value pair |
| `GET key` | Retrieve a value |
| `DEL key` | Delete a key |
| `EXISTS key` | Check whether a key exists |
| `KEYS` | List all keys |
| `SIZE` | Return the number of keys |
| `CLEAR` | Delete all keys |

## Requirements

- Linux
- GCC 13 or later
- GNU Make

## Build

```bash
git clone https://github.com/himasudo/kvstore.git
cd kvstore
make
```

## Run

```bash
./kvstore
```

Runtime files are created in the working directory:

```text
kvstore.wal
kvstore.snapshot
```

## Connect

The supported RESP commands can be used through `redis-cli`:

```bash
redis-cli -p 6379 SET foo bar
redis-cli -p 6379 GET foo
redis-cli -p 6379 SIZE
```

Raw RESP works as well:

```bash
printf "*3\r\n\$3\r\nSET\r\n\$3\r\nfoo\r\n\$3\r\nbar\r\n" | nc localhost 6379
```

## Tests

Build the test binaries:

```bash
make test
```

The suite covers the command pipeline, WAL and snapshot recovery, record-format integrity, concurrent mutation ordering, checkpoint/write races, and malformed or torn persistence records.

For a longer concurrent ordering run:

```bash
make stress
```

ThreadSanitizer targets are available for the concurrency-sensitive paths:

```bash
make test_mutation_ordering-tsan
./test_mutation_ordering-tsan

make test_checkpoint-tsan
./test_checkpoint-tsan
```

## License

MIT — see [LICENSE](LICENSE).
