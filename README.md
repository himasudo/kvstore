# kvstore

A networked, concurrent, persistent key-value storage engine written in C++23.

Designed from scratch as a systems programming project, kvstore implements
a Redis-comparable architecture: a custom wire protocol, in-memory hash map
storage, TCP networking, concurrent client handling, and crash-resistant
persistence via a write-ahead log.

## Status

Active development. Protocol layer complete (parser, dispatcher, encoder).
Networking layer next.

## Architecture

- **Storage engine** — `std::unordered_map` with heterogeneous lookup
- **Protocol** — custom RESP-inspired length-prefixed binary-safe text protocol
- **Parser** — converts raw wire bytes into typed `Command` structs
- **Dispatcher** — executes commands against the store, returns typed results via `std::variant`
- **Encoder** — serializes results back to wire format
- **Networking** — TCP server with epoll-based event loop (planned)
- **Concurrency** — reader-writer locking via `std::shared_mutex` (planned)
- **Persistence** — write-ahead log with crash recovery (planned)

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
- Linux (POSIX networking planned for Layer 3)
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

## Testing

```bash
make test
./test_pipeline
```

## License

MIT — see [LICENSE](LICENSE) for details.