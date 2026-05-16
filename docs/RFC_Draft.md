# `NeoTape：面向 LTO 磁帶的可定位多卷 Length-Framed Payload Transport Format`

`RFC Draft v0.1`

## `Status of This Memo`

`本文件是一份設計草案，而非既有標準。它描述一種暫稱為 NeoTape 的 multi-volume length-framed payload transport format，用於將 payload byte stream 可靠地寫入 LTO 磁帶。v0.1 推薦 POSIX pax/tar 作為初始 payload profile，使未修改的 bsdtar / libarchive 能夠還原資料。`

`本草案的核心假設是：NeoTape core 應承擔 volume、filemark、slice、segment、continuation、catalog、checksum 與錯誤恢復等 transport semantics；payload archive semantics 則由 payload profile 承擔。v0.1 推薦 NeoTape/PAX profile，但 core framing 不依賴 pax/tar EOA。`

# `Abstract`

`NeoTape 是一種針對 LTO 磁帶設計的 seekable multi-volume length-framed payload transport container。它將整體備份切分為多個 logical slices；每個 logical slice 是 writer 在串流過程中決定關閉的一段 payload bytes，後接 NeoTape Slice Trailer，trailer 內記錄實際 slice_payload_size 與 slice_payload_blake3。每個 logical slice 對應一個主要 LTO tape file，內含一個或多個 length-framed physical segments；每個 segment 前置 NeoTape segment header，其中明確記錄 segment_payload_size，之後接續該 segment 的 payload byte range。filemark 位於 slice boundary，而不是每個 segment boundary，因此磁帶機原生 file seek 能力定位到可完整驗證的 slice，同時避免在整卷磁帶上建立過多 filemarks。`

`NeoTape 的讀取工具 neotape-cat-volumes 會讀取多卷磁帶、驗證 volume / segment / slice trailer headers、處理 End of Tape continuation，並依 payload profile 將多個 logical slices 重組輸出至 stdout。Minimal reader 只需要依 length fields 串接 payload bytes；對 NeoTape/PAX payload profile，下游可以直接使用 bsdtar -xpf - 還原。`

# `1. Introduction`

`本設計的主要使用情境是將一般 POSIX-like filesystem 或檔案樹備份到 LTO 磁帶。ZFS RAID-Z2 HDD 陣列只是 motivating example：在大量小檔案情境下，單一 I/O thread 可能無法穩定餵滿 LTO 原生寫入速率，因此 writer 可使用多 I/O threads、metadata prefetch、類 mbuffer 記憶體緩衝功能、libarchive pax writer、NeoTape target backend 與換卷流程整合為一個 pipeline。`

`NeoTape format itself is filesystem-agnostic. The writer MAY use different source reader profiles depending on the source filesystem and workload. For example, ZFS small-file workloads may benefit from multiple concurrent file reader threads, while an XFS filesystem or fast NVMe-backed filesystem may saturate available I/O bandwidth with a single file reader plus a tree walker thread that prefetches metadata ahead of serialization for slice packing decisions.`

`同時，傳統單一超長 tar/pax 串流在磁帶上有幾個問題：`

* `中間錯誤可能導致後續資料難以重新同步`  
* `單一 archive 若橫跨多卷磁帶，需要明確的換卷邏輯`  
* `若單一檔案大於單卷 LTO 容量，archive stream 必須能在檔案內容中間跨卷 continuation。`  
* `LTO 並非 block device；它不能任意 seek 到 byte offset，但可以利用 filemark 快速跳到 tape file boundary。`  
* `archive 外層不宜使用需要尾端 trailer 或回填的 wrapper，因為 EOT 位置不可可靠預測`

`NeoTape 因此採用 LTO 原生分隔能力：每個 archive volume 有 volume header；每個 logical slice 是一個主要 tape file；slice tape file 內可包含多個 physical segments，每個 segment 由 segment header 與 length-framed payload byte range 組成；logical slice 完成後在同一 slice tape file 內寫入 Slice Trailer，記錄 size、BLAKE3 與可選 slice-local catalog；slice tape file 完成後寫入 filemark。最後以 Archive End Header 宣告整體 archive cleanly complete。`

# `2. Terminology`

## `Logical Hierarchy:`

`Archive`

`一整組 NeoTape 備份，由一卷或多卷 LTO 磁帶組成。Archive 由 archive_uuid 識別。`

`Volume`

`Archive volume 是某個 archive instance 在一卷 physical cartridge 上的部分。它有 tape_seq_num，從 1 起算。其 volume header 位於該 archive instance 在該 cartridge 上的第一個 archive tape file；若從 BOT 初始化，tape file 0 是 cartridge header，第一個 archive volume header 通常位於 tape file 1。`

`Tape File`

`由 LTO filemark 分隔出的磁帶邏輯檔案。NeoTape v0.1 使用 tape file 作為 volume header、logical slice、archive end 等 slice-level 或 archive-level 可 seek 物理 boundary；physical segment 通常不是獨立 tape file。`

`Logical Slice`

`一段 length-framed payload byte range。每個 logical slice 由一個或多個 physical segments 組成，總長度由 Slice Trailer 的 slice_payload_size 驗證。Payload 可以是 NeoTape/PAX profile、raw byte stream 或未來定義的其他 profile。`

`Physical Segment`

`logical slice tape file 內的一個 NeoTape segment。它包含一個 segment header block 與後續 payload blocks。Physical segment 是某個 logical slice 的連續 byte range。單一 Logical Slice 可以由多個 physical segments 組成；segments 之間以 explicit length fields 串接，通常不以 LTO filemark 分隔，並可在 EOT 時跨越多卷磁帶。`

`Continuation`

`當 logical slice 尚未完成但目前 volume 遇到 EOT / ENOSPC 時，下一卷磁帶可建立同一 logical slice 的下一個 physical segment。Continuation segment 不是新的 logical slice，而是前一 segment 的 byte-exact continuation。`

`EOA`

`End of Archive。對 tar/pax 而言，通常是至少兩個 512-byte zero records。In NeoTape core, EOA is payload-profile data only and is never used as a slice or segment boundary. On-tape logical slices do not need slice-local EOA markers.`  
`Slice Trailer`  
`每個 logical slice 的 payload bytes 完成後必須接續的 NeoTape transport metadata record。Slice Trailer 記錄 slice_payload_size、slice_payload_blake3、可選 slice-local catalog、payload profile 與其他 advisory metadata。它不是 payload stream 的一部分，reader MUST NOT output it to stdout。Slice Trailer 的邏輯位置由跨 segments / cartridges 累積的 payload length 決定；若 EOT 發生在 payload 完成後但 trailer commit 前，Slice Trailer 會位於下一卷 cartridge。`

## `NeoTape header`

`Physical cartridge`

`一組實體 LTO 磁帶匣。每個 physical cartridge 可以包含零個、一個或多個完整 NeoTape archive instances，依序以 tape files 排列。`

`Archive instance`

`一個由 archive_uuid 識別的完整 NeoTape 備份實例。Archive instance 可以佔用一卷或多卷 physical cartridges，也可以與其他 archive instances 共用同一卷 physical cartridge 的不同 tape file range。`

`Cartridge Header:`

`Physical cartridge 起始處的 mandatory immutable media header。它描述卡帶層級的格式、初始化資訊、預設 tape block size、format version、restore instructions、cartridge name 以及 neotape-cat-volumes source bundle。它不是 archive table of contents，也不能作為後續 append archive 的可變索引。`  
`NeoTape 定義的固定格式 metadata header，用於 volume、segment、slice trailer 與 archive end record。NeoTape header 不屬於 payload stream，不會輸出給下游 extractor。`

# 3\. Design Goals

`NeoTape 的設計目標如下：`

