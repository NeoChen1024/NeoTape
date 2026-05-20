# Security

Status: extracted from RFC_Draft.md §22; normative.

## Metadata Bundle Trust Model

Readers MUST treat header metadata bundles (Medium Header ar archives, SLICE_METADATA Frame payloads) as untrusted data.

### Member Names as Identifiers

ar member names are identifiers for byte blobs, not restore paths. Readers MUST:

- Treat the metadata bundle as a flat collection of named byte blobs.
- NOT interpret member names as filesystem paths.
- Reject absolute paths and parent-directory components (e.g. `../`) if any extended name syntax is supported.
- NOT restore ownership, permissions, device nodes, symlinks, hardlinks, xattrs, ACLs, or executable bits from metadata bundles.

### Catalog as Index

The catalog is an advisory index, not authoritative metadata. Readers MUST:

- Validate path safety before using catalog data for partial restore selection.
- Reject or specially handle absolute paths, parent-directory traversal components, and policy-sensitive file types.
- Treat catalog/payload discrepancies by trusting payload-profile metadata.

## Integrity vs. Authentication

BLAKE3 is used for integrity verification, not authentication.

If authenticity or tamper resistance is required, NeoTape SHOULD add signatures or keyed authentication metadata over the relevant BLAKE3 digests in a future extension or deployment profile.

## Executable Content

If an archive contains a restore helper binary (e.g. as a metadata bundle member), the reader MUST NOT automatically execute it. Source code and specification text are preferred over binaries for long-term preservation.

## Payload Path Safety

When restoring payload bytes through a downstream tool (e.g. bsdtar), path safety should be enforced by the downstream tool's security options, including:

- Rejecting or remapping absolute paths.
- Rejecting or remapping parent-directory traversal components.
- Controlling ownership, permission, device node, xattr, and ACL restoration.
