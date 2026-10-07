# Correctness notes

This file records the guarantees the storage path is intended to provide and the gaps that are still being worked on.

## Mutation durability

For a mutating command, the WAL record must become durable before the mutation is acknowledged to the client.

The in-memory state and the order recovered from the WAL must eventually use the same mutation order. This is not fully guaranteed yet when multiple worker threads mutate the same key concurrently; mutation ordering is the next persistence change.

`SET`, `DEL`, and `CLEAR` are all mutations and must be represented in the WAL.

## Recovery

Startup recovery is:

1. load the latest valid snapshot;
2. replay WAL records that follow it in order.

Replaying the WAL should reconstruct the same committed state that existed before the process stopped.

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
