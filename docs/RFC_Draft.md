# NeoTape：面向 LTO 磁帶的可定位多卷 Length-Framed Payload Transport Format

RFC Draft v0.1

## Status of This Memo

本文件是一份設計草案，而非既有標準。它描述一種暫稱為 NeoTape 的 multi-volume length-framed payload transport format，用於將 payload byte stream 可靠地寫入 LTO 磁帶。v0.1 推薦 POSIX pax/tar 作為初始 payload profile，使未修改的 bsdtar / libarchive 能夠還原資料。

Authoritative split-out specification files live under `docs/spec/`. When a
topic is defined in `docs/spec/`, that definition supersedes the corresponding
or conflicting text in this RFC draft. This draft remains useful as background
rationale and historical design context, but it is not authoritative for topics
already split into `docs/spec/`.

本草案的核心假設是：NeoTape core 應承擔 volume、filemark、slice、Frame、continuation、catalog、checksum 與錯誤恢復等 transport semantics；payload archive semantics 則由 payload profile 承擔。v0.1 推薦 NeoTape/PAX profile，但 core framing 不依賴 pax/tar EOA。

# Abstract

NeoTape 是一種針對 LTO 磁帶設計的 seekable multi-volume length-framed payload transport container。它將整體備份切分為多個 logical slices；每個 logical slice 是 writer 在串流過程中決定關閉的一段 payload bytes；最後一個 Frame Header（帶 `END` flag）記錄該 slice 的 slice_content_size 與 slice_content_blake3。每個 logical slice 對應一個主要 LTO tape file，內含一個或多個 length-framed Frames；每個 Frame 前置 NeoTape Frame Header，其中明確記錄 frame_payload_size，之後接續該 Frame 的 payload byte range。filemark 位於 slice boundary，而不是每個 Frame boundary，因此磁帶機原生 file seek 能力定位到可完整驗證的 slice，同時避免在整卷磁帶上建立過多 filemarks。

NeoTape 的讀取工具 neotape-cat-volumes 會讀取多卷磁帶、驗證 volume / Frame Headers、處理 End of Tape continuation，並依 payload profile 將多個 logical slices 重組輸出至 stdout。Minimal reader 只需要依 length fields 串接 payload bytes；對 NeoTape/PAX payload profile，下游可以直接使用 bsdtar -xpf - 還原。

# 1. Introduction

本設計的主要使用情境是將一般 POSIX-like filesystem 或檔案樹備份到 LTO 磁帶。ZFS RAID-Z2 HDD 陣列只是 motivating example：在大量小檔案情境下，單一 I/O thread 可能無法穩定餵滿 LTO 原生寫入速率，因此 writer 可使用多 I/O threads、metadata prefetch、類 mbuffer 記憶體緩衝功能、libarchive pax writer、NeoTape target backend 與換卷流程整合為一個 pipeline。

NeoTape format itself is filesystem-agnostic. The writer MAY use different source reader profiles depending on the source filesystem and workload. For example, ZFS small-file workloads may benefit from multiple concurrent file reader threads, while an XFS filesystem or fast NVMe-backed filesystem may saturate available I/O bandwidth with a single file reader plus a tree walker thread that prefetches metadata ahead of serialization for slice packing decisions.

同時，傳統單一超長 tar/pax 串流在磁帶上有幾個問題：

* 中間錯誤可能導致後續資料難以重新同步
* 單一 archive 若橫跨多卷磁帶，需要明確的換卷邏輯
* 若單一檔案大於單卷 LTO 容量，archive stream 必須能在檔案內容中間跨卷 continuation。
* LTO 並非 block device；它不能任意 seek 到 byte offset，但可以利用 filemark 快速跳到 tape file boundary。
* archive 外層不宜使用需要尾端 trailer 或回填的 wrapper，因為 EOT 位置不可可靠預測

NeoTape 因此採用 LTO 原生分隔能力：每個 archive volume 有 volume header；每個 logical slice 是一個主要 tape file；slice tape file 內可包含多個 Frames，每個 Frame 由 Frame Header 與 length-framed payload byte range 組成；logical slice 的最後一個 Frame 帶有 `END` flag，在 Frame Header 中記錄 slice_content_size 與 slice_content_blake3；slice tape file 完成後寫入 filemark。最後以 Archive End Header 宣告整體 archive cleanly complete。

# 2. Terminology

Terminology is split into [docs/spec/terminology.md](spec/terminology.md).

When terminology in this RFC draft conflicts with `docs/spec/terminology.md`,
the split-out terminology document is authoritative.

# 3\. Design Goals

NeoTape 的設計目標如下：

1. NeoTape/PAX payload profile compatibility：v0.1 推薦的 NeoTape/PAX payload profile 應與 libarchive/bsdtar 的 POSIX pax 格式相容。
2. Length-framed payload container：NeoTape core is payload-format agnostic. Frame and slice boundaries are determined by explicit length fields in NeoTape headers, not by parsing pax/tar EOA or any payload-internal marker.
3. Restore simplicity：最小還原工具 neotape-cat-volumes 只負責磁帶 transport、multi-volume sequencing 與 length-framed payload concatenation；payload 解讀由 payload profile 或下游工具處理。
4. LTO-native seekability：使用 LTO filemark 作為 volume、logical slice、archive end 等 coarse-grained boundary，使磁帶能快速 seek 到完整可驗證的 slice，而不是為每個 internal Frame 建立 filemark。
5. Target backend abstraction：NeoTape writer SHOULD support multiple backing stores for the same logical format, including direct sequential tape device output and ordinary filesystem spool output. Filesystem spool output is useful for debugging, test fixtures, offline archive preparation, staging while the tape drive is busy, and deterministic reproduction of volume/Frame layout.
6. Multi-volume continuation：支援 logical slice 與單一檔案跨卷延續，不受單卷磁帶容量限制。
7. No mandatory trailer rewrite：不要求回到磁帶開頭或中間回填 metadata；不依賴 EOT 前仍有空間可寫 trailer。
8. Streaming writer：slice 是邏輯概念，不要求 writer 先將整個 slice spool 到本機檔案或 RAM。
9. Error containment：錯誤應盡量限制在單一 logical slice 內；後續 slice 應能透過 slice-level filemark 與下一個 slice tape file 的 Frame Header 重新同步。Frame Headers remain useful for length framing and diagnostics inside a slice, but they are not normally separate filemark seek points.
10. Long-term recoverability：每卷可在 header metadata bundle 中內嵌格式說明、restore 工具 source code、README、catalog 摘要與檢查碼。
11. Multi-archive media use：若單一 archive 無法填滿整卷磁帶，格式應允許在同一 physical medium 上順序寫入多個完整 archive instances，且每個 archive 仍保持自己的 archive_uuid 與 clean end header。
12. Media self-description：每張 physical medium 必須在 BOT 有 immutable Medium Header，用於保存 format version、block size、格式說明與 minimal reader source code，使磁帶離開外部 database 後仍可自我描述。
13. Filesystem-agnostic operation：NeoTape 格式不得假設 source filesystem 必須是 ZFS。Writer 應允許依 filesystem 與 workload 選擇 reader profile，例如 multi-threaded small-file reader、single sequential reader、metadata-prefetch walker 或混合模式。
14. Single-record archive-time header commitment：volume、Frame、archive end 等可能在接近 EOT 時寫入的 fixed headers 必須能放入單一 tape record；Medium Header 是例外，因為它只在 BOT 初始化時寫入，可由多個 records 組成。

