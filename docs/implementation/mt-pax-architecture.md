# mt-pax — Multi-threaded pax writer architecture

## Overview

`mt-pax` is a multi-threaded Pax archive writer that parallelises file I/O
for high-concurrency small-file workloads (e.g. ZFS).  It uses a
producer-consumer pipeline with up to five distinct thread roles.

## Thread types

### 1. Walker thread (main thread)

**One instance.**  The original call stack — `main` → `parse_args` →
`write_pax_archive` — performs the filesystem walk on the main thread.

Responsibilities:

- Traverses source paths via `archive_read_disk`
- Resolves hardlinks through `archive_entry_linkresolver`
- Computes archive paths via `SourceSpec`
- Dispatches each entry into one of three queues:
  | Entry type                  | Destination                    |
  | --------------------------- | ------------------------------ |
  | Non-file (dir, symlink, …) | `bb0` (BlockingQueue)        |
  | Small file (< 4 MiB)        | Worker slot via `idle_queue` |
  | Large file (≥ 4 MiB)       | Large slot                     |
- Non-file entries are serialised inline (the walker calls `serialize_entry`
  itself) and the resulting bytes are pushed onto `bb0`.
- Small files are handed to an idle worker via `assign_small` — the walker
  pops a free slot from `idle_queue`, writes the `WorkItem` under the slot
  mutex, marks the slot `BUSY`, and wakes the worker.
- Large files are handed to the *large slot* (the last slot, index
  `nworkers`): the walker waits for the slot to become `IDLE`, fills it,
  marks it `BUSY`, and notifies the serializer.

### 2. Serializer thread

**One instance.**  Receives results from all three dispatch paths, orders
them by sequence number, and pushes them into the output `BoundedBuffer`
(`bb1`).

Responsibilities:

- Drains `bb0` and `completed_queue` (worker results) into a
  `std::map<uint64_t, Result>` keyed by sequence number.
- Emits in-order results from the map to `bb1`.
- Checks the large slot: if it holds a `WorkItem` whose `seq` matches
  `expected`, calls `stream_large_entry` which writes the entry directly
  into `bb1` (via the `BBSink` accumulator), then marks the slot `IDLE` and
  notifies the walker.
- Uses a generation counter (`notify_generation`) to avoid lost wake-ups:
  the serializer sleeps on `notify_cv` until it observes a new generation,
  then re-checks all sources.

### 3. Worker threads

**Zero or (--io-thread - 1) instances.**  Only created when
`--io-thread N` with `N > 1`.  When `N = 1` (default), no workers exist
and all file entries go through the large slot (serializer reads them).

Each worker has a dedicated `WorkerSlot` (mutex + CV + state + work item).
The worker loops:

1. Waits on its slot CV until `BUSY` or `done`.
2. Calls `serialize_entry(entry, fd)` on the assigned work — this opens a
   fresh libarchive writer for one entry, writes the header and file data,
   discards the EOA trailer via `drop_mode`, and returns the raw bytes.
3. Pushes the resulting `{seq, bytes}` to `completed_queue`.
4. Marks its slot `IDLE` and pushes the slot index back to `idle_queue`.
5. Bumps `notify_generation` to wake the serializer.

The `completed_queue` capacity is `2 × nworkers`, providing natural
backpressure.

### 4. Large slot (serializer-processed)

**Not a thread — a single slot** (index `nworkers`) that the **serializer
thread** processes directly.  When `nworkers = 0` (default `--io-thread 1`),
this is the only file path and every file (regardless of size) goes here.

Flow:

1. Walker calls `assign_large` → waits on slot CV for `IDLE` state → fills
   `WorkItem` → sets `BUSY` → notifies serializer.
2. Serializer detects `BUSY && seq == expected` → calls
   `stream_large_entry(BBSink, entry, fd)`.
3. `stream_large_entry` opens a libarchive writer, pipes the entry through
   a `BBSink` accumulator (flushes to `bb1` every 4 MiB), discards EOA
   via `drop_mode`, and returns.
4. Serializer marks slot `IDLE`, notifies the slot CV (waking the walker),
   and notifies `notify_cv`.

### 5. Output thread

**One instance.**  Reads chunks from `bb1` (`BoundedBuffer`) and writes
them to the output file (or stdout).

Responsibilities:

- Pops chunks from `bb1`.  An empty chunk signals termination.
- Feeds each chunk through the BLAKE3 hasher.
- Supports `-P <percent>` (waterline) mode: when the buffer is partially
  full it uses `pop_after_fill` to avoid fragmenting writes on spinning
  media; after a drain to empty, it reverts to `pop`.
- Propagates write errors via `output_error` atomic.

### 6. Stats thread

**One instance.**  Prints a live progress line to stderr every second.

Reads `ArchiveStats` counters (`input_bytes`, `output_bytes`,
`walked_entries`) and `bb1` fill-level, computes rates, and overwrites the
previous line with `\r`.  A newline in the verbose or BLAKE3 output lines
ensures the stats line doesn't corrupt them.

## Data flow diagram

```
┌──────────┐  non-file entries
│  Walker  │──────────────────►┌──────┐
│ (main)   │  serialize_entry  │  bb0 │───┐
│          │  + push Result    └──────┘   │
│          │                              │
│          │  small files                 │
│          │  ───────────────────────────►│
│          │  assign_small → slot BUSY    │
│          │    ┌─────────┐               │
│          │    │ Worker  │── Result──►┌──┴──────────┐     ┌──────┐     ┌──────┐
│          │    │ 0..N-2  │  completed │ Serializer  │────►│ bb1  │────►│Output│
│          │    └─────────┘   queue    │(merge+order)│     │ Buf. │     │thread│
│          │                           └───┬─────────┘     └──────┘     └──────┘
│          │  large files                  │
│          │  ────────────────────────────►│
│          │  assign_large → slot BUSY     │
│          │     ┌──────────┐              │
│          │     │Large slot│─stream──────►│
│          │     │(slot N-1)│ (BBSink)     │
│          │     └──────────┘              │
└──────────┘                               │
        notify_generation ─────────────────┘
```

## Sequence numbering

Every dispatched entry (file or non-file) receives a monotonically
increasing `seq` from the walker.  The serializer emits entries strictly in
`seq` order using a `std::map` to reorder results from `bb0` and
`completed_queue`.  The large slot is always at the expected sequence
because the walker blocks until the slot becomes `IDLE`, so it never overtakes
the serializer's progress.

## Shutdown sequence

1. Walker finishes walking → `idle_queue.close()` (wakes worker waiters,
   drops future pushes) → `done = true`.
2. All slot CVs and `notify_cv` are notified.
3. Workers: wake, see `done && !BUSY`, exit.
4. Serializer: enters drain loop — repeatedly collects `bb0` and
   `completed_queue` into the pending map, emits what it can, checks the
   large slot, and exits when `done`, `pending.empty()`, and no slot is
   `BUSY`.
5. Worker threads joined, serializer joined.
6. `bb1.close()`, output thread joined.
7. Stats thread joined, BLAKE3 hash printed.
