# Pax Writer Reliability Redesign

## Context

`src/neotape_pax_writer.cpp` currently implements the unplanned pax writer as a multi-stage pipeline:

- Walker thread traverses sources and dispatches entries.
- Worker threads serialize small files.
- Serializer thread merges metadata entries, worker results, and the large-file slot in sequence order.
- Output thread drains `BoundedBuffer` and calls `PaxWriterCallbacks::write_chunk`.
- Stats thread reports progress.

The current serializer coordinates three independent readiness sources with a manual `notify_generation` condition variable. This can lose wake-ups when a producer notifies between the serializer's source checks and its wait setup. A missed wake can leave the serializer asleep forever, blocking the walker on the large slot or blocking shutdown on `serializer_thread.join()`.

## Goals

- Keep the performance model: worker pool for small files, serializer-owned streaming for large files, and an output thread for buffered writes.
- Replace fragile multi-source polling with one explicit ordered control stream.
- Make every wait condition state-based and close-aware, with no generation-counter wake protocol.
- Centralize cancellation, error propagation, resource cleanup, and thread joining.
- Bound memory, file descriptors, and queued work.
- Preserve current `PaxWriterOptions`, `PaxWriterCallbacks`, and CLI behavior unless a compatibility change is explicitly required.

## Non-Goals

- No actor framework or general-purpose runtime.
- No new public CLI options.
- No change to pax byte format, EOA suppression, BLAKE3 output, planned-mode slice behavior, or tape writer callback contracts.
- No attempt to make libarchive calls themselves interruptible while they are actively reading or writing one entry.

## Chosen Architecture

Use a channel-driven ordered pipeline. The walker assigns every emitted archive item one monotonically increasing sequence number and sends exactly one `OrderItem` for that sequence to `order_queue`. The serializer consumes `order_queue` in FIFO order, so archive order is represented by one stream instead of being reconstructed by polling several sources.

Small regular files still go through worker threads. Their `OrderItem` contains `WorkerResult{seq}` and the actual work is placed on `work_queue`. When a worker finishes, it publishes the serialized bytes into `ResultStore` keyed by `seq` and notifies waiters. The serializer waits for that exact result only when the ordered stream reaches it.

For small files, the walker must submit the `WorkItem` before publishing the matching `OrderItem`. If `work_queue.push()` fails because cancellation closed the queue, the walker frees the entry, closes the fd, and stops dispatching. Only accepted work may become visible in `order_queue`, so the serializer never waits for a worker result that cannot be produced.

Large regular files remain serializer-owned. Their `OrderItem` carries the `archive_entry*` and fd, and the serializer calls `stream_large_entry()` directly. This keeps large files streaming through `bb1` without buffering the whole file in memory.

Metadata-only entries are serialized by the walker before enqueueing, then sent as `InlineBytes`. This preserves the existing low-overhead path for directories, symlinks, hardlinks, and skipped zero-data entries.

## Data Flow

```text
Walker
  -> order_queue: InlineBytes | WorkerResult | LargeEntry, one item per seq
  -> work_queue: small regular-file jobs

Workers
  -> ResultStore: serialized small-file bytes keyed by seq

Serializer
  -> consumes order_queue in sequence order
  -> InlineBytes: emit bytes to bb1
  -> WorkerResult(seq): wait ResultStore[seq], then emit
  -> LargeEntry: stream_large_entry() to bb1

Output thread
  -> drains bb1
  -> callbacks.write_chunk()
  -> BLAKE3/output byte accounting
```

## Core Components

### `ClosableQueue<T>`

A bounded blocking queue used for `order_queue` and `work_queue`.

- `push(T)` returns `false` when closed.
- `pop()` returns `std::nullopt` only after the queue is closed and drained.
- `close()` wakes all pushers and poppers.
- Wait predicates are based only on queue state: not full, not empty, or closed.

### `ResultStore`

A bounded, close-aware map from `seq` to worker result.

- `put(seq, Result)` publishes one worker result and wakes waiters.
- `take(seq)` blocks until that result is present or cancellation closes the store.
- `close()` wakes all waiters and causes pending `take()` calls to return cancellation.
- Capacity is at least the active worker count and may be larger if later profiling shows it improves throughput.

