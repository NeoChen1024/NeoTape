"""Explicit opt-in, destructive Partition-0 hardware regression.

Run only on the test tape named by --device. Compression is restored in finally.
The source snapshot and source.plan must already exist. No source tree is edited.
"""
import argparse
import asyncio
import csv
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import time

def message_header(data):
    return data[0], struct.unpack_from("<Q", data, 1)[0]

async def proxy(listen_path, source_path, log_path, ready_path):
    async def handle(reader, writer):
        remote_reader, remote_writer = await asyncio.open_unix_connection(source_path)
        with log_path.open("w", buffering=1) as log:
            async def forward(inp, out, direction):
                while True:
                    header = await inp.readexactly(9)
                    kind, size = message_header(header)
                    if size > 16 * 1024**2:
                        raise RuntimeError("oversized wire message")
                    payload = await inp.readexactly(size)
                    if kind == 5:
                        seq = struct.unpack("<Q", payload)[0]
                        log.write(f"ack\t{seq}\t{'drop' if seq == 255 else 'forward'}\n")
                        if seq == 255:
                            continue
                    elif kind == 2:
                        seq = struct.unpack_from("<Q", payload, 122)[0]
                        log.write(f"frame\t{seq}\t{payload[9]}\n")
                    out.write(header + payload)
                    await out.drain()
            tasks = [asyncio.create_task(forward(reader, remote_writer, "up")),
                     asyncio.create_task(forward(remote_reader, writer, "down"))]
            done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in pending:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
        remote_writer.close()
        writer.close()
    server = await asyncio.start_unix_server(handle, path=str(listen_path))
    ready_path.write_text("ready\n")
    async with server:
        await server.serve_forever()

def wait_socket(path, process):
    deadline = time.monotonic() + 30
    while not path.exists():
        if process.poll() is not None:
            raise RuntimeError(f"server exited before listening: {process.args}")
        if time.monotonic() > deadline:
            raise RuntimeError(f"socket timeout: {path}")
        time.sleep(.05)

def verify_tree(root):
    manifest = json.loads((root / "source-manifest.json").read_text())
    restored = root / "restored"
    expected = {item["path"] for item in manifest}
    actual = set()
    for folder, dirs, files in os.walk(restored, followlinks=False):
        for name in dirs + files:
            actual.add(str((Path(folder) / name).relative_to(restored)))
    if actual != expected:
        raise RuntimeError(f"path set mismatch: missing={len(expected-actual)} extra={len(actual-expected)}")
    for item in manifest:
        path = restored / item["path"]
        if item["kind"] == "l":
            if not path.is_symlink() or os.readlink(path) != item["target"]:
                raise RuntimeError(f"symlink mismatch: {path}")
        elif item["kind"] == "d":
            if not path.is_dir() or path.is_symlink():
                raise RuntimeError(f"directory mismatch: {path}")
        else:
            if not path.is_file() or path.is_symlink() or path.stat().st_size != item["size"]:
                raise RuntimeError(f"file type/size mismatch: {path}")
            with path.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            if digest != item["sha256"]:
                raise RuntimeError(f"source digest mismatch: {path}")
            if path.stat().st_mode & 0o7777 != item["mode"]:
                raise RuntimeError(f"file mode mismatch: {path}")
    return len(manifest)

