# kvstore
 
A networked, concurrent, persistent key-value storage engine written in C++23.
 
Designed from scratch as a systems programming project, kvstore implements
a Redis-comparable architecture: a custom wire protocol, in-memory hash map
storage, TCP networking, concurrent client handling, and crash-resistant
persistence via a write-ahead log.
 
## Status
 
Active development. TCP networking layer complete (epoll event loop, RESP protocol).
Concurrency layer next.
 
## Architecture
 
- **Storage engine** — `std::unordered_map` with heterogeneous lookup
- **Protocol** — RESP (Redis Serialization Protocol) — compatible with redis-cli and any RESP client
- **Parser** — converts raw wire bytes into typed `Command` structs
- **Dispatcher** — executes commands against the store, returns typed results via `std::variant`
- **Encoder** — serializes results back to RESP wire format
- **Networking** — single-threaded TCP server with epoll-based level-triggered event loop
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
 
Server listens on port 6379 by default.
 
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