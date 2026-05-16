# Roadmap Draft：NeoTape Implementation Plan

# Status

`This roadmap is a planning draft. It is not part of the normative NeoTape v0.1 format definition. Its purpose is to guide incremental implementation, validation, and specification refinement. During implementation, concrete byte-level header layouts, catalog schemas, CLI behavior, and backend behavior SHOULD be fed back into the main specification.`

# Roadmap Principles

`1. Build the smallest end-to-end system first.`  
`2. Keep NeoTape core payload-format agnostic.`  
`3. Prefer filesystem spool mode before real tape hardware.`  
`4. Make every milestone produce testable artifacts.`  
`5. Treat the byte-level format as a versioned contract once the first reader/writer interoperate.`  
`6. Add tape-device behavior only after the spool backend and logical reader are reliable.`  
`7. Keep stdout pure payload bytes; diagnostics, prompts, and progress belong on stderr or /dev/tty.`

# Phase 0: Repository and Test Harness

`Goal: establish a development skeleton before freezing binary layout.`

`Deliverables:`  
`- Repository layout for neotape core library, writer CLI, reader CLI, test fixtures, and format notes.`  
`- Shared constants for magic values, header type ids, version ids, and payload profile ids.`  
`- Basic CRC32C and BLAKE3 utility wrappers.`  
`- Golden test fixture mechanism for binary records.`  
`- CI jobs that run unit tests and fixture round-trip tests.`

`Initial scope:`  
`- No real tape backend.`  
`- No libarchive integration yet.`  
`- No complete catalog format yet.`  
`- Use small artificial block sizes in tests, while preserving the production rule that archive-time fixed headers must fit in one tape record.`

# Phase 1: Minimal Binary Header Layout

`Goal: define enough concrete byte layout to create parseable NeoTape records.`

`Deliverables:`  
`- Fixed binary layout for common archive-time headers.`  
`- Header parser and serializer.`  
`- CRC32C verification over fixed header bytes.`  
`- BLAKE3 verification utility for payload ranges.`  
`- Explicit endian choice, integer widths, alignment rules, reserved fields, and string/UUID encoding.`

`Candidate headers to freeze first:`  
`- Volume Header`  
`- Segment Header`  
`- Slice Trailer fixed header`  
`- Archive End Header`

`Deferred:`  
`- Full Cartridge Header byte layout.`  
`- Full Slice Trailer metadata item schema.`  
`- Final catalog binary schema.`

`Spec feedback expected:`  
`- Exact byte offsets for common fields.`  
`- Required vs optional fields per header type.`  
`- Rules for unknown flags and forward-compatible reserved fields.`  
`- Maximum header_size and metadata_size constraints.`

# Phase 2: Filesystem Spool Backend MVP

`Goal: implement NeoTape logical archive creation without tape hardware.`

`Deliverables:`  
`- neotape-write --target=spool.`  
`- Deterministic spool directory layout.`  
`- Volume directory creation.`  
`- Tape-file object creation for volume header, slice files, and archive end header.`  
`- Segment header + payload + Slice Trailer sequence inside each slice tape file.`  
`- Configurable virtual volume size to simulate EOT/ENOSPC.`

`MVP payload source:`  
`- raw payload file or stdin.`

`MVP payload profile:`  
`- raw.`

`Validation:`  
`- Spool manifest lists archive_uuid, volume order, tape-file names, sizes, BLAKE3 digests, declared block_size, and virtual volume size.`  
`- A debug tool can inspect every spool tape-file object and validate header order.`

`Spec feedback expected:`  
`- Whether spool filenames should be normative or advisory.`  
`- Whether manifest is purely advisory or partially standardized.`  
`- Exact behavior when virtual volume limit is hit before a header, during payload, or before Slice Trailer metadata completion.`

# Phase 3: Minimal Reader for Spool Archives

`Goal: implement neotape-cat-volumes against the virtual tape abstraction.`

`Deliverables:`  
`- Virtual tape reader interface: read-record, next-file, next-volume, EOT/volume-limit event.`  
`- Spool adapter implementing that interface.`  
`- Header validation and sequencing.`  
`- Segment payload concatenation by segment_payload_size.`  
`- Slice Trailer verification by slice_payload_size and slice_payload_blake3.`  
`- Archive End Header validation.`  
`- stdout payload emission for raw profile.`

`Validation:`  
`- raw input -> spool archive -> neotape-cat-volumes -> byte-identical output.`  
`- Multi-volume virtual EOT fixture.`  
`- Corrupt header fixture.`  
`- Corrupt payload fixture.`  
`- Missing Archive End Header fixture.`  
`- Missing Slice Trailer fixture.`

