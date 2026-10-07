# Correctness notes

This file records the guarantees the storage path is intended to provide and the gaps that are still being worked on.

## Mutation durability

For a mutating command, the WAL record must become durable before the mutation is acknowledged to the client.

`SET`, `DEL`, and `CLEAR` are all mutations and are represented in the WAL.

Mutations are currently serialized by the dispatcher. The mutation lock covers both WAL append/sync and the corresponding in-memory update. This makes WAL order and in-memory mutation order agree, so replay cannot produce a different committed ordering from the live store.

Reads do not take the mutation lock; they continue to use the store's shared mutex. A read may observe the old value while a write is still waiting for its WAL sync, which is valid because that write has not completed yet.

This is intentionally a correctness-first design. The single mutation lock also serializes fsync-heavy writes, so later performance work should replace it with an ordered WAL writer/group-commit path without weakening the ordering invariant.

## Recovery

Startup recovery is:

1. load the latest valid snapshot;
2. replay the WAL in order.

Replaying the WAL should reconstruct the same committed state that existed before the process stopped.

The mutation-ordering regression test compares the live store with a fresh store rebuilt only from the WAL after concurrent writes. A longer version can be run with `make stress`.

## Snapshot checkpointing

Checkpoint creation is serialized with mutations through the same dispatcher mutation lock. While that lock is held the server:

1. copies the current store into a temporary snapshot;
2. fsyncs the snapshot file;
3. renames it into place;
4. fsyncs the parent directory so the rename is durable;
5. resets the WAL.

A mutation therefore cannot land in the WAL after the snapshot's state was captured and then be removed by the WAL reset. Reads can continue during the checkpoint, but writes wait for it to finish.

This deliberately favors a simple correctness argument over write availability: snapshot I/O currently pauses mutations. A later LSN/segmented-WAL design should allow checkpoint I/O and new writes to proceed concurrently while retiring only WAL records known to be covered by the snapshot.

`test_checkpoint` races multiple writers against checkpoint creation and verifies that loading the snapshot followed by WAL replay reconstructs the live store.

## WAL recovery boundaries

WAL recovery treats the file as untrusted input. Before allocating a record payload it validates the advertised record size against fixed minimum and maximum bounds. Key/value lengths are then checked against the actual record boundary before any string is constructed.

A partial final header or payload is treated as a torn tail. Recovery truncates the file back to the last complete record and fsyncs that repair. Structural corruption inside a complete record, an unknown opcode, or invalid command-specific fields is treated as corruption rather than silently skipped.

WAL files now carry a `KVWL` format header and version number. Record payloads use CRC32C and are verified before decoding. Multi-byte fields in the versioned WAL format are encoded little-endian. Unrecognized magic or format versions are rejected.


## Snapshot recovery boundaries

Published snapshots are treated as complete checkpoints, not append-only logs. Recovery validates every record before applying any snapshot entry to the store.

Record sizes are bounded before allocation, key/value lengths are checked against the record boundary, non-SET opcodes are rejected, and truncated headers or payloads fail recovery. If validation fails after earlier valid records, none of those records are applied to the target store.
