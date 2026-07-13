# Recovery Bundle Implementation

Status: implementation note.

## Current Strategy

NeoTape's current recovery-bundle implementation does not build a separate
minimal restore utility.

Instead, the recovery bundle is a tar archive of the NeoTape repository source
tree, with build artifacts excluded. This avoids maintaining two independent
restore implementations and keeps the recovery path aligned with the main
codebase.

The bundle is intended for operator-assisted recovery and source preservation,
not for automatic execution by a reader.

## Makefile Target

The repository provides:

```sh
make bot_bundle
```

This writes:

```text
output/bot.tar
```

## Writing the Bundle

Pass the generated tar to a non-append writer invocation:

```sh
bin/neotape-write --source unix:///run/neotape/archive.sock \
  --target tape:/dev/nst0 --erase \
  --recovery-bundle output/bot.tar
```

For physical tape, recovery-bundle records use a block size separate from the
NeoTape frame size. The default is 256 KiB; it can be overridden with
`--recovery-bundle-block-size <SIZE>`. The writer zero-pads only the final
bundle record and writes a filemark before switching to the volume's NeoTape
frame size. It does not use the traditional 10 KiB tar record size.

For a spool target, the writer preserves the tar byte-for-byte as
`recovery-bundle.tar`. Readers ignore this file because it is outside the
numbered `.nts` stream. `--recovery-bundle` is incompatible with `--append`.

## Bundle Contents

`make bot_bundle` archives the current repository working tree directly, so the
contents of checked-out `3rdparty/` git submodules are included in the bundle.
The tar step excludes common build artifacts and VCS metadata.

Excluded content includes:

- `bin/`
- `build/`
- `output/`
- `*.o`
- `*.a`
- `compile_commands.json`

This means the bundle contains the source, docs, scripts, tests, and other
project files needed to rebuild or inspect NeoTape later, without packaging the
current compiled outputs. It also contains the checked-out third-party source
trees needed to rebuild the project.

## Rationale

This design is deliberate:

- it avoids implementing the restore path twice;
- it keeps the recovery material close to the maintained code;
- it preserves documentation and build scripts alongside the sources;
- it reduces long-term divergence between a "main" implementation and a
  separate emergency implementation.

## Relationship to Spec

The normative format documents only say that an optional recovery bundle may be
written before the first NeoTape slice.

This file defines the current repository-level implementation choice for how
that bundle is produced.
