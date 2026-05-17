# Spool Directory as Optical/Removable Multi-Volume Backup Staging

Status: design note / implementation note.

This document describes how the NeoTape filesystem spool backend can be used as a staging format for CD, DVD, BD, or other removable filesystem media. This is not the primary purpose of the spool backend, and it does not change the NeoTape core format.

## Scope

The spool backend represents NeoTape archive volumes as ordinary filesystem objects. A spool archive may contain one or more virtual volume directories. Each virtual volume directory can be copied, archived, burned, or transported independently.

Optical media usage is a secondary use case:

- The writer produces a spool archive or a set of spool volume directories.
- The user or a wrapper tool may burn each volume directory to CD/DVD/BD media.
- The reader later accepts one or more mounted volume directories and reconstructs the archive stream.

NeoTape does not need to treat optical media as a special core transport. Optical media support can be modeled as a reader input mode over filesystem-backed volume directories.

## Conceptual Layout

A spool archive may look like this:

```text
archive.spool/
  volume-000001/
    tape-file-000000.medium-header.ntf
    tape-file-000001.volume-header.ntf
    tape-file-000002.slice-000001.ntf
    tape-file-000003.slice-000002.ntf
    manifest.json
  volume-000002/
    tape-file-000001.volume-header.ntf
    tape-file-000002.slice-000003.ntf
    tape-file-000003.archive-end.ntf
    manifest.json
```

Each `volume-*` directory is a self-contained filesystem representation of one NeoTape virtual volume. For optical media, each directory can be used as the source tree for a disc image or a direct burn operation.

Example conceptual usage:

```sh
growisofs -Z /dev/dvd -R -J archive.spool/volume-000001
growisofs -Z /dev/dvd -R -J archive.spool/volume-000002
```

The exact disc creation command is outside the NeoTape core format. Implementations may provide helper scripts or wrappers, but the spool format should remain ordinary filesystem data.

## Difference from Tape Backend

The spool backend is not a real sequential tape device. It must not be forced to emulate every tape-device behavior.

In particular:

- Spool output is ordinary filesystem data.
- Volume boundaries are directory boundaries.
- Tape-file boundaries are regular file boundaries.
- Filemarks are represented structurally by files and metadata, not by physical tape positioning.
- Spool writers do not need the tape-device append safety rule that seeks to EOD before writing.

A spool writer may choose normal filesystem CLI semantics, such as creating a new output directory, failing if the output exists, or overwriting only when an explicit option is provided. Those choices are separate from tape-device destructive-position safety.

## Reader Support for Optical/Removable Volumes

Reader support for optical or removable media should be implemented as a multi-volume filesystem reader.

Inputs may include:

- A complete spool archive directory.
- A list of `volume-*` directories.
- A list of mounted optical discs.
- A staged copy of several burned volume directories.

Example conceptual commands:

```sh
neotape-cat-volumes /mnt/disc1 /mnt/disc2 /mnt/disc3 | bsdtar -xpf -
```

or:

```sh
neotape-cat-volumes --removable --expected-volumes=3
```

The reader should validate each supplied volume before consuming payload bytes.

At minimum, the reader should check:

- NeoTape magic and header type.
- Header CRC32C.
- `archive_uuid` consistency.
- Volume sequence number continuity.
- Slice continuation rules.
- SLICE_END segment header integrity.
- Archive End Header integrity when present.

If a wrong disc or wrong volume directory is supplied, the reader should reject it in normal mode. Interactive removable-media mode may prompt the user for the expected volume.

## Capacity Policy

Optical media have much smaller capacity than LTO media. The NeoTape core format does not need special rules for this. The writer or wrapper should choose suitable virtual volume size limits.

Examples:

```text
CD-R:     approximately 650-700 MiB practical payload budget
DVD-R:    approximately 4.3 GiB practical payload budget
BD-R SL:  approximately 25 GB nominal media class
BD-R DL:  approximately 50 GB nominal media class
BD-R XL:  larger media classes, depending on drive and media support
```

Implementations should reserve space for NeoTape headers, trailers, manifests, filesystem overhead, and disc filesystem metadata. The volume size limit should be conservative rather than attempting to fill media to the last byte.

## Non-Goals

This design note does not require:

- A dedicated optical writer backend in the initial implementation.
- Direct control of optical drives by NeoTape.
- ISO/UDF image generation inside the core writer.
- Tape-device append semantics for spool archives.

Optical usage is best treated as an export/deployment mode for spool volumes, plus reader support for multiple filesystem-backed volumes.

## Summary

The spool backend should remain a filesystem representation of NeoTape virtual volumes. CD/DVD/BD backup is a useful secondary workflow: produce spool volume directories, burn or copy them to removable media, then read them back through a multi-volume filesystem reader. This keeps the core format clean while still enabling practical removable-media backups.
