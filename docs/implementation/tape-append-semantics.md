# Tape Backend Append and Initialization Semantics

Status: design note / implementation note.

This document defines the intended safety behavior for the NeoTape tape-device backend. The goal is to prevent accidental destruction of existing tape data while still allowing multiple NeoTape archive instances to be stored on the same physical medium.

## Core Rule

A tape-device writer must not write from BOT (beginning of tape) except during explicit media initialization.

Normal archive creation on a tape device is append-only:

1. Locate the logical end of valid data.
2. Verify that the previous archive instance ended cleanly, when possible.
3. Return to the logical end of valid data.
4. Write the new archive instance.

This is a tape-device backend safety rule. It does not apply to filesystem spool backends, because spool archives are ordinary filesystem objects and do not share the destructive position-dependent behavior of sequential tape devices.

## Initialization Mode

Initialization is the only mode that intentionally writes from BOT.

Initialization may create or rewrite media-level structures such as:

- Optional recovery bundle.
- Initial media identity metadata.
- Optional implementation-specific label or diagnostic metadata.

Because initialization can make existing tape contents inaccessible or overwrite them, it must be explicit.

Example conceptual command:

```sh
neotape init --target=tape /dev/nst0
```

If the implementation detects existing data and the operation would be destructive, it should require an explicit force option or an interactive confirmation.

Example conceptual command:

```sh
neotape init --target=tape /dev/nst0 --force --label MYTAPE001
```

The exact CLI spelling is not fixed by this note, but the operation must be visibly distinct from normal archive writing.

## Normal Append Mode

Normal tape-device archive writing should append a new archive instance after the existing valid data.

Example conceptual command:

```sh
neotape backup --target tape:/dev/nst0 ./source
```

The writer should not assume that the current device position is safe. Before writing, it should perform append preflight.

## Append Preflight

The intended append preflight is:

```text
1. Seek to logical end of valid data.
2. Locate the previous archive-end tape file.
3. Read and verify the Archive End Header.
4. Seek to logical end of valid data again.
5. Begin writing the new archive instance.
```

The verification step should check at least:

- NeoTape magic.
- Header type is Archive End Header.
- Header CRC32C.
- Header size and version fields.
- Archive UUID fields.
- Archive-end checksum or summary fields, if present.

If the previous archive end cannot be found or verified, strict append mode should refuse to write.

## Linux mt-st Implementation Note

For the Linux `mt-st` tape control tool and compatible tape devices, the intended fast path is conceptually:

```sh
mt -f /dev/nst0 eod
mt -f /dev/nst0 bsfm 2
# read and verify the previous Archive End Header tape file
mt -f /dev/nst0 eod
# append the new NeoTape archive instance
```

The `eod` operation moves to the end of valid data and is suitable for appending on streamer tape drives. The `bsfm` operation spaces backward over filemarks and then forward to the beginning of a file, which can be used to locate the previous tape file.

The exact filemark count is backend-layout dependent. `bsfm 2` is the expected fast path when the final tape layout is:

```text
[Archive End Header tape file][filemark][logical EOD]
```

Implementations must validate this against the actual NeoTape filemark policy and the Linux st driver options in use. In particular, implementations should avoid depending on implicit close-time filemark behavior. NeoTape should explicitly control when filemarks are written.

## Append Policies

The implementation may expose append policies.

Suggested policies:

```text
strict
  Default. Append only if the previous archive instance has a valid Archive End Header.

inspect
  Locate and report the previous tape-file state without writing.

force
  Dangerous. Append at logical EOD even if the previous archive end cannot be verified.
  This should require an explicit option and should produce a clear warning.
```

Strict mode should be the default.

## Blank Tape Behavior

If normal append mode is used on a blank tape, the implementation may either:

- fail and require explicit initialization first; or
- initialize only if the tape is confidently detected to be blank and the user selected an option such as `--init-if-blank`.

The safer default is to fail and ask for explicit initialization.

Example conceptual command:

```sh
neotape backup --target tape:/dev/nst0 --init-if-blank ./source
```

## Incomplete Tail Handling

A tape may end with an incomplete archive because of interruption, EOT, device error, power loss, or software failure.

Examples:

```text
[Volume Header][Slice content without final END Frame]
[Archive End Header partially written]
[Unknown or unreadable final tape file]
```

In strict append mode, the writer should not append silently after such a tail.

Instead, it should enter one of these paths:

- fail with diagnostics;
- offer inspect mode;
- offer salvage/recovery tools;
- allow explicit force append only when the user accepts the risk.

This preserves the possibility of later salvage and avoids accidentally making recovery more confusing.

## Multiple Archive Instances per Medium

The append-only rule supports multiple independent NeoTape archive instances on a single physical medium.

Conceptual layout:

```text
[Optional recovery bundle]
[Archive A Volume Header]
[Archive A Slice...]
[Archive A End Header]
[Archive B Volume Header]
[Archive B Slice...]
[Archive B End Header]
[Archive C Volume Header]
...
```

Each archive instance has its own `archive_uuid` and clean end marker. Readers may scan archive instances by tape-file boundaries and filemarks.

## Summary

Tape-device writes are position-sensitive and potentially destructive. NeoTape therefore treats initialization and normal writing as separate operations. Initialization may write from BOT, but normal writing must append after existing valid data and should verify the previous Archive End Header before appending.