1. `NeoTape/PAX payload profile compatibility：v0.1 推薦的 NeoTape/PAX payload profile 應與 libarchive/bsdtar 的 POSIX pax 格式相容。`  
2. `Length-framed payload container：NeoTape core is payload-format agnostic. Segment and slice boundaries are determined by explicit length fields in NeoTape headers and Slice Trailers, not by parsing pax/tar EOA or any payload-internal marker.`  
3. `Restore simplicity：最小還原工具 neotape-cat-volumes 只負責磁帶 transport、multi-volume sequencing 與 length-framed payload concatenation；payload 解讀由 payload profile 或下游工具處理。`  
4. `LTO-native seekability：使用 LTO filemark 作為 volume、logical slice、archive end 等 coarse-grained boundary，使磁帶能快速 seek 到完整可驗證的 slice，而不是為每個 internal segment 建立 filemark。`  
5. `Target backend abstraction：NeoTape writer SHOULD support multiple backing stores for the same logical format, including direct sequential tape device output and ordinary filesystem spool output. Filesystem spool output is useful for debugging, test fixtures, offline archive preparation, staging while the tape drive is busy, and deterministic reproduction of volume/segment layout.`  
6. `Multi-volume continuation：支援 logical slice 與單一檔案跨卷延續，不受單卷磁帶容量限制。`  
7. `No mandatory trailer rewrite：不要求回到磁帶開頭或中間回填 metadata；不依賴 EOT 前仍有空間可寫 trailer。`  
8. `Streaming writer：slice 是邏輯概念，不要求 writer 先將整個 slice spool 到本機檔案或 RAM。`  
9. `Error containment：錯誤應盡量限制在單一 logical slice 內；後續 slice 應能透過 slice-level filemark 與下一個 slice tape file 的 segment header 重新同步。Segment headers remain useful for length framing and diagnostics inside a slice, but they are not normally separate filemark seek points.`  
10. `Long-term recoverability：每卷可在 header metadata bundle 中內嵌格式說明、restore 工具 source code、README、catalog 摘要與檢查碼。`  
11. `Multi-archive media use：若單一 archive 無法填滿整卷磁帶，格式應允許在同一 physical cartridge 上順序寫入多個完整 archive instances，且每個 archive 仍保持自己的 archive_uuid 與 clean end header。`  
12. `Media self-description：每張 physical cartridge 必須在 BOT 有 immutable cartridge header，用於保存 format version、block size、格式說明與 minimal reader source code，使磁帶離開外部 database 後仍可自我描述。`  
13. `Filesystem-agnostic operation：NeoTape 格式不得假設 source filesystem 必須是 ZFS。Writer 應允許依 filesystem 與 workload 選擇 reader profile，例如 multi-threaded small-file reader、single sequential reader、metadata-prefetch walker 或混合模式。`  
14. `Single-record archive-time header commitment：volume、segment、slice trailer、archive end 等可能在接近 EOT 時寫入的 fixed headers 必須能放入單一 tape record；cartridge header 是例外，因為它只在 BOT 初始化時寫入，可由多個 records 組成。`

# 4\. Non-Goals

`NeoTape v0.1 不嘗試：`

* `定義新的檔案封存語義。UID/GID、xattrs、ACL、hardlink、symlink、device node、sparse file 等由 pax/libarchive 表示。`  
* `取代 bsdtar、GNU tar 或 libarchive。`  
* `在每個 segment 或 logical slice 內保證可獨立解出檔案。NeoTape core 只保證 length-framed payload transport；payload 是否可獨立解讀由 payload profile 決定。`  
* `要求使用 LTO partition。metadata partition 可作為未來擴充，但 v0.1 使用 tape file 與 filemark 即可。`  
* `要求 OS-specific raw SCSI API。一般 operation 應只依賴標準 sequential tape device 行為；raw SCSI passthrough 僅可作為診斷或進階工具的 optional extension。`  
* `支援 legacy non-LTO tape media。NeoTape v0.1 以 LTO-class media 為目標，避免為小 record-size 的過時媒體降低 archive-time header 的 single-record commit requirement。`  
* `定義所有 payload formats 的語義。NeoTape core is payload-format agnostic, but each payload profile must define its own interpretation and stdout behavior. v0.1 recommends a NeoTape/PAX payload profile for backup compatibility.`  
* `要求 writer 必須直接寫入實體磁帶裝置。Conforming writers MAY write the same NeoTape volume/segment/header layout to an ordinary filesystem spool directory, as long as the resulting files preserve the same logical sequence, lengths, checksums, and volume transition semantics.`  
* `要求 source filesystem 是 ZFS。ZFS 可作為一致性 snapshot 與小檔案多執行緒讀取最佳化的典型案例，但格式與 reader/writer pipeline 應適用於 XFS、UFS、ext4、NFS-mounted trees、object-staged file trees 或其他可由使用者空間列舉與讀取的來源。`

# 5\. Tape Model

`NeoTape assumes modern LTO tape drives accessed through the standard SCSI sequential-access tape device model. It is intentionally not a generic legacy tape format for DDS/DAT/QIC-era devices with small maximum record sizes. LTO remains the practical target because it is the modern tape ecosystem still actively used for backup and archival storage, with current generations and public roadmaps continuing beyond earlier generations.`

`NeoTape does not require raw SCSI passthrough for normal operation. It only requires the operating system's standard tape device interface for open/read/write, writing filemarks, spacing filemarks, rewind/offline, detecting EOT/EOD, and setting or using variable block mode. The precise mapping of those operations is implementation-specific, but the on-tape format is not tied to a private OS or vendor API.`

`NeoTape v0.1 requires tape record sizes large enough to hold every archive-time fixed header as a single record. Implementations MUST support at least 256 KiB tape records for volume, segment, slice trailer, and archive end header records. Each archive volume MUST declare a fixed block_size in its Volume Header, and all NeoTape records in that archive volume MUST use that block size unless a future profile explicitly defines a compatible record-framing exception. The cartridge header is excluded from this archive-volume block_size rule because it is written at BOT during cartridge initialization and may span multiple records. block_size SHOULD be a positive multiple of 512 bytes for NeoTape/PAX payload profile compatibility. 8 MiB is the recommended default for high-throughput LTO operation when supported, but the chosen value is fixed per archive volume after the Volume Header is committed.`

`A large fixed block_size also helps the target backend preserve long contiguous byte ranges for the tape drive. When drive hardware compression is enabled, larger records reduce artificial fragmentation introduced by the NeoTape transport layer and give the drive a better opportunity to compress the payload stream according to its own internal compression model. NeoTape MUST NOT assume any particular compression ratio, and catalog or capacity planning MUST distinguish native payload bytes from drive-compressed physical occupancy.`

`A physical cartridge may contain multiple archive instances. The example below shows a single archive instance; in multi-archive mode, the next archive may begin at the tape file immediately after a clean NeoTape end header.`

`NeoTape v0.1 uses slice-level filemark granularity. Segment-level filemarks are not recommended for normal archives because they can create hundreds or thousands of tape files on a large LTO cartridge, while only a completed logical slice has an authoritative Slice Trailer and slice_payload_blake3. Therefore, filemarks SHOULD delimit volume headers, completed logical slices, and archive end records. Physical segments SHOULD be length-framed records inside a slice tape file unless a future profile or explicit diagnostic mode opts into finer-grained filemarks.`

`NeoTape 使用 filemark 作為 seekable boundary。典型磁帶布局如下：`

`Tape file 0:`  
  `NeoTape cartridge header`  
`filemark`  
`Tape file 1:`  
  `NeoTape archive volume header`  
`filemark`  
`Tape file 2:`  
  `NeoTape segment header for slice 1 segment 1`  
  `payload bytes for slice 1 segment 1`  
  `NeoTape segment header for slice 1 segment 2, if needed`  
  `payload bytes for slice 1 segment 2`  
  `NeoTape Slice Trailer for logical slice 1`  
`filemark`  
`Tape file 3:`  
  `NeoTape segment header for slice 2 segment 1`  
  `payload bytes for slice 2 segment 1`  
  `NeoTape segment header for slice 2 segment 2, if needed`  
  `payload bytes for slice 2 segment 2`  
  `NeoTape Slice Trailer for logical slice 2`  
`filemark`  
`...`  
`Final tape file for archive instance:`  
  `NeoTape Archive End Header`  
`filemark`

`若遇到 EOT，目前 slice tape file 可能未以正常 filemark 關閉，且最後一個 segment 可能只部分 committed，或 logical slice payload 已完成但 Slice Trailer 尚未 committed。下一卷應以 volume header 開始，之後建立同一 logical slice 的 continuation slice tape file，接續剩餘 payload range，或在 payload 已完成時寫入該 logical slice 的 Slice Trailer；完成後才寫入 slice-level filemark。`

## 5.1 Cartridge Header

`A physical NeoTape cartridge MUST begin at BOT with a NeoTape cartridge header in tape file 0, followed by a filemark. This header is an immutable media initialization record. It is written when the cartridge is initialized and MUST NOT be treated as a mutable table of contents.`

`Unlike archive volume, segment, and end headers, the cartridge header MAY span multiple tape records. It is written at BOT during cartridge initialization, where EOT/ENOSPC is not a practical concern for any reasonable header size. The first cartridge-header record MUST contain enough fixed binary/ASCII information to identify the format, header version, total cartridge-header length or metadata location, and integrity information for the remaining cartridge-header records.`

`The cartridge header exists to make the tape self-describing even if it is separated from any external database. It SHOULD contain a fixed binary/ASCII prefix, the NeoTape format family and version, cartridge_uuid, cartridge label, initialization timestamp, minimum supported tape record size, recommended tape record size, recommended logical slice size, and a metadata bundle containing at least the NeoTape format specification and minimal neotape-cat-volumes source archive.`

`The recommended tape record size and recommended logical slice size are descriptive defaults. A writer appending a new archive instance MAY override them based on the current drive, operating system tape stack, workload, and user configuration. Therefore, a writer is not required to rewind to BOT and read the cartridge header before appending a new archive instance; it may rely on explicit user configuration or previously cached/probed device settings.`

`The cartridge header MAY contain additional cartridge-info members in its ar-style metadata bundle for physical-media notes that are not tied to a specific archive instance. Examples include owner or organization, human contact instructions, intended storage pool, handling notes, printable labels, and other recovery hints. This information is descriptive only and MUST NOT be required for archive restore correctness.`

`Because the cartridge header cannot be updated after later archives are appended, it MUST NOT record the list of archive instances, free space, current used capacity, last archive UUID, last write timestamp, or any other mutable media state. Archive discovery MUST be performed by scanning tape files for per-archive NeoTape volume headers and matching NeoTape end headers.`

`Physical media identity such as barcode, manufacturer serial, and library inventory SHOULD normally be handled by barcode labels, MAM, tape library inventory, or an external tape database. The cartridge header MAY repeat such values as hints, but these repeated values are not authoritative.`

`After the mandatory cartridge header, a physical cartridge may contain one or more archive instances. Therefore, the layout begins as:`

`Tape file 0:`  
  `NeoTape cartridge header`  
`filemark`

`Tape file 1:`  
  `NeoTape archive volume header for the first archive instance`  
`filemark`

`Tape file 2..N:`  
  `NeoTape logical slice tape files for the first archive instance; each slice tape file contains one or more segment headers, payload ranges, and the Slice Trailer for that logical slice`

`Final file for archive instance:`  
  `NeoTape end header`  
`filemark`

`Next tape file, if capacity remains:`  
  NeoTape archive volume header for another archive instance  
...

## 

## 

## 5.2 Multiple Archives per Physical Tape

`A physical LTO cartridge MAY contain more than one complete NeoTape archive instance. This is useful when an archive does not fill the remaining capacity of a tape. In this mode, each archive instance begins at a tape file boundary with its own NeoTape volume header, has its own archive_uuid, logical slices, segment numbering, catalog, and end header, and is cleanly closed before the next archive instance begins.`

The tape\_seq\_num field is scoped to a single archive\_uuid. Therefore, if two independent archives are written sequentially on the same physical cartridge, both may have tape\_seq\_num \= 1 for their first archive volume. Implementations SHOULD NOT treat tape\_seq\_num as a globally unique physical-cartridge file number.

## 5.3 Target Backends and Filesystem Spool Mode

`NeoTape separates the logical archive format from the target backend used by a writer. A writer MAY write directly to a sequential tape device, or MAY write to an ordinary filesystem spool directory that represents the same archive as separated volume and tape-file objects.`

`The tape-device backend maps NeoTape volume headers, logical slices, and archive end records to physical LTO tape files separated by filemarks. The filesystem-spool backend maps each NeoTape tape file to a regular file in a deterministic directory layout. For slice tape files, the file contains one or more segment headers and payload ranges followed by that slice's Slice Trailer. The spool layout SHOULD preserve archive_uuid, tape_seq_num, tape_file_num, slice sequence numbers, segment sequence numbers, and payload lengths in filenames or a small manifest so that tools can inspect and replay the archive without parsing every byte.`

`A recommended spool layout is:`

`archive-<archive_uuid>/`  
  `volume-000001/`  
    `tape-file-000000.cartridge-header.ntf`  
    `tape-file-000001.volume-header.ntf`  
    `tape-file-000002.slice-000001.ntf`  
    `tape-file-000003.slice-000002.ntf`  
  `volume-000002/`  
    `tape-file-000001.volume-header.ntf`  
    `...`

`Filesystem spool mode MUST preserve the same logical record order as tape mode. It MUST NOT require a different reader algorithm for archive correctness. A reader MAY treat spool files as a virtual tape: file boundaries stand in for filemarks, and volume directories stand in for cartridges or archive volumes. The same neotape-cat-volumes logical reader SHOULD be able to accept either a tape device path or a spool directory path, with the target adapter providing read-record, next-file, next-volume, and EOT/volume-limit events.`

`Because ordinary filesystems do not provide physical EOT, a spool writer MAY accept a manual or configured volume capacity limit such as --spool-volume-size or --target-volume-size. When the next committed header, segment payload, Slice Trailer metadata block, or Archive End Header would exceed the configured volume capacity, the writer MUST perform the same logical transition it would perform on EOT: close the current volume at the last valid boundary, create the next volume, write its volume header, and continue the same logical slice or trailer metadata when required.`

`This manual capacity limit is a simulation of media capacity, not an archive semantic. It is useful for preparing archive volumes before the physical tape drive is available, testing multi-volume continuation, and staging several archives while another process is using the tape drive. A later copy-to-tape tool MAY replay the spool directory to a real tape backend, preserving filemark boundaries and volume ordering.`

`A spool archive SHOULD include a machine-readable manifest with at least archive_uuid, writer version, target backend, logical volume order, per-file sizes, BLAKE3 digests, declared block_size values, whether drive hardware compression is expected during replay, and the configured virtual volume size. The manifest is advisory; restore correctness still comes from NeoTape headers, lengths, and checksums inside the spool files. A spool writer SHOULD track virtual volume limits in native input bytes unless an implementation explicitly models expected compressed occupancy as an advisory estimate.`

# 6\. Archive Model

`Archive 由多個 logical slices 依 slice_seq_num 排列而成：`

`Archive = LogicalSlice[1] + LogicalSlice[2] + ... + LogicalSlice[N]`

`每個 LogicalSlice 是由 writer 在串流過程中決定關閉的 payload byte range，後接 NeoTape Slice Trailer：`

`LogicalSlice = payload_bytes[actual size known at slice close] + NeoTape Slice Trailer`

`The actual slice_payload_size is not known when the logical slice begins. It is recorded later in the Slice Trailer after the final segment of that logical slice has been written.`

`NeoTape core does not require payload bytes to be pax, nor does it use payload-internal end markers for framing. A payload profile defines how those bytes should be interpreted. The NeoTape/PAX payload profile remains the recommended v0.1 backup profile because it preserves bsdtar/libarchive compatibility.`

`For NeoTape/PAX payload profile, neotape-cat-volumes MAY treat the length-framed slice payloads as ranges of one larger pax stream. On-tape logical slices do not need to be independently valid pax archives and do not need slice-local pax EOA markers. Any pax finalization needed for bsdtar compatibility is a profile-specific stdout policy, not the NeoTape core slice boundary rule.`

`因此，磁帶上的每個 slice 都可作為錯誤恢復與重新同步單位；下游工具是否需要知道 slice 的存在由 payload profile 決定。`

# 7\. Physical Segment Model

`Logical slice 可由一個或多個 physical segments 組成：`

`LogicalSlice[k] = Segment[k,1].payload + Segment[k,2].payload + ... + Segment[k,m].payload`

`每個 physical segment 的 payload 長度由 segment header 的 segment_payload_size 明確宣告。Reader MUST read exactly segment_payload_size bytes for that segment payload and MUST NOT inspect payload bytes to discover the segment end. In v0.1, filemark normally remains the logical-slice tape-file boundary and seek boundary, while segment_payload_size is the authoritative framing field inside that slice tape file.`

`Slice target size remains a writer policy, commonly around 64 GiB or device/workload-specific. Segment target size SHOULD normally match the writer's bounded memory buffer size, such as 4 GiB, 8 GiB, or 16 GiB. Therefore, segment_payload_size is known before writing each segment.`

`Logical slice completion is writer-declared at the final segment. The writer does not need to know the final slice_payload_size when the slice begins; it only needs to know each segment_payload_size before committing that segment header. When the writer decides to close the logical slice, it marks the final segment with SLICE_END_HINT. The following committed NeoTape record MUST be the Slice Trailer, which records the actual slice_payload_size and slice_payload_blake3. Payload-internal markers, such as pax EOA, MUST NOT be used for NeoTape core framing, and on-tape logical slices do not need to contain pax EOA markers.`

`Segment header 的 flags 可表示：`  
`- SLICE_START：此 segment 是 logical slice 的第一段。`  
`- SLICE_CONTINUATION：此 segment 延續同一 logical slice 的前一段。`  
`- SLICE_END_HINT：表示此 segment 是 writer 宣告的 logical slice final segment；下一個 NeoTape record MUST be the Slice Trailer for this logical slice. Reader then verifies actual slice_payload_size and slice_payload_blake3 from the Slice Trailer.`  
`- PAYLOAD_512_ALIGNED：payload 起點與長度符合 512-byte alignment；對 NeoTape/PAX payload profile，writer SHOULD preserve 512-byte tar record alignment when practical.`

# 8\. Header Types

`NeoTape v0.1 定義五種 header type：`

`1. Cartridge Header`  
`2. Volume Header`  
`3. Segment Header`  
`4. Slice Trailer`  
`5. Archive End Header`

`Volume, segment, slice trailer, and archive end fixed headers MUST fit within a single tape record and SHOULD occupy a single record at their commit point. Cartridge header is the exception: it starts at BOT tape file 0 and MAY span multiple records, but its first record MUST contain a fixed binary/ASCII prefix sufficient to identify NeoTape and locate the cartridge metadata bundle.`

`Common header fields：`

`- magic：例如 "NeoTape\0"`  
`- header_version：v0.1 使用 1`  
`- header_type：volume / segment / slice_trailer / archive_end；cartridge header 使用 dedicated BOT prefix and fields`  
`- header_size`  
`- header_crc32c`  
`- archive_uuid`  
`- archive_write_timestamp_utc`  
`- tape_seq_num`  
`- flags`  
`- metadata_offset`  
`- metadata_size`  
`- metadata_blake3`

`Header 後方可內嵌 metadata bundle。NeoTape v0.1 SHOULD use a restricted classic ar-style member container for header metadata bundles. The bundle is only a flat collection of named byte blobs, such as README, RESTORE, FORMAT-SPEC, neotape-cat-volumes source code, checksums, and optional small human-facing assets. It is not a filesystem archive and MUST NOT be used to restore paths, permissions, ownership, symlinks, hardlinks, device nodes, ACLs, xattrs, or other filesystem metadata. Implementations SHOULD avoid ZIP/tar for header metadata bundles unless explicitly configured, because ar-style containers have simpler parsing requirements and fewer path-handling security concerns. 固定 prefix 不得依賴 metadata bundle 才能辨識 archive_uuid 與 tape_seq_num。`

`Header fixed fields use CRC32C to catch accidental corruption and misreads with minimal parser complexity. Metadata bundles, catalog files, optional segment payload hashes, logical-slice hashes, and archive-level manifests SHOULD use BLAKE3. SHA-256 MAY appear only as compatibility metadata if explicitly requested, but BLAKE3 is the preferred NeoTape integrity hash.`

`Common timestamp fields SHOULD use UTC and SHOULD be stored in a fixed textual format such as RFC 3339 / ISO 8601. A writer MAY also store a monotonic or implementation-specific timestamp in metadata bundle for diagnostics, but archive semantics should rely on UTC wall-clock timestamp only as descriptive metadata.`

# 9\. Volume Header

`Volume header 位於每個 archive volume 的第一個 archive tape file。對已初始化的 physical cartridge 而言，它通常位於 cartridge header 之後的下一個 tape file；若同一 cartridge append 多個 archive instances，後續 archive instance 的 volume header 位於前一 archive clean end header 之後的下一個 tape file。必要欄位：`

`- archive_uuid`  
`- tape_seq_num`  
`- volume_label`  
`- format_version`  
`- block_size`  
`- archive_write_timestamp_utc`  
`- archive_sequence_on_media（optional, for multiple archives on one physical cartridge）`

`不應記錄：`

`- is_last_volume`  
`- total_volumes`  
`- archive_total_size`  
`- payload_total_size`

block\_size is the fixed NeoTape record size for this archive volume. It is not a recommendation. After the Volume Header is committed, the writer MUST use this block\_size for all NeoTape records in the same archive volume, and a reader SHOULD treat a record-size change inside the volume as a format error unless an explicit future extension allows it. Writers SHOULD choose a block\_size large enough to keep LTO streaming efficient and to avoid unnecessarily fragmenting the drive hardware-compression input stream.

這些欄位要麼最後才知道，要麼會引入回填需求。整體完成狀態由 end header 表示。

# 10\. Segment Header

`Segment header 位於 slice tape file 內每個 segment 的開頭 record。第一個 segment header 通常位於該 slice tape file 的第一個 NeoTape record；後續 segment header 由前一個 segment_payload_size 精確定位。必要欄位：`

`- archive_uuid`  
`- tape_seq_num`  
`- logical_slice_seq_num`  
`- segment_seq_num_within_slice`  
`- global_segment_seq_num（可選但推薦）`  
`- segment_payload_size`  
`- segment_payload_offset_within_slice`  
`- segment_content_type：PAYLOAD / TRAILER_METADATA`  
`- payload_profile：pax / raw / future profile id`  
`- flags：SLICE_START / SLICE_CONTINUATION / SLICE_END_HINT`  
`- header checksum`

`Segment header MUST record segment_payload_size. The reader uses this length, not payload contents or filemark position, to determine the end of the segment payload and the location of the next segment header or Slice Trailer inside the same slice tape file. The segment_payload_size SHOULD normally match the writer's memory buffer size or another bounded chunk size, such as 4 GiB, 8 GiB, or 16 GiB, except for the final segment of a logical slice.`

`Segment payload content type SHOULD be explicit:`

`- PAYLOAD：opaque bytes belonging to the current logical slice.`  
`- TRAILER_METADATA：Slice Trailer metadata continuation, not part of the logical slice payload.`

If a segment records optional payload integrity metadata, it SHOULD use BLAKE3 over the committed payload byte range. Slice-level integrity is authoritative at the Slice Trailer level via slice\_payload\_blake3.

# 11\. Slice Trailer

`Every logical slice MUST be followed by a NeoTape Slice Trailer after the writer has committed the final segment for that logical slice. The writer does not need to know slice_payload_size when the logical slice begins. The actual slice_payload_size is recorded in the Slice Trailer after the final segment has been written. The Slice Trailer is the next committed NeoTape record after that logical slice payload completes; it may physically reside on a later cartridge if EOT occurs before the trailer can be committed. The Slice Trailer is NeoTape transport metadata and is not part of the payload stream. It MUST NOT be emitted to stdout by neotape-cat-volumes.`

`The Slice Trailer fixed header MUST fit within one tape record. It is an archive-time fixed header and therefore follows the same single-record commit rule as volume, segment, and archive end headers.`

`Slice Trailer metadata is stored in a length-framed metadata area following the fixed header. The fixed header MUST record metadata_total_size, metadata_block_size, metadata_record_count, metadata_blake3, and metadata_format_version when metadata is present. metadata_total_size is the exact number of metadata bytes after the fixed header, excluding filemarks and tape-record padding.`

`The metadata area is a sequence of Metadata Blocks. Each Metadata Block begins with a small fixed block header containing block_seq_num, block_payload_size, metadata_offset, flags, and block_crc32c, followed by exactly block_payload_size bytes. Readers MUST concatenate Metadata Block payloads by metadata_offset and MUST verify metadata_blake3 over the resulting metadata byte string before trusting any contained catalog or advisory metadata.`

`Metadata Blocks MAY span multiple tape records and MAY continue across physical segments or volumes using TRAILER_METADATA continuation segments. A continuation segment with segment_content_type = TRAILER_METADATA is not payload and MUST NOT be emitted to stdout. If EOT occurs in the middle of a metadata area, the next volume MUST resume with a volume header followed by the next TRAILER_METADATA segment or fail the archive as not cleanly complete.`

`Within the reconstructed metadata byte string, individual metadata items SHOULD use a simple length-framed item table rather than path-like names alone: item_type, item_flags, item_name_size, item_payload_size, item_payload_blake3, item_name bytes, then item_payload bytes. This permits multiple catalog formats, compressed catalog payloads, warnings, source-read diagnostics, and profile-specific metadata without requiring a filesystem archive parser.`

`Recommended Slice Trailer fields:`

* `archive_uuid`  
* `logical_slice_seq_num`  
* `last_segment_seq_num_within_slice`  
* `last_global_segment_seq_num`  
* `slice_payload_size`  
* `slice_payload_blake3`

* `slice_catalog_present`  
* `slice_catalog_size`  
* `slice_catalog_blake3`  
* `metadata_offset`  
* `metadata_size`  
* `metadata_blake3`

`slice_payload_blake3 is computed over exactly slice_payload_size bytes of concatenated logical slice payload, excluding the Slice Trailer itself. NeoTape core does not inspect those bytes to find an end marker. For the NeoTape/PAX payload profile, the payload may be an arbitrary range of a larger pax stream and does not need to end with pax EOA.`

The Slice Trailer metadata area MAY contain slice-local catalog data, warnings, source-read diagnostics, payload-profile information, and other advisory metadata as length-framed metadata items. Slice-local catalog data is an index/hint and MUST NOT replace the authoritative payload metadata, such as pax entries in the NeoTape/PAX profile.

# 

# 12\. Archive End Header

`End header 是最後一個 cleanly completed archive record，位於最後一卷磁帶的最後一個 NeoTape tape file。它宣告整體 archive 已完整結束。`

`Because NeoTape core framing is length-based, Archive End Header does not depend on pax/tar EOA detection. Payload-profile-specific end markers, including pax EOA, remain inside payload bytes and are interpreted only by the relevant payload profile or downstream extractor.`

`建議欄位：`

* `archive_uuid`  
* `tape_seq_num`  
* `last_logical_slice_seq_num`  
* `last_global_segment_seq_num`  
* `clean_end = true`  
* `catalog_present`  
* `catalog_blake3`  
* `writer_version`  
* `archive_end_timestamp_utc`

若沒有讀到 Archive End Header，則 archive 不應被視為 cleanly complete，即使所有 expected logical slices and Slice Trailers 都已讀到。Archive-level completion 必須由 Archive End Header 判斷。

# 13\. Catalog

`NeoTape catalog is an advisory byte index, not the authoritative filesystem metadata source. For NeoTape/PAX payload profile, authoritative restore metadata remains in pax entries. Catalog data exists to support fast listing, partial restore planning, audit, and salvage.`

`NeoTape v0.1 defines a compact binary-safe catalog entry format. A path catalog record is a NUL-terminated byte string:`

`/<uid>/<gid>/<source_dev_maj:min>/<source_inode>/<file_type>/<mode_octal>/<size>/<mtime>/<logical_slice_seq_num>/<segment_seq_num>/<payload_offset>/<payload_size>/<filepath>\0`

`The number of slash-delimited fields before filepath is fixed by the catalog schema version. Parsers MUST split only the fixed number of leading fields; the remainder up to the terminating NUL is the filepath byte string. This works because POSIX-style path components cannot contain slash, and path strings cannot contain NUL. Therefore, filepath may contain ordinary directory separators without escaping. Also the forward slash right before the filepath is not part of the filepath itself.`

`For example:`

`/1000/1000/8:1/1234567/reg/0100644/4096/1710000000/12/3/987654321/4096/home/neo/file.txt\0`

`All numeric fields are ASCII decimal unless explicitly specified otherwise. mode_octal is ASCII octal. device fields use ASCII major:minor. file_type SHOULD use a small fixed token set such as reg, dir, symlink, hardlink, block, char, fifo, sock, or other. Unknown or unavailable values MUST be encoded as empty fields, not omitted, so the field count remains stable.`

`For device nodes, source_dev_maj:min identifies the filesystem device containing the inode; a separate optional rdev_maj:min extension field MAY be defined by a later catalog schema when device-node target identity is needed. source_inode is advisory and MAY be unavailable on filesystems without stable inode numbers. Hardlink grouping SHOULD use source_dev_maj:min plus source_inode when available.`

`Catalog records MUST NOT be interpreted as paths to restore from the metadata bundle itself. They are index entries only. Readers MUST validate path safety before using catalog data for partial restore selection, including rejection or special handling of absolute paths, parent-directory traversal, and policy-sensitive file types.`

`Catalog data MAY appear in two places:`

`1. Per-slice catalog data inside each Slice Trailer metadata area. This describes payload ranges known to be contained in the logical slice and is useful for partial restore and salvage.`  
`2. Final archive-level catalog data near the end of the archive, either inside the Archive End Header metadata area or, for NeoTape/PAX profile, optionally duplicated as payload entries such as:`  
   `.neotape/catalog/catalog.ntc`  
   `.neotape/catalog/catalog.ntc.zst`  
   `.neotape/catalog/BLAKE3SUMS`  
   `.neotape/FORMAT-SPEC.txt`

`The recommended uncompressed catalog media type is application/x-neotape-catalog-v1. Compression MAY be applied to the catalog payload as a metadata item attribute, but readers MUST know the uncompressed size and BLAKE3 digest from the surrounding metadata framing before trusting decompressed output.`

`Catalog remains advisory. If catalog and payload-profile metadata disagree, the payload-profile metadata wins for restore semantics.`

# 14\. Writer Pipeline

`推薦 writer pipeline：`

`source filesystem / file tree`  
  `-> tree walker / metadata prefetcher`  
  `-> planner / slice packer`  
  `-> source reader profile`  
  `-> reorder / batching / mbuffer-like memory buffer`  
  `-> payload profile encoder, e.g. continuous libarchive pax writer`  
  `-> NeoTape length-framed slicer / target backend writer`  
  `-> target backend: LTO tape device or filesystem spool directory`

`Planner 可依檔案大小與讀取特性分配：`

`- 大檔案可由主 I/O path 順序讀取。`  
`- 小檔案可由多個 worker threads 並行讀取。`  
`- serializer 依 planner 決定的順序餵給 payload profile encoder。`  
`- writer 可在 target slice size 附近透過調整小檔案順序，使每個 logical slice 大小接近目標。`

Slice target size 作為 heuristic，並非 hard limit。若目前檔案大於 target 或大於單卷磁帶容量，logical slice 可超過 target，並透過 continuation 跨卷。

## 14.1 Source Reader Profiles

`The NeoTape on-tape format is independent of the source filesystem. Source reading is an implementation concern of the writer. A conforming writer MAY provide multiple reader profiles:`

* `multi-threaded-small-file：多個 worker threads 並行讀取小檔案，適合 metadata-heavy 或 seek-limited workloads，例如 HDD-backed ZFS 小檔案樹`  
* `single-reader-prefetch-metadata：單一 file reader 負責資料讀取，另一個 tree walker 提前掃描與 prefetch metadata，協助 slice packing；適合單一 sequential reader 已足以 saturate I/O bandwidth 的 filesystem，例如 XFS 或高速 NVMe-backed filesystem`  
* `hybrid：大檔案走 sequential reader，小檔案走 bounded worker pool，serializer 仍依 planner 決定的順序餵入 payload profile encoder`  
* `external-manifest：writer 從外部 manifest 或 file list 取得檔案集合與 metadata hints，再依可用資訊做 slice packing`

`A tree walker MAY run ahead of the serializer to collect path, type, size, mtime, directory structure, hardlink grouping hints, and other metadata required for planning. This metadata prefetch step is advisory; authoritative file metadata is still the metadata actually emitted by the selected payload profile encoder.`

Reader profile selection MUST NOT affect the on-tape format. It only affects how efficiently the writer feeds the payload profile encoder and how well it can pack logical slices.

# 

# 15\. Writer State Machine

`Writer 主要狀態：`

`INIT`  
  `建立 archive_uuid，初始化 writer options。`

`OPEN_VOLUME`  
  `等待或開啟 target backend。對 tape backend，開啟或要求插入磁帶，寫入 volume header，寫 filemark。對 filesystem spool backend，建立下一個 volume directory 或 volume file sequence，寫入 volume header object。`

`OPEN_SEGMENT`  
  `寫入 segment header。若開始新 logical slice，flags 包含 SLICE_START；若延續 EOT 前未完成的 logical slice，flags 包含 SLICE_CONTINUATION。`

  `Segment header is an atomic commit unit. If EOT/ENOSPC occurs before the complete segment header block is successfully written, the segment MUST be treated as not created. The writer MUST open the next volume, write a new volume header if needed, and write the same segment header on the next tape. No continuation semantics are needed until at least one payload block of the segment has been committed.`

`WRITE_SEGMENT_PAYLOAD`  
  `接收 payload profile encoder output bytes，聚合成 target records 寫入。對 tape backend，target records 是 fixed-size tape blocks；對 filesystem spool backend，target records MUST preserve the same fixed block_size semantics, either as record-framed objects or as regular files with an explicit manifest describing record boundaries。成功寫入完整 target record 後才視為 commit。`

`EOT_DETECTED_OR_VOLUME_LIMIT`  
  `若 tape writer 遇到 ENOSPC/EOM/EOT，或 filesystem spool backend 達到手動設定的 virtual volume capacity，不應直接讓 payload profile encoder 視為不可恢復的 archive error。transport layer 應暫停或切換 backend volume、寫新 volume header 與 continuation segment header，然後繼續同一 logical slice 的 payload byte range。`

`CLOSE_LOGICAL_SLICE`  
  `當 buffered payload bytes 達到 segment target size，writer closes the current segment by writing exactly segment_payload_size bytes. 當 writer 決定 current logical slice 應結束時，該 segment 會成為 final segment and carries SLICE_END_HINT. For NeoTape/PAX payload profile, the writer MAY choose slice boundaries at pax member boundaries, but NeoTape core does not require pax EOA to determine the boundary.`

`WRITE_SLICE_TRAILER`  
  `在 writer 決定關閉 current logical slice，並寫完該 slice 的 final segment 後，寫入 NeoTape Slice Trailer fixed header，並可接續寫入 slice-local metadata bundle / catalog。The actual slice_payload_size is known only at this point and is recorded in the Slice Trailer together with slice_payload_blake3. After the complete Slice Trailer metadata area is committed, the writer SHOULD write a filemark to close the slice tape file. If EOT occurs after the final segment is committed but before the Slice Trailer is committed, the Slice Trailer MUST be written on the next cartridge after its volume header, as part of a continuation slice tape file. Slice Trailer fixed header is an archive-time fixed header and MUST fit within one tape record. Its metadata area MAY span multiple Metadata Blocks and MAY continue across volumes using TRAILER_METADATA continuation segments.`

`WRITE_CATALOG`  
  `Writer SHOULD store per-slice catalog data in Slice Trailer metadata bundles when useful for partial restore or salvage. The final archive-level catalog MAY also be duplicated in the final logical slice as pax entries and/or in the Archive End Header metadata bundle.`

`WRITE_END_HEADER`  
  `所有 logical slices 與其 Slice Trailers 完成後，寫入 Archive End Header，宣告 clean_end。`

`DONE`  
  `正常結束。`

`ERROR`  
  在無法恢復的 target backend I/O error、metadata mismatch、source read error 或 payload profile fatal error 時進入。

# 

# 16\. Reader / neotape-cat-volumes Model

`neotape-cat-volumes 是最小還原工具。它的職責：`

* `讀取 volume header`  
* `在使用者策略允許時，若 volume header 損壞、UUID 不符或 tape_seq_num 不符，掃描後續 tape files 尋找下一個候選 volume header`  
* `驗證 archive_uuid 與 tape_seq_num`  
* `依 filemark 讀取 volume、slice、archive-end tape files`  
* `驗證 segment header 中的 logical_slice_seq_num 與 segment_seq_num`  
* `將同一 logical slice 的 segments payload 串接`  
* `依 segment_payload_size 讀取每個 segment payload；MUST NOT inspect payload bytes to discover segment boundaries`  
* `在讀到帶有 SLICE_END_HINT 的 final segment 後，讀取並驗證該 logical slice 的 Slice Trailer`  
* `對 NeoTape/PAX payload profile，直接依 profile policy 輸出 payload bytes；core reader 不需要偵測或抑制 pax EOA`  
* `讀到 Archive End Header 時，依 payload profile 完成 stdout 輸出，並以 exit code 0 結束`  
* `將 stdout 保持為純 payload bytes；對 NeoTape/PAX payload profile，stdout 是 pax bytes；所有 prompt/log 都不得污染 stdout`

`推薦用法：`

  `# NeoTape/PAX payload profile example:`  
  `neotape-cat-volumes /dev/nst0 | bsdtar -xpf - --acls --xattrs`

`若要先落地檢查：`

  `neotape-cat-volumes /dev/nst0 > archive.pax`  
  bsdtar \-tvf archive.pax

# 

# 17\. Reader State Machine

`Reader 主要狀態：`

`START`  
  `初始化 expected archive_uuid（若未指定，從第一卷讀取），expected_tape_seq_num = 1，expected_slice_seq_num = 1。`

`READ_VOLUME_HEADER`  
  `讀取目前 archive volume 的 volume header。第一個 archive instance 通常位於 cartridge header 之後；若從 BOT 掃描，reader MUST skip cartridge header and scan for the requested archive_uuid。若 volume header checksum/header_type invalid、UUID 不符或 seq 不符，reader MUST NOT emit payload bytes from that candidate volume. It SHOULD enter MISMATCH_HANDLER, where policy may fail, prompt, or scan forward by filemarks to the next candidate Volume Header. This scan-forward option is important when a physical cartridge contains several independent archive instances and the inserted cartridge may be positioned near the tail of a different archive.`

`READ_NEXT_TAPE_FILE`  
  `讀取下一個 tape file 的第一個 block，判斷 header_type。For a logical slice tape file, the first NeoTape record is normally a Segment Header; subsequent Segment Headers inside the same slice are located by segment_payload_size rather than by filemark.`

`READ_SEGMENT_HEADER`  
  `驗證 segment 屬於目前 archive，且 slice/segment seq 符合預期。`

`STREAM_SEGMENT_PAYLOAD`  
  `依 segment_payload_size 讀取 exactly that many payload bytes。Reader MUST NOT parse payload bytes to determine segment or slice boundaries.`

`SLICE_PAYLOAD_COMPLETE`  
  `已讀完帶有 SLICE_END_HINT 的 final segment。進入 READ_SLICE_TRAILER。`

`READ_SLICE_TRAILER`  
  `讀取並驗證 NeoTape Slice Trailer。Reader MUST verify slice_payload_blake3 before treating the logical slice as NeoTape-complete. 若 trailer metadata 跨 segment 或跨卷，reader MUST follow Slice Trailer metadata continuation semantics。`

`READ_END_HEADER`  
  `驗證 clean_end。Archive End Header resides in its own archive-end tape file, delimited by filemarks like volume headers and completed logical slices. 依 payload profile 決定 stdout finalization policy，exit 0。`

`EOT_BEFORE_END`  
  `若在尚未讀到 end header 前遇到 physical EOT，提示插入下一卷，然後回到 READ_VOLUME_HEADER。`

`ERROR_HANDLER`  
  處理 read error、UUID mismatch、seq mismatch、header checksum error、slice incomplete 等。

# 18\. Error Handling

`NeoTape 建議採用 Retry / Inspect / Fail / Force-Salvage 模型。`

* `Volume header mismatch or corruption：`  
* `預設不允許輸出 payload bytes。`  
* `互動模式可提供 Retry、Inspect、ScanNextVolumeHeader、Fail。`  
* `ScanNextVolumeHeader MUST advance by tape-file/filemark boundaries and only accept a candidate whose header magic, type, size, checksum, archive_uuid, and expected tape_seq_num all validate.`  
* `This option is especially useful on cartridges containing multiple archive instances, where the wrong candidate may be the tail or clean end area of another archive.`  
* `Force 僅在 salvage mode 且使用者明確輸入完整詞時提供；Force MUST mark stdout/output as not fully verified.`  
* `UUID mismatch：`  
* `預設不允許繼續。`  
* `互動模式可提供 Retry、Inspect、ScanNextVolumeHeader、Fail。`  
* `Force 僅在 salvage mode 且使用者明確輸入完整詞時提供。`

`Tape sequence mismatch：`

* `預設提示插入正確卷。`  
* `若讀到更高 seq，可能代表缺卷。`  
* `若讀到更低/同 seq，可能代表插錯卷或倒帶。`

`Segment sequence mismatch：`

* `預設 fail 或 prompt retry。`  
* `若 salvage mode 開啟，可嘗試在目前 slice tape file 內依 length-framed segment headers 重新同步；若無法可靠同步，則 seek 到下一個 slice-level filemark，尋找下一個 slice tape file 的 segment header。`

`Read error：`

* `自動 retry N 次。`  
* `仍失敗則 prompt Retry / Inspect / Fail。`  
* `Skip damaged block 僅在 salvage mode 提供，並應明確警告會破壞 pax stream。`

* `EOT before segment, slice trailer, or archive end header：`  
* `若 segment header 已 committed 但實際讀到或寫入的 payload bytes 少於 segment_payload_size，則該 segment is incomplete。下一卷必須以同一 logical slice 的 continuation segment 接續剩餘 payload range 或重新宣告下一段 payload range。`  
* `若 writer 已寫完某 logical slice 的 final segment，但尚未 commit 完整 Slice Trailer，則 archive 尚未 cleanly complete；下一卷必須提供該 logical slice 的 Slice Trailer 或其 metadata continuation，並在 trailer 完成後才寫入 slice-level filemark。`

\- 若已讀到完整 Slice Trailer 但 slice-level filemark 或 Archive End Header 尚未讀到，則 archive 尚未 cleanly complete；下一個 tape file 可能包含下一個 logical slice 的 first segment header，或直接包含 Archive End Header。

# 

# 19\. Control Plane

`neotape-cat-volumes 的 stdout 必須只輸出 payload bytes；對 NeoTape/PAX payload profile，stdout 是 pax bytes。互動控制不得使用 stdout。`

`LTO 磁帶機不一定有穩定可用的磁帶 eject / insert 偵測功能，這部分還得繼續研究。`

`推薦分離：`

* `stdout：資料平面，payload bytes only；對 NeoTape/PAX payload profile，stdout is pax bytes`  
* `stderr：log/progress/warnings`  
* `/dev/tty：互動 prompt、換帶、Retry/Inspect/Fail`

`控制模式：`

* `--control=auto：若可用則使用 tty，否則依 policy`  
* `--control=tty：必須能開 /dev/tty`  
* `--control=none：完全不互動，適用 cron/systemd/CI/robot changer`

`錯誤策略：`

* `--on-mismatch=prompt|fail|scan-next-volume-header`  
* `--on-volume-header-error=prompt|fail|scan-next-volume-header`  
* `--on-eot=prompt|fail`  
* `--retry=N`  
* \--salvage

# 

# 20\. Compatibility with pax / bsdtar / libarchive

`NeoTape 不定義檔案 metadata 表示法。Writer 應使用 libarchive 的 pax writer，以取得對 UID/GID、xattrs、ACL、symlink、hardlink、device node、long path 與其他 POSIX metadata 的支援。Reader 端應能將 NeoTape 還原為標準 pax stream，交給 bsdtar 或其他 pax-compatible extractor。`

`NeoTape core framing does not depend on pax/tar EOA detection. Segment and slice boundaries are determined by NeoTape length fields, not by parsing the payload format.`

`The NeoTape/PAX payload profile is still recommended for v0.1 backup interoperability. In this profile, on-tape logical slices do not need to be independently valid pax archives and do not need to contain slice-local pax EOA markers. A writer MAY store a continuous pax byte stream split into length-framed NeoTape slices. A profile-aware output tool MAY append or preserve the final pax EOA required by downstream bsdtar, but this is a pax-profile output policy, not core container framing.`

Because slice boundaries are length-framed, a minimal NeoTape reader can emit payload bytes without parsing pax headers or performing EOA suppression.

# 

# 21\. Recovery Considerations

`Slice/segment 設計提供以下恢復能力：`

* `可使用 LTO filemark seek 到特定 logical slice。`  
* `若某個 segment 壞掉，可嘗試重讀同一 slice tape file，並依 segment_payload_size 重新定位 slice 內部 segment boundaries。`  
* `若 slice 內部無法可靠重新同步，可在 salvage mode 跳到下一個 slice-level filemark，尋找下一個 logical slice 的第一個 segment header。`  
* `若某個 logical slice 壞掉，其他 logical slices 仍可能可讀。`  
* `Catalog 可用於定位檔案所在 logical slice，協助 partial restore。`

`Cartridge Header 提供的恢復能力：`

* `每卷 volume header metadata bundle 可內嵌 neotape-cat-volumes source code 與格式說明，提升長期可恢復性。`  
* \- Slice Trailer records provide per-slice BLAKE3 verification and optional slice-local catalog metadata, allowing fast audit, salvage, and partial restore planning without treating catalog data as authoritative archive metadata.

* # 

* # 22\. Security Considerations

`NeoTape reader 不應信任 metadata bundle 內的 member name、size、mode、timestamp 或 hash。若 header metadata bundle 使用 ar-style container，reader SHOULD treat it as a flat collection of named byte blobs, not as a filesystem archive. It MUST NOT interpret member names as paths, MUST reject absolute paths and parent-directory components if any extended name syntax is supported, and MUST NOT restore ownership, permissions, device nodes, symlinks, hardlinks, xattrs, ACLs, or executable bits from header metadata bundles. Catalog 只是索引，不是權威 metadata。實際還原時仍需依 bsdtar/libarchive 的安全選項控制 absolute paths、.. path components、device nodes、ownership restore、ACL/xattr restore 等風險。`

`若 archive 包含可執行 restore helper binary，reader 不應自動執行。Source code 與 spec 比 binary 更適合作為長期保存資料。`

BLAKE3 is used for integrity verification, not as an authentication mechanism. If authenticity or tamper resistance is required, NeoTape SHOULD add signatures or keyed authentication metadata over the relevant BLAKE3 digests in a future extension or deployment profile.

# 23\. Future Extensions

`未來可考慮：`

`- LTO partition metadata mode`  
`- Per-segment hash chain`  
`- Per-slice Merkle tree`  
`- Partial restore index`  
`- Filesystem-native payload profiles for ZFS send streams and Btrfs send streams. In these profiles, each dataset, subvolume, or snapshot send stream is modeled as a payload sub-stream that normally maps to one or more NeoTape logical slices. A logical slice SHOULD NOT contain bytes from more than one filesystem-native sub-stream unless explicitly allowed by that payload profile.`  
`- Filesystem-native stream catalogs may record dataset/subvolume identity, snapshot names, parent snapshot dependencies, receive order, stream-level checksums, and the logical slice range containing each sub-stream. NeoTape core remains payload-format agnostic and does not parse or interpret ZFS/Btrfs stream semantics; restore semantics are handled by the relevant receive tool or payload profile implementation.`  
`- Changer/robot integration`  
`- Machine-readable JSON control protocol`  
`- Reed-Solomon or parity segments`  
`- Encryption and key metadata integration`  
`- Multiple catalog replicas across volumes`  
`- Multi-archive cartridge index for quickly listing all archive instances stored sequentially on one physical tape.`  
`- Optional low-level SCSI passthrough profiles for diagnostics only; normal operation should use the standard sequential tape device interface.`

`Explicitly out of scope for v0.1:`

`- Legacy DDS/DAT/QIC/non-LTO media profiles with small maximum tape record sizes.`  
\- Multi-record volume, segment, or end headers. These headers may be written near EOT and therefore must remain single-record commit units. This restriction does not apply to the BOT cartridge header.

# Appendix A. Example Multi-Volume Layout

`Tape 1:`

`File 0: Cartridge header, possibly multiple records, immutable media bootstrap`  
`File 1: Volume header, archive_uuid = A, tape_seq_num = 1`  
`File 2: Slice 1 tape file: segment header slice = 1 segment = 1, payload = first 8 GiB; segment header slice = 1 segment = 2, payload = rest of slice 1; Slice Trailer for slice 1, slice_payload_size = 64 GiB, slice_payload_blake3 = ...`  
`File 3: Slice 2 tape file: segment header slice = 2 segment = 1, segment_payload_size = 8 GiB; payload = first part of slice 2; EOT before slice complete`

`Tape 2:`

`File 0: Cartridge header, possibly multiple records, immutable media bootstrap for this physical cartridge`  
`File 1: Volume header, archive_uuid = A, tape_seq_num = 2`  
`File 2: Continuation of slice 2 tape file: segment header slice = 2 segment = 2, segment_payload_size = remaining bytes of slice 2; payload = rest of slice 2; Slice Trailer for slice 2, slice_payload_size = 64 GiB, slice_payload_blake3 = ..., optional slice-local catalog`  
`File 3: Slice 3 tape file: segment header slice = 3 segment = 1, segment_payload_size = complete slice 3 size; payload = complete final slice payload bytes; Slice Trailer for slice 3, slice_payload_size = ..., slice_payload_blake3 = ..., slice-local catalog`  
`File 4: Archive End Header, clean_end = true, last_slice_seq_num = 3`

`neotape-cat-volumes stdout for NeoTape/PAX payload profile:`

`slice 1 payload bytes`  
`slice 2 payload bytes`  
`slice 3 payload bytes`

No Slice Trailer bytes are emitted to stdout. On-tape logical slices do not need to contain pax EOA markers; pax stream finalization, if needed, is a NeoTape/PAX payload profile output policy.

# Appendix B. Minimal neotape-cat-volumes Responsibilities

`A minimal implementation may avoid libarchive entirely. It only needs:`

`- standard tape device open/read/write operations`  
`- NeoTape header parser`  
`- CRC32C header verification and BLAKE3 verification for slice trailers, metadata bundles, catalogs, and optional payload hashes`  
`- filemark/EOT handling`  
`- segment sequencing logic`  
`- length-framed segment and slice reader`  
`- stdout byte stream output`  
`- /dev/tty prompt support`

A minimal implementation may avoid libarchive and does not need to parse pax headers, understand filenames, restore files, interpret ACLs, or handle xattrs. It follows NeoTape segment length fields, observes SLICE\_END\_HINT to know that the following NeoTape record must be the Slice Trailer, verifies the trailer's slice\_payload\_size and slice\_payload\_blake3, and emits payload bytes according to the selected payload profile.

# Appendix C. Draft CLI

`Restore NeoTape/PAX payload profile interactively:`  
  `neotape-cat-volumes --payload-profile=pax --control=auto /dev/nst0 | bsdtar -xpf - --acls --xattrs`

`Restore without interaction:`  
  `neotape-cat-volumes --control=none --on-eot=fail --on-mismatch=fail /dev/nst0 > payload.out`

`Create filesystem spool output with a virtual volume size:`  
  `neotape-write --target=spool --spool-dir ./archive.spool --target-volume-size=12T --payload-profile=pax /source/tree`

`Replay prepared spool to tape:`  
  `neotape-spool-to-tape ./archive.spool /dev/nst0`

`Inspect volume metadata:`  
  `neotape-cat-volumes --inspect-volume /dev/nst0`

`Salvage mode:`  
  neotape-cat-volumes \--salvage \--control=tty /dev/nst0 \> salvage.out

# Appendix D. Open Questions

`1. Header binary layout exact byte offsets.`  
`2. CLOSED: Fixed archive-time headers use CRC32C for fast corruption detection. All non-header integrity hashes SHOULD use BLAKE3 unless a future revision defines a specific exception.`

`3. Whether segment payload should always begin at 512-byte tar record boundary.`  
`4. CLOSED: Header metadata bundles SHOULD use a restricted classic ar-style member container rather than ZIP/tar, unless a future revision defines a stronger reason to use another format.`

`5. How much catalog duplication should be required across final pax slice and end header.`  
`6. CLOSED: If EOT occurs before a segment header is fully committed, the segment MUST be considered not created. The writer MUST open the next volume and write the same segment header there.`  
`7. Precise mapping from standard SCSI sequential-access semantics to portable user-space operations: write/read fixed-size records, write filemark, space filemarks, rewind/offline, detect EOT/EOD, configure drive hardware compression, and report native vs compressed physical occupancy when available.`  
`8. Recommended default target logical slice size and override policy: 16 GiB, 32 GiB, 64 GiB, or device/workload-specific.`  
`9. CLOSED: A physical cartridge MUST begin with an immutable NeoTape cartridge header. This header is not a mutable table of contents and MUST NOT be used to record the list of archives later appended to the cartridge. Its role is media initialization, format bootstrap, default tape parameters, and long-term self-description.`  
`10. CLOSED: NeoTape v0.1 targets LTO-class tape media only. Legacy non-LTO tape formats are out of scope.`  
`11. CLOSED: Cartridge headers may span multiple tape records because they are written at BOT during initialization. Volume, segment, slice trailer, and archive end fixed headers remain single-record commit units.`

`12. CLOSED: Every logical slice MUST be finalized by a NeoTape Slice Trailer after the writer decides to close that logical slice and has written its final segment. The actual slice_payload_size is known only at slice close time and is recorded in the Slice Trailer. The Slice Trailer is the next NeoTape record after the logical slice payload completes; it may be on a later cartridge if EOT occurs first. Slice BLAKE3 and optional slice-local catalog metadata belong in the Slice Trailer, not in the next slice's segment header.`

`13. SUPERSEDED: Slice-local pax EOA detection is no longer required for NeoTape framing. Slice and segment boundaries are length-framed by NeoTape headers; pax EOA is payload-profile data only.`  
`14. CLOSED: Segment headers MUST carry explicit segment_payload_size, and Slice Trailers MUST carry slice_payload_size and slice_payload_blake3. NeoTape core framing is therefore payload-format agnostic.`

`End of Draft Spec v0.1`

`---`

