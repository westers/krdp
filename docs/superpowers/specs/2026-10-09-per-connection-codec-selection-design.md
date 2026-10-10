# Per-connection codec, encoder and decoder selection, multi-GPU hosts and NVIDIA backends (design, 2026-10-09, OPT-062)

Status: SIGNED OFF 2026-10-09 (S0, see "Sign-off 2026-10-09" at the end); originally a draft. Design only: no code, configuration or host was changed while writing it.
Two routes only (Console :3391, Virtual :3395); there is no :3389 route. Stock RDP clients keep today's behaviour.
Builds on `2026-09-29-nvidia-video-backends-design.md` (device model, one device per stream) and KRDPCTL v2 (e)/(g).

## 1. Verdict and model (read this first)

- **Buildable, with five corrections to the brief** (section 1.1). None blocks the design; each changes a rule.
- **The client chooses, per connection:** an ordered codec list (default HEVC, AV1, AVC), one global
  **Encoding on the host** mode and one global **Decoding on this computer** mode, each Hardware / Software / Any.
  Any = hardware preferred, software when that end has no hardware for the codec.
- **The host limits, per codec:** whether software encoders may be used (Allowed / Never; AVC see Q7). Hardware
  encoders are always allowed. A later host policy chooses *which* hardware device encodes (Automatic or a PCI
  id, plus whether load balancing is allowed). The client never chooses a device; it only displays it.
- **The rule:** the chosen codec is the first codec in the client's list for which the host can encode it in the
  requested encode mode within its software allowance AND the client can decode it in the requested decode mode.
  If none qualifies, standard AVC is used as the **baseline** (always possible, may violate the requested modes,
  always announced). "Available" means a real trial-opened encoder/decoder at that end, never an FFmpeg name.
- **Adaptive switches stay inside the request:** a slow link, the CPU guard or an encoder failure may move only
  between candidates that already satisfy the request (same list, same modes, same allowance). A Hardware-only
  request is never moved to a software encoder; only the baseline can break the modes, and it says so.
- **Where it lives:** standards-first is unchanged. AVC420/AVC444 are still negotiated by RDPGFX caps; the new
  fields are additive on the existing private `codec` record, behind `capabilities.video.preferences`.
- **Multi-GPU (Hal is the only two-GPU host):** "Hardware" means any hardware device that can encode the codec.
  Automatic prefers the GPU that owns the capture buffers (no cross-GPU copy), then other permitted devices only
  when load balancing is on and the preferred device is at capacity, then software if allowed. Decisions happen
  at connect and at encoder restarts only. The cross-GPU copy cost is an **open measurement** (M1).
- **NVIDIA scope expansion (Steve):** NVENC for every codec our pipeline can carry on Ada: H.264 4:2:0 (AVC420),
  HEVC Main 4:2:0 (exists), AV1 Main 4:2:0 (new), and client NVDEC for HEVC (exists) and AV1 (new). AVC444 on
  NVENC and HEVC 4:4:4 are assessed and not planned now (section 7.6).
- **Fleet results with the defaults** (order HEVC, AV1, AVC; Any; Any; software allowed): Buzz gets HEVC hardware
  encode + hardware decode from Hal, Sol, cray and ace, which is what it gets today (section 4.3).
- **AV1 software end-to-end numbers:** the parallel Sol-encode/Buzz-decode test had produced logs but no
  `RESULTS.md` in `~/dev/rdp/evidence/2026-10-09-av1-software-test/` when this was written, so no "software AV1
  is usable at X" figure is given here. Section 12.4 says where it slots in.

### 1.1 Corrections and conflicts found (with citations)

| # | Brief / assumption | What the code does | Consequence in this design |
|---|---|---|---|
| C1 | The client's codec order is honoured | The server **ignores the order**: the contract says so (`~/dev/rdp/KRDPCTL-V2-CONTRACT.md:266`, "order is ignored"); `VideoStream::setPrivateCodecPolicy` keeps a set (`src/VideoStream.cpp:1080-1086`) and `CodecPolicy::select` walks `BestCompressionFirst` AV1 > HEVC > AVC (`src/CodecPolicy.h:110`, `:598-620`). Hal/cray send AV1 to a client that lists both. | New `order` field and an order-honouring selection behind a capability. The client's Advanced help "the first one the PC supports is used" (`src/qml/AdvancedPage.qml:104`) is wrong today. |
| C2 | Decode = Hardware can fall back to AVC | Our client **always decodes AVC in software**: FreeRDP's GFX AVC path, built `WITH_VAAPI=OFF` (`krdp-client/src/core/DecodeCaps.h:75-79`). | AVC never satisfies Decode = Hardware. The baseline is exempt and announced (section 6). Hardware AVC decode would need our own AVC decode path (not planned, Q-free note in 7.6). |
| C3 | "Hardware" encode can be guaranteed | Software H.264 is always the last resort, even under `never` (`src/CodecPolicy.h:32`, `:616-619`; `src/EncoderSupport.h:205`). KPipeWire's AVC hardware policy is HardwareFirst and falls back to libx264 by itself, e.g. above VA-API's 4096 limit (`src/EncoderSelection.h:22-31`). Sol has **no AVC hardware** encoder at all ("avc sw, hevc hw+sw, av1 sw", `~/dev/rdp/evidence/2026-09-30-t05-avc-selection/native-console.log`). | The baseline may be software AVC under Encode = Hardware; the reply and Stats say so (backend is reported truthfully, contract line 423). |
| C4 | Console viewers use the controller's codec | **AVC for everyone** while more than one client is admitted; the private codec runs only while the controller is alone (`server/ConsoleHostController.cpp:2584-2603`, `:1294-1310`; contract (e) "Console host", lines 452-457). | Unchanged (one worker encodes for all; a viewer may not decode the controller's codec). The notice names it. |
| C5 | The host's setting limits software use | A per-user preference **overrides** the host's `SoftwareEncoding` (`server/ConsoleHostController.cpp:1055`, `:2070`; `server/VirtualSessionTransport.cpp:265`), so a user's `prefer` beats a host `never` today. The KCM even labels the host values "Host Video Defaults" (`src/kcm/brokerhostsettings.cpp:557`). | The new per-codec software allowance is a **ceiling** that no user preference or client request can exceed. |
| C6 | "Sol's 4090 is the NVENC test machine" | Sol has an **RTX 2070 (TU106, Turing)** at PCI 09:00.0, driver 595.91.07 (read-only `nvidia-smi`/`lspci` on Sol, 2026-10-09); the NVIDIA spec already says "Its RTX 2070 must never advertise hardware AV1" (`docs/superpowers/specs/2026-09-29-nvidia-video-backends-design.md:31`). The only 4090 is Hal's (AD102, PCI 04:00.0, `lspci` on Hal). | Sol remains the NVENC test machine for **H.264 and HEVC** (and for CPU-staging cost). **AV1 NVENC and AV1 NVDEC can only be proven on Hal**, which needs Steve's explicit go for every native run. |
| C7 | Hal's NVIDIA stack is usable | Hal's kernel module is 595.91.07 (`/proc/driver/nvidia/version`) but the userspace is 595.99.02 (`libnvidia-encode-595`, `libnvidia-compute-595`); `nvidia-smi` fails with "Driver/library version mismatch". | Until Hal is rebooted (Steve's decision), CUDA/NVENC/NVDEC/NVML on Hal must be assumed unusable; every Hal NVIDIA slice and M1 wait for it. The client's NVDEC on Hal will fall back to AMD VA-API meanwhile. |

Also noted: the client's hardware decoder silently retries in software when it fails to open or rejects the
first packet (`krdp-client/src/rdp/GraphicsPipeline.cpp:576-603`); under Decode = Hardware that must become a
codec renegotiation instead (section 8.3).

## 2. Current state (what exists, with file:line)

### 2.1 Client (`~/dev/krdp-client`, 0.8.0, `CMakeLists.txt:4`)

- **Global settings, not per connection.** `ChromaSettings` holds `privateCodecOrder` (default `hevc,av1`; empty =
  standard AVC only), `adaptivePrivateCodecs` (default on), `allowSoftwarePrivateDecode` (default off) and
  `preferAvc420` (`src/ui/ChromaSettings.h:56-65`, defaults `:134-136`), stored in `farside-clientrc` `[Chroma]`
  (`src/ui/ChromaSettings.cpp:13-20`, load `:64-74`; the order is written only when not the default, `:198-213`;
  the software switch only when on, `:225-233`). Only `hevc`/`av1` survive normalization (`:22-32`).
- **Advanced page** (`src/qml/AdvancedPage.qml:90-134`): "Prefer AVC420", "Farside codec order" (five fixed orders,
  `:101-118`), "Switch codec when latency stays high" (`:120-126`), "Allow software HEVC/AV1 decoding" (`:128-133`).
- **The request** is built in `App::sendCodecPolicy` (`src/ui/App.cpp:877-928`): wanted = global order unless the
  acceptance-only `private-codec=` debug action overrides it (`App.cpp:880-882`, `src/ui/DebugActions.cpp:403-410`);
  the per-connection decoder-failure fallback empties it (`App.cpp:884-885`); `CodecPolicy::choose` drops what the
  host does not advertise or this machine cannot decode (`src/core/CodecPolicy.h:96-112`; called `App.cpp:889`);
  a notice when the user's first choice is lost (`App.cpp:905-913`); no record at all for a host without a `video`
  group (`App.cpp:915-921`). Sent fields: `codecs`, `adaptive` (only for the shipped "auto" preference and the
  Advanced switch, `App.cpp:925`), `decode` (`App.cpp:927`). The host's answer is parsed in
  `src/ui/SessionModelControl.cpp:863-874` (`selected`, `backend`, `reason`) and fed to Stats
  (`src/ui/SessionStats.cpp:405-411`). Console's AVC reply is not treated as a refusal (`App.cpp:716-721`).
- **Decode capability.** `VaapiProbe::cached()` probes once per process, picks **one** render node (the first that
  decodes HEVC or AV1, else the first that initialises; `KRDPC_RENDER_NODE` forces one, `KRDPC_HW_DECODE=0`
  disables hardware) and adds CUDA HEVC (NVDEC) if a CUDA device "0" opens (`src/rdp/VaapiProbe.cpp:103-164`).
  `DecodeCaps::hardware` (`src/core/DecodeCaps.h:36-48`): HEVC = VA-API HEVC or NVDEC; AV1 = VA-API only (no NVDEC
  AV1 yet); AVC = VA-API flag, but unused (C2). `requestable` adds software-only codecs only with the Advanced
  switch (`DecodeCaps.h:56-68`). `decodeMap` sends `avc:"sw"` always and hw/sw for the requested private codecs
  (`DecodeCaps.h:84-93`).
- **Decoder open order** per private surface (`src/rdp/GraphicsPipeline.cpp:576-585`): NVDEC first only with
  `FARSIDE_HEVC_DECODER=nvdec` (HEVC only), then VA-API, then NVDEC, then software; a hardware decoder that fails
  before its first picture is reopened in software (`GraphicsPipeline.cpp:594-603`). The opened backend is logged
  ("hardware (VAAPI /dev/dri/renderD128)", "hardware (NVDEC CUDA 0)", `:569-572`), but only hw/sw reaches Stats
  (`m_privateDecodePath`, `GraphicsPipeline.cpp:622-626`, `:762-765`). FreeRDP must pass private frames at all
  (`privateCodecsPassThrough`, `GraphicsPipeline.cpp:699-707`, called once in `src/main.cpp:634`).
- **Per-connection storage** is `Connection` (`src/core/Connection.h:69-196`), JSON in `toJson`/`connectionFromJson`
  (`Connection.h:417-460`, `:462-...`), form fields in `applyFormFields` (`Connection.h:312-370`); per-connection
  precedents: `requestClientMonitors` (`Connection.h:104`, `:334`, `:425`), `screenMap` (`:145`, `:365-367`, `:443-449`).
- **Edit PC form** (`src/qml/ConnectionForm.qml`): Display card with Connection type (`:514-531`), Windows and
  monitors (`:533-540`), When I connect (`:546-559`), clipboard and reconnect switches (`:561-571`); Devices card
  follows (`:577-...`); `fill()` (`:144-178`), `save()` (`:180-...`), the `--debug-actions` form field setter
  (`:280-305`).
- **Stats and quality meter.** The Host encoder row uses the server's software encoder load only when the
  server's `backend` is `software` (`src/ui/SessionStats.cpp:526`; `src/core/ConnectionQuality.h:95`, `:524`);
  This computer uses decode time x presented fps (`ConnectionQuality.h:93`, `:298-300`).

### 2.2 Server (`~/dev/krdp`, master `624821e4`)

- **Policy (pure):** `CodecPolicy::SoftwareEncoding {Auto, Never, Prefer}` (`src/CodecPolicy.h:29-50`); families
  and `BestCompressionFirst` (`:107-120`); `Backends {hardware, software, liveBitrate}` and `Encoders` (`:121-140`);
  `Input` with `client` (an unordered list of families), `adaptive`, link and CPU figures (`:429-478`); `select()`
  (`:598-620`): normal link = best-compressing client family **with hardware**; slow link or `prefer` = best
  compression, software allowed unless the CPU guard blocks it; else AVC (hardware if any). `step()`
  (`:962-1300`) adds the slow-link state machine, CPU guard (preset, then codec block with back-off, then frame
  rate, `:1085-1210`), anti-flap. `avcChoiceReason()` explains an AVC answer (`:1345-1378`).
- **Encoder probe:** `EncoderSupport::probeUncached()` (`src/EncoderSupport.cpp:284-332`): `selectVaapiDriver()`
  first (Hal's nvidia-vaapi-driver answers encode queries falsely, `:289-298`; driver choice
  `src/RdpConnection.cpp:561-593`); VA-API: the **first** render node (name order, the virtual grant first,
  `src/RenderNodes.h:56-70`) on which `h264_vaapi` trial-opens, then `hevc_vaapi`/`av1_vaapi` on that same node
  (`EncoderSupport.cpp:110-140`); NVENC: **HEVC only, only when that VA-API node has no HEVC**, CUDA device
  `"0"` hard-coded (`:74-99`, `:301-306`). So a host has at most one hardware device per codec and Hal never probes
  its 4090. Software: libx264/libopenh264, KPipeWire's libx265/SVT-AV1 report (`:208-219`, `:311-317`).
  Overrides `FARSIDE_ENCODERS=avc=hw+sw,hevc=hw,av1=none` and `FARSIDE_FORCE_SOFTWARE_ENCODING=1`
  (`:246-282`, `:321-329`; the header comment still says `KRDP_*`, `src/EncoderSupport.h:22-30`).
- **Capabilities:** `videoCapabilities()` lists `avc420` always, `avc444` with a hardware 4:4:4 encoder,
  `hevc`/`av1` when an encoder exists (under `never` only hardware) (`src/EncoderSupport.cpp:344-362`),
  serialized with `softwareEncoding` (`src/LayoutControl.h:202-219`, `src/LayoutControl.cpp:449-455`).
- **Request/answer:** `CodecRequest::parse` accepts `codecs` (only `hevc`/`av1` strings, else invalid),
  `adaptive`, `decode` (`src/CodecRequest.cpp:13-49`); `apply` sets decode paths then the policy and logs
  "KRDPCTL: codec asked [..] (decode avc=.. hevc=.. av1=..), selected X (hardware|software): reason"
  (`:56-91`). The record: `selected`, `backend`, `reason` (`src/LayoutControl.cpp:745-759`).
- **Brokers:** Console applies the request only while the controller is the only admitted client
  (`server/ConsoleHostController.cpp:1278-1311`, `:2584-2603`). Virtual applies it per connection
  (`server/VirtualSessionTransport.cpp:1950-1964`). Each gets a host `VideoCodecHost` from its own probe and
  arguments (`server/consolehost.cpp:65-66`, `:89`, `:140-147`; `server/virtualhost.cpp:46`, `:65`, `:111-117`), then
  the worker reports what it really has ("Worker video encoders: ...", `server/WorkerCodecBridge.cpp:184-203`), and a
  chosen codec the worker lacks is left at once (`src/VideoStream.cpp:1144-1160`). Worker wire v15
  (`server/ConsoleWorkerWire.h:64`); broker and worker are paired.
- **Encoder backend policy:** HEVC/AV1 use exactly the chosen backend (HardwareOnly/SoftwareOnly); AVC hardware keeps
  HardwareFirst (`src/EncoderSelection.h:22-31`, `:48-98`).
- **Host settings:** brokers read EnvironmentFile fields per scope (`server/BrokerHostSettings.cpp:14-28`:
  `SoftwareEncoding` -> `SOFTWARE_ENCODING`, `Av1Tiles` -> `AV1_TILES`, `VaapiDriver`; Virtual session scope
  `RenderPci`, `VaapiDriver`), validated in `normalize` (`:183-199`) by the privileged helper
  (`server/hostsettingshelper.cpp:295-347`), passed as `--software-encoding`/`--av1-tiles` by the units
  (`server/krdp-console-host.service.in:24`, `:31`, `:36`; `server/krdp-virtual-host.service.in:21`, `:28`, `:33`),
  published sanitized under `/var/lib/farside-public/*.json` (`server/BrokerHostPublicSnapshot.cpp:41-60`). Hal's
  `session.json` already lists `renderDevices` with PCI ids: nvidia 0000:04:00.0 renderD129, amdgpu 0000:c5:00.0
  renderD128, `RenderPci` effective `0000:c5:00.0`.
- **Per-user preferences** (`~/.config/farsideserverrc` of the authenticated user, `server/UserConfiguration.cpp:53`)
  include `SoftwareEncoding` and `Av1Tiles` (`server/BrokerUserSettings.h:22-38`, `server/BrokerUserSettings.cpp:87-94`,
  key list `:126`).
- **KCM:** host "Software encoding" (Automatic / Prefer hardware / Allow the best codec in software) and "AV1 tiles"
  in the Advanced "encoding" section (`src/kcm/brokerhostsettings.cpp:572-576`); the same two in My Preferences
  (`src/kcm/brokerpreferences.cpp:175-179`). Field specs are data-driven (`src/kcm/settingfielddefinition.h:16-81`),
  layout rules in `docs/settings-ui.md`.
- **kcfg pin:** `server/krdpserversettings.kcfg` has `SoftwareEncoding`/`Av1Tiles` (`:43-50`). The legacy migration
  requires the kcfg key set to equal the per-user preference keys plus a fixed special list and pins a digest of the
  defaults (`server/LegacySettingsMigration.cpp:113-127`).

### 2.3 Hardware facts used below

| Host | GPUs (PCI) | Hardware encode today (probe) | Notes |
|---|---|---|---|
| Hal | AMD 780M iGPU 0000:c5:00.0 renderD128 (compositor), RTX 4090 AD102 0000:04:00.0 renderD129 (`lspci`; `/var/lib/farside-public/session.json`) | AVC/HEVC/AV1/AVC444 on AMD VCN 4.0.2 (`~/dev/rdp/archive/CLAUDE-2026-10-04-full.md:131`, `:770`) | NVENC never probed (C1 probe rule); VA-API H.264 max surface 4096 (`archive/CLAUDE-2026-10-04-full.md:148-149`); KWin uses linear buffers (`KWIN_DRM_USE_MODIFIERS=0`, `~/dev/rdp/CLAUDE.md:50`, `archive/CLAUDE-2026-10-04-full.md:175`); NVIDIA userspace mismatch (C7). One VCN, ~2.5x less than cray (`archive :402`). |
| Sol | RTX 2070 TU106 0000:09:00.0 only | HEVC NVENC; AVC and AV1 software only (`evidence/2026-09-30-t05-avc-selection/native-console.log`) | Turing: no AV1 encode or decode. |
| cray | Radeon 8060S, two VCN 4.0.5 | AVC/HEVC/AV1 hw (`evidence/2026-09-29-aud-fix14/cray/12-install-restart.txt`) | |
| ace | Intel N150 Xe-LP | AVC/HEVC hw, AV1 software (`evidence/2026-09-28-release-c27909d/ace/M-ace-journal.txt`) | 4C/4T, often loaded by llama-server. |
| Buzz (client) | Intel UHD 620/CML GT2 | decode: AVC, HEVC in VA-API; AV1 software only (dav1d) (`evidence/2026-09-28-buzz-av1-decode/RESULTS.md:3-14`) | AV1 software decode was the cray incident (`DecodeCaps.h:12-17`). |

FFmpeg 8.0.1 on Hal and Sol lists `h264_nvenc`, `hevc_nvenc`, `av1_nvenc`, the `*_cuvid` decoders and the `cuda`
hwaccel (read-only `ffmpeg -encoders/-decoders/-hwaccels`, 2026-10-09). A listed `av1_nvenc` on Sol is not a
hardware capability (Turing): only a trial open counts.

## 3. Requirements (Steve's final answers, restated as testable rules)

R1 Per connection: an ordered codec list, one encode mode (host side) and one decode mode (this computer), each
Hardware / Software / Any; defaults HEVC, AV1, AVC; Any; Any. R2 Fall through to the next codec in the list when one
is not satisfiable, with a notice saying what was chosen and why. R3 Host: per codec, software encoders Allowed or
Never; the client is honoured only within it; hardware always allowed. R4 Nothing satisfiable: standard AVC with a
notice. R5 Stock clients unaffected. R6 Auto-switches never break the request (Hardware-only never goes to
software; flips only within the list). R7 Multi-GPU: device choice is host policy (per codec preferred device,
Automatic default, load balancing allowed or not); client only displays it; device failure falls through to the next
device, then software only if allowed. R8 NVENC for every codec the 4090 supports that our pipeline can carry, plus
client NVDEC. R9 Cross-GPU cost is measured before it is used by default.

## 4. The model

### 4.1 Encode side (per codec f, at the host)

Inputs: request encode mode E; host allowance `software(f)` (Allowed/Never); hardware devices for f that are
permitted for this route and have capacity (`hw(f)`, section 7); software encoder present (`sw(f)`); runtime blocks
(CPU guard hold, size limit).

| E | hw(f) | sw(f) present | software(f) | Encode outcome | Skip reason |
|---|---|---|---|---|---|
| Hardware | yes | any | any | hardware (best device, 7.2) | |
| Hardware | no | any | any | unavailable | `noHardwareEncoder` |
| Software | any | yes | Allowed | software | |
| Software | any | yes | Never | unavailable | `softwareNotAllowed` |
| Software | any | no | any | unavailable | `noSoftwareEncoder` |
| Any | yes | any | any | hardware | |
| Any | no | yes | Allowed | software | |
| Any | no | yes | Never | unavailable | `softwareNotAllowed` (and `noHardwareEncoder`) |
| Any | no | no | any | unavailable | `noEncoder` |

Runtime refinements: a software candidate held back by the CPU guard counts as unavailable with `cpuGuard`; a
hardware device that cannot encode this stream's size (e.g. `h264_vaapi` above 4096 on Hal, `CodecPolicy` already
knows the pixels, `src/CodecPolicy.h:438`) counts as not present for that codec with `sizeLimit` (Q4).

### 4.2 Decode side (per codec f, at the client)

| D | decHw(f) | decSw(f) | Decode outcome | Skip reason |
|---|---|---|---|---|
| Hardware | yes | any | hardware (device order 8.2) | |
| Hardware | no | any | unavailable | `clientCannotDecodeInHardware` |
| Software | any | yes | software | |
| Software | any | no | unavailable | `clientCannotDecode` |
| Any | yes | any | hardware | |
| Any | no | yes | software | |
| Any | no | no | unavailable | `clientCannotDecode` |

`decHw(avc)` is always false on our client (C2). `decSw(f)` requires FreeRDP pass-through for private codecs
(`GraphicsPipeline.cpp:699-707`) and a libavcodec decoder (`GraphicsPipeline.cpp:742-755`).

### 4.3 Choice, baseline and worked examples

`choice = first f in order with encode(f) != unavailable and decode(f) != unavailable`. If none: **baseline AVC**,
encoded in hardware if the host has an AVC hardware encoder for this size, else software (whatever E and the AVC
allowance say), decoded in software; reply `baseline:true` with the skip list. **Ordering inside Any is
codec-major** (HEVC hw, HEVC sw, AV1 hw, AV1 sw, AVC ...), the literal reading of answer (2); see Q3.

Fixtures (defaults unless stated; Hal = today's AMD-only probe; software allowed everywhere unless stated):

| Case | Hal (AMD hw AVC/HEVC/AV1) | Sol (NVENC HEVC; AVC, AV1 sw) | cray (hw all) | ace (hw AVC/HEVC; AV1 sw) |
|---|---|---|---|---|
| Buzz defaults: HEVC,AV1,AVC / Any / Any | HEVC hw -> hw | HEVC hw (NVENC) -> hw | HEVC hw -> hw | HEVC hw -> hw |
| **Steve's goal:** AV1,HEVC,AVC / Any / Software, AV1 sw allowed | AV1 hw (VCN) -> sw dav1d, tiles auto = rows (decode.av1 sw, `src/CodecPolicy.h:101-105`) | AV1 sw (SVT-AV1, 30 fps cap `:278`, CPU guard) -> sw | AV1 hw -> sw | AV1 sw -> sw; ace's CPU guard likely steps to the next candidate HEVC hw -> sw |
| Goal case, host AV1 software Never | AV1 hw -> sw (allowance irrelevant: hardware) | skip AV1 `softwareNotAllowed`; HEVC hw -> sw; notice | AV1 hw -> sw | skip AV1 `softwareNotAllowed`; HEVC hw -> sw |
| AV1,HEVC,AVC / Hardware / Hardware | skip AV1 `clientCannotDecodeInHardware`; HEVC hw -> hw | skip AV1 `noHardwareEncoder`; HEVC | skip AV1 (Buzz); HEVC | skip AV1 (both reasons); HEVC |
| AVC only / Hardware / Hardware | baseline AVC hw -> sw (`clientCannotDecodeInHardware`) | baseline AVC **sw** -> sw (`noHardwareEncoder` + client) | baseline AVC hw -> sw | baseline AVC hw -> sw |
| HEVC,AV1,AVC / Software / Any | HEVC sw (x265) -> hw | HEVC sw -> hw | HEVC sw -> hw | HEVC sw -> hw |
| HEVC,AV1,AVC / Any / Any, Hal **after** S8 (NVENC present, AMD at capacity, balancing on) | HEVC hw on **4090** -> hw; Stats shows device | n/a | n/a | n/a |

Today's behaviour for comparison: with `allowSoftwarePrivateDecode` off Buzz asks only `[hevc]` and gets HEVC
everywhere; with it on Buzz asks `[hevc,av1]` and Hal/cray answer **AV1** (C1), which is how Buzz ended up decoding
AV1 in software at 10-14 fps. The defaults above reproduce today's default result on every fleet host.

## 5. Protocol (KRDPCTL v2, private channel, additive)

### 5.1 Capability

`capabilities.video` gains (all optional; old clients ignore them):

```json
"video": {
  "codecs": [{"name":"avc420","hw":true,"sw":true}, {"name":"hevc","hw":true,"sw":true}, ...],
  "softwareEncoding": "auto",
  "preferences": 1,
  "software": {"avc":"allowed", "hevc":"allowed", "av1":"never"},
  "encoders": [
    {"codec":"hevc","backend":"vaapi","device":"0000:c5:00.0","name":"AMD Radeon 780M","hw":true},
    {"codec":"hevc","backend":"nvenc","device":"0000:04:00.0","name":"NVIDIA GeForce RTX 4090","hw":true},
    {"codec":"hevc","backend":"libx265","hw":false}
  ]
}
```

- `preferences: 1` = the server honours `order`/`encode`/`decoders` (5.2). Absent = old server: the client sends the
  old record and shows its own fall-through notice from `codecs`.
- `software`: the host ceiling per family (`avc` is `allowed` or `lastResort`, Q7).
- `encoders`: the broker's probe (approved by Steve, answer 5); the worker's report refines it after attach
  (`WorkerCodecBridge.cpp:184-203`), and the `codec` reply is authoritative. Until S7 there is one device per codec.
- `codecs` keeps its meaning for old clients, but `sw` for a family is true only when the allowance permits it
  (generalizes today's `never` rule, `EncoderSupport.cpp:353-357`).

### 5.2 Request (new client, server with `preferences`)

```json
{"type":"codec","v":1,"requestId":"r1",
 "codecs":["hevc","av1"], "adaptive":true, "decode":{"avc":"sw","hevc":"hw","av1":"sw"},
 "order":["av1","hevc","avc"], "encode":"any", "decodeMode":"software",
 "decoders":{"avc":["sw"], "hevc":["sw"], "av1":["sw"]}}
```

- `codecs`, `adaptive`, `decode` stay exactly as today, so an old server keeps working (`CodecRequest.cpp:13-49`
  ignores unknown keys; an `avc` entry in `codecs` would be rejected as invalid, `:34-46`, hence a separate `order`).
- `order`: distinct entries from `avc`, `hevc`, `av1`; `avc` is appended last when missing; unknown names or
  duplicates are `error`/`invalid`.
- `encode`: `hardware` | `software` | `any`. Invalid value = `error`/`invalid`. Missing = legacy behaviour.
- `decoders`: per family the decode paths the client permits under its decode mode, in its preference order
  (`[]` = cannot); this is the client's half of the decision table (4.2). `decodeMode` is informative (logs, stats).
  `decode` (what the client will actually use, for AV1 tiles) = first entry of `decoders`.
- `adaptive` keeps its meaning (the server may switch for link/CPU), now a per-connection setting.

### 5.3 Reply and push

```json
{"type":"codec","v":1,"ok":true,"selected":"hevc","backend":"hardware","reason":"initial choice",
 "encoder":{"backend":"nvenc","device":"0000:09:00.0","name":"NVIDIA GeForce RTX 2070"},
 "decodePath":"sw", "baseline":false,
 "skipped":[{"codec":"av1","why":["softwareNotAllowed"]}]}
```

- `selected`/`backend`/`reason` unchanged (old clients read only these). New: `encoder` (concrete backend:
  `vaapi`/`nvenc`/`libx264`/`libopenh264`/`libx265`/`libsvtav1`/`libaom-av1`, device PCI id and display name for
  hardware), `decodePath` (the path the server assumed: first of `decoders[selected]`), `baseline`, `skipped`
  (every codec before `selected` in `order`, with all reasons from 4.1/4.2 plus `consoleShared`, `notController`,
  `encoderFailed`, `deviceFailed`). Pushes carry the same fields at the switch time.
- Reason codes are stable identifiers (the client localizes them); `reason` stays informative English.

### 5.4 Stats (`stats-sample`, contract (g))

`video` gains `encoder` (concrete backend), `device`, `deviceName`; `policy` gains `order`, `encode`, `decodeMode`,
`allowance` (the three host values) and, when relevant, `balance` (`{"state":"off"|"preferred"|"overflow","from":"<pci>"}`).
Size stays within the < 2 KiB budget (contract line 540): device ids are 12 bytes, names are bounded to 48.

### 5.5 Compatibility matrix

| Client | Server | Behaviour |
|---|---|---|
| new | new (`preferences`) | full model |
| new | old (no `preferences`) | client sends today's record built from its per-connection order/decode mode (private codecs only, hw-only filter = decode mode); server picks by compression; client compares `selected` with its own first choice and shows the notice; encode mode cannot be enforced and the notice says "this PC's Farside version cannot follow the encoding choice" |
| old | new | request without `order`/`encode`: **the client's order is honoured** (Steve, 2026-10-09: "honor clients order, we don't need to keep old selections"). The old `codecs` list becomes the order (AVC is not appended: the client did not list it), encode and decode mode `Any`, decoders from the reported paths (`requestFromRecord()`); `SoftwareEncoding` only limits software through the allowance (`never` = no software HEVC/AV1) and no longer picks hardware-only or best-compression-first. There is no `Request::legacy` and no second selection path (OPT-063). |
| stock RDP | any | no KRDPCTL `codec`; RDPGFX caps pick AVC420/AVC444 as today (`src/CodecPolicy.h:19-23`) |

KRDPCTL `ProtocolVersion` stays 1 (`src/LayoutControl.h:33`): the change is additive and capability-gated.

## 6. Server policy

### 6.1 Where the decision is made

A new pure layer in `src/CodecPolicy.h` (no channel, no encoder):

- `struct Request { QList<Family> order; Mode encode; std::array<QList<DecodePath>,3> decoders; bool adaptive; }`
  (`Mode {Hardware, Software, Any}`), `struct Allowance { bool avcSoftware, hevcSoftware, av1Software; }`.
- `QList<Candidate> candidates(const Request &, const Encoders &, const Allowance &, qint64 pixels)`: walks `order`,
  produces for each family either a `Candidate {family, hardware, device}` or a `Skip {family, reasons}` (tables 4.1
  and 4.2). The baseline candidate is appended last and flagged.
- `select()` takes the candidate list when a `Request` exists: normal link = first candidate not blocked by the CPU
  guard; slow link (adaptive) = the best-compressing candidate in the list (AV1 > HEVC > AVC), never a backend the
  list does not contain; CPU guard steps = preset, then the **next candidate after the current one**, then frame
  rate (existing order, `src/CodecPolicy.h:1085-1210`), never below the baseline. Recovery returns to the first
  candidate. Without a `Request`, today's `select()` runs unchanged except that the allowance is applied to
  `Encoders` first (a family with Never loses its software backend).
- `VideoStream` keeps the request (`d->clientFamilies` becomes the candidate list) and re-runs `candidates()` when
  the worker reports its real encoders (`updateEncoderPolicy`, `src/VideoStream.cpp:1144-1160`) or an encoder becomes
  unavailable (`privateCodecUnavailable`, `:1117-1125`).

### 6.2 Enforcing the host allowance

- The allowance is applied in one place: `Encoders` passed to the policy and to `videoCapabilities()` have their
  software backend cleared for a family whose allowance is Never. `FARSIDE_ENCODERS` and
  `FARSIDE_FORCE_SOFTWARE_ENCODING` (`src/EncoderSupport.cpp:246-282`, `:321-329`) change what the probe *found*;
  the allowance is applied **after** them, so a test override can never re-enable a forbidden software encoder.
- The per-user `SoftwareEncoding` preference is clamped by the allowance (C5): it can choose `auto`/`prefer` among
  allowed backends for legacy clients, never exceed the ceiling.
- AVC: software AVC stays possible as the baseline whatever the allowance (picture first, `CodecPolicy.h:616-619`).
- Changes apply at the next connection (the brokers read settings at start; the snapshot's `application` is
  `broker-restart`). A running connection keeps its policy.

### 6.3 Mid-session rules (R6)

| Event | Allowed move |
|---|---|
| Slow link (adaptive) | to a better-compressing **candidate**; Encode = Hardware only between hardware candidates |
| Link recovered | back to the first candidate |
| CPU guard (software only) | preset, next candidate in order, frame rate; never to a skipped codec. **Only to a codec the client listed** (OPT-063): AVC appended as the baseline is not a step, so a client that fixed its codec (order `[av1]`) stays on it and the guard lowers the frame rate (`mayLeave()`). A step to another software codec carries the frame-rate cap over, and no cap rises again for 60 s after a step (doubled after a flap) |
| Encoder failure / unavailable (OPT-055 ladder, `src/EncoderFailurePolicy.h`) | next device for the same codec (7.4), then next candidate, then baseline |
| `adaptive:false` | only CPU guard and failures (contract line 268-269) |
| Console another viewer admitted | AVC for everyone (C4), back to the request when alone again |

### 6.4 Notice and log

- The server never shows UI; the client builds the notice from the reply (8.4).
- Log (Info, once per decision; switches throttled with the existing LogThrottle, `docs/log-levels.md`):
  `KRDPCTL: codec order [av1,hevc,avc] encode any decode software; skipped av1 (softwareNotAllowed); selected hevc
  (hardware, nvenc 0000:09:00.0): initial choice`. Device failures and balance decisions log one Info line each.

## 7. Multi-GPU hosts and NVIDIA backends

### 7.1 What "Hardware" means with two encoders

"Hardware" = any hardware encoder device (VA-API on AMD/Intel, NVENC) that trial-opens the codec. The client's
modes do not name a device or vendor (Steve, answers 2 and 4). Per codec on Hal (target state after N-slices):

| Codec | AMD VCN (VA-API, capture GPU) | RTX 4090 (NVENC) | Today in our code |
|---|---|---|---|
| AVC420 | yes (≤ 4096) | yes, 4:2:0 (NVENC H.264 max 4096 wide) | VA-API only; no `h264_nvenc` path |
| AVC444 (RDPGFX two-stream) | yes (KPipeWire composite, `archive/CLAUDE-2026-10-04-full.md:671`) | not planned (7.6) | VA-API only |
| HEVC Main 4:2:0 | yes | yes | NVENC exists in KPipeWire but is probed only when VA-API lacks HEVC (`src/EncoderSupport.cpp:301-306`) |
| AV1 Main 4:2:0 | yes | yes on Ada (not on Sol's Turing) | **no `av1_nvenc` path** (KPipeWire knows only `hevc_nvenc`, `kpipewire/src/softwarecodecencoder.cpp:183`, `:216`) |

### 7.2 Device ranking (host policy)

Automatic, per codec and per stream:
1. the **capture GPU** (the device that owns the capture buffers: Console = the GPU the compositor renders the
   captured outputs on; Virtual = the desktop's granted `RenderPci` device) if it has an engine for the codec and
   is not at capacity;
2. other permitted hardware devices, in stable PCI order, **only if load balancing is on** and the capture GPU is
   at capacity, failed, or cannot do the codec/size (e.g. Hal AVC above 4096, or a future host whose iGPU lacks AV1);
3. software, if the encode mode and the allowance permit;
4. the next codec in the list.

A specific preferred device (host setting, 9.2) replaces step 1; if it is unavailable, steps 2-4 follow (a truthful
fallback, `nvidia-backends-design.md:17`). Defaults on the fleet: Hal = AMD, Sol = NVIDIA, cray/ace = their iGPU,
matching the approved NVIDIA design (`nvidia-backends-design.md:11`, `:17`). The capture GPU is determined by the
worker (which GPU's render node can import the captured DMA-BUF / which the compositor uses; `boot_vga` as the
fallback) and logged; S7 settles the mechanism.

Permitted devices: Console workers may use every GPU the host has (subject to the device policy); Virtual workers
only the GPUs in `RenderPci` (`server/BrokerHostSettings.cpp:17`, `:191-199`), and an NVIDIA device additionally
needs its `/dev/nvidia*` nodes and libraries granted, else it is reported unavailable
(`nvidia-backends-design.md:15`). On Hal today Virtual is AMD-only (`RenderPci=0000:c5:00.0`).

### 7.3 Capacity and load balancing (Steve, answer 1)

Signals, per device, sampled at decision points:

| Device | Signal | Source | "At capacity" when |
|---|---|---|---|
| NVIDIA | encoder sessions, encoder utilization, average latency, free memory | NVML `nvmlDeviceGetEncoderStats`, `...GetEncoderUtilization`, `...GetEncoderCapacity`, `...GetMemoryInfo` | sessions ≥ driver limit − our needed sessions (read, never hard-coded; GeForce drivers cap concurrent NVENC sessions), utilization ≥ 85 % over 5 s, or < 512 MiB free |
| AMD | VCN activity | `/sys/class/drm/cardN/device/gpu_metrics` (`vcn_activity`; present on Hal as `card1`) | ≥ 85 % over 5 s |
| any | our streams on it | encoder `encodeMs` p95 vs frame interval, fps shortfall (`video.encodeMs`, `framesEncoded`, contract (g)) | p95 ≥ 70 % of the frame budget (same threshold as `CpuGuardLimit`, `src/CodecPolicy.h:236`) for 10 s, or open failure |

Unknown signals (e.g. NVML failing as on Hal now, C7) count as "no evidence": never balance onto an unknown device;
an unknown preferred device is still tried and its open result decides.

When: at connect (stream creation) and at existing encoder restart points (resize, codec switch, failure ladder,
keyframe-on-demand restarts that already reopen). No live migration of a running encoder for balance alone; a
stream that would benefit is flagged and moves at its next natural restart, at most once per 60 s.

AI workloads on the 4090: NVENC/NVDEC are dedicated engines, so CUDA utilization alone is not a veto
(`nvidia-backends-design.md:19`); free VRAM, failed context creation and measured latency are. Load balancing is off
by default (Q8), so Hal never uses its 4090 for encoding unless Steve turns it on or names it as preferred device.

### 7.4 Device failure and fallback (Steve, answer 6)

Probe failure (e.g. Hal's "NVIDIA VAAPI initialization failure, then AMD probe success",
`archive/CLAUDE-2026-10-04-full.md:131`): the device is skipped for that codec, logged once at Info with the reason.
Runtime open/encode failure: next device for the same codec (if permitted), then software if E and the allowance
permit, then the next candidate, then baseline; each step pushes `codec` with `deviceFailed`/`encoderFailed` and a
keyframe. The OPT-055 error latch and restart ladder (`src/EncoderFailurePolicy.h`) apply per device.

### 7.5 Cross-GPU buffer cost (open measurement M1)

Why it matters: on Hal the capture buffers live in the AMD iGPU's memory (system RAM, linear because of
`KWIN_DRM_USE_MODIFIERS=0`); NVENC on the 4090 needs them in VRAM. Our NVENC path already stages through the CPU
even on one GPU (system-memory YUV420P, `src/EncoderSupport.cpp:72-73`; `nvidia-backends-design.md:24`). Raw sizes
(BGRx 4 B/px; NV12 1.5 B/px) at 60 fps: 1920x1080 497 / 187 MB/s, 2560x1440 885 / 332 MB/s, 5120x1440 1770 / 664
MB/s: small for PCIe 4.0 x16, so the risk is CPU time (de-tiling/reading write-combined GTT memory, colour
conversion) and latency, not bus bandwidth.

M1 plan: a standalone benchmark binary (not installed, no daemons, no PipeWire/KWin/D-Bus), own GBM/VA-API buffers
on renderD128 in the compositor's format (linear BGRx), synthetic moving content, 600 frames per row:

| Path | Steps |
|---|---|
| A0 | AMD VA-API encode of the same buffers (baseline) |
| A | CPU: map dma-buf -> swscale BGRx->NV12 -> CUDA upload -> NVENC |
| B | AMD VPP BGRx->NV12 on the iGPU -> map NV12 -> CUDA upload -> NVENC |
| C | zero-copy import of the AMD dma-buf on the NVIDIA side (Vulkan `VK_EXT_external_memory_dma_buf` + CUDA interop, or CUDA external memory); record "unsupported" if the driver refuses a foreign dma-buf |

Rows: 1920x1080, 2560x1440, 5120x1440 (HEVC/AV1 only; H.264 ≤ 4096) at 30 and 60 fps; codecs H.264, HEVC, AV1.
Measured: per-stage p50/p95 latency, end-to-end capture-ready -> packet, CPU (getrusage, % of one core), bytes moved,
AMD `gpu_busy_percent`/`gpu_metrics` (compositor impact), NVML encoder utilization.

Run first on **Sol** (paths A/B-equivalent single-GPU staging cost on the 2070, no Steve gate needed beyond the
usual Sol rules), then on **Hal only with Steve's explicit go, Hal idle, zero RDP connections, after the C7 driver
fix**. Decision thresholds at 2560x1440@60: best cross-GPU path adds **≤ 3 ms p95** over A0 and **≤ 15 %** of one core
per stream, AMD gfx busy rises ≤ 5 points -> NVENC on Hal is an eligible load-balancing target (and may be offered
as preferred device). Above **8 ms p95 or 30 % of a core** -> NVENC on Hal only as an overflow before software, never
preferred by default. In between: eligible for overflow only.

### 7.6 NVIDIA backend scope (Steve's expansion)

Ada (RTX 4090) per NVIDIA's support matrix, to be confirmed on Hal once NVML works (C7): NVENC H.264 4:2:0 and
4:4:4 8-bit; HEVC 4:2:0 and 4:4:4, 8/10-bit; AV1 4:2:0 8/10-bit; NVDEC H.264 4:2:0, HEVC 4:2:0/4:4:4, AV1 4:2:0.
Turing (Sol's 2070): the same minus AV1 encode and AV1 decode. The 4090 has two NVENC engines per the matrix
(unverified here).

| Item | Decision | Why |
|---|---|---|
| H.264 NVENC 4:2:0 (AVC420) | **Do** (N2) | Sol has no AVC hardware at all today; AVC is the baseline for stock clients. Full range VUI as AVC requires (`src/EncoderSelection.h:12-20`). |
| H.264 NVENC 4:4:4 for AVC444 | **Not as 4:4:4.** RDPGFX AVC444 is two 4:2:0 streams (luma + chroma auxiliary), not an H.264 High 4:4:4 stream, so native NVENC 4:4:4 cannot be carried. Two `h264_nvenc` 4:2:0 sessions with our auxiliary non-reference rewrite might work but the rewrite is VA-API specific (`archive/CLAUDE-2026-10-04-full.md:671`) and doubles NVENC sessions. | Optional N5, only after N2 and a stream-compatibility design; AVC444 stays on VA-API (`nvidia-backends-design.md:11`). |
| HEVC NVENC Main 4:2:0 | exists (KPipeWire `9d6b08c`; `kpipewire/src/pipewireproduce.cpp:970-981`) | generalize device selection (N1) |
| HEVC Main10 / AV1 10-bit | **Not planned** | desktop sources are 8-bit BGRx; no quality gain for the cost |
| HEVC 4:4:4 (Rext) | **Not planned** | our private 0x8001 could carry it, but Buzz (Gen9.5) and AMD VCN clients cannot decode HEVC 4:4:4 in hardware, so it would be a software-decode path; revisit only if a text-clarity case appears |
| AV1 NVENC 4:2:0 8-bit | **Do** (N3), Hal-only proof (C6) | limited range like HEVC/AV1 today; quantiser mapping like AV1-Q (contract (e) table: qindex = 182 − 1.18 × quality; verify `av1_nvenc` CQP takes qindex 0-255); tiles via `av1_nvenc` tile options mapped from `resolveAv1Tiles` (`src/CodecPolicy.h:101-105`); padding/display size measured like `docs/amd-encoder-padding.md` (the client's crop rule is AMD-specific, `krdp-client/src/rdp/PrivateFrameGeometry.h:12`); a sequence header on every keyframe (the R6 lesson: a keyframe without one cannot start a decoder) |
| NVDEC AV1 (client) | **Do** (S6) | FFmpeg's native `av1` decoder with the CUDA hwaccel, like HEVC (`GraphicsPipeline.cpp:553-562`); Ampere or newer only |
| NVDEC H.264 (client) | **Not possible without replacing FreeRDP's AVC decode** (C2) | separate project |

All NVENC work shares: device by PCI id mapped to the CUDA ordinal (`cuDeviceGetPCIBusId`), not `"0"`
(`kpipewire/src/softwarecodecencoder.cpp:223-240`, `:330-336`; `src/EncoderSupport.cpp:79`); low latency (no
B-frames, no lookahead, `tune ull`, `preset p1-p4`); IDR on demand; reopen on resize; CQP for quality mode with the
existing QP/qindex maps and a bitrate mode for the slow-link target; NVENC errors (out of memory, session limit) feed
the OPT-055 ladder; the backend reports `hardware` (KPipeWire's NVENC runs from the hardware branch,
`pipewireproduce.cpp:973-981`) so the CPU guard does not apply, but the CPU staging time is reported (8.5).

## 8. Client

### 8.1 Per-connection settings and storage

New `Connection` fields (`src/core/Connection.h`), JSON keys and defaults for old profiles:

| Field | JSON key | Values | Default (missing key) |
|---|---|---|---|
| `codecOrder` | `codecOrder` | ordered list of `hevc`, `av1`, `avc`; AVC always present | `["hevc","av1","avc"]` |
| `codecsEnabled` | (folded into order: a disabled codec is omitted; AVC cannot be omitted) | | all three |
| `encodeMode` | `encodeMode` | `hardware`/`software`/`any` | `any` |
| `decodeMode` | `decodeMode` | `hardware`/`software`/`any` | `any` |
| `adaptiveCodec` | `adaptiveCodec` | bool | `true` |

Written only when not the default (the `ChromaSettings` convention, `ChromaSettings.cpp:99-109`), read through
`connectionFromJson`/`applyFormFields` like `requestClientMonitors`. Unknown values keep the default and log once.

### 8.2 Decode devices on a two-GPU client (Hal as client)

- The probe becomes a per-codec list of usable decoders: every VA-API node that has the profile (not only the first,
  `VaapiProbe.cpp:122-139`), plus NVDEC per CUDA device for HEVC and (Ampere+) AV1, each with PCI id and name.
- Order for Hardware/Any: the VA-API device of the GPU that presents the window first (frames are downloaded to the
  CPU anyway, `GraphicsPipeline.cpp:613-620`, so this is about the cheapest download), then other VA-API devices,
  then NVDEC; Any then adds software. On Hal: AMD VA-API, then NVDEC (4090). On Buzz: Intel VA-API.
- `FARSIDE_HEVC_DECODER=nvdec` (generalized to `FARSIDE_DECODER=nvdec|vaapi`, applying to HEVC and AV1) only
  reorders the hardware devices; it never permits software under Decode = Hardware. `KRDPC_HW_DECODE=0` removes all
  hardware (Decode = Hardware then falls to the baseline, and the notice names the variable). `KRDPC_RENDER_NODE`
  keeps forcing the VA-API node.
- Enforcement (C8): under Decode = Hardware a decoder that does not open, or fails before its first picture, tries the
  next hardware device; if none, the client marks the codec "cannot decode in hardware" for this session and sends a
  new `codec` request (the existing per-connection fallback path, `App.cpp:686-709`), never a silent software retry.
  Under Any the software retry stays and the change is pushed to Stats.
- The device that decoded each surface is kept (`privateDecodePath` grows a device id) and shown in Stats.

### 8.3 Edit PC: a new "Video" card

Placed after Display and before Devices (`ConnectionForm.qml:511-577`). Kirigami FormCard, KDE HIG wording:

- **Codec order** (`FormButtonDelegate` "Codecs", description "HEVC, then AV1, then H.264") opening a small dialog with
  a reorderable list: each row a checkbox, the codec name, a one-line hint ("Widely decoded in hardware", "Best
  compression; often decoded in software", "Standard; always available") and Move up / Move down buttons (keyboard
  accessible). The H.264 row's checkbox is checked and disabled with the tooltip "Always used when nothing above can
  be used" (Q2).
- **Encoding on the host** (`FormComboBoxDelegate`): "Hardware first, then software" (any), "Hardware only",
  "Software only". Help: "Software encoding uses the host's processor and is only used if the host allows it for
  that codec."
- **Decoding on this computer**: same three entries. The description is live from the probe, e.g. "This computer
  decodes HEVC in hardware (Intel). AV1 only in software." or "...HEVC and AV1 in hardware (AMD; NVIDIA also
  available)."
- **Switch codecs when the network is slow** (`FormSwitchDelegate`, the per-connection `adaptiveCodec`): "Only between
  the codecs and modes chosen above."
- Debug form fields (`ConnectionForm.qml:280-305`): `codecOrder=av1,hevc,avc`, `encodeMode=`, `decodeMode=`,
  `adaptiveCodec=`.

### 8.4 Notices (fall-through, refusal, baseline)

Shown once per connection, built from `skipped`/`baseline` (new server) or from the client's own comparison (old server):

- "Sol doesn't allow software AV1 encoding, so HEVC is used (hardware on Sol, software here)."
- "This computer can't decode AV1 in hardware, so HEVC is used."
- "Nothing in your codec list can be used with hardware only, so standard H.264 is used (software on Sol and here)."
- "Another client is watching this console, so H.264 is used for everyone." (C4)
- "Hardware decoding is turned off on this computer (KRDPC_HW_DECODE=0)."

The existing "%1 can't send %2, so %3 is used." (`App.cpp:910-911`) is replaced by these.

### 8.5 Stats panel and quality meter (5b)

- Stats shows two lines: "Encoding: HEVC, hardware (NVIDIA GeForce RTX 2070)" and "Decoding: HEVC, hardware
  (Intel VA-API)"; the codec event marker names the reason code.
- The Host encoder row keeps using the software encoder load only for a software backend (`SessionStats.cpp:526`):
  correct as long as `backend` is truthful (NVENC = hardware). New optional `video.stagingMs` (CPU download/convert
  for NVENC) feeds the same row for a staged hardware encoder, because a hardware backend can still be CPU-limited.
- This computer stays decode-time based (`ConnectionQuality.h:298-300`) and becomes correct for any device; the
  device is display-only and never feeds adaptive quality.
- The server's CPU guard stays software-only (`src/CodecPolicy.h:448-450`): with Encode = Hardware it never runs; with
  Software it steps only inside the candidate list (6.3).

### 8.6 Migration of the global Advanced settings

- One-shot at first start of the new client (beside `LegacyMigration::run()`, `src/main.cpp:565`, `:597`): every saved
  connection without the new keys gets `codecOrder` = the global `PrivateCodecOrder` (if the user had set one,
  `ChromaSettings.cpp:198-213`) + `avc`, else the default; `decodeMode = any`; `encodeMode = any`; `adaptiveCodec` =
  global `AdaptivePrivateCodecs`. The global `[Chroma]` codec keys are then removed (the user's file is
  backed up first, as with earlier migrations).
- The Advanced page loses "Farside codec order", "Switch codec when latency stays high" and "Allow software
  HEVC/AV1 decoding"; "Prefer AVC420" stays global (it is chroma, not codec choice).
- Debug: `allow-sw-decode=` is removed and replaced by `video=<order>/<encode>/<decode>` (e.g.
  `video=av1,hevc,avc/any/software`); `private-codec=` stays acceptance-only and overrides the order for the process.

## 9. Host settings and KCM

### 9.1 New broker keys (per route, Console and Virtual)

| Key | Env | Argument | Values | Default |
|---|---|---|---|---|
| `SoftwareAvc` | `FARSIDE_<ROUTE>_SOFTWARE_AVC` | `--software-avc` | `auto`/`allowed`/`last-resort` | `auto` |
| `SoftwareHevc` | `FARSIDE_<ROUTE>_SOFTWARE_HEVC` | `--software-hevc` | `auto`/`allowed`/`never` | `auto` |
| `SoftwareAv1` | `FARSIDE_<ROUTE>_SOFTWARE_AV1` | `--software-av1` | `auto`/`allowed`/`never` | `auto` |

- `auto` derives from the existing `SoftwareEncoding`: `never` -> Never (AVC: last resort); `auto`/`prefer` ->
  Allowed. No file rewrite is needed and old files keep their meaning (migration by derivation).
- `SoftwareEncoding` stays, relabelled "Codec choice for older Farside apps" (Advanced): it governs only requests
  without `order`/`encode`. `prefer` keeps its old meaning for those clients only; new clients express it as
  "Software only" or an AV1-first order.
- Files: fields list (`server/BrokerHostSettings.cpp:14-28`), `normalize` (`:183-199`), unit `Environment=` defaults and
  `ExecStart` arguments (`server/krdp-console-host.service.in:24-36`, `server/krdp-virtual-host.service.in:21-33`),
  argument parsing (`server/consolehost.cpp:65-97`, `server/virtualhost.cpp:46-67`), the helper reuses
  `BrokerHostSettings::parse/edit` so its validation follows (`server/hostsettingshelper.cpp:295-347`).
- Public snapshot: the keys appear in `values`/`defaults`/`effective` automatically
  (`server/BrokerHostPublicSnapshot.cpp:48`); add a sanitized `videoEncoders` list (codec, backend, device PCI, name,
  hw) from the broker's last probe, through `pick()` (`:30-35`).
- **kcfg: no change.** These are host keys; `krdpserversettings.kcfg` is the per-user/legacy schema whose key set and
  default digest are pinned (`server/LegacySettingsMigration.cpp:113-127`). Only if Steve wants a per-user allowance
  would the pin (special list `:116-117`, digest `:126`) and `BrokerUserSettings::preferenceKeys()`
  (`server/BrokerUserSettings.cpp:126`) need a deliberate update; this design does not.
- Per route vs per host: per route (Q6) because the existing storage, units and helper are per route and Virtual
  admins may want to be stricter; the KCM shows the rows on both route pages with the same defaults.

Later (S8, after S7 and M1): `EncoderDeviceAvc|Hevc|Av1` = `auto` or `vaapi:<PCI>`/`nvenc:<PCI>` (validated against
the PCI regex already used for `RenderPci`, `BrokerHostSettings.cpp:193-197`, and against the snapshot's device list)
and `EncoderLoadBalancing` = `true`/`false` (default `false`, Q8).

### 9.2 KCM

Console and Virtual pages, "Picture and sound" section (not Advanced: it is a policy decision), following
`docs/settings-ui.md` and the data-driven `add()` (`src/kcm/brokerhostsettings.cpp:549-556`):

- Group label "Software encoding", three rows: "H.264 (AVC)": Allowed / Only as a last resort; "HEVC": Allowed /
  Never; "AV1": Allowed / Never; each with "Use default" (= derived). Help (?): "Hardware encoders are always used when
  available. Connections that ask for software encoding get it only for codecs allowed here." A row shows
  `unavailable` text when the probe found no software encoder for that codec.
- My Preferences "Software encoding" (`src/kcm/brokerpreferences.cpp:175-177`) gains the help "Limited by what the host
  allows" and is clamped (C5).
- S8 adds "Encoding device" per codec (Automatic / the devices from the snapshot by name and PCI id) and "Use other
  graphics devices when busy" in the Advanced group of each route.
- Tests: `KcmSettingsTest`, `BrokerHostsPageTest`, `BrokerHostSettingsModelTest` field definitions; offscreen renders
  compared with the approved design before delivery (`docs/settings-ui.md`).

## 10. Edge cases

- **Nothing satisfiable / Hardware-only on a host with no hardware at all** (a Virtual desktop without a GPU grant):
  baseline AVC software, `baseline:true`, notice. Never a black screen.
- **Host removes software permission mid-connection:** applies at the next connect (broker restart); the running
  connection keeps its candidates.
- **Reconnect/resume:** the client resends the same request from the profile; Virtual reattach keeps the per-connection
  request (`VirtualSessionTransport.cpp:1950-1964`); the per-connection decoder-failure fallback still empties the
  private list for that connection (`App.cpp:884-885`).
- **Two Console clients:** C4 (AVC for everyone, controller's request resumes when alone).
- **AVC444:** AVC family. Encode Hardware/Any with a hardware 4:4:4 path and client caps -> AVC444 (unless Prefer AVC420);
  Encode Software -> AVC420 (software H.264 is 4:2:0, contract (e) table). Client decode is software either way (C2).
- **AV1 tiles/padding:** `Av1Tiles` stays a host setting; `decode.av1` keeps driving `auto` (`src/CodecPolicy.h:101-105`).
  NVENC AV1 padding is measured before it is advertised (N3).
- **Sol HEVC NVENC is hardware** (reported `hardware`, `pipewireproduce.cpp:973-981`); Encode = Hardware accepts it.
- **CPU guard and slow link:** 6.3.
- **Stock clients:** unchanged (5.5). **Old client + new server:** legacy selection plus allowance. **New client + old
  server:** 5.5 row 2.
- **Hal VA-API H.264 above 4096** (5120x1440 single surface): `sizeLimit` on the AMD device; Any -> next device (if
  balancing) or software; Hardware -> next codec (HEVC/AV1 accept 8192 wide). Multi-monitor mode already splits per
  monitor.
- **Driver mismatch / NVML missing** (Hal now): NVIDIA devices fail their probe and are skipped; nothing else changes.

## 11. Test plan

### 11.1 Pure (server `autotests/CodecPolicyTest.cpp`, new `CodecSelectionTest`)

Fixtures: `Encoders` + device lists for Hal-today, Hal-with-NVENC (two devices), Sol, cray, ace, software-only;
client decode fixtures Buzz (HEVC hw, AV1 sw), Hal-client (AMD + NVDEC), no-hardware. Rows: all 3 x 3 x allowance
combinations of tables 4.1/4.2 for each codec, every row of the 4.3 table, codec-major order, baseline flags, skip
reasons. Mid-session: slow link within candidates, recovery to first, CPU guard next-candidate, Hardware-only never
software, failure ladder per device, Console shared. Allowance after `FARSIDE_ENCODERS` override. Device ranking:
capture GPU first, balancing off/on, capacity signals unknown/known, preferred device unavailable.
`SlowLinkReplayTest` re-run unchanged for legacy requests.

### 11.2 Wire

New `CodecRequestTest`: valid new record, old record only, invalid `order`/`encode`/`decoders`, `avc` missing from
order, duplicates; reply fields; capability JSON (`EncoderSupportTest`); stats sample size with 16 surfaces and the
new fields (< 2 KiB). Client `ControlCapabilitiesTest`, `CodecPolicyTest`, `DecodeCapsTest` for the `decoders` map and
the old-server fallback.

### 11.3 Brokers and settings

`ConsoleHostControllerTest` (controller alone/shared, push fields), `VirtualSessionTransportTest` (per connection,
attach), `BrokerHostSettingsTest` (new keys, `auto` derivation from each `SoftwareEncoding`), `BrokerUserSettingsTest`
(clamp), `BrokerHostRuntimeTest`/snapshot sanitize, `LegacySettingsMigrationTest` unchanged (proves no kcfg change),
KCM tests (9.2).

### 11.4 Client model/QML (offscreen, Buzz)

`ConnectionStoreTest`/`ConnectionModelTest` (defaults, JSON round trip, migration from global keys), a Video-card QML
test (reorder by keyboard, AVC row locked, combos, debug fields), `SessionStatsTest`/`StatsPanelTest` (encode/decode
lines, reason markers), `PrivateDecodeTest` (Decode = Hardware never retries in software; Any does), `AppTest` notices.

### 11.5 Native and end-to-end

- Sol native (daemon tests allowed on Sol): worker probe with real NVENC HEVC and, after N2, H.264; `FARSIDE_ENCODERS`
  to emulate Hal/cray/ace tables; the allowance ceiling against overrides.
- Sol server / Buzz client matrix, reusing the structure of `evidence/2026-10-09-av1-software-test/` (`baseline.sh`,
  `buzz-client.sh`, `sampler.py`, `ssim.sh`): rows = the 4.3 table for Sol plus emulated hosts; per row record the
  reply (selected, encoder, skipped), first frame, 60 s motion fps, server/client CPU, decode p95, notices, Stats.
- Only Hal can prove: AV1 NVENC (N3), AV1 NVDEC on a client, two-GPU ranking/balancing, M1. Hal runs are
  daemon-free and need Steve's explicit go each time; live RDP checks on Hal are Steve's hands-on pass.
- cray/ace: optional confirmation rows of the 4.3 table (no code path specific to them).

### 11.6 AV1 software guidance

When `evidence/2026-10-09-av1-software-test/RESULTS.md` exists, its Sol-encode CPU and Buzz-decode fps set the help
text for "Software only" and the AV1 hint in the codec dialog (e.g. "AV1 in software reaches N fps at 1080p on a
4-core laptop").

## 12. Slices

| Slice | Content | Done when | Size | Risk | Starts |
|---|---|---|---|---|---|
| S0 | Spec sign-off, open questions answered | Steve approves; Q1-Q8 decided | - | - | now |
| S1 | Server pure policy: `Request`, `Allowance`, `candidates()`, order-honouring `select()`, mid-session rules | 11.1 table green; legacy tests unchanged | M | behaviour change for new requests only | after S0 |
| S2 | Protocol: parse/apply new fields, reply/push fields, `capabilities.video.preferences/software/encoders`, stats fields; contract (e)/(g) updated | 11.2 green; contract committed | M | wire compat (5.5) | after S1 |
| S3 | Host settings: keys, units, args, helper, snapshot, KCM rows, user-pref clamp | 11.3 green; offscreen KCM renders reviewed; Sol broker restart reads them | M | unit/ExecStart change needs paired package | after S1 (parallel with S2) |
| S4 | Client model: `Connection` fields, JSON, migration, debug actions, old-server fallback request | 11.4 model tests green | M | profile migration (backup first) | after S0 |
| S5 | Client UI: Video card, notices, Stats lines | QML tests green; Buzz screenshots compared with this design | M | HIG review | after S2 + S4 |
| S6 | Client decode: enforcement, multi-device probe list, device reporting, NVDEC AV1 | 11.4 decode tests; Buzz VA-API rows; Hal NVDEC only with Steve | M | NVDEC AV1 proof Hal-only | after S0 (enforcement), AV1 NVDEC after C7 fix |
| S7 | Server device inventory: probe every VA-API node and CUDA device per codec, PCI-stable identity, CUDA PCI mapping, capture-GPU detection, worker wire v16 | Sol/Hal (offscreen, Steve OK) inventories match `lspci`; paired broker/worker | L | **is the PCI-stable GPU selection work** of the NVIDIA design | after S0 |
| S8 | Device policy + load balancing: `EncoderDevice*`, `EncoderLoadBalancing`, capacity signals, connect/restart decisions | 11.1 device rows; Sol single-GPU rows; Hal two-GPU only with Steve | L | 4090 contention with AI | after S7 + M1 |
| N1 | KPipeWire NVENC generalization: `h264_nvenc`/`av1_nvenc` library kinds, device by PCI | KPipeWire unit tests; Sol trial opens H.264/HEVC | M | KPipeWire pin bump | after S0 |
| N2 | AVC420 NVENC | Sol: Buzz decodes AVC from NVENC; full range; IDR; resize; ladder | M | AVC HardwareFirst semantics | after N1 |
| N3 | AV1 NVENC (Ada) | Hal (Steve go, after C7): padding table, sequence header per IDR, tiles, qindex map, decodes on Buzz (sw) and Hal (VA-API/NVDEC) | M | Hal-only proof | after N1 + C7 fix |
| N4 | NVENC rate control/keyframe/resize/OPT-055 ladder hardening (all codecs) | Sol fault-injection rows | M | | after N1 |
| N5 | AVC444 on NVENC (optional) | separate design accepted | L | stream compatibility | after N2, only if wanted |
| M1 | Cross-GPU benchmark (7.5) | Sol staging rows, then Hal rows with Steve's go; thresholds applied | S | Hal live-use | Sol part after S0; Hal part after C7 fix + go |
| S9 | Acceptance: Sol/Buzz matrix (11.5), Steve's Hal hands-on, fleet rollout per release rules | matrix passes; Steve confirms in a real window | M | | after S5, S6 (+ S8/N-slices for their rows) |

Independent after sign-off (Steve's request): **NVENC AVC/AV1 backends (N1-N4)**, **NVDEC decode (S6)**, **device
inventory (S7)**, **measurement (M1, Sol part)**. **Load balancing and device policy (S8)** wait for S7 and M1.
Version implications: client 0.9.0 (S4-S6); server gains `video.preferences` (S2); S7/S8 bump the worker wire to v16
(`server/ConsoleWorkerWire.h:64`), so broker and worker must be installed together; N-slices bump the KPipeWire pin.

## 13. Open questions for Steve (recommended answers)

1. **Per-codec "Hardware encoders: Allowed/Never" on the host?** Recommend **no**. The device policy (S8) covers "keep
   the 4090 for AI" (Automatic never picks it on Hal unless load balancing is on or it is named), and a client
   wanting no hardware can ask "Software only".
2. **Is AVC an orderable entry?** Recommend **orderable but always enabled** (checkbox locked), default last. Putting
   it first means "H.264 unless the mode forbids it", which is a valid choice (e.g. Hardware-only on cray picks AVC
   hardware).
3. **"Any" order: codec-major or backend-major?** Codec-major (HEVC hw, HEVC sw, AV1 hw, ...) follows your answer (2)
   and makes your goal case work on Sol (AV1 software there); backend-major would give Sol HEVC hardware instead.
   Recommend **codec-major**. Consequence: a host with no HEVC hardware but software allowed now gets software HEVC
   (30 fps cap, CPU guard) where today it gets software H.264.
4. **Hardware that cannot do the case (e.g. AMD H.264 above 4096):** recommend it counts as **unavailable for that
   stream** (`sizeLimit`) and the normal rules apply; no "hardware may be slower than software" heuristics.
5. **Saved defaults?** Recommend **client-side "defaults for new PCs"** later (one place in Advanced), not a server
   per-host default; new PCs use HEVC, AV1, AVC / Any / Any until then.
6. **Allowance per route or per host?** Recommend **per route** (existing storage/units/helper), shown on both pages
   with the same defaults.
7. **What does "Never" mean for AVC?** Software H.264 is the picture of last resort for stock clients and the
   baseline. Recommend **"Allowed / Only as a last resort"** for AVC (today's `never`), not a hard Never.
8. **Device policy defaults:** recommend the client **never** asks for a device or vendor (as you answered), load
   balancing **off by default**, Hal default device Automatic (= AMD); the NVIDIA design's "a client may request a
   backend" (`nvidia-backends-design.md:17`) is superseded by this.

## 14. Steps that need Steve

- Reboot Hal when convenient to clear the NVIDIA driver/library mismatch (C7); nothing NVIDIA on Hal can be measured
  before that.
- M1 on Hal: explicit go, Hal idle, zero RDP connections; the benchmark starts no daemons.
- N3/S6 AV1 NVENC/NVDEC proof on Hal: explicit go per run; live RDP in a real window is Steve's hands-on check.

## Sign-off 2026-10-09 (S0)

Steve signed off the design with the recommended answer to every open question in section 13:

- Codec list default HEVC, AV1, AVC; Encoding on the host default Any; Decoding on this computer default Any.
- One global (not per codec) Hardware / Software / Any pair per connection.
- Host: per-codec software encoders Allowed / Never; for AVC Allowed or "Only as a last resort" (Q7). No per-codec
  hardware switch on the host (Q1).
- AVC is always in the list but can be moved (Q2).
- "Any" tries every way to send the first codec (hardware, then software if allowed) before the next codec (Q3, codec-major).
- Hardware that cannot encode the screen size counts as unavailable for that stream (Q4).
- Defaults for new PCs live on the client (Q5).
- The software allowance is per route (Console, Virtual), not per host (Q6).
- Load balancing is off by default; the device is host policy (per codec Automatic or a PCI id); the client never
  picks a device (Q8).
- NVENC for every codec the RTX 4090 supports is in scope (N1-N4). AV1 NVENC/NVDEC can be proven on Hal only after
  its NVIDIA driver is reloaded by a reboot (Hal's `nvidia-smi` currently fails); Sol's RTX 2070 has no AV1.

Constraint added the same day: Hal's RTX 4090 runs a training job; nothing may touch Hal's GPUs, and Hal must not be
rebooted, until Steve says so. Hal-side work is source and fake-only unit tests at lowest priority.
