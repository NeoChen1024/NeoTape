"""Create a test source and independent source-content manifest.

By default a 36.5 GiB subset is copied into the run directory. With
--in-place every file is hashed where it is and the plan archives SOURCE
itself; the operator guarantees SOURCE is not modified during the run.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys

args = argparse.ArgumentParser()
args.add_argument("source", type=Path)
args.add_argument("root", type=Path)
args.add_argument("planner", type=Path)
args.add_argument("--in-place", action="store_true")
opts = args.parse_args()
source, root, planner = opts.source.resolve(), opts.root, opts.planner
root.mkdir(parents=True, exist_ok=True)
snapshot = source if opts.in_place else root / "source"
if not opts.in_place:
    snapshot.mkdir()
files, links = [], []
for directory, dirs, names in os.walk(source, followlinks=False):
    for name in dirs + names:
        path = Path(directory) / name
        info = path.lstat()
        relative = path.relative_to(source)
        if stat.S_ISLNK(info.st_mode):
            links.append((relative, info))
        elif stat.S_ISREG(info.st_mode):
            files.append((relative, info))
small, small_bytes = [], 0
for entry in sorted(files, key=lambda e: str(e[0])):
    if entry[1].st_size <= 256 * 1024 and small_bytes < 64 * 1024**2:
        small.append(entry)
        small_bytes += entry[1].st_size
chosen = {p for p, _ in small}
selected = list(small)
total = small_bytes
target = float("inf") if opts.in_place else int(36.5 * 1024**3)
for entry in sorted(files, key=lambda e: (-e[1].st_size, str(e[0]))):
    if entry[0] not in chosen and total < target:
        selected.append(entry)
        chosen.add(entry[0])
        total += entry[1].st_size
print(f"selected_files={len(selected)} bytes={total} GiB={total/1024**3:.3f}", flush=True)

def copy(entry):
    relative, original = entry
    path = source / relative
    digest = hashlib.sha256()
    if opts.in_place:
        with path.open("rb") as inp:
            while block := inp.read(4 * 1024**2):
                digest.update(block)
    else:
        destination = snapshot / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        with path.open("rb") as inp, destination.open("xb") as out:
            while block := inp.read(4 * 1024**2):
                digest.update(block)
                out.write(block)
        shutil.copystat(path, destination)
    after = path.stat()
    if (original.st_ino, original.st_size, original.st_mtime_ns) != (after.st_ino, after.st_size, after.st_mtime_ns):
        raise RuntimeError(f"source changed while reading: {relative}")
    return dict(path=str(relative), kind="f", size=original.st_size,
                sha256=digest.hexdigest(), mode=stat.S_IMODE(original.st_mode))

with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    manifest = list(pool.map(copy, selected))
for relative, info in links:
    target = os.readlink(source / relative)
    if not opts.in_place:
        destination = snapshot / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.symlink_to(target)
    manifest.append(dict(path=str(relative), kind="l", target=target))
for directory, dirs, names in os.walk(snapshot):
    relative = Path(directory).relative_to(snapshot)
    if str(relative) != ".":
        if not opts.in_place:
            shutil.copystat(source / relative, Path(directory), follow_symlinks=False)
        manifest.append(dict(path=str(relative), kind="d"))
(root / "source-manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=True))
subprocess.run([str(planner), "-C", str(snapshot), "-o", str(root / "base.plan"),
                "--slice-size", "8G", "."], check=True)
records = (root / "base.plan").read_bytes().split(b"\0\n")
directives, entries = [], []
for record in records:
    if not record:
        continue
    if record.startswith(b"/chdir/"):
        directives.append(record)
    else:
        fields = record[1:].split(b"/", 9)
        if len(fields) != 10:
            raise RuntimeError("invalid planner record")
        entries.append(fields)
smoke_paths = {os.fsencode(str(p)) for p, _ in small}
front = [e for e in entries if e[2] != b"f" or e[9].removeprefix(b"./") in smoke_paths]
front_paths = {e[9] for e in front}
back = [e for e in entries if e[9] not in front_paths]
output = bytearray()
for d in directives:
    output.extend(d + b"\0\n")
slice_no, file_no, used = 0, 0, 0
for e in front + back:
    size = int(e[3])
    limit = 64 * 1024**2 if slice_no == 0 else 8 * 1024**3
    if file_no and size and used + size > limit:
        slice_no, file_no, used = slice_no + 1, 0, 0
    e[0], e[1] = str(slice_no).encode(), str(file_no).encode()
    output.extend(b"/" + b"/".join(e) + b"\0\n")
    file_no += 1
    used += size
(root / "source.plan").write_bytes(output)
(root / "recovery-note.txt").write_text("NeoTape LTO-5 hardware regression. Test data only.\n")
subprocess.run(["bsdtar", "-cf", str(root / "recovery.tar"), "-C", str(root),
                "recovery-note.txt"], check=True)
print(f"manifest_entries={len(manifest)} slices={slice_no+1}", flush=True)
