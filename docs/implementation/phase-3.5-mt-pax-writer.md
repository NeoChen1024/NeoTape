# Phase 3.5: Multi-Threaded Pax Writer and EOA Suppression

Status: implementation note.

Phase 3.5 is the multi-threaded pax writer (`bin/mt-pax`, source
`src/mt-pax.cpp`). It produces the same POSIX pax byte stream as the original
single-threaded writer, but uses a worker pool to serialize small files in
parallel and a streaming path for large files.

## Overview

Phase 0 produces a POSIX pax byte stream using libarchive's pax writer. The
single-threaded writer opens one archive writer for the entire
run and closes it once at the end, producing exactly one End-of-Archive marker
(two consecutive zero-filled 512-byte blocks) at the end of the stream. This is
the normal pax archive layout expected by bsdtar.

The multi-threaded writer (`src/mt-pax.cpp`) cannot share a single libarchive
`archive_write` object across threads. Instead, it creates a fresh archive
writer per entry, serializes that entry's pax header and data bytes in
isolation, and concatenates the per-entry byte buffers in the serializer
thread. If each per-entry writer closed normally, every entry would be followed
by its own EOA — producing an invalid pax stream with hundreds or thousands of
interleaved EOA markers.

## EOA Suppression Pattern

The per-entry writer pattern uses a **drop-mode sink callback** to suppress the
EOA blocks that `archive_write_close()` generates. The sequence is always:

1. Call `archive_write_finish_entry()` — this finalizes the current entry's
   pax metadata (timestamps, size, checksum).
2. **Before** calling `archive_write_close()`, set a `drop` flag to `true`.
3. Call `archive_write_close()` — libarchive writes the trailing EOA (two
   zero-filled 512-byte tar blocks) by invoking the write callback again. Since
   the `drop` flag is already `true`, the callback silently returns without
   appending anything to the output buffer.
4. Call `archive_write_free()` to release the writer.

### BufCtx / `serialize_entry`

Used for whole-entry buffering (small files, metadata-only entries).
`src/mt-pax.cpp:272-287`:

```cpp
struct BufCtx {
    vector<std::byte> buf;
    bool drop = false;
};

// drop_open (no-op), drop_write, drop_close (no-op)

la_ssize_t drop_write(archive *, void *client, const void *data, size_t len) {
    auto *ctx = static_cast<BufCtx *>(client);
    if (ctx->drop)                     // ← EOA suppression: silently discard
        return static_cast<la_ssize_t>(len);
    auto *bytes = static_cast<const std::byte *>(data);
    ctx->buf.insert(ctx->buf.end(), bytes, bytes + len);
    return static_cast<la_ssize_t>(len);
}
```

In `serialize_entry` at `src/mt-pax.cpp:321-324`:

```cpp
check_archive(archive_write_finish_entry(a), a, "finish entry");
ctx.drop = true;                       // ← suppress EOA before close
check_archive(archive_write_close(a), a, "close writer");
archive_write_free(a);
return std::move(ctx.buf);             // clean pax bytes, no trailing EOA
```

### BBSink / `stream_large_entry`

Used for streaming large files directly into the output BoundedBuffer. The
sink accumulates into `sink.accum` and flushes to BB1 in flight, then uses the
same drop-before-close pattern at `src/mt-pax.cpp:353-363`:

```cpp
check_archive(archive_write_finish_entry(a), a, "finish entry");

// flush remaining accum bytes to BB1
if (!sink.accum.empty()) {
    size_t chunk_size = sink.accum.size();
    sink.dest->push(std::move(sink.accum));
    sink.stats->input_bytes.fetch_add(chunk_size, std::memory_order_relaxed);
}

sink.drop_mode = true;                 // ← suppress EOA before close
archive_write_close(a);
archive_write_free(a);
```

The `bb_sink_write` callback (`src/mt-pax.cpp:95-111`) checks `sink.drop_mode`
and returns immediately without forwarding data when it is `true`.

## Why This Works

libarchive's pax writer writes EOA only during `archive_write_close()`, not during
`archive_write_header()` or `archive_write_finish_entry()`. By toggling the drop
flag between `finish_entry` and `close`, the entry's substantive pax bytes
(header + data) are captured while the trailing EOA is silently discarded.

The final output therefore contains only the concatenated pax entry bytes in
sequence order, with no inter-entry EOA markers. The NeoTape transport layer
does not rely on EOA for framing—it uses explicit length fields in NeoTape
Frame Headers—so the absence of EOA throughout the stream is harmless.

## Contrast with Single-Threaded Writer

The single-threaded writer opens a single archive writer and writes all entries through it.
EOA appears only once, after the final `archive_write_close()`.
No suppression is needed; the single trailing EOA is the
normal pax end marker that bsdtar expects.

```
// single-threaded writer — single EOA at end
[entry 1 pax bytes][entry 2 pax bytes]...[entry N pax bytes][EOA]

// mt-pax.cpp without suppression — would produce EOA per entry
[entry 1][EOA][entry 2][EOA]...[entry N][EOA]  ← invalid pax stream

// mt-pax.cpp with suppression — clean concatenation
[entry 1][entry 2]...[entry N]                  ← length-framed stream
```

## Implications for NeoTape

Because NeoTape core uses length-framed payload transport (Frame Header
`frame_payload_size`, slice-level BLAKE3), the EOA status of the pax byte
stream is irrelevant at the transport layer. The reader (`neotape-cat-volumes`)
follows NeoTape length fields to extract payload ranges and passes the
concatenated pax bytes to the downstream payload profile (e.g. bsdtar).

The single-threaded pax writer's output is therefore compatible with both:

- **Direct bsdtar consumption**: The stream ends with no EOA. bsdtar will read
  until EOF and emit a warning about the missing EOA, but will still extract
  all entries. For clean bsdtar compatibility, a future NeoTape-aware output
  tool MAY append the two 512-byte EOA blocks at the end of the final slice's
  pax bytes before piping to bsdtar.
- **NeoTape length-framed slicing**: The stream has no EOA noise between
  entries, so any chunk of it (a Frame payload range, a slice) contains
  contiguous pax entry bytes with no spurious end markers.
