#!/usr/bin/env python3
"""Generate compile_commands.json for clangd.

This script produces a compilation database from the project's Makefile
patterns, using absolute paths and explicit -isystem paths to a compatible
libstdc++ installation so that clangd can parse the codebase without hitting
libstdc++/GCC version mismatches.
"""

import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# GCC 16's libstdc++ headers confuse clangd 22; point clangd at GCC 15's.
GCC_TOOLCHAIN = "/usr/lib/gcc/x86_64-pc-linux-gnu/15.2.1"

COMMON_FLAGS = [
    "-O2",
    "-g",
    "-Wall",
    "-Wextra",
    "-pipe",
    "-fPIE",
    "-fPIC",
    "-march=native",
    "-pedantic",
    f"-I{ROOT / 'include'}",
    f"-I{ROOT / 'tests'}",
    f"-I{ROOT / '3rdparty/BLAKE3/c'}",
    f"-I{ROOT / '3rdparty/crc32c/include'}",
    "-I/usr/local/include",
    f"-isystem{GCC_TOOLCHAIN}/include/c++",
    f"-isystem{GCC_TOOLCHAIN}/include/c++/x86_64-pc-linux-gnu",
]

CXX_FLAGS = ["-std=c++20"] + COMMON_FLAGS
C_FLAGS = ["-std=c17"] + COMMON_FLAGS


def add(entries, path, flags):
    path = str(path)
    src = ROOT / path
    if not src.exists():
        return
    entries.append(
        {
            "directory": str(ROOT),
            "file": str(src),
            "command": " ".join(
                ["c++" if path.endswith(".cpp") or path.endswith(".cc") else "cc"]
                + flags
                + ["-c", str(src), "-o", str(ROOT / "build" / f"{src.stem}.o")]
            ),
        }
    )


def main():
    entries = []

    for cpp in sorted((ROOT / "src").glob("*.cpp")):
        add(entries, cpp.relative_to(ROOT), CXX_FLAGS)

    for test in sorted((ROOT / "tests").glob("*.cpp")):
        add(entries, test.relative_to(ROOT), CXX_FLAGS)

    for cc in sorted((ROOT / "3rdparty/crc32c/src").glob("*.cc")):
        add(entries, cc.relative_to(ROOT), CXX_FLAGS)

    for c in sorted((ROOT / "3rdparty/BLAKE3/c").glob("*.c")):
        add(entries, c.relative_to(ROOT), C_FLAGS)

    out = ROOT / "compile_commands.json"
    with open(out, "w", encoding="utf-8") as f:
        json.dump(entries, f, indent=2)
        f.write("\n")

    print(f"Generated {out} with {len(entries)} entries")


if __name__ == "__main__":
    main()