def main():
    args = argparse.ArgumentParser()
    args.add_argument("--device", required=True)
    args.add_argument("--root", type=Path, required=True)
    args.add_argument("--repo", type=Path, required=True)
    args.add_argument("--proxy", action="store_true")
    args.add_argument("--bin-dir", type=Path)
    opts = args.parse_args()
    root, repo = opts.root.resolve(), opts.repo.resolve()
    if opts.proxy:
        asyncio.run(proxy(root / "proxy.sock", root / "archiver.sock",
                          root / "v1-wire.tsv", root / "proxy.ready"))
        return
    binaries = opts.bin_dir.resolve() if opts.bin_dir else repo / "build/hardware/bin"
    secret = repo / "3rdparty/signify/regress/regresskey.sec"
    public = repo / "3rdparty/signify/regress/regresskey.pub"
    children = []
    logs = []
    results = {}
    def command(label, arguments, accepted=(0,), timeout=3600):
        print(f"START {label}", flush=True)
        begin = time.monotonic()
        with (root / f"{label}.log").open("w") as log:
            result = subprocess.run(list(map(str, arguments)), stdout=log,
                                    stderr=subprocess.STDOUT, timeout=timeout)
        results[label] = dict(exit=result.returncode, seconds=time.monotonic()-begin)
        (root / "results.json").write_text(json.dumps(results, indent=2))
        print(f"END {label}: {results[label]}", flush=True)
        if result.returncode not in accepted:
            raise RuntimeError(f"{label} failed; see {label}.log")
        return result.returncode
    def server(label, arguments):
        log = (root / f"{label}.log").open("w")
        logs.append(log)
        process = subprocess.Popen(list(map(str, arguments)), stdout=log, stderr=log)
        children.append(process)
        return process
    def interrupted(signum, frame):
        raise RuntimeError(f"interrupted by signal {signum}")
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        status = subprocess.check_output(["mt", "-f", opts.device, "status"], text=True)
        (root / "initial-status.log").write_text(status)
        if "partition=0" not in status or "ONLINE" not in status:
            raise RuntimeError("expected online test tape at Partition 0")
        command("compression-off", ["mt", "-f", opts.device, "compression", "0"], timeout=60)
        # No full erase/reformat: --erase only permits rewind and overwriting
        # this existing test partition in the current NeoTape CLI.
        archiver = server("archiver", [binaries / "neotape-archiver",
            "--listen", "unix://" + str(root / "archiver.sock"),
            "--plan", root / "source.plan", "--volume-block-size", "1M",
            "--fec", "--sign-secret-key", secret, "--retention-frame-count", "64",
            "--output-buffer-size", "256M", "--io-thread", "8",
            "--archive-name", "lto5-hardware-20260918", "--debug"])
        wait_socket(root / "archiver.sock", archiver)
        extractor = server("extractor", [binaries / "neotape-extractor",
            "--listen", "unix://" + str(root / "extractor.sock"),
            "-o", root / "restored.pax", "--verify-pubkey", public, "--require-signed"])
        wait_socket(root / "extractor.sock", extractor)
        proxy_process = server("proxy", [sys.executable, __file__, "--proxy",
            "--device", opts.device, "--root", root, "--repo", repo])
        wait_socket(root / "proxy.sock", proxy_process)
        for volume in (1, 2, 3):
            address = root / ("proxy.sock" if volume == 1 else "archiver.sock")
            options = [binaries / "neotape-write", "--source", "unix://" + str(address),
                "--target", "tape:" + opts.device, "--erase",
                "--verify-pubkey", public, "--output-buffer-size", "256M", "--debug"]
            if volume == 1:
                options += ["--recovery-bundle", root / "recovery.tar",
                            "--max-volume-bytes", "257M"]
            code = command(f"v{volume}-write", options, (3,) if volume < 3 else (0,))
            command(f"v{volume}-after-write-status", ["mt", "-f", opts.device, "status"], timeout=60)
            command(f"v{volume}-read", [binaries / "neotape_capture_volume",
                "tape:" + opts.device, "unix://" + str(root / "extractor.sock"),
                root / f"volume-{volume}", public])
            command(f"v{volume}-after-read-status", ["mt", "-f", opts.device, "status"], timeout=60)
            if volume == 1:
                acks = [int(row[1]) for row in csv.reader((root / "v1-wire.tsv").open(), delimiter="\t") if row and row[0] == "ack"]
                read = [int(row["global"]) for row in csv.DictReader((root / "volume-1/records.tsv").open(), delimiter="\t") if row["event"] == "record"]
                if acks != read or 255 not in acks:
                    raise RuntimeError("V1 ACK/readback mismatch or lost-ACK injection not reached")
                print(f"V1 CHECKPOINT: {len(read)} committed records read back, ACK 255 withheld", flush=True)
            # Retained originals are now on SSD before overwriting the partition.
        if archiver.wait(timeout=60) != 0 or extractor.wait(timeout=60) != 0:
            raise RuntimeError("archive/extractor completion failure")
        text = (root / "extractor.log").read_text()
        if "suppressed replay global_seq=255" not in text:
            raise RuntimeError("expected lost-ACK retry was not suppressed")
        restored = root / "restored"
        restored.mkdir()
        command("unpack", ["bsdtar", "-xpf", root / "restored.pax", "-C", restored])
        print("START compare-to-source-manifest", flush=True)
        results["source_comparison"] = dict(entries=verify_tree(root), passed=True)
        (root / "results.json").write_text(json.dumps(results, indent=2))
        print("SOURCE COMPARISON PASSED", flush=True)
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=15)
        for log in logs:
            log.close()
        # Always attempt restoration, including when the disable command or
        # any hardware stage failed partway through.
        command("compression-on", ["mt", "-f", opts.device, "compression", "1"], timeout=60)

if __name__ == "__main__":
    main()
