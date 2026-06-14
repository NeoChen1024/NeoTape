#!/usr/bin/env python3
"""Inspect a raw spool file (tape-file-*.pending) written by neotape-read
and report the sequence of NeoTape records, global frame numbers, and gaps."""
import os
import struct
import sys

FIXED_HEADER_SIZE = 1024
HDR_TYPE_OFFSET = 9
HDR_BLOCK_SIZE_OFFSET = 10
FHDR_GLOBAL_SEQ_OFFSET = 324
AE_LAST_GLOBAL_SEQ_OFFSET = 324


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <spool-file>")
        sys.exit(2)
    path = sys.argv[1]
    f = open(path, "rb")

    rec_no = 0
    prev_global = None
    frame_count = 0
    while True:
        header = f.read(FIXED_HEADER_SIZE)
        if len(header) == 0:
            break
        if len(header) != FIXED_HEADER_SIZE:
            print(f"truncated header at record {rec_no}")
            break
        htype = header[HDR_TYPE_OFFSET]
        rec_no += 1
        if htype == 1:  # volume
            block_size = struct.unpack("<I", header[HDR_BLOCK_SIZE_OFFSET:HDR_BLOCK_SIZE_OFFSET + 4])[0]
            print(f"record {rec_no}: volume header, block_size={block_size}")
            # Volume header records are only fixed_header_size bytes long.
        elif htype == 2:  # frame
            block_size = struct.unpack("<I", header[HDR_BLOCK_SIZE_OFFSET:HDR_BLOCK_SIZE_OFFSET + 4])[0]
            gseq = struct.unpack("<Q", header[FHDR_GLOBAL_SEQ_OFFSET:FHDR_GLOBAL_SEQ_OFFSET + 8])[0]
            print(f"record {rec_no}: frame global_seq={gseq}")
            frame_count += 1
            if prev_global is not None and gseq != prev_global + 1:
                print(f"  GAP: expected {prev_global + 1}, got {gseq}")
            prev_global = gseq
            f.seek(block_size - FIXED_HEADER_SIZE, os.SEEK_CUR)
        elif htype == 3:  # archive_end
            gseq = struct.unpack("<Q", header[AE_LAST_GLOBAL_SEQ_OFFSET:AE_LAST_GLOBAL_SEQ_OFFSET + 8])[0]
            print(f"record {rec_no}: archive_end last_global_seq={gseq}")
            break
        else:
            print(f"record {rec_no}: unknown header type {htype}")
            break

    print(f"total records seen: {rec_no}, frames: {frame_count}")


if __name__ == "__main__":
    main()