# 4\. Non-Goals

NeoTape v0.1 不嘗試：

* 定義新的檔案封存語義。UID/GID、xattrs、ACL、hardlink、symlink、device node、sparse file 等由 pax/libarchive 表示。
* 取代 bsdtar、GNU tar 或 libarchive。
* 在每個 Frame 或 logical slice 內保證可獨立解出檔案。NeoTape core 只保證 length-framed payload transport；payload 是否可獨立解讀由 payload profile 決定。
* 要求使用 LTO partition。metadata partition 可作為未來擴充，但 v0.1 使用 tape file 與 filemark 即可。
* 要求 OS-specific raw SCSI API。一般 operation 應只依賴標準 sequential tape device 行為；raw SCSI passthrough 僅可作為診斷或進階工具的 optional extension。
* 支援 legacy non-LTO tape media。NeoTape v0.1 以 LTO-class media 為目標，避免為小 record-size 的過時媒體降低 archive-time header 的 single-record commit requirement。
* 定義所有 payload formats 的語義。NeoTape core is payload-format agnostic, but each payload profile must define its own interpretation and stdout behavior. v0.1 recommends a NeoTape/PAX payload profile for backup compatibility.
* 要求 writer 必須直接寫入實體磁帶裝置。Conforming writers MAY write the same NeoTape volume/Frame/header layout to an ordinary filesystem spool directory, as long as the resulting files preserve the same logical sequence, lengths, checksums, and volume transition semantics.
* 要求 source filesystem 是 ZFS。ZFS 可作為一致性 snapshot 與小檔案多執行緒讀取最佳化的典型案例，但格式與 reader/writer pipeline 應適用於 XFS、UFS、ext4、NFS-mounted trees、object-staged file trees 或其他可由使用者空間列舉與讀取的來源。

# 5\. Tape Model

NeoTape assumes modern LTO tape drives accessed through the standard SCSI sequential-access tape device model. It is intentionally not a generic legacy tape format for DDS/DAT/QIC-era devices with small maximum record sizes. LTO remains the practical target because it is the modern tape ecosystem still actively used for backup and archival storage, with current generations and public roadmaps continuing beyond earlier generations.

NeoTape does not require raw SCSI passthrough for normal operation. It only requires the operating system's standard tape device interface for open/read/write, writing filemarks, spacing filemarks, rewind/offline, detecting EOT/EOD, and setting or using variable block mode. The precise mapping of those operations is implementation-specific, but the on-tape format is not tied to a private OS or vendor API.

NeoTape v0.1 requires tape record sizes large enough to hold every archive-time fixed header as a single record. Implementations MUST support at least 256 KiB tape records for volume, Frame, and archive end header records. Each archive volume MUST declare a fixed volume_block_size in its Volume Header, and all NeoTape records in that archive volume MUST use that block size unless a future profile explicitly defines a compatible record-framing exception. The Medium Header is excluded from this archive-volume volume_block_size rule because it is written at BOT during medium initialization and may span multiple records. volume_block_size SHOULD be a positive multiple of 512 bytes for NeoTape/PAX payload profile compatibility. 8 MiB is the recommended default for high-throughput LTO operation when supported, but the chosen value is fixed per archive volume after the Volume Header is committed.

A large fixed volume_block_size also helps the target backend preserve long contiguous byte ranges for the tape drive. When drive hardware compression is enabled, larger records reduce artificial fragmentation introduced by the NeoTape transport layer and give the drive a better opportunity to compress the payload stream according to its own internal compression model. NeoTape MUST NOT assume any particular compression ratio, and catalog or capacity planning MUST distinguish native payload bytes from drive-compressed physical occupancy.

A physical medium may contain multiple archive instances. The example below shows a single archive instance; in multi-archive mode, the next archive may begin at the tape file immediately after a clean NeoTape end header.

NeoTape v0.1 uses slice-level filemark granularity. Frame-level filemarks are not recommended for normal archives because they can create hundreds or thousands of tape files on a large LTO medium, while only a completed logical slice (its last Frame Header with `END`) has an authoritative slice_content_blake3. Therefore, filemarks SHOULD delimit volume headers, completed logical slices, and archive end records. Frames SHOULD be length-framed records inside a slice tape file unless a future profile or explicit diagnostic mode opts into finer-grained filemarks.

NeoTape 使用 filemark 作為 seekable boundary。典型磁帶布局如下：

Tape file 0:
  NeoTape Medium Header
filemark
Tape file 1:
  NeoTape archive volume header
filemark
Tape file 2:
  NeoTape Frame Header for slice 1 Frame 1
  payload bytes for slice 1 Frame 1
  NeoTape Frame Header for slice 1 Frame 2 (`END`)
  payload bytes for slice 1 Frame 2
filemark
Tape file 3:
  NeoTape Frame Header for slice 2 Frame 1
  payload bytes for slice 2 Frame 1
  NeoTape Frame Header for slice 2 Frame 2 (`END`)
  payload bytes for slice 2 Frame 2
filemark
...
Final tape file for archive instance:
  NeoTape Archive End Header
filemark

若遇到 EOT，目前 slice tape file 可能未以正常 filemark 關閉，且最後一個 Frame 可能只部分 committed，或 logical slice payload 已完成但 final SLICE_CONTENT Frame with END 尚未 committed。下一卷應以 volume header 開始，之後建立同一 logical slice 的 continuation tape file，接續剩餘 payload range，或以 final SLICE_CONTENT Frame with END 完成該 logical slice；完成後才寫入 slice-level filemark。

## 5.1 Medium Header

The Medium Header is split into [docs/spec/01-medium-header.md](spec/01-medium-header.md).

A physical NeoTape medium MUST begin at BOT with a NeoTape Medium Header in tape file 0, followed by a filemark. This header is an immutable media initialization record. It is written when the medium is initialized and MUST NOT be treated as a mutable table of contents.

The Medium Header MAY span multiple tape records. It contains medium-level identity such as medium_uuid, initialization timestamp, medium label, format/default policy hints, and an ar metadata bundle. It MUST NOT record mutable media state such as archive lists, free space, used capacity, last archive UUID, last write timestamp, or authoritative append position.

After the mandatory Medium Header, a physical medium may contain one or more archive instances. Therefore, the layout begins as:

Tape file 0:
  NeoTape Medium Header
filemark

Tape file 1:
  NeoTape archive volume header for the first archive instance
filemark

Tape file 2..N:
  NeoTape logical slice tape files for the first archive instance; each slice tape file contains one or more Frame Headers and payload ranges; the last Frame Header carries END with slice verification fields

Final file for archive instance:
  NeoTape end header
filemark

Next tape file, if capacity remains:
  NeoTape archive volume header for another archive instance
...

## 5.2 Multiple Archives per Physical Tape

A physical LTO medium MAY contain more than one complete NeoTape archive instance. This is useful when an archive does not fill the remaining capacity of a tape. In this mode, each archive instance begins at a tape file boundary with its own NeoTape volume header, has its own archive_uuid, logical slices, Frame numbering, catalog, and end header, and is cleanly closed before the next archive instance begins.

The tape\_seq\_num field is scoped to a single archive\_uuid. Therefore, if two independent archives are written sequentially on the same physical medium, both may have tape\_seq\_num \= 1 for their first archive volume. Implementations SHOULD NOT treat tape\_seq\_num as a globally unique physical-medium file number.

## 5.3 Target Backends and Filesystem Spool Mode

NeoTape separates the logical archive format from the target backend used by a writer. A writer MAY write directly to a sequential tape device, or MAY write to an ordinary filesystem spool directory that represents the same archive as separated volume and tape-file objects.

The tape-device backend maps NeoTape volume headers, logical slices, and archive end records to physical LTO tape files separated by filemarks. The filesystem-spool backend maps each NeoTape tape file to a regular file in a deterministic directory layout. For slice tape files, the file contains one or more Frame Headers and payload ranges; the last Frame Header carries END with slice verification fields. The spool layout SHOULD preserve archive_uuid, volume_seq_num, tape_file_num, slice sequence numbers, Frame sequence numbers, and payload lengths in filenames or a small manifest so that tools can inspect and replay the archive without parsing every byte.

A recommended spool layout is:

archive-<archive_uuid>/
  tape-000001/
    tape-file-000000.medium-header.ntf
    tape-file-000001.volume-header.ntf
    tape-file-000002.slice-000001.ntf
    tape-file-000003.slice-000002.ntf
  tape-000002/
    tape-file-000001.volume-header.ntf
    ...

Filesystem spool mode MUST preserve the same logical record order as tape mode. It MUST NOT require a different reader algorithm for archive correctness. A reader MAY treat spool files as a virtual tape: file boundaries stand in for filemarks, and tape directories stand in for media or archive volumes. The same neotape-cat-volumes logical reader SHOULD be able to accept either a tape device path or a spool directory path, with the target adapter providing read-record, next-file, next-volume, and EOT/volume-limit events.

Because ordinary filesystems do not provide physical EOT, a spool writer MAY accept a manual or configured volume capacity limit such as --virtual-tape-size. When the next committed header, Frame payload, SLICE_METADATA Frame, or Archive End Header would exceed the configured volume capacity, the writer MUST perform the same logical transition it would perform on EOT: close the current tape at the last valid boundary, create the next tape directory, write its volume header, and continue or drop the incomplete SLICE_METADATA Frames; the final SLICE_CONTENT Frame with END has already been committed.

This manual capacity limit is a simulation of media capacity, not an archive semantic. It is useful for preparing archive volumes before the physical tape drive is available, testing multi-volume continuation, and staging several archives while another process is using the tape drive. A later copy-to-tape tool MAY replay the spool directory to a real tape backend, preserving filemark boundaries and tape directory ordering.

A spool archive SHOULD include a machine-readable manifest with at least archive_uuid, writer version, target backend, logical volume order, per-file sizes, BLAKE3 digests, declared volume_block_size values, whether drive hardware compression is expected during replay, and the configured virtual volume size. The manifest is advisory; restore correctness still comes from NeoTape headers, lengths, and checksums inside the spool files. A spool writer SHOULD track virtual volume limits in native input bytes unless an implementation explicitly models expected compressed occupancy as an advisory estimate.

# 6\. Archive Model

Archive 由多個 logical slices 依 slice_seq_num 排列而成：

Archive = LogicalSlice[1] + LogicalSlice[2] + ... + LogicalSlice[N]

每個 LogicalSlice 是由 writer 在串流過程中決定關閉的 payload byte range，最後一個 Frame Header 帶有 `END` flag：

LogicalSlice = payload_bytes + FrameHeader[END, slice_content_size, slice_content_blake3]

The actual slice_content_size is not known when the logical slice begins. It is recorded in the final Frame Header's slice_content_size and slice_content_blake3 fields.

NeoTape core does not require payload bytes to be pax, nor does it use payload-internal end markers for framing. A payload profile defines how those bytes should be interpreted. The NeoTape/PAX payload profile remains the recommended v0.1 backup profile because it preserves bsdtar/libarchive compatibility.

For NeoTape/PAX payload profile, neotape-cat-volumes MAY treat the length-framed slice payloads as ranges of one larger pax stream. On-tape logical slices do not need to be independently valid pax archives and do not need slice-local pax EOA markers. Any pax finalization needed for bsdtar compatibility is a profile-specific stdout policy, not the NeoTape core slice boundary rule.

因此，磁帶上的每個 slice 都可作為錯誤恢復與重新同步單位；下游工具是否需要知道 slice 的存在由 payload profile 決定。

# 7\. Frame Model

Logical slice 可由一個或多個 Frames 組成：

LogicalSlice[k] = Frame[k,1].payload + Frame[k,2].payload + ... + Frame[k,m].payload

每個 Frame 的 payload 長度由 Frame Header 的 frame_payload_size 明確宣告。Reader MUST read exactly frame_payload_size bytes for that Frame payload and MUST NOT inspect payload bytes to discover the Frame end. In v0.1, filemark normally remains the logical-slice tape-file boundary and seek boundary, while frame_payload_size is the authoritative framing field inside that slice tape file.

Slice target size remains a writer policy, commonly around 64 GiB or device/workload-specific. Frame target size SHOULD normally match the writer's bounded memory buffer size, such as 4 GiB, 8 GiB, or 16 GiB. Therefore, frame_payload_size is known before writing each Frame.

Logical slice completion is writer-declared at the final Frame. The writer does not need to know the final slice_content_size when the slice begins; it only needs to know each frame_payload_size before committing that Frame Header. When the writer decides to close the logical slice, it marks the final Frame with END and sets its slice_content_size and slice_content_blake3 fields to the authoritative values. Payload-internal markers, such as pax EOA, MUST NOT be used for NeoTape core framing, and on-tape logical slices do not need to contain pax EOA markers.

Frame Header 的 flags 可表示：

- START：此 Frame 是某個 content-type group 的第一段。
- END：此 Frame 是某個 content-type group 的最後一段。當 `frame_content_type = SLICE_CONTENT` 時，此 Frame Header 的 slice_content_size 與 slice_content_blake3 為 authoritative。Reader verifies slice-level BLAKE3 from these fields.

對 NeoTape/PAX payload profile，writer SHOULD preserve 512-byte tar record
alignment when practical. That alignment is a payload-profile rule, not a core
Frame Header flag.

# 8\. Header Types

NeoTape v0.1 定義四種 header type：

1. Medium Header
2. Volume Header
3. Frame Header
4. Archive End Header

Volume, Frame, and archive end fixed headers MUST fit within a single tape record and SHOULD occupy a single record at their commit point. Medium Header is the exception: it starts at BOT tape file 0 and MAY span multiple records, but its first record MUST contain a fixed binary/ASCII prefix sufficient to identify NeoTape and locate the medium metadata bundle.

Common header prefix fields：

- magic：例如 "NeoTape\0"
- header_version：v0.1 使用 1
- header_type：medium / volume / Frame / archive_end

All NeoTape fixed headers begin with this exact 10-byte common prefix. Header-specific fields follow after `header_type`. Every fixed header field area occupies exactly 1024 bytes, and each fixed header places its CRC32C field in the final 4 bytes.

Other common archive-time fields may include:

- header_crc32c
- archive_uuid
- archive_name
- payload_profile
- volume_write_at_utc
- volume_seq_num
- flags

Header 後方可內嵌 metadata bundle。Medium Header metadata bundle details are split into [docs/spec/01-medium-header.md](spec/01-medium-header.md). 固定 prefix 不得依賴 metadata bundle 才能辨識 header type。

Header fixed fields use CRC32C to catch accidental corruption and misreads with minimal parser complexity. Metadata bundles, catalog files, optional Frame payload hashes, logical-slice hashes, and archive-level manifests SHOULD use BLAKE3. SHA-256 MAY appear only as compatibility metadata if explicitly requested, but BLAKE3 is the preferred NeoTape integrity hash.

Common timestamp fields MUST use UTC and MUST be stored as a 20-byte NUL-terminated string using the exact `strftime` format `%Y-%m-%dT%H:%M:%S`, encoded as `YYYY-MM-DDTHH:MM:SS\0`. Writers MUST NOT use timezone suffixes, numeric offsets, fractional seconds, locale-specific text, RFC 3339 variants, ISO 8601 variants, or any other date format. A writer MAY also store a monotonic or implementation-specific timestamp in metadata bundle for diagnostics, but archive semantics should rely on UTC wall-clock timestamp only as descriptive metadata.

# 9\. Volume Header

Volume Header is split into [docs/spec/02-volume-header.md](spec/02-volume-header.md).

Volume header 位於每個 archive volume 的第一個 archive tape file。對已初始化的 physical medium 而言，它通常位於 Medium Header 之後的下一個 tape file；若同一 medium append 多個 archive instances，後續 archive instance 的 volume header 位於前一 archive clean end header 之後的下一個 tape file。

`volume_block_size` is the fixed NeoTape record size for this archive volume. It is not a recommendation. After the Volume Header is committed, the writer MUST use this `volume_block_size` for all NeoTape records in the same archive volume. 整體完成狀態由 Archive End Header 表示。

# 10\. Frame Header

Frame Header is split into [docs/spec/03-frame-header.md](spec/03-frame-header.md).

Frame Header 位於 slice tape file 內每個 Frame 的開頭 record。第一個 Frame Header 通常位於該 slice tape file 的第一個 NeoTape record；後續 Frame Header 由前一個 frame_payload_size 精確定位。

Frame Header MUST record frame_payload_size. The reader uses this length, not payload contents or filemark position, to determine the end of the Frame payload and the location of the next Frame Header inside the same slice tape file. Frame content type SHOULD distinguish SLICE_CONTENT from SLICE_METADATA.

# 11\. Logical Slice Completion

Slice-level integrity is defined in
[docs/spec/03-frame-header.md](spec/03-frame-header.md#slice-level-integrity).

The last SLICE_CONTENT Frame of a logical slice carries the `END` flag. Its
Frame Header records the authoritative `slice_content_size` and
`slice_content_blake3` for the entire logical slice.

`slice_content_blake3` is computed over exactly `slice_content_size` bytes of
concatenated payload from all SLICE_CONTENT Frames in the logical slice, in Frame
sequence order. SLICE_METADATA Frame bytes are NOT included in the
slice-level BLAKE3.

If SLICE_METADATA Frames are present, they follow the last SLICE_CONTENT Frame
and precede the slice-level filemark. These Frames carry advisory metadata,
warnings, source-read diagnostics, and other payload-profile metadata.

# 12\. Archive End Header

Archive End Header is split into [docs/spec/04-archive-end-header.md](spec/04-archive-end-header.md).

End header 是最後一個 cleanly completed archive record，位於最後一卷磁帶的最後一個 NeoTape tape file。它宣告整體 archive 已完整結束。

Because NeoTape core framing is length-based, Archive End Header does not depend on pax/tar EOA detection. Payload-profile-specific end markers, including pax EOA, remain inside payload bytes and are interpreted only by the relevant payload profile or downstream extractor.

若沒有讀到 Archive End Header，則 archive 不應被視為 cleanly complete，即使所有 expected logical slices 都已讀到。Archive-level completion 必須由 Archive End Header 判斷。

# 13\. Catalog

NeoTape catalog is an advisory byte index, not the authoritative filesystem metadata source. For NeoTape/PAX payload profile, authoritative restore metadata remains in pax entries. Catalog data exists to support fast listing, partial restore planning, audit, and salvage.

NeoTape v0.1 defines a compact binary-safe catalog entry format. A path catalog record is a NUL-terminated byte string:

/`<uid>`/`<gid>`/[source_dev_maj:min](source_dev_maj:min)/<source_inode>/<file_type>/<mode_octal>/`<size>`/`<mtime>`/<logical_slice_seq_num>/<frame_seq_num_within_slice>/<payload_offset>/<payload_size>/`<filepath>`\0

The number of slash-delimited fields before filepath is fixed by the catalog schema version. Parsers MUST split only the fixed number of leading fields; the remainder up to the terminating NUL is the filepath byte string. This works because POSIX-style path components cannot contain slash, and path strings cannot contain NUL. Therefore, filepath may contain ordinary directory separators without escaping. Also the forward slash right before the filepath is not part of the filepath itself.

For example:

/1000/1000/8:1/1234567/reg/0100644/4096/1710000000/12/3/987654321/4096/home/neo/file.txt\0

All numeric fields are ASCII decimal unless explicitly specified otherwise. mode_octal is ASCII octal. device fields use ASCII major:minor. file_type SHOULD use a small fixed token set such as reg, dir, symlink, hardlink, block, char, fifo, sock, or other. Unknown or unavailable values MUST be encoded as empty fields, not omitted, so the field count remains stable.

For device nodes, source_dev_maj:min identifies the filesystem device containing the inode; a separate optional rdev_maj:min extension field MAY be defined by a later catalog schema when device-node target identity is needed. source_inode is advisory and MAY be unavailable on filesystems without stable inode numbers. Hardlink grouping SHOULD use source_dev_maj:min plus source_inode when available.

Catalog records MUST NOT be interpreted as paths to restore from the metadata bundle itself. They are index entries only. Readers MUST validate path safety before using catalog data for partial restore selection, including rejection or special handling of absolute paths, parent-directory traversal, and policy-sensitive file types.

Catalog data MAY appear in two places:

1. Per-slice metadata inside SLICE_METADATA Frames. This describes payload ranges known to be contained in the logical slice and is useful for partial restore and salvage.
2. Final archive-level catalog data near the end of the archive, either inside the Archive End Header metadata area or, for NeoTape/PAX profile, optionally duplicated as payload entries such as:
   .neotape/catalog/catalog.ntc
   .neotape/catalog/catalog.ntc.zst
   .neotape/catalog/BLAKE3SUMS
   .neotape/FORMAT-SPEC.txt

The recommended uncompressed catalog media type is application/x-neotape-catalog-v1. Compression MAY be applied to the catalog payload as a metadata item attribute, but readers MUST know the uncompressed size and BLAKE3 digest from the surrounding metadata framing before trusting decompressed output.

Catalog remains advisory. If catalog and payload-profile metadata disagree, the payload-profile metadata wins for restore semantics.

# 14\. Writer Pipeline

推薦 writer pipeline：

source filesystem / file tree
  -> tree walker / metadata prefetcher
  -> planner / slice packer
  -> source reader profile
  -> reorder / batching / mbuffer-like memory buffer
  -> payload profile encoder, e.g. continuous libarchive pax writer
  -> NeoTape length-framed slicer / target backend writer
  -> target backend: LTO tape device or filesystem spool directory

Planner 可依檔案大小與讀取特性分配：

- 大檔案可由主 I/O path 順序讀取。
- 小檔案可由多個 worker threads 並行讀取。
- serializer 依 planner 決定的順序餵給 payload profile encoder。
- writer 可在 target slice size 附近透過調整小檔案順序，使每個 logical slice 大小接近目標。

Slice target size 作為 heuristic，並非 hard limit。若目前檔案大於 target 或大於單卷磁帶容量，logical slice 可超過 target，並透過 continuation 跨卷。

## 14.1 Source Reader Profiles

The NeoTape on-tape format is independent of the source filesystem. Source reading is an implementation concern of the writer. A conforming writer MAY provide multiple reader profiles:

* multi-threaded-small-file：多個 worker threads 並行讀取小檔案，適合 metadata-heavy 或 seek-limited workloads，例如 HDD-backed ZFS 小檔案樹
* single-reader-prefetch-metadata：單一 file reader 負責資料讀取，另一個 tree walker 提前掃描與 prefetch metadata，協助 slice packing；適合單一 sequential reader 已足以 saturate I/O bandwidth 的 filesystem，例如 XFS 或高速 NVMe-backed filesystem
* hybrid：大檔案走 sequential reader，小檔案走 bounded worker pool，serializer 仍依 planner 決定的順序餵入 payload profile encoder
* external-manifest：writer 從外部 manifest 或 file list 取得檔案集合與 metadata hints，再依可用資訊做 slice packing

A tree walker MAY run ahead of the serializer to collect path, type, size, mtime, directory structure, hardlink grouping hints, and other metadata required for planning. This metadata prefetch step is advisory; authoritative file metadata is still the metadata actually emitted by the selected payload profile encoder.

Reader profile selection MUST NOT affect the on-tape format. It only affects how efficiently the writer feeds the payload profile encoder and how well it can pack logical slices.

# 15\. Writer State Machine

Writer 主要狀態：

INIT
  建立 archive_uuid，初始化 writer options。

OPEN_VOLUME
  等待或開啟 target backend。對 tape backend，開啟或要求插入磁帶，寫入 volume header，寫 filemark。對 filesystem spool backend，建立下一個 volume directory 或 volume file sequence，寫入 volume header object。

OPEN_FRAME
  寫入 Frame Header。若開始新 content-type group，flags 包含 START；若關閉該 group，flags 包含 END。

  Frame Header is an atomic commit unit. If EOT/ENOSPC occurs before the complete Frame Header block is successfully written, the Frame MUST be treated as not created. The writer MUST open the next volume, write a new volume header if needed, and write the same Frame Header on the next tape. No continuation semantics are needed until at least one payload block of the Frame has been committed.

WRITE_FRAME_PAYLOAD
  接收 payload profile encoder output bytes，聚合成 target records 寫入。對 tape backend，target records 是 fixed-size tape blocks；對 filesystem spool backend，target records MUST preserve the same fixed volume_block_size semantics, either as record-framed objects or as regular files with an explicit manifest describing record boundaries。成功寫入完整 target record 後才視為 commit。

EOT_DETECTED_OR_VOLUME_LIMIT
  若 tape writer 遇到 ENOSPC/EOM/EOT，或 filesystem spool backend 達到手動設定的 virtual volume capacity，不應直接讓 payload profile encoder 視為不可恢復的 archive error。transport layer 應暫停或切換 backend volume、寫新 volume header，然後以同一 logical_slice_seq_num 的下一個完整 Frame 繼續同一 logical slice 的 payload byte range。

CLOSE_LOGICAL_SLICE
  當 buffered payload bytes 達到 Frame target size，writer closes the current Frame by writing exactly frame_payload_size bytes. 當 writer 決定 current logical slice 應結束時，該 Frame 會成為 final Frame and carries END. For NeoTape/PAX payload profile, the writer MAY choose slice boundaries at pax member boundaries, but NeoTape core does not require pax EOA to determine the boundary.

FINALIZE_LOGICAL_SLICE
   在 writer 決定關閉 current logical slice，並寫完該 slice 的 final Frame 後，該 Frame Header 帶有 `END` flag 並記錄 slice_content_size 與 slice_content_blake3。The actual slice_content_size is known only at this point and is recorded in the final Frame Header. Optionally, the writer may add zero or more SLICE_METADATA Frames after the final SLICE_CONTENT Frame. After all Frames are committed, the writer SHOULD write a filemark to close the slice tape file. If EOT occurs before the END Frame Header is committed, the next volume MUST write the next complete Frame for the same logical_slice_seq_num.

WRITE_CATALOG
  Writer SHOULD store per-slice metadata in SLICE_METADATA Frames when useful for partial restore or salvage. The final archive-level catalog MAY also be duplicated in the final logical slice as pax entries and/or in the Archive End Header metadata bundle.

WRITE_END_HEADER
  所有 logical slices 完成後，寫入 Archive End Header，宣告 clean_end。

DONE
  正常結束。

ERROR
  在無法恢復的 target backend I/O error、metadata mismatch、source read error 或 payload profile fatal error 時進入。

# 16\. Reader / neotape-cat-volumes Model

neotape-cat-volumes 是最小還原工具。它的職責：

* 讀取 volume header
* 在使用者策略允許時，若 volume header 損壞、UUID 不符或 volume_seq_num 不符，掃描後續 tape files 尋找下一個候選 volume header
* 驗證 archive_uuid 與 volume_seq_num
* 依 filemark 讀取 volume、slice、archive-end tape files
* 驗證 Frame Header 中的 logical_slice_seq_num 與 frame_seq_num_within_slice
* 將同一 logical slice 的 Frame payloads 串接
* 依 frame_payload_size 讀取每個 Frame payload；MUST NOT inspect payload bytes to discover Frame boundaries
* 在讀到帶有 END 的 final Frame 後，從 Frame Header 的 slice_content_size 與 slice_content_blake3 驗證該 logical slice
* 對 NeoTape/PAX payload profile，直接依 profile policy 輸出 payload bytes；core reader 不需要偵測或抑制 pax EOA
* 讀到 Archive End Header 時，依 payload profile 完成 stdout 輸出，並以 exit code 0 結束
* 將 stdout 保持為純 payload bytes；對 NeoTape/PAX payload profile，stdout 是 pax bytes；所有 prompt/log 都不得污染 stdout

推薦用法：

# NeoTape/PAX payload profile example:

  neotape-cat-volumes /dev/nst0 | bsdtar -xpf - --acls --xattrs

若要先落地檢查：

  neotape-cat-volumes /dev/nst0 > archive.pax
  bsdtar \-tvf archive.pax

# 17\. Reader State Machine

Reader 主要狀態：

START
  初始化 expected archive_uuid（若未指定，從第一卷讀取），expected_volume_seq_num = 1，expected_slice_seq_num = 1。

READ_VOLUME_HEADER
  讀取目前 archive volume 的 volume header。第一個 archive instance 通常位於 Medium Header 之後；若從 BOT 掃描，reader MUST skip Medium Header and scan for the requested archive_uuid。若 volume header checksum/header_type invalid、UUID 不符或 seq 不符，reader MUST NOT emit payload bytes from that candidate volume. It SHOULD enter MISMATCH_HANDLER, where policy may fail, prompt, or scan forward by filemarks to the next candidate Volume Header. This scan-forward option is important when a physical medium contains several independent archive instances and the inserted medium may be positioned near the tail of a different archive.

READ_NEXT_TAPE_FILE
  讀取下一個 tape file 的第一個 block，判斷 header_type。For a logical slice tape file, the first NeoTape record is normally a Frame Header; subsequent Frame Headers inside the same slice are located by frame_payload_size rather than by filemark.

READ_FRAME_HEADER
  驗證 Frame 屬於目前 archive，且 slice/Frame seq 符合預期。

STREAM_FRAME_PAYLOAD
  依 frame_payload_size 讀取 exactly that many payload bytes。Reader MUST NOT parse payload bytes to determine Frame or slice boundaries.

SLICE_CONTENT_COMPLETE
   已讀完帶有 END 的 final Frame。從此 Frame Header 取得 slice_content_size 與 slice_content_blake3。Reader MUST verify slice_content_blake3 over the concatenated SLICE_CONTENT Frame payloads before treating the logical slice as NeoTape-complete. 若存在 SLICE_METADATA Frames，在 BLAKE3 驗證之後繼續讀取。SLICE_METADATA 為 advisory，其 BLAKE3 驗證失敗不影響 slice 完整性判斷。

READ_END_HEADER
  驗證 clean_end。Archive End Header resides in its own archive-end tape file, delimited by filemarks like volume headers and completed logical slices. 依 payload profile 決定 stdout finalization policy，exit 0。

EOT_BEFORE_END
  若在尚未讀到 end header 前遇到 physical EOT，提示插入下一卷，然後回到 READ_VOLUME_HEADER。

ERROR_HANDLER
  處理 read error、UUID mismatch、seq mismatch、header checksum error、slice incomplete 等。

# 18\. Error Handling

NeoTape 建議採用 Retry / Inspect / Fail / Force-Salvage 模型。

* Volume header mismatch or corruption：
* 預設不允許輸出 payload bytes。
* 互動模式可提供 Retry、Inspect、ScanNextVolumeHeader、Fail。
* ScanNextVolumeHeader MUST advance by tape-file/filemark boundaries and only accept a candidate whose header magic, type, size, checksum, archive_uuid, and expected volume_seq_num all validate.
* This option is especially useful on media containing multiple archive instances, where the wrong candidate may be the tail or clean end area of another archive.
* Force 僅在 salvage mode 且使用者明確輸入完整詞時提供；Force MUST mark stdout/output as not fully verified.
* UUID mismatch：
* 預設不允許繼續。
* 互動模式可提供 Retry、Inspect、ScanNextVolumeHeader、Fail。
* Force 僅在 salvage mode 且使用者明確輸入完整詞時提供。

Tape sequence mismatch：

* 預設提示插入正確卷。
* 若讀到更高 seq，可能代表缺卷。
* 若讀到更低/同 seq，可能代表插錯卷或倒帶。

Frame sequence mismatch：

* 預設 fail 或 prompt retry。
* 若 salvage mode 開啟，可嘗試在目前 slice tape file 內依 length-framed Frame Headers 重新同步；若無法可靠同步，則 seek 到下一個 slice-level filemark，尋找下一個 slice tape file 的 Frame Header。

Read error：

* 自動 retry N 次。
* 仍失敗則 prompt Retry / Inspect / Fail。
* Skip damaged block 僅在 salvage mode 提供，並應明確警告會破壞 pax stream。
* EOT before Frame or archive end header：
* 若 Frame Header 已 committed 但實際讀到或寫入的 payload bytes 少於 frame_payload_size，則該 Frame is incomplete。下一卷必須以同一 logical slice 的 continuation Frame 接續剩餘 payload range 或重新宣告下一段 payload range。
* 若 writer 已寫完某 logical slice 的 final Frame 但 END Frame Header 尚未 committed，則 archive 尚未 cleanly complete；下一卷必須以 final SLICE_CONTENT Frame with END 完成該 logical slice 後才寫入 slice-level filemark。

\- 若已讀到 final SLICE_CONTENT Frame with END 但 slice-level filemark 或 Archive End Header 尚未讀到，則 archive 尚未 cleanly complete；下一個 tape file 可能包含下一個 logical slice 的 first Frame Header，或直接包含 Archive End Header。

# 19\. Control Plane

neotape-cat-volumes 的 stdout 必須只輸出 payload bytes；對 NeoTape/PAX payload profile，stdout 是 pax bytes。互動控制不得使用 stdout。

LTO 磁帶機不一定有穩定可用的磁帶 eject / insert 偵測功能，這部分還得繼續研究。

推薦分離：

* stdout：資料平面，payload bytes only；對 NeoTape/PAX payload profile，stdout is pax bytes
* stderr：log/progress/warnings
* /dev/tty：互動 prompt、換帶、Retry/Inspect/Fail

控制模式：

* --control=auto：若可用則使用 tty，否則依 policy
* --control=tty：必須能開 /dev/tty
* --control=none：完全不互動，適用 cron/systemd/CI/robot changer

錯誤策略：

* --on-mismatch=prompt|fail|scan-next-volume-header
* --on-volume-header-error=prompt|fail|scan-next-volume-header
* --on-eot=prompt|fail
* --retry=N
* \--salvage

# 20\. Compatibility with pax / bsdtar / libarchive

NeoTape 不定義檔案 metadata 表示法。Writer 應使用 libarchive 的 pax writer，以取得對 UID/GID、xattrs、ACL、symlink、hardlink、device node、long path 與其他 POSIX metadata 的支援。Reader 端應能將 NeoTape 還原為標準 pax stream，交給 bsdtar 或其他 pax-compatible extractor。

NeoTape core framing does not depend on pax/tar EOA detection. Frame and slice boundaries are determined by NeoTape length fields, not by parsing the payload format.

The NeoTape/PAX payload profile is still recommended for v0.1 backup interoperability. In this profile, on-tape logical slices do not need to be independently valid pax archives and do not need to contain slice-local pax EOA markers. A writer MAY store a continuous pax byte stream split into length-framed NeoTape slices. A profile-aware output tool MAY append or preserve the final pax EOA required by downstream bsdtar, but this is a pax-profile output policy, not core container framing.

Because slice boundaries are length-framed, a minimal NeoTape reader can emit payload bytes without parsing pax headers or performing EOA suppression.

# 21\. Recovery Considerations

Slice/Frame 設計提供以下恢復能力：

* 可使用 LTO filemark seek 到特定 logical slice。
* 若某個 Frame 壞掉，可嘗試重讀同一 slice tape file，並依 frame_payload_size 重新定位 slice 內部 Frame boundaries。
* 若 slice 內部無法可靠重新同步，可在 salvage mode 跳到下一個 slice-level filemark，尋找下一個 logical slice 的第一個 Frame Header。
* 若某個 logical slice 壞掉，其他 logical slices 仍可能可讀。
* Catalog 可用於定位檔案所在 logical slice，協助 partial restore。

Medium Header 提供的恢復能力：

* 每卷 volume header metadata bundle 可內嵌 neotape-cat-volumes source code 與格式說明，提升長期可恢復性。
* \- END Frame Headers provide per-slice BLAKE3 verification. SLICE_METADATA Frames may carry optional slice-local metadata, allowing fast audit, salvage, and partial restore planning without treating catalog data as authoritative archive metadata.
* 
* # 22\. Security Considerations

NeoTape reader 不應信任 metadata bundle 內的 member name、size、mode、timestamp 或 hash。Header metadata bundle is a restricted ar archive and reader SHOULD treat it as a flat collection of named byte blobs, not as a filesystem archive. It MUST NOT interpret member names as paths, MUST reject absolute paths and parent-directory components if any extended name syntax is supported, and MUST NOT restore ownership, permissions, device nodes, symlinks, hardlinks, xattrs, ACLs, or executable bits from header metadata bundles. Catalog 只是索引，不是權威 metadata。實際還原時仍需依 bsdtar/libarchive 的安全選項控制 absolute paths、.. path components、device nodes、ownership restore、ACL/xattr restore 等風險。

若 archive 包含可執行 restore helper binary，reader 不應自動執行。Source code 與 spec 比 binary 更適合作為長期保存資料。

BLAKE3 is used for integrity verification, not as an authentication mechanism. If authenticity or tamper resistance is required, NeoTape SHOULD add signatures or keyed authentication metadata over the relevant BLAKE3 digests in a future extension or deployment profile.

# 23\. Future Extensions

未來可考慮：

- LTO partition metadata mode
- Per-Frame hash chain
- Per-slice Merkle tree
- Partial restore index
- Filesystem-native payload profiles for ZFS send streams and Btrfs send streams. In these profiles, each dataset, subvolume, or snapshot send stream is modeled as a payload sub-stream that normally maps to one or more NeoTape logical slices. A logical slice SHOULD NOT contain bytes from more than one filesystem-native sub-stream unless explicitly allowed by that payload profile.
- Filesystem-native stream catalogs may record dataset/subvolume identity, snapshot names, parent snapshot dependencies, receive order, stream-level checksums, and the logical slice range containing each sub-stream. NeoTape core remains payload-format agnostic and does not parse or interpret ZFS/Btrfs stream semantics; restore semantics are handled by the relevant receive tool or payload profile implementation.
- Changer/robot integration
- Machine-readable JSON control protocol
- Reed-Solomon or parity Frames
- Encryption and key metadata integration
- Multiple catalog replicas across volumes
- Multi-archive medium index for quickly listing all archive instances stored sequentially on one physical tape.
- Optional low-level SCSI passthrough profiles for diagnostics only; normal operation should use the standard sequential tape device interface.

# Appendix A. Example Multi-Volume Layout

Tape 1:

File 0: Medium Header, possibly multiple records, immutable media bootstrap
File 1: Volume header, archive_uuid = A, volume_seq_num = 1
File 2: Slice 1 tape file: Frame Header slice = 1 Frame = 1, payload = first 8 GiB; Frame Header slice = 1 Frame = 2 (`END`), payload = rest of slice 1, slice_content_size = 64 GiB, slice_content_blake3 = ...
File 3: Slice 2 tape file: Frame Header slice = 2 Frame = 1, frame_payload_size = 8 GiB; payload = first part of slice 2; EOT before slice complete

Tape 2:

File 0: Medium Header, possibly multiple records, immutable media bootstrap for this physical medium
File 1: Volume header, archive_uuid = A, volume_seq_num = 2
File 2: Continuation of slice 2 tape file: Frame Header slice = 2 Frame = 2 (`END`), frame_payload_size = remaining bytes of slice 2; payload = rest of slice 2, slice_content_size = 64 GiB, slice_content_blake3 = ..., optional SLICE_METADATA Frames
File 3: Slice 3 tape file: Frame Header slice = 3 Frame = 1 (`END`), frame_payload_size = complete slice 3 size; payload = complete final slice payload bytes, slice_content_size = ..., slice_content_blake3 = ..., optional SLICE_METADATA Frames
File 4: Archive End Header, clean_end = true, last_slice_seq_num = 3

neotape-cat-volumes stdout for NeoTape/PAX payload profile:

slice 1 payload bytes
slice 2 payload bytes
slice 3 payload bytes

No NeoTape header bytes (Frame Headers, SLICE_METADATA) are emitted to stdout. On-tape logical slices do not need to contain pax EOA markers; pax stream finalization, if needed, is a NeoTape/PAX payload profile output policy.

# Appendix B. Minimal neotape-cat-volumes Responsibilities

A minimal implementation may avoid libarchive entirely. It only needs:

- standard tape device open/read/write operations
- NeoTape header parser
- CRC32C header verification and BLAKE3 verification for END Frame Headers, metadata bundles, catalogs, and optional payload hashes
- filemark/EOT handling
- Frame sequencing logic
- length-framed Frame and slice reader
- stdout byte stream output
- /dev/tty prompt support

A minimal implementation may avoid libarchive and does not need to parse pax
headers, understand filenames, restore files, interpret ACLs, or handle xattrs.
It follows NeoTape Frame length fields, reads `slice_content_size` and
`slice_content_blake3` from the final `SLICE_CONTENT` Frame Header carrying
`END`, verifies the slice-level BLAKE3, and emits payload bytes according to the
selected payload profile.

# Appendix C. Draft CLI

Restore NeoTape/PAX payload profile interactively:
  neotape-cat-volumes --payload-profile=pax --control=auto /dev/nst0 | bsdtar -xpf - --acls --xattrs

Restore without interaction:
  neotape-cat-volumes --control=none --on-eot=fail --on-mismatch=fail /dev/nst0 > payload.out

Create filesystem spool output with a virtual volume size:
  neotape-write --target=spool -o ./archive.spool --virtual-tape-size=12T /source/tree

Replay prepared spool to tape:
  neotape-spool-to-tape ./archive.spool /dev/nst0

Inspect volume metadata:
  neotape-cat-volumes --inspect-volume /dev/nst0

Salvage mode:
  neotape-cat-volumes \--salvage \--control=tty /dev/nst0 \> salvage.out

# Appendix D. Open Questions

1. Header binary layout exact byte offsets.
2. CLOSED: Fixed archive-time headers use CRC32C for fast corruption detection. All non-header integrity hashes SHOULD use BLAKE3 unless a future revision defines a specific exception.
3. Whether Frame payload should always begin at 512-byte tar record boundary.
4. CLOSED: Medium Header metadata bundles MUST use a restricted classic ar archive as the top-level container. The ar container itself SHOULD NOT be compressed, though individual members such as a minimal reader source package MAY be compressed.
5. How much catalog duplication should be required across final pax slice and end header.
6. CLOSED: If EOT occurs before a Frame Header is fully committed, the Frame MUST be considered not created. The writer MUST open the next volume and write the same Frame Header there.
7. Precise mapping from standard SCSI sequential-access semantics to portable user-space operations: write/read fixed-size records, write filemark, space filemarks, rewind/offline, detect EOT/EOD, configure drive hardware compression, and report native vs compressed physical occupancy when available.
8. Recommended default target logical slice size and override policy: 16 GiB, 32 GiB, 64 GiB, or device/workload-specific.
9. CLOSED: A physical medium MUST begin with an immutable NeoTape Medium Header. This header is not a mutable table of contents and MUST NOT be used to record the list of archives later appended to the medium. Its role is media initialization, format bootstrap, default tape parameters, and long-term self-description.
10. CLOSED: NeoTape v0.1 targets LTO-class tape media only. Legacy non-LTO tape formats are out of scope.
11. CLOSED: Medium Header records may span multiple tape records because they are written at BOT during initialization. Volume, Frame, and archive end fixed headers remain single-record commit units.
12. SUPERSEDED: Slice-level integrity fields are now recorded in the final `SLICE_CONTENT` Frame Header carrying `END`. The `slice_content_size` and `slice_content_blake3` fields are authoritative for each logical slice. `SLICE_METADATA` Frames may carry optional slice-local metadata.
13. SUPERSEDED: Slice-local pax EOA detection is no longer required for NeoTape framing. Slice and Frame boundaries are length-framed by NeoTape headers; pax EOA is payload-profile data only.
14. CLOSED: Frame Headers MUST carry explicit frame_payload_size, and the END Frame Header MUST carry slice_content_size and slice_content_blake3. NeoTape core framing is therefore payload-format agnostic.

End of Draft Spec v0.1

---
