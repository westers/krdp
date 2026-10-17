# Clipboard file transfer for Console (:3391) and Virtual (:3395) (design, 2026-10-17, OPT-078)

Status: DESIGN ONLY. No code. Builds on `2026-10-13-console-clipboard-bridge-design.md` (text, OPT-067; images, OPT-072).
Wire numbers: master is worker wire v19; the HTML-format change (branch `html-clipboard`) takes v20; file transfer uses **v21**.

## 1. Verdict and goals

Copy files in Dolphin on one side, paste them on the other, with no size limit, in both directions, over the existing
`cliprdr` static channel (MS-RDPECLIP `FileGroupDescriptorW` + `FILECONTENTS_*`), so stock clients get it too where they
implement it. Transfers must never make the display or audio degrade badly: the file stream is **scavenger traffic**.

Recommended answers in one place (details and alternatives in section 10):

- Transfer: standard `FileGroupDescriptorW` + range requests with 64-bit offsets; lock/unlock and huge-file flags on.
- Host to client: serve ranges on demand from the user's real files (no staging).
- Client to host: **lazy** staging into a private per-session directory; the file appears on the Wayland clipboard as
  `text/uri-list` only after the descriptor list is validated, and the bytes are pulled at paste time.
- QoS: **receiver-driven request window (c) plus server-side adaptive scavenger pacing (b)**, with a small token bucket
  as the final cap. One mechanism family: "the sender of file bytes never has more than W bytes outstanding, and W
  shrinks when video or audio is busy". Section 5.
- Client setting `Clipboard transfer priority`: Auto (default) / Balanced / Fixed cap / Unlimited.
- Server settings `ClipboardFiles` (default `both`), `ClipboardFileMaxRate` (ceiling), `ClipboardFileMinFreeBytes`.

## 2. Facts from this machine (feasibility check)

FreeRDP 3 headers installed under `/usr/include/freerdp3/freerdp/`:

- `channels/cliprdr.h:47-50`: `CB_FILECONTENTS_REQUEST = 0x0008`, `CB_FILECONTENTS_RESPONSE = 0x0009`,
  `CB_LOCK_CLIPDATA = 0x000A`, `CB_UNLOCK_CLIPDATA = 0x000B`.
- `channels/cliprdr.h:70-73`: `CB_STREAM_FILECLIP_ENABLED 0x4`, `CB_FILECLIP_NO_FILE_PATHS 0x8`, `CB_CAN_LOCK_CLIPDATA 0x10`,
  `CB_HUGE_FILE_SUPPORT_ENABLED 0x20`. Request flags `FILECONTENTS_SIZE 0x1`, `FILECONTENTS_RANGE 0x2` (`:76-77`).
- `channels/cliprdr.h:~180-207`: `CLIPRDR_FILE_CONTENTS_REQUEST {streamId, listIndex, dwFlags, nPositionLow, nPositionHigh,
  cbRequested, haveClipDataId, clipDataId}` (64-bit offset = High:Low) and `CLIPRDR_FILE_CONTENTS_RESPONSE {streamId,
  cbRequested, requestedData}`; `CLIPRDR_LOCK_CLIPBOARD_DATA` / `UNLOCK` carry `clipDataId`.
- `server/cliprdr.h:96-136`: the server context has `streamFileClipEnabled`, `fileClipNoFilePaths`, `canLockClipData`,
  `hasHugeFileSupport`, and callbacks `ClientFileContentsRequest/Response`, `ServerFileContentsRequest/Response`,
  `ClientLockClipboardData`, `ServerLockClipboardData`, `ClientUnlockClipboardData`, `ServerUnlockClipboardData`.
  So the server API **can** send and receive file contents requests, lock and unlock, and huge-file offsets. No stop
  condition is met and no workaround is needed.
- Today `src/Clipboard.cpp:303-307` sets all four capability fields to FALSE, and only `ClientFormatList/Response/DataRequest/
  DataResponse` are installed (`:172-194`). File support is therefore new code in the same file plus a new owner for the
  state machine (`src/ClipboardFiles*.h`, pure).
- Threading: the `cliprdr` callbacks run on the connection's single session thread (`RdpConnection::run`,
  `src/RdpConnection.cpp:1626-1660`: one loop does socket read, channel manager and video/audio servicing; the loop waits at
  most 100 ms). A file response written synchronously on that thread delays everything else on the connection, so the
  design never writes more than one paced chunk per loop pass.
- Congestion signals that already exist: `RdpConnection::socketQueuedBytes()` (SIOCOUTQ, `RdpConnection.cpp:1052-1062`),
  `RdpConnection::tcpInfo()` (RTT, retransmits, delivery rate, `:1064-`), `Stats::Snapshot` (`src/StatsReporter.h:84-132`:
  `capacityKbps`, `sentKbps`, `appLimited`, `sendQueueBytes`, `congested`, `slow`, `limit`), `LinkEvidence`
  (`src/LinkEvidence.h`: verdict slow/fast, headroom, clientLimited; `SendQueueBackedUp = 64 KiB`),
  `FrameQueuePolicy::LinkClearBytes = 64 KiB` (`src/FrameQueuePolicy.h:226`) and `DeliveryThrottle` (`:764`, how video backs off:
  fewer frames when the queue is not clear).
- **Gap:** there is no audio underrun/late counter in `StatsReporter.h`, `PipeWireAudioPlayback.h` or `ExternalAudioQueue.h`
  (grep for underrun/drop/late finds nothing). The acceptance test (section 8) therefore needs a new counter (phase 0).
- Existing private channel for settings: `KRDPCTL` (`src/LayoutControl.h`) with the `audio-priority` record
  (`src/AudioPriority.h`, v1, id + enabled) as the template for a `clipboard-transfer` record.
- Client: clipboard code is `~/dev/krdp-client/src/ui/SessionModelClipboard.cpp` (local clipboard through `KSystemClipboard`,
  data-control protocol, explicitly because `QClipboard` on Wayland drops sets with stale serials, lines 34-45) and
  `src/core/ClipboardTakeover.h` (attach grace, echo memory). The existing text/image limits (1 MiB, 16 MiB) are in
  `src/ClipboardLimits.h` and do not apply to files.

## 3. Direction and staging

### 3.1 Host to client (worker serves the user's files)

1. The worker (user session, `server/consoleworker.cpp`) sees a selection offering `text/uri-list` (and
   `application/x-kde-cutselection` = "1" for cut). Only `file://` URIs of regular files and directories are accepted.
2. The worker walks the selection once (bounded: section 4.6), `lstat`s each entry, and builds a list of descriptors
   (relative path, size, mtime, attributes, a `(dev, ino)` identity). Directories are expanded recursively in
   deterministic order; the descriptor index is the `listIndex` of later requests.
3. The broker sends `FormatList` with `FileGroupDescriptorW`; the client asks for the descriptor (`FormatDataRequest`) and
   the worker returns it. Nothing is read from disk for content until the client requests ranges.
