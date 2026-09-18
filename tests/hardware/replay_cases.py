"""Exercise extractor fault handling using previously captured tape records."""
import filecmp
import json
from pathlib import Path
import subprocess
import sys
import time

repo, root = map(lambda x: Path(x).resolve(), sys.argv[1:])
binary = repo / "build/dev/bin"
public = repo / "3rdparty/signify/regress/regresskey.pub"
rejected = {"too-many-missing", "bad-signature", "conflicting-replay"}
results = {}
for case in sorted(p for p in root.iterdir() if p.is_dir()):
    socket = root / (case.name + ".sock")
    output = root / (case.name + ".pax")
    with (root / (case.name + ".log")).open("w") as log:
        extractor = subprocess.Popen([str(binary / "neotape-extractor"), "--listen",
            "unix://" + str(socket), "-o", str(output), "--verify-pubkey", str(public),
            "--require-signed"], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 10
            while not socket.exists():
                if extractor.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("extractor did not listen")
                time.sleep(.02)
            reader = subprocess.run([str(binary / "neotape-read"), "--source",
                "spool:" + str(case), "--connect", "unix://" + str(socket)],
                stdout=log, stderr=log, timeout=60)
            status = extractor.wait(timeout=60)
            if case.name in rejected:
                if status == 0:
                    raise RuntimeError(f"{case.name} unexpectedly accepted")
            else:
                if status != 0 or reader.returncode != 0 or not filecmp.cmp(root / "expected.pax", output, shallow=False):
                    raise RuntimeError(f"{case.name} recovery failed")
            results[case.name] = dict(extractor_exit=status, reader_exit=reader.returncode,
                                     expected_rejection=case.name in rejected, passed=True)
            print(case.name, "PASS", flush=True)
        finally:
            if extractor.poll() is None:
                extractor.terminate()
                extractor.wait(timeout=10)
    (root / "results.json").write_text(json.dumps(results, indent=2))