### `PaxPipeline`

An internal RAII owner for threads, queues, cancellation state, stats, and output buffer.

- Starts worker, serializer, output, and stats threads after initialization succeeds.
- Owns a shared `std::exception_ptr` guarded by a mutex.
- Provides `request_cancel(std::exception_ptr)` to record the first failure, close all channels, close `bb1`, and wake all threads.
- Joins all threads before returning or rethrowing the stored failure.

## Resource Ownership

- `archive_entry*` and fd are transferred exactly once into either a `WorkItem` or a `LargeEntry`.
- Worker-owned entries are freed and closed by the worker after `serialize_entry()` returns or throws.
- Serializer-owned large entries are freed and closed by the serializer after `stream_large_entry()` returns or throws.
- Inline metadata entries are freed by the walker immediately after serialization.
- Queue items should use small RAII wrappers where practical so cancellation paths cannot leak entries or fds.

## Error Handling and Cancellation

Any exception or fatal callback error in a worker, serializer, output thread, or walker triggers pipeline cancellation.

- The first error is stored as `exception_ptr`; later errors do not replace it.
- `order_queue`, `work_queue`, and `ResultStore` are closed.
- `bb1.close()` wakes the output thread.
- Threads check close/cancel results at blocking boundaries.
- After join, `write_pax_archive()` rethrows the stored error.

Existing helper functions that call `std::exit()` inside worker-capable paths should be converted to throw exceptions for this pipeline. Process exit from a worker thread bypasses structured cleanup and makes failure behavior hard to test.

## Backpressure

- `order_queue` should be bounded by a moderate number of entries, such as a small multiple of `io_thread` plus a fixed floor.
- `work_queue` should be bounded similarly so the walker cannot open many small-file fds ahead of workers.
- `ResultStore` capacity should be at least the maximum number of active worker jobs, so every worker can publish completion even when the serializer is blocked on an earlier sequence.
- `bb1` remains byte-bounded by `output_buf_size`.

This gives each stage backpressure without cyclic waits. The serializer is the only consumer of `order_queue`, workers are the only consumers of `work_queue`, and output is the only consumer of `bb1`.

## Output Thread Semantics

The output thread should continue to own callback writes and BLAKE3 updates for unplanned mode. It should catch callback exceptions, call `request_cancel()`, and exit. Waterline mode using `pop_after_fill()` remains valid, but `BoundedBuffer::close()` must always wake it so shutdown cannot wait for the waterline.

`callbacks.begin_slice(0)` still runs before threads start, and `callbacks.end_slice(0)` runs only after the output thread is joined successfully. If cancellation occurs, `end_slice(0)` should not be called unless the output contract is explicitly changed to support failed slices.

## Planned Mode

Planned mode is currently single-threaded and slice-aware. This redesign targets the unplanned multi-threaded writer first. Planned mode can keep its current architecture unless a separate reliability issue is found. Shared improvements, such as throwing instead of exiting in common serialization helpers, should apply to both modes where safe.

## Testing Strategy

- Add focused unit-style tests for `ClosableQueue` close behavior, blocked pusher wake-up, blocked popper wake-up, and bounded capacity.
- Add focused tests for `ResultStore::take(seq)` waiting, publish wake-up, close wake-up, and out-of-order worker completion.
- Add a stress test that archives many directories, symlinks, small files, and files larger than `SMALL_FILE_THRESHOLD` with `--io-thread` values `1`, `2`, and a higher count.
- Add a regression test that repeatedly runs a workload likely to interleave metadata, worker results, and large entries.
- Add an injected failing callback test to confirm cancellation closes queues and joins all threads.
- Keep the existing smoke backup/restore test passing for spool pax backup and planned pax backup.

## Rollout Plan

Implement the redesign behind the existing `write_pax_archive()` path with no user-visible flag. Keep the planned-mode path separate. After tests and manual smoke runs pass, update `docs/implementation/mt-pax-architecture.md` to describe the new single ordered stream and remove references to `notify_generation`, `bb0`, `completed_queue`, and the large slot wake protocol.