4. Content: the worker opens each file on first request (`O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK`, `fstat` must match the
   descriptor's type and identity), serves `pread` ranges, and keeps at most a few fds open per lock. No copy is made on the host,
   so there is no disk cost and no cleanup.
5. TOCTOU: if size or mtime changed since the descriptor (compare `fstat` at open and at each range against the descriptor),
   the file is reported as changed: the response is FAIL for that stream and the client shows "file changed while copying".
   A vanished file is the same. Never silently truncate or pad.

### 3.2 Client to host (staging)

Lazy versus fully prefetched:

| | Lazy (pull on paste) | Prefetch (copy all on Ctrl+C) |
|---|---|---|
| Disk use | only what is pasted | whole selection even if never pasted |
| Copy of a 50 GB tree | instant | minutes of bandwidth, scavenger-slow |
| Paste feels | starts when the host app reads | instant after prefetch |
| Needs host app to read through a path | yes | yes |
| Failure mid-way | at paste (app sees I/O error) | at copy (user told early) |

Wayland has no delayed-file provider for non-FUSE consumers: `text/uri-list` points at paths. Dolphin copies by reading those
paths, so the paths must exist and be readable when the paste starts. A FUSE filesystem would give true laziness but needs
`/dev/fuse`, the `fuse3` setuid helper or a user namespace, an extra mounted filesystem per session (and per Virtual
namespace), and unmount handling on crash; it is rejected for phase 1 (see 3.3).

**Recommendation: hybrid "prefetch on demand-intent".** The host side stages lazily per file but *eagerly in order, in the
background, on the scavenger budget*, starting when the client's descriptor list is accepted:

- The staging dir `$XDG_RUNTIME_DIR/krdp-clip/<connection-id>/<seq>/` is created 0700. Files are written to `name.part`
  (O_EXCL, 0600) and renamed to `name` only when complete, so a reader never sees a half file as final.
- `text/uri-list` is offered to Wayland **when the first file is complete** (small files: immediately), listing all entries;
  for entries not yet complete the list points at the final name and a paste that outruns the transfer waits: the worker keeps the
  placeholder absent, the application gets ENOENT. To avoid that, the offer for a multi-file selection is made when all
  files **up to a configurable prefix** (default: total <= 256 MiB, else after the first file) are complete, and the UI shows
  "preparing 12 GB, N% done". This is the honest cost of no FUSE: large pastes are available after staging.
- Phase 3 (optional, only if Steve wants true lazy paste of multi-GB): FUSE read-only file system from the worker. Not in scope.

Cleanup: staging for a seq is removed when (a) a newer copy replaces the offer, (b) the connection ends, (c) the worker
starts (sweep of stale `krdp-clip/*` whose connection-id is unknown), (d) 24 h age. Deletes use the `${var:?}` pattern and
only below the verified staging root (never follow symlinks; `openat`/`unlinkat` relative to a dirfd of the root).

Disk-space handling for "no size limit": before the first byte and every 64 MiB, `statvfs` the staging filesystem. Setting
`ClipboardFileMinFreeBytes` (default **max(2 GiB, 5% of the filesystem)**) is the free space that must remain after the
transfer; when the declared total plus that reserve exceeds free space the transfer is refused up front with a user-visible
message ("Not enough space on the host: need 12.0 GB, 3.1 GB free"); if space drops mid-way (other writers), abort, delete
the partial, and report. `$XDG_RUNTIME_DIR` is a tmpfs sized by RAM, which is unsuitable for GB files; staging therefore lives
in `$XDG_CACHE_HOME/krdp-clip/` (the user's home filesystem) with the quota guard above. Open: whether to prefer a
`ClipboardFileStagingDir` setting (recommended: no, a fixed location, fewer attack surfaces).

### 3.3 Why not FUSE or a portal

xdg-desktop-portal has no file-clipboard provider; Klipper stores `text/uri-list` as paths only. FUSE adds a mount in the
user's namespace visible to every app, needs `fuse3` and a setuid helper on the host image, and a crash leaves a dead
mount ("Transport endpoint is not connected") that breaks Dolphin. The staging directory is boring and debuggable.

### 3.4 Client side presentation (Qt/Wayland)

- Local copy (client to host): the client reads `text/uri-list` from `KSystemClipboard` (data control). It validates each URI
  with the same walk as the worker (no symlink follow, regular files and dirs), builds descriptors, and serves `FILECONTENTS`
  ranges with `pread`. Nothing is copied on the client.
- Received files (host to client): same staging model on the client in `$XDG_CACHE_HOME/farside-client/clip/<seq>/`,
  the client sets `text/uri-list` plus `application/x-kde-cutselection=0` through `KSystemClipboard::setMimeData`.
  Same background staging with progress UI, same offer rule (all <= 256 MiB, else after first file complete with the progress
  notice). `ClipboardTakeover`'s echo memory must record a file selection's identity (list of names+sizes hash) so the
  bridge does not re-offer files it staged itself.
- Drag-and-drop is a separate feature (needs `QDrag` and the same staging); not in this design.

## 4. Security (hard requirements; each has a unit test, section 8)

### 4.1 Names and paths (the sanitizer, pure `ClipboardFileNames.h`)
The descriptor `cFileName` is UTF-16, relative, with `\` separators. For every component:
- Reject the whole transfer (not just the entry) on: empty component, `.` or `..`, an absolute path (leading `\`, `/`), a drive
  letter (`C:`), a UNC prefix, an alternate data stream (`:`), NUL, any control character U+0000-U+001F and U+007F, and
  components of length > 255 UTF-8 bytes or total path > 4096.
- Normalise to NFC and reject if the NFC form differs in a way that makes two entries collide; detect case-insensitive and
  normalisation-insensitive collisions inside one transfer and rename the later one `name (2).ext`.
- Unicode tricks: strip/refuse bidirectional controls (U+202A-202E, U+2066-2069), zero-width and format characters (U+200B-200F,
  U+2060, U+FEFF), and unpaired surrogates; replace with `_` for display-only components, refuse in extensions that decide
  execution. The sanitised name is what is created on disk and shown in the UI; the original is never used as a path.
- Windows reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`, with or without extension), trailing dots and spaces, and
  `~1`-style names are renamed on the Linux host only for client-to-host (so a Windows-origin set round-trips); host to
  client leaves names that Windows would reject to the client to sanitise (the client's sanitizer is the same code).
- `\` in a component is a separator only at the descriptor level; after splitting, a component containing `/` is rejected.

### 4.2 Filesystem safety
- Staging root created 0700 by the session user (`mkdir 0700`, `fchmod`, verify `st_uid == getuid()` and mode on every open;
  refuse if the root is a symlink or owned by someone else). All creates use `openat` relative to a dirfd with
  `O_CREAT|O_EXCL|O_NOFOLLOW`, mode 0600 (files) / 0700 (dirs); **no execute bit is ever set**, whatever the descriptor attributes say.
- The worker/broker never writes outside the staging root and never overwrites an existing file there (EXCL; a collision is
  renamed). Extended attributes, ACLs, setuid/setgid/sticky, and mtime are not applied (mtime may be set with `futimens` only).
- Host to client sources: symlinks are **refused** (entry skipped and reported; not resolved), as are sockets, FIFOs, device
  nodes, and files on `/proc`, `/sys`, `/dev`; a directory symlink is not descended. Each open uses `O_NOFOLLOW` per component
  (`openat2` with `RESOLVE_NO_SYMLINKS|RESOLVE_BENEATH` where available) so a swap after the walk cannot escape.
- The files read must be readable by the session user, which the worker runs as; the worker never elevates.

### 4.3 Hostile descriptors and requests
- Counts: at most **100 000 entries** and descriptors list of at most 64 MiB of wire bytes per selection, nesting depth 64, total
  path bytes 32 MiB. This is a safety valve against memory exhaustion, **not** a size limit; over it the transfer is refused
  with a message. The values are constants in `ClipboardFileLimits.h` (not user settings).
- Sizes: `nFileSizeHigh:Low` is 64-bit; any size is accepted by the planner (the free-space guard decides). Arithmetic is done
  in `quint64` with overflow checks; `offset + cbRequested` overflow is an error.
- A response whose length differs from what was requested (other than a short read at EOF, which is declared) is an error.
  A peer that streams more bytes than the descriptor's size is cut off and the file deleted.
- FILECONTENTS requests: `listIndex` must be < count; `FILECONTENTS_SIZE` must carry `cbRequested == 8`; `RANGE` requires
  `cbRequested` in 1..`MaxRangeBytes` (1 MiB; see 6), `offset+len <= size`, and `streamId` unique among outstanding requests;
  `clipDataId` must name a locked snapshot when `CB_CAN_LOCK_CLIPDATA` is negotiated, otherwise requests are valid only for the
  current offer. Anything else is answered with a failed response, counted, and 3 violations in one transfer abort it.
- At most `MaxOutstanding` (section 5) requests are accepted in flight; extra requests are refused, not queued.

### 4.4 Clipboard lock semantics
`CB_CAN_LOCK_CLIPDATA` is negotiated. On a file offer the receiver sends `LockClipboardData(clipDataId)`; the sender snapshots the descriptor
list and keeps its file identities until `Unlock` (or session end, or 10 min without a request after the offer was replaced).
A new copy while locked does not invalidate the lock; requests with the old `clipDataId` still read the old list. Maximum 8
simultaneous locks per connection; the least recently used is dropped with a notice. Unlock releases fds and staging. A peer that
does not negotiate locking gets the simple model: replacing the clipboard aborts outstanding transfers.

### 4.5 Policy per state (extends `ConsoleClipboardPolicy::plan`, a new `files` field)
| State | host to client files | client to host files |
|---|---|---|
| Greeter | deny | deny (a file dropped on a login screen is never wanted; text/images still allowed) |
| Locked, same account | allow | **deny** (recommend: a file on a locked desktop would land under the user's session without the user present; the owner already authenticated, but staged data on a locked box is not worth it) |
| Locked, other account | deny | deny |
| Unlocked, same account | allow | allow |
| Viewer (not controller) | deny | deny |
| Virtual (authenticated owner) | allow | allow |
| No seat / no uid | deny | deny |

Effective = state rule AND server setting `ClipboardFiles` (`both` | `host-to-client` | `client-to-host` | `off`, default `both`,
same shape and place as `ClipboardDirection`, section 7.1 of the bridge spec) AND `ClipboardDirection` (a file offer is
text-class data: if the direction is narrowed, files narrow with it). The KCM page gets a "Allow file copy" combo next to the
existing clipboard direction (UI, help text, validation, packaging as for every setting). Default `both`: Steve asked for the
feature; the state table already narrows risky states.

### 4.6 Walk bounds and other host protections
The directory walk is bounded by the count and depth limits above, a time budget (5 s per selection; over budget the transfer
is refused with a message), and never crosses to another filesystem unless the user selected that path (`st_dev` changes inside a
directory tree are skipped and reported). Klipper history receives the `text/uri-list` string only (paths, not contents);
for writes made while locked there are none (denied). Logs carry counts, bytes, ids and reasons, **never file names or contents**
(canary-token test, as for text).

## 5. Wire design (worker wire v21)

New records (CBOR/JSON-compatible, same framing as v16-v20; all carry `seq` = offer sequence, 64-bit offsets/lengths):

- `fileOffer {seq, direction, count, totalBytes?, cut}` plus the raw `FileGroupDescriptorW` bytes as the existing data record.
- `fileRequest {seq, clipDataId, streamId, listIndex, kind: size|range, offset: u64, length: u32}`.
- `fileResponse {seq, streamId, status: ok|fail|changed|denied|gone|nospace, length: u32}` + payload frame (binary, not base64).
- `fileCancel {seq, streamId?}` (omitted streamId = the whole offer), `fileProgress {seq, doneBytes: u64, totalBytes, state}`
  (worker to broker, at most 4/s, for UI and stats), `fileQos {seq, mode, capBps, window}` (broker to worker, section 6).
- `fileUnlock {seq, clipDataId}`.
Everything is capped by the existing wire frame limit: a file range travels as many frames of <= 256 KiB if the current limit is
smaller; v21 bumps `ProtocolVersion` to 21 (paired broker/worker, no back-compat, per the standing rule).

**Chunk size and 64-bit offsets.** Range requests are up to **256 KiB** (`MaxRangeBytes` constant 1 MiB is only the validation
ceiling). The cliprdr PDU can carry that in one response; FreeRDP's static-channel layer splits it into
`CHANNEL_CHUNK_LENGTH` (1600 byte) pieces. Offsets are `nPositionHigh:Low`. `hasHugeFileSupport` / `CB_HUGE_FILE_SUPPORT_ENABLED` is
set so file sizes and offsets above 4 GiB are legal; with a client that lacks it, files > 4 GiB are skipped with a notice (not
truncated). Resume is not required: an interrupted transfer restarts the file; staged complete files are kept for that offer.

**Cancel/abort.** User cancel in the client UI, a new copy, a lock failure, `nospace`, 3 protocol violations, or connection loss
all end in `fileCancel`; the receiving side deletes `.part` files, keeps completed ones only until cleanup, and surfaces one
reason code. Cancel is honoured within one outstanding window (<= `W` bytes), because already-requested ranges may still arrive
and are dropped.

## 6. Bandwidth QoS (the core problem)

### 6.1 The problem, quantified
All channels share one TCP connection and one session thread; there is no kernel QoS between them. Static channel writes are
split by FreeRDP into 1600-byte chunks, each its own PDU, so on the wire GFX/audio/file chunks interleave at 1.6 KB granularity
**if the sender writes them in turn**. The head-of-line blocking is not the chunk size; it is what has already been handed to
the kernel ahead of the next video/audio PDU (the send queue). At 100 Mbit/s a 256 KiB response written at once occupies the wire
for 21 ms and delays a 20 ms audio packet or a keyframe by that much; a 4 MiB write would delay it 335 ms (an audible glitch).
At 20 Mbit/s the same 256 KiB is 105 ms. So the rule is: **never have more than ~2 chunks' time of file bytes in the send queue.**

### 6.2 Options evaluated
- (a) Token bucket with a fixed cap. Simple, predictable, but blind: a cap safe for a 5 Mbit/s uplink wastes a gigabit LAN; a cap
  safe for LAN swamps Wi-Fi. Keep it only as the user's explicit cap and as the host owner's ceiling.
- (b) Adaptive scavenger pacing driven by existing signals. Uses leftover capacity; the signals exist (send queue, `LinkEvidence`
  capacity/headroom, `limit`, `congested`/`slow`). Weakness: video/audio load changes quickly and the signals lag by an interval
  (the sampler runs about every second), so (b) alone overshoots at the start of a video burst.
- (c) Request-window flow control by the receiver. The file sender cannot send what was not asked for, so the receiver's
  outstanding-request count is a hard bound on in-flight bytes and costs nothing on the sender. Weakness: the receiver does not know
  the host's video/audio state, only its own measurements (RTT, throughput, jitter); and in the host-to-client direction the
  receiver is the client, which is not authoritative for the host.
- (d) Chunk size and interleaving: necessary but not sufficient; fixes the 6.1 numbers only if (c)/(b) bound the queue.
- (e) Second TCP connection or UDP/multitransport (RDP-UDP, MS-RDPEMT). Out of scope: our server uses neither (no multitransport
  in `RdpConnection`), a second connection would need a new auth/binding path and would compete with video anyway on the
  same bottleneck without the ability to yield, and stock clients don't support it. A congestion-controlled (LEDBAT-like)
  separate flow is the textbook answer but is multi-month work.

### 6.3 Recommendation: windowed scavenger (c) with server-side adaptive limit (b), capped by (a)
One controller, `FileTransferPacer` (pure, injectable clock, in `src/ClipboardFilePacing.h`), owned by the side that **sends file
bytes** and authoritative for host to client (the server) and self-imposed for client to host (the client paces its responses; the
server additionally paces how many requests it issues, which is what bounds the client's response rate and works with any client).

Inputs per 100 ms tick (cheap, all existing except the audio counter): `socketQueuedBytes`, the latest `Stats::Snapshot`
(`sentKbps` of video+audio, `capacityKbps`, `appLimited`, `congested`, `slow`, `limit`), `LinkEvidence` headroom verdict, video
backlog (`FrameQueuePolicy` clear/not clear), and new audio counters (phase 0).

Allowed file rate `R` in bit/s:
- `base = max(floor, capacity - sentOther)` where `capacity` is `capacityKbps` (TCP delivery rate) and `sentOther` is the
  non-file rate (video+audio) smoothed over 2 s. Unknown capacity: `base = 4 * floor` until known.
- Scale by state: link idle (no frame sent for 1 s, audio silent): `R = base` (full speed); video active but queue clear: `R = 50%` of
  `base`; send queue >= `LinkClearBytes` (64 KiB), video backlog, or audio late: `R = floor`; `LinkEvidence` slow or `congested`: `R = floor`.
  Multiplicative decrease (halve per tick on pressure), additive increase (+5% of `base` per tick without pressure): AIMD, so it yields fast
  and recovers in about 2 s.
- `floor` = **256 kbit/s (32 KiB/s)** by default: a transfer never reaches zero, but a 1 GiB file at the floor would take 9 hours,
  which is the intended behaviour on a saturated link (the UI says "waiting for the link").
- The window is derived from `R`: outstanding requests `W = clamp(ceil(R * 0.1 s / 256 KiB), 1, 16)`, i.e. at most about
  100 ms of file bytes in flight, at least one request, at most 4 MiB. The token bucket holds `R * 50 ms` of burst so a response
  is written as one or a few 64 KiB slices, never the full window at once. At 100 Mbit/s: `W = 5`; at the floor: `W = 1` and
  responses sliced to 16 KiB.
- Additionally the pacer refuses to enqueue a slice when `socketQueuedBytes + slice > max(64 KiB, 2 * slice)`: the hard HOL bound,
  about 5 ms at 100 Mbit/s and 21 ms at 25 Mbit/s for the 64 KiB slice.

Failure modes by design:
| Situation | Behaviour |
|---|---|
| Idle display, LAN | full speed up to the user's mode (Auto: capacity) |
| Active video, headroom | about half of the leftover capacity; video unaffected (queue stays under 64 KiB) |
| Congested or slow link | decays to the floor within ~1 s; never zero; recovers by AIMD after ~2 s of no pressure |
| Capacity unknown (new connection) | starts at the floor x4 (1 Mbit/s) and probes up by +5% per tick |
| Client slow to read | (c): its window of requests bounds bytes in flight; SIOCOUTQ rule stops slices |
| Server session thread busy | no writes beyond one slice per loop pass; the 100 ms loop wait is not extended |
| Peer ignores pacing (misbehaving client sends unrequested data) | the worker never sends unrequested file data; FILECONTENTS responses without a request are dropped as violations |

The client paces its **requests** with its own copy of the same pacer (inputs: its measured throughput, RTT, and the server's `fileQos` hint),
so even if the server's pacing were bypassed the bound holds in both directions.

### 6.4 Acceptance (measurable)
Baseline: play a 60 s video (1080p60 test pattern or the existing synthetic scene) with audio for 60 s over the Sol to Buzz link,
record: frame delivery delay p50/p99 (`Stats` frame delay / `sendQueueBytes`), video `framesSent` and `coalesced` per second, audio
late/underrun counters (new), RTT. Then repeat while a 2 GiB file moves in each direction. Pass:
- video: `framesSent` rate within **10%** of baseline, p99 send-queue bytes **<= 256 KiB** and p99 added frame delay **<= 40 ms** over baseline;
- audio: underruns/late packets **<= baseline + 2** over 60 s, no gap > 100 ms;
- the transfer finishes (progress never stalls > 30 s on an uncongested link) and, with `tc tbf` 20 Mbit/s on Sol, drops to near the floor while video is
  active and takes the leftover when the display is idle;
- Unlimited mode is expected to fail the first two criteria (documented, warning shown).

## 7. Client advanced setting

`Clipboard transfer priority` (Settings > Advanced > Clipboard files; stored **global** in the client config with a per-connection
override in the connection's advanced section, key `clipboardTransferPriority` in `connections.json`):

| Value | Behaviour | Default |
|---|---|---|
| Auto | adaptive scavenger as in 6.3 | yes |
| Balanced | like Auto but never above 50% of measured capacity, even on an idle display | |
| Fixed cap | user enters MB/s (0.1 to 1000, validated, shown in Mbit/s as well); the pacer still yields to congestion below the cap | |
| Unlimited | no pacing besides `W <= 16` and the HOL queue rule; a confirm dialog explains the display/audio impact; resets to Auto after the session | |

Plus a read-only line "Host limit: Balanced (set by the host owner)" when the server ceiling applies.

Communication: a `clipboard-transfer` KRDPCTL record, `v:1`, `{mode, capBytesPerSec?}` sent by the client after the capabilities handshake and on
change, modelled on `AudioPriority` (`src/AudioPriority.h`: strict parse, id <= 64, reply with `effective`). Server advertises
`clipboardFiles: {v:1, directions, maxMode, maxRateBps}` in `capabilities` (KRDPCTL v2 capabilities group). The server computes
`effective = min(client request, server ceiling)` and replies with it; the reply is shown in the UI. A stock client (no KRDPCTL) runs
under Auto with window `W <= 4`.

Server ceiling: broker host key `ClipboardFileMaxRate` (per route like `ClipboardDirection`; values `auto` (default, no ceiling beyond Auto's own logic),
`balanced`, or a MB/s number; `0` is invalid, use `ClipboardFiles=off`). Server enforcement is authoritative for host to client;
for client to host it bounds how many requests the server issues.

## 8. UI

- Client: a transfer pane in the session toolbar (and a tray notification): file names count, bytes done/total, current rate, ETA,
  state ("Preparing", "Waiting for the link", "Copying", "Done", "Failed: no space on host"), Cancel button. A completion
  notification via `KNotification` (success and failure, click opens the folder). Large paste attempts that outrun staging show
  "Still preparing 12 GB (37%)".
- Host: a one-line notification in the user's session (`org.freedesktop.Notifications`) at start of client-to-host staging
  ("Receiving 3 files from <client>") so a file landing on the desktop is never a silent surprise, plus failures.
- KCM: "Allow file copy" combo (`ClipboardFiles`), "Maximum transfer rate" combo/number (`ClipboardFileMaxRate`), "Reserve free
  disk space" (`ClipboardFileMinFreeBytes`, GiB spin box, 0 means the default rule). Each with help text, validation, defaults,
  packaging and unit tests as per the standing settings requirement.

## 9. Test plan

Pure units (no display): path/name sanitizer (traversal, drive letters, backslashes, reserved names, Unicode controls, collisions, NFC);
descriptor codec (round trip, truncated, hostile counts, 64-bit sizes, odd UTF-16); FILECONTENTS request validator (range, overflow, stream
reuse, lock ids); `FileTransferPacer` with a fake clock (idle to full, pressure halves per tick, floor never zero, recovery in about 2 s, window
and slice derivation, unknown capacity start); QoS decision table (mode x ceiling x stats snapshot); disk guard (`statvfs` injected);
policy table (`ConsoleClipboardPolicy` new `files` rows, all states x settings); `ClipboardFileLimits`; staging janitor with a temp dir (no symlink follow, `.part` rename, EXCL).
Link-wrap tests (`-Wl,--wrap=` as in the repo) for `pread`, `openat`, `statvfs`, `fsync` failures. Broker-level test with a fake worker (v21 records, cancel, lock,
changed file). Loopback integration: `Clipboard` against a libfreerdp client in-process transferring 3 files (0 B, 1 B, 5 GiB sparse) with the pacer driven by a fake clock.
Canary test: file names and contents never appear in logs.

Live (Sol server, Buzz client; never Hal): matrix of Console unlocked, Console locked (client to host denied, host to client allowed), greeter (deny), Virtual;
stock clients (xfreerdp smoke: host to client small file); section 6.4 video+audio acceptance with a 2 GiB file each way, once on LAN and once under `tc tbf` 20 Mbit/s.
Counters available today for the acceptance run: `sentKbps`, `capacityKbps`, `appLimited`, `sendQueueBytes`, `congested`, `slow`, `limit`, `retransmits`, TCP RTT,
`SurfaceCounters {framesSent, coalesced, keyframes}`, `LinkEvidence` verdict. Missing (phase 0 adds them): audio underrun/late counter, per-frame delivery delay histogram,
and the file transfer's own rate/window counters in `StatsReporter`.

## 10. Open questions with recommended answers (not waiting for answers)

1. Client-to-host files while the Console is locked: **deny** (4.5). Alternative: allow for the owner.
2. Lazy versus prefetch: **staged in background, offer when small selections are complete or the first file of large ones is** (3.2); FUSE only in a later phase if wanted.
3. Staging location: `$XDG_CACHE_HOME` (home filesystem), not tmpfs; fixed, not configurable.
4. Free-space reserve default: `max(2 GiB, 5%)`.
5. Default `ClipboardFiles`: `both`.
6. Default client priority: Auto; Unlimited resets each session.
7. Floor rate: 256 kbit/s; window cap 16 requests of 256 KiB.
8. Entries safety valve: 100 000 entries, depth 64 (not a size limit).
9. Directories: supported (recursive) from phase 1; symlinks refused.
10. Drag and drop: separate item, after clipboard files.
11. Should the greeter allow client to host files for password files? **No.**
12. Should file offers be hidden from Klipper history? Klipper only sees paths; accept.
13. Cut (move) semantics: host to client cut is treated as copy (never delete the source); client to host cut is copy.
14. Stock Windows `mstsc` as the client: file paste uses the same PDUs; smoke only (own client first).

## 11. Risks

- The session thread is single: a pacer bug could stall video; mitigate with the one-slice-per-pass rule and the unit-tested fake-clock controller.
- Large pastes feel slow without FUSE (staged first); the UI must say so.
- `capacityKbps` is absent or noisy at low send rates (OPT-061 noted this); mitigate with the conservative unknown start and the queue rule that does not depend on it.
- No audio underrun counter exists today: acceptance cannot be measured until phase 0.
- Wire v20/v21 sequencing with the `html-clipboard` branch: bump at merge time.
- FreeRDP behaviours of the `cliprdr` server (huge file flag, lock) have not been exercised in this repo; spike S1 below.