`Spec feedback expected:`  
`- Exact reader state machine transitions.`  
`- Exit codes.`  
`- Error classes.`  
`- Which errors are retryable, salvageable, or fatal.`

# Phase 4: NeoTape/PAX Payload Profile MVP

`Goal: produce useful POSIX-style backups through libarchive/bsdtar-compatible pax streams.`

`Deliverables:`  
`- neotape-write --payload-profile=pax for file trees.`  
`- Initial source reader profile: single-reader-prefetch-metadata or simple tree walker.`  
`- Continuous pax stream generation.`  
`- Slice boundaries chosen by writer policy, preferably at pax member boundaries when practical but not required by core.`  
`- neotape-cat-volumes --payload-profile=pax emits pax bytes to stdout.`

`Validation:`  
`- neotape-cat-volumes archive.spool | bsdtar -xpf - restores test trees.`  
`- Tests for xattrs, ACLs, symlinks, hardlinks, sparse files, long paths, and device nodes where supported by platform policy.`  
`- Confirm that on-tape slice boundaries do not require slice-local pax EOA.`

`Spec feedback expected:`  
`- Profile-specific stdout finalization policy for pax EOA.`  
`- Whether PAYLOAD_512_ALIGNED is mandatory, recommended, or advisory for pax.`  
`- How to represent source-read warnings and partial-read diagnostics in Slice Trailer metadata.`

# Phase 5: Slice Trailer Metadata and Catalog v0

`Goal: make partial restore planning and audit possible without making catalog authoritative.`

`Deliverables:`  
`- Metadata Block framing for Slice Trailer metadata areas.`  
`- Length-framed metadata item table.`  
`- Initial catalog item schema.`  
`- Per-slice catalog emission for pax profile.`  
`- Archive-level catalog emission in Archive End Header metadata or final payload entries.`

`Validation:`  
`- List archive contents without replaying entire payload where catalog is present.`  
`- Verify metadata_blake3 before trusting catalog bytes.`  
`- Confirm restore correctness does not depend on catalog correctness.`  
`- Catalog/payload disagreement tests where payload profile metadata wins.`

`Spec feedback expected:`  
`- Exact metadata item table byte layout.`  
`- Catalog entry schema versioning.`  
`- Path encoding and safety rules.`  
`- Compression attribute model for catalog payloads.`

# Phase 6: Tape Device Backend Prototype

`Goal: replay the same logical format onto a real sequential tape device.`

`Deliverables:`  
`- Tape backend adapter using standard OS tape device operations.`  
`- Fixed block_size configuration.`  
`- Write volume header, filemark, slice tape files, archive end header.`  
`- Detect EOT/EOM/ENOSPC and transition to next volume.`  
`- Read path for /dev/nst0 or equivalent non-rewinding tape device.`

`Validation:`  
`- Small archive write/read on real or emulated tape device.`  
`- Filemark spacing tests.`  
`- Multi-volume manual prompt test.`  
`- Mismatch handling test.`  
`- ScanNextVolumeHeader test on a cartridge containing multiple archive instances.`

`Spec feedback expected:`  
`- Portable mapping from SCSI sequential-access behavior to user-space operations.`  
`- Required behavior for fixed vs variable block mode.`  
`- Hardware compression reporting and native vs physical occupancy reporting.`  
`- Tape eject/insert detection reliability.`

# Phase 7: Recovery and Salvage Modes

`Goal: make failure behavior explicit and testable.`

`Deliverables:`  
`- Retry / Inspect / Fail / Force-Salvage policy model.`  
`- --control=auto|tty|none.`  
`- --on-mismatch=prompt|fail|scan-next-volume-header.`  
`- --on-volume-header-error=prompt|fail|scan-next-volume-header.`  
`- --on-eot=prompt|fail.`  
`- --retry=N.`  
`- --salvage.`

`Validation:`  
`- Corrupted volume header.`  
`- UUID mismatch.`  
`- tape_seq_num mismatch.`  
`- segment sequence mismatch.`  
`- read error inside segment payload.`  
`- incomplete slice.`  
`- missing Archive End Header.`

`Spec feedback expected:`  
`- Exact prompt behavior.`  
`- JSON control protocol needs.`  
`- stdout contamination prevention rules.`  
`- Salvage output marking requirements.`

# Phase 8: Cartridge Header and Long-Term Self-Description

