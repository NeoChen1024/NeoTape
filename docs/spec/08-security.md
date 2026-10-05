# Security

Status: normative.

## Metadata Trust Model

Readers MUST treat optional recovery bundles and `ch_metadata` frame payloads as untrusted data.

### Recovery Bundle

The recovery bundle is not part of the NeoTape stream. Readers skip it when
locating the first NeoTape frame and MUST NOT extract, interpret, or execute
it. An operator who unpacks it does so with an ordinary tar tool and that
tool's own path-safety options.

### Catalog as Index

The catalog ([09-plan-metadata.md](09-plan-metadata.md)) is an advisory index,
not authoritative metadata. Its paths, owners, sizes, and times describe what
was planned; they never create or modify files. Readers MUST:

- Ignore `/chdir/` records, which name directories on the archiving host.
- Present catalog paths so that control characters or text-direction controls
  cannot make one path appear to be another.
- Validate path safety before using catalog data for partial restore selection.
- Treat catalog/payload discrepancies by trusting payload metadata.
- Stay within bounded memory regardless of catalog size or record length.

## Integrity vs. Authentication

BLAKE3 (`frame_hash`) is used for per-frame integrity verification, not authentication.

When the `SIGNED` flag is set and the signature verifies against a trusted
public key, the frame is authentic and tamper-evident. The signature encoding
is defined in [00-format-common.md](00-format-common.md#signing-sequence); the
validation modes and their accept/reject rules are defined in
[04-validation.md](04-validation.md#signature-validation-modes).

## Executable Content

If an archive contains a restore helper binary (e.g. as a recovery bundle member), the reader MUST NOT automatically execute it. Source code and specification text are preferred over binaries for long-term preservation.

## Payload Path Safety

When restoring payload bytes through a downstream tool (e.g. bsdtar), path safety should be enforced by the downstream tool's security options, including:

- Rejecting or remapping absolute paths.
- Rejecting or remapping parent-directory traversal components.
- Controlling ownership, permission, device node, xattr, and ACL restoration.

## Transport Security

The socket protocol is plaintext. Its threat model, the optional
challenge-response authentication of the Archiver, and the recommendation to
tunnel connections that need confidentiality or mutual authentication are
defined in [07-tcp-protocol.md](07-tcp-protocol.md#security).

Signed frames protect integrity and authenticity regardless of transport.
Neither signatures nor sequence validation prove archive freshness: a
complete, valid old archive can be replayed unless the deployment checks an
expected `archive_uuid` or applies another freshness policy.
