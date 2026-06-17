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

The `signature` field (72 bytes) holds an 8-byte key ID plus a 64-byte Ed25519 signature over `frame_hash`. When the `SIGNED` flag is set, the signature provides authenticity and tamper resistance. When the flag is clear, the entire `signature` field MUST be zero.

## Executable Content

If an archive contains a restore helper binary (e.g. as a recovery bundle member), the reader MUST NOT automatically execute it. Source code and specification text are preferred over binaries for long-term preservation.

## Payload Path Safety

When restoring payload bytes through a downstream tool (e.g. bsdtar), path safety should be enforced by the downstream tool's security options, including:

- Rejecting or remapping absolute paths.
- Rejecting or remapping parent-directory traversal components.
- Controlling ownership, permission, device node, xattr, and ACL restoration.

## Transport Security

The NeoTape TCP protocol ([`12-tcp-protocol.md`](12-tcp-protocol.md)) provides no
authentication, encryption, replay defense, or peer identity verification.
It is designed for localhost or trusted LAN deployments (2.5 Gbps minimum;
typical target 10 Gbps or higher).

When signed frames are used ([`SIGNED` flag](01-frame-header.md) with
[Ed25519 signature](00-format-common.md) over `frame_hash`), **integrity
and authenticity are protected at the frame level** — a tampered frame
will fail verification regardless of transport.

When a deployment requires confidentiality or peer authentication,
the operator SHOULD tunnel the connection through an external secure
channel such as SSH port forwarding (`ssh -L` / `ssh -R`), WireGuard,
or a TLS proxy (e.g. stunnel).  The NeoTape wire protocol remains
plaintext and does not attempt to replicate what these well-audited
tools already provide.