`Goal: define the immutable BOT cartridge header and recovery bundle.`

`Deliverables:`  
`- Cartridge Header binary/ASCII prefix.`  
`- Cartridge metadata bundle format using restricted ar-style flat member container.`  
`- Embedded FORMAT-SPEC, RESTORE, README, and minimal neotape-cat-volumes source archive.`  
`- Cartridge initialization tool.`

`Validation:`  
`- Initialize blank virtual cartridge or tape.`  
`- Read BOT cartridge header without external database.`  
`- Verify metadata bundle integrity.`  
`- Confirm header does not contain mutable archive index or free-space state.`

`Spec feedback expected:`  
`- Cartridge header first-record minimum fields.`  
`- Multi-record cartridge header layout.`  
`- Recommended content of self-description bundle.`  
`- Relationship with barcode, MAM, and external tape database.`

# Phase 9: Filesystem-Native Payload Profiles

`Goal: explore ZFS and Btrfs send stream support without changing NeoTape core.`

`Deliverables:`  
`- zfs-send payload profile draft.`  
`- btrfs-send payload profile draft.`  
`- Payload sub-stream model for dataset/subvolume/snapshot streams.`  
`- Catalog fields for dataset/subvolume identity, snapshot names, parent snapshot dependencies, receive order, stream-level checksum, and logical slice range.`

`Validation:`  
`- Store one full send stream as one or more NeoTape logical slices.`  
`- Restore by emitting payload bytes to the relevant receive tool.`  
`- Incremental stream dependency ordering tests.`  
`- Multi-subvolume archive listing via catalog.`

`Spec feedback expected:`  
`- Whether a logical slice may contain multiple filesystem-native sub-streams.`  
`- How to model parent snapshot dependencies.`  
`- How much receive-order metadata belongs in Slice Trailer vs Archive End Header.`  
`- Whether stream-level checksums are profile metadata or core metadata.`

# Phase 10: Format Stabilization and Compatibility Testing

`Goal: prepare a stable v0.1 interop target.`

`Deliverables:`  
`- Frozen byte layout for v0.1 headers.`  
`- Test vectors for every header type.`  
`- Golden spool archives.`  
`- Golden pax archives.`  
`- Reader compatibility matrix.`  
`- Versioning and feature flag policy.`

`Validation:`  
`- Old reader behavior against archives with unknown optional metadata.`  
`- New reader behavior against minimal old archives.`  
`- Corruption tests with deterministic diagnostics.`  
`- Cross-platform tests across Linux distributions and filesystems.`

`Spec feedback expected:`  
`- MUST/SHOULD/MAY cleanup.`  
`- Removal of obsolete open questions.`  
`- Separation between normative core, payload profiles, and implementation notes.`

# Phase 11: Production Hardening

`Goal: make NeoTape usable for real backup workflows.`

`Deliverables:`  
`- Progress reporting.`  
`- Resume/retry support where safe.`  
`- Tape changer/robot integration experiments.`  
`- Operational logging.`  
`- External tape database integration hooks.`  
`- Packaging.`  
`- Documentation for backup, verify, restore, inspect, and salvage workflows.`

`Validation:`  
`- Large archive stress tests.`  
`- Long-running write tests.`  
`- Restore drills.`  
`- Partial restore drills.`  
`- Damaged media simulation.`  
`- Multi-archive cartridge scanning.`

# Open Implementation Questions

`1. Which language should the first implementation use: Rust, Go, C, or a hybrid with a small portable C reader?`  
`2. Should the minimal reader be dependency-light enough to embed in cartridge metadata bundle as source code?`  
`3. What is the first stable block_size default for real LTO drives: 1 MiB, 4 MiB, 8 MiB, or device-specific?`  
`4. Should the first public implementation support only spool mode and pax profile, delaying tape backend until format tests are stable?`  
`5. Should the catalog format be binary-only, text-friendly, or dual-layer with binary records plus optional human-readable summaries?`  
`6. How should NeoTape handle encrypted payload profiles while preserving catalog usability?`  
`7. What is the minimum supported platform set for v0.1?`  
`8. How should release artifacts include format test vectors and compatibility fixtures?`

`Near-Term Recommended Next Step`

`Implement Phase 0 through Phase 3 first: repository skeleton, concrete minimal headers, spool writer, and spool reader for raw payload. This creates a small closed loop where header layout, length framing, slice trailer verification, and multi-volume continuation can be tested before introducing libarchive, real tape hardware, or filesystem-native profiles.`

`----`  
`End of Roadmap`