# mt-pax — Multi-threaded pax writer architecture

## Overview

`mt-pax` is a multi-threaded Pax archive writer that parallelises file I/O
for high-concurrency small-file workloads (e.g. ZFS).  It uses a
channel-driven ordered pipeline with up to five distinct thread roles.

## Library Interface

The implementation lives in `src/neotape_pax_writer.cpp` and is exposed through
`include/neotape/pax_writer.hpp`. `src/mt-pax.cpp` is a CLI wrapper that maps
command-line output modes onto library callbacks.

The library emits slice lifecycle events and pax byte chunks. Unplanned source
walking emits one logical slice. Planned mode consumes `neotape-plan` metadata
incrementally through the shared `PlanReader` codec,
honors `/chdir/<path>` directives, and opens/closes slices according to the
plan's slice numbers. The debug slice files written by the CLI contain raw pax
payload bytes; only the final slice includes the pax End-of-Archive marker.

Both input modes use the same archive session and `SlicePipeline` owner for
hardlink resolution, ordered emission, cancellation, hashing and end markers.
See [the refactor tracker](2026-09-refactor.md) for the shared I/O boundaries.

## Thread types

### 1. Walker thread (main thread)

**One instance.**  The original call stack — `main` → `parse_args` →
`write_pax_archive` — performs the filesystem walk on the main thread.

Responsibilities:

- Traverses source paths via `archive_read_disk`
- Resolves hardlinks through `archive_entry_linkresolver`
- Computes archive paths via `SourceSpec`
- Dispatches each entry into the ordered pipeline through `PaxPipeline`:
  | Entry type                  | Destination                             |
  | --------------------------- | --------------------------------------- |
  | Non-file (dir, symlink, …) | `order_queue` as `InlineBytes`         |
  | Small file (< 4 MiB)       | `work_queue` + `order_queue` as `WorkerResult` |
  | Large file (> 4 MiB)       | `order_queue` as `LargeEntry`           |
- Non-file entries are serialised inline (the walker calls `serialize_entry`
  itself) and enqueued as `InlineBytes`.
- Small files: the walker first pushes a `WorkItem` onto `work_queue`, then
  pushes a `WorkerResult` marker onto `order_queue`.
- Large files: the walker pushes a `LargeEntry` directly onto `order_queue`.

### 2. Serializer thread

**One instance.**  Consumes `order_queue` in strict FIFO sequence order and
pushes ordered output into the `BoundedBuffer` (`bb1`).

Responsibilities:

- For `InlineBytes`: pushes the pre-serialised bytes directly to `bb1`.
- For `WorkerResult`: blocks on `ResultStore::take(seq)` for the worker-produced
  bytes, then pushes to `bb1`.
- For `LargeEntry`: calls `stream_large_entry` directly, streaming the
  entry into `bb1` via the `BBSink` accumulator without buffering the
  whole file in memory.

The serializer uses a single ordered stream; there is no generation counter or
multi-source polling.  A `PaxPipeline` owner provides `request_cancel()` for
coordinated shutdown.

### 3. Worker threads

**Zero or (--io-thread - 1) instances.**  Only created when
`--io-thread N` with `N > 1`.  When `N = 1` (default), no workers exist
and all file entries go through the `LargeEntry` path (serializer reads them).

Each worker loops:

1. Pops a `WorkItem` from `work_queue` (a `ClosableQueue`).
2. Calls `serialize_entry(entry, fd)` — opens a fresh libarchive writer
   for one entry, writes the header and file data, discards the EOA trailer
   via `drop_mode`, and returns the raw bytes.
3. Publishes the resulting `{seq, bytes}` to `ResultStore` keyed by `seq`.
4. Returns to step 1.

On exception, the worker triggers full pipeline cancellation by closing
`work_queue`, `order_queue`, `ResultStore`, and `bb1`.

### 4. Output thread

**One instance.**  Drains `bb1` (`BoundedBuffer`) and writes chunks via
`PaxWriterCallbacks`.

Responsibilities:

- Pops chunks from `bb1`.  An empty chunk signals termination (buffer was
  closed).
- Feeds each chunk through the BLAKE3 hasher.
- Supports `-P <percent>` (waterline) mode: when the buffer is partially
  full, it uses `pop_after_fill` to avoid fragmenting writes on spinning
  media; after a drain to empty, it reverts to `pop`.
- On callback exception, triggers pipeline cancellation via `request_cancel()`.

### 5. Stats thread

**One instance.**  Prints a live progress line to stderr every second.

A shared `PeriodicProgress` owns the timer and promptly joins on stop.
`RateSampler` computes elapsed-time rates from cumulative `ArchiveStats`
counters. The pax renderer retains the mbuffer-style line and overwrites it
with `\r`. Buffer sampling holds the slice-owner lifetime lock; joins and
slow output callbacks run outside that lock.

## Data flow diagram

```
┌──────────┐  non-file entries (InlineBytes)
│  Walker  │─────────────────────────────────────────────┐
│ (main)   │                                             │
│          │  small files (WorkerResult)                 │
│          │  ─────┐                                     │
│          │       │  ┌─────────┐                        │
│          │       ├─►│ Worker  │── Result──►┌──────────┐│
│          │       │  │ 0..N-1  │  ResultStore│           │
│          │       │  └─────────┘             │           │
│          │  work │                          │ Serializer│────►│ bb1  │────►│Output│
│          │ queue │                          │(ordered)  │     │ Buf. │     │thread│
│          │  ────►│                          │           │     └──────┘     └──────┘
│          │       │                          │           │
│          │  large files (LargeEntry)        └────┬──────┘
│          │  ─────────────────────────────────────┘
│          │        order_queue
└──────────┘
```

## Sequence numbering

Every dispatched entry receives a monotonically increasing `seq` from the
walker.  The walker emits exactly one `OrderItem` per sequence into
`order_queue`.  The serializer consumes `order_queue` in FIFO order, so
archive order is represented by one stream rather than reconstructed by
polling multiple sources.

## Backpressure and capacity

| Channel        | Capacity                             |
| -------------- | ------------------------------------ |
| `order_queue`  | `max(64, io_thread * 8)` items       |
| `work_queue`   | `max(1, io_thread * 4)` items        |
| `ResultStore`  | `order_queue_capacity + worker_count`|
| `bb1`          | `output_buf_size` bytes              |

`ResultStore` capacity covers every WorkerResult order item that can be in
`order_queue`, plus one per active worker for in-flight completions, so workers
can always publish when the serializer is temporarily blocked on large-entry
streaming or `bb1` backpressure.

## Shutdown sequence

1. Walker finishes walking and calls `PaxPipeline::finish_input()` — closes
   `work_queue` and `order_queue`.
2. Workers drain accepted work and exit when `work_queue` is closed and
   drained.
3. Serializer drains `order_queue`, takes any remaining `ResultStore` results
   in order, then exits when `order_queue` is closed and drained.
4. `PaxPipeline::join()` closes `ResultStore`, joins workers and serializer,
   closes `bb1`, and joins the output thread.
5. Any stored pipeline exception is rethrown after all threads are joined.
6. Stats thread joined, BLAKE3 hash printed.
