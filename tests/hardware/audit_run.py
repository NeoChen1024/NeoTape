"""Cross-check hardware ACK ledger, captured records, and retry identity."""
import csv
import json
from pathlib import Path
import re
import sys

root = Path(sys.argv[1]).resolve()
rows = []
volumes = []
retry_images = []
for volume in (1, 2, 3):
    directory = root / f"volume-{volume}"
    offsets = {}
    current = []
    for row in csv.DictReader((directory / "records.tsv").open(), delimiter="\t"):
        if row["event"] != "record":
            continue
        sequence, file_number = int(row["global"]), int(row["file"])
        size = int(row["bytes"])
        offset = offsets.get(file_number, 0)
        if sequence == 255:
            path = next(directory.glob(f"neotape-{file_number}.*.nts"))
            with path.open("rb") as stream:
                stream.seek(offset)
                image = bytearray(stream.read(size))
            if len(image) != size:
                raise RuntimeError("truncated captured retry")
            for start, count in ((114, 8), (408, 72), (480, 32)):
                image[start:start+count] = bytes(count)
            retry_images.append(bytes(image))
        offsets[file_number] = offset + size
        current.append(sequence)
        rows.append((volume, row))
    if current != list(range(current[0], current[-1] + 1)):
        raise RuntimeError(f"non-contiguous readback in volume {volume}")
    for file_number, size in offsets.items():
        path = next(directory.glob(f"neotape-{file_number}.*.nts"))
        if path.stat().st_size != size:
            raise RuntimeError("capture file length disagrees with record ledger")
    volumes.append(dict(volume=volume, records=len(current), first=current[0], last=current[-1],
                        bytes=sum(offsets.values())))
sequence = [int(row["global"]) for _, row in rows]
unique = sorted(set(sequence))
if unique != list(range(unique[-1] + 1)):
    raise RuntimeError("archive-global readback gap")
if len(sequence) - len(unique) != 1 or len(retry_images) != 2 or retry_images[0] != retry_images[1]:
    raise RuntimeError("retry count or normalized record equivalence failed")
acks = list(map(int, re.findall(r"ack frame global_seq=(\d+)", (root / "archiver.log").read_text())))
if acks != unique:
    raise RuntimeError("producer ACK ledger differs from readback logical sequence")
if int(rows[-1][1]["channel"]) != 255:
    raise RuntimeError("last physical record is not archive_end")
prefix = (root / "volume-1/prefix.bin").read_bytes()
bundle = (root / "recovery.tar").read_bytes()
if not prefix.startswith(bundle) or any(prefix[len(bundle):]):
    raise RuntimeError("recovery bundle bytes or padding differ from source")
result = dict(passed=True, recovery_bundle_verified=True, volumes=volumes, unique_records=len(unique), physical_records=len(rows),
              suppressed_replay=255, source_comparison=json.loads((root / "results.json").read_text())["source_comparison"])
(root / "audit.json").write_text(json.dumps(result, indent=2))
print(json.dumps(result, indent=2))
