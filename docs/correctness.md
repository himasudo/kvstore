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
2. replay WAL records that follow it in order.

Replaying the WAL should reconstruct the same committed state that existed before the process stopped.

The mutation-ordering regression test compares the live store with a fresh store rebuilt only from the WAL after concurrent writes. A longer version can be run with `make stress`.

## Snapshot checkpointing

The current snapshot implementation copies the store, writes and renames a snapshot, then truncates the WAL. A mutation can arrive after the store copy but before WAL truncation, which creates a data-loss window.

Until checkpointing is tied to an explicit WAL position/LSN, snapshots should not be described as providing a complete crash-consistency guarantee.

The planned fix is to make checkpoints identify the WAL position they cover and only retire WAL data that is known to be included in that checkpoint.

## Disk format

WAL and snapshot readers must treat lengths and opcodes as untrusted input. Future format work will add:

- record size validation before allocation;
- bounds checks for key/value lengths;
- checksums;
- format versioning;
- defined handling for a torn final record.
