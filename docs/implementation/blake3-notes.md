# BLAKE3 Integration Notes

Status: implementation note.

## Overview

BLAKE3 is used in NeoTape for payload integrity verification. It is preferred
over SHA-256 for its speed and simplicity.

## Integration Points

### Frame-level: `frame_payload_blake3`

Computed over exactly `frame_payload_size` bytes of a single Frame's payload.
Calculated by the writer before the Frame header is committed. Verified by the
reader after reading the Frame payload.

### Slice-level: `slice_content_blake3`

Computed over exactly `slice_content_size` bytes of concatenated SLICE_CONTENT
Frame payloads for one logical slice, in Frame sequence order. Calculated by
the writer after the last SLICE_CONTENT Frame is finalized. Verified by the
reader after the END Frame for the slice is reached.

### Metadata bundle: `metadata_bundle_blake3`

Optional hash of the Medium Header ar metadata bundle. Computed over the
entire metadata bundle byte range.

### Output-level (pax writer)

The single-threaded and multi-threaded pax writers hash the entire output stream
with BLAKE3 and print the digest to stderr on completion.

## Implementation (`include/neotape/format.hpp` and `src/neotape_format.cpp`)

The `nt_hash` type is fixed at 32 bytes (256-bit BLAKE3 output):

```cpp
using Hash = std::array<uint8_t, 32>;
```

Hash computation uses the BLAKE3 C API directly (from the bundled submodule):

```cpp
#include "blake3.h"

blake3_hasher hasher;
blake3_hasher_init(&hasher);
blake3_hasher_update(&hasher, data, len);
blake3_hasher_finalize(&hasher, hash.data(), hash.size());
```

## Streaming Use

For large Frame payloads and the output stream, BLAKE3 is updated incrementally:

```cpp
blake3_hasher hasher;
blake3_hasher_init(&hasher);

// Stream chunks:
while (/* more data */) {
    size_t n = read(fd, buf, sizeof(buf));
    blake3_hasher_update(&hasher, buf, n);
    output->write(buf, n);
}

blake3_hasher_finalize(&hasher, hash.data(), hash.size());
```

This avoids loading the entire payload into memory before computing the hash.

## Build Integration

BLAKE3 is built as a static library from the bundled submodule:

```makefile
# From 3rdparty/BLAKE3/
# Produces lib/libb3sum.a
```

The library is linked as `-lb3sum` with `-I 3rdparty/BLAKE3/c` in the include
path.

## Performance Notes

- BLAKE3 benefits from SIMD acceleration (SSE2, SSE4.1, AVX2, AVX-512, NEON).
  The bundled implementation automatically detects and uses the best available
  instruction set.
- On x86-64 with AVX2, BLAKE3 can hash at several GB/s per core, making it
  suitable for streaming at LTO-native speeds.
- The 256-bit output (32 bytes) provides a comfortable security margin for
  integrity verification without the overhead of larger hashes.
