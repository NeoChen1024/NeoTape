# Security

Status: normative.

## Metadata Trust Model

Readers MUST treat optional recovery bundles and `ch_metadata` frame payloads as untrusted data.

Recovery bundle and catalog member names are identifiers for byte blobs, not restore paths. Readers MUST:

- Treat the bundle as a flat collection of named byte blobs.
- NOT interpret member names as filesystem paths.
- Reject absolute paths and parent-directory components (e.g. `../`) if any extended name syntax is supported.
- NOT restore ownership, permissions, device nodes, symlinks, hardlinks, xattrs, ACLs, or executable bits from recovery bundles or catalogs.

### Catalog as Index

The catalog is an advisory index, not authoritative metadata. Readers MUST:

- Validate path safety before using catalog data for partial restore selection.
- Reject or specially handle absolute paths, parent-directory traversal components, and policy-sensitive file types.
- Treat catalog/payload discrepancies by trusting payload metadata.

## Integrity vs. Authentication

BLAKE3 (`frame_hash`) is used for per-frame integrity verification, not authentication.

The `signature` field (72 bytes) holds an opaque 8-byte key ID copied
byte-for-byte from the signify-compatible key file, followed by a 64-byte
Ed25519 signature over the domain-separated message
`NeoTape-frame\0 || frame_hash`. The key ID is not an integer and has no byte
order. The context string includes its trailing NUL byte. When the `SIGNED`
flag is set and the signature verifies against a trusted public key, it
provides authenticity and tamper resistance. When the flag is clear, the
entire `signature` field MUST be zero. The validation modes and their
accept/reject rules are defined in [05-validation.md](05-validation.md).

## Executable Content

If an archive contains a restore helper binary (e.g. as a recovery bundle member), the reader MUST NOT automatically execute it. Source code and specification text are preferred over binaries for long-term preservation.

## Payload Path Safety

When restoring payload bytes through a downstream tool (e.g. bsdtar), path safety should be enforced by the downstream tool's security options, including:

- Rejecting or remapping absolute paths.
- Rejecting or remapping parent-directory traversal components.
- Controlling ownership, permission, device node, xattr, and ACL restoration.

## Transport Security

The NeoTape TCP protocol ([`08-tcp-protocol.md`](08-tcp-protocol.md)) is
plaintext and provides no confidentiality. Its base mode provides no peer
authentication. The optional challenge-response mode provides one-way
Archiver authentication to a Writer configured with a trusted public key, but
does not provide client authentication.

When signed frames are used ([`SIGNED` flag](02-frame-header.md) with
[Ed25519 signature](00-format-common.md) over
`NeoTape-frame\0 || frame_hash`) and verified against a trusted public key,
integrity and authenticity are protected at the frame level. A tampered frame
will fail verification regardless of transport.

Challenge-response uses a fresh nonce to prevent replay of an old
`auth_response`. Frame sequence validation rejects duplication and reordering
within the archive state being validated. Neither mechanism proves archive
freshness or prevents replay of a complete, otherwise-valid old archive; a
deployment requiring that property must independently enforce an expected
`archive_uuid` or another freshness policy.

When a deployment requires confidentiality or mutual peer authentication,
the operator SHOULD tunnel the connection through an external secure
channel such as SSH port forwarding (`ssh -L` / `ssh -R`), WireGuard,
or a TLS proxy (e.g. stunnel).  The NeoTape wire protocol remains
plaintext and does not attempt to replicate what these well-audited
tools already provide.
