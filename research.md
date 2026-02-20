# KRdp Research: Wayland Damage + ICA/Thinwire-Inspired Optimization

## Goal
Reduce encoded bandwidth by leveraging compositor damage metadata and protocol-side region optimization, inspired by ICA/Thinwire and RDPGFX region/caching behavior.

## Status Snapshot (2026-02-20)
- `OPT-001` Damage metadata plumbed through encoded stream and consumed in KRDP: `DONE`.
- `OPT-002` Damage-first send path with rectangle coalescing: `DONE`.
- `OPT-003` Packet/metadata pairing with fallback and resync behavior: `DONE`.
- `OPT-004` Tile activity classification with per-region quality bias: `DONE`.
- `OPT-005` Progressive refinement after motion settles (with cooldown): `DONE`.
- `OPT-006` Congestion adaptation (frame rate + QP bias): `DONE`.
- `OPT-007` H264 Main preference and fallback profile handling: `DONE`.
- `OPT-008` AVC444 negotiation scaffold with AVC420 fallback and intent bias: `DONE` (transport remains AVC420 by design; true AVC444 is tracked separately in `OPT-010`).
- `OPT-009` True multi-monitor protocol layout (server advertises multiple monitors/surfaces): `TODO`.
- `OPT-010` True AVC444 transport path end-to-end: `TODO`.
- `OPT-011` Automatic GPU encode-device selection (avoid decode-only VAAPI backends): `TODO`.
- `OPT-012` Explicit tile/content cache reuse strategy: `TODO`.

## Tracking Rule
- Every optimization item must have a stable ID in the form `OPT-###`.
- Every ID must carry a status: `TODO`, `WIP`, `BLOCKED`, `PARTIAL`, or `DONE`.
- When status changes, update this file with the date and short reason.

## Current KRdp Capture Path (Source Evidence)
KRdp already uses PipeWire and encoded streams.

- `src/AbstractSession.cpp` constructs `PipeWireEncodedStream` and configures framerate/quality.
- `src/PlasmaScreencastV1Session.cpp` uses PipeWire encoded streams and sets `H264Baseline` encoder.
- `src/PortalSession.cpp` (not shown here) also configures PipeWire encoded streams.
- Build links to `KPipeWire` and `KPipeWireRecord` in `CMakeLists.txt` and `src/CMakeLists.txt`.

This confirms the current pipeline is PipeWire-based and already receives encoded H.264 packets.
KRDP now consumes encoded frame metadata (damage/size/PTS) where available and falls back safely when metadata is missing or delayed.

## Wayland / PipeWire Damage Metadata
The key opportunity is to get damage rectangles per frame and use them to constrain what we encode or transmit.

- `ext-image-copy-capture-v1` provides damage events for each frame; first frame is full damage, subsequent frames are deltas. It also supports client-provided `damage_buffer` metadata to reduce copies.
- `wlr-screencopy-unstable-v1` provides `copy_with_damage` and a damage event list.
- PipeWire exposes damage via `SPA_META_VideoDamage` metadata on buffers.

If KRdp uses PipeWire frames (raw or encoded), confirm whether `KPipeWire` or `PipeWireEncodedStream` exposes `SPA_META_VideoDamage` and whether we can propagate it into KRdp’s encoder or region logic.

## KPipeWire Findings (How Damage Is Exposed)
The KPipeWire library already parses PipeWire damage metadata, but it is only available on **raw** streams, not on the encoded stream API currently used by KRdp.

- `PipeWireSourceStream` has `setDamageEnabled(bool)` and emits `frameReceived(const PipeWireFrame &frame)` where `PipeWireFrame` includes `std::optional<QRegion> damage`.  
- In `PipeWireSourceStream::handleFrame`, KPipeWire reads `SPA_META_VideoDamage` and converts it into a `QRegion`.  
- The damage meta is only requested if `setDamageEnabled(true)` was set **before** stream parameter negotiation (it toggles `m_withDamage`, which controls whether `SPA_META_VideoDamage` is added to the stream params).  
- `PipeWireEncodedStream` does **not** expose damage; `PipeWireEncodedStream::Packet` only carries encoded bytes + keyframe flag.  
- `PipeWireProduce` (used by `PipeWireEncodedStream`) constructs a `PipeWireSourceStream` but never calls `setDamageEnabled(true)`, so damage metadata is not requested by default.

Implication: to tap into damage information in KRdp, either:
1) Use `PipeWireSourceStream` directly and consume `PipeWireFrame.damage` (then handle encoding yourself or augment KPipeWire’s encode path), or  
2) Extend KPipeWire to plumb damage through `PipeWireEncodedStream` (e.g., enable damage in `PipeWireProduce::initialize()` and add a new signal or metadata on `Packet`).

### ABI-safe patch plan (Option B)
Patch file (private fork): `patches/kpipewire/0001-damage-metadata-encoded-stream.patch`

Changes included:
- Add `PipeWireBaseEncodedStream::setDamageEnabled(bool)` and `damageEnabled()` (must be set before `start()`).
- Enable `SPA_META_VideoDamage` on the underlying `PipeWireSourceStream` when damage is enabled.
- Add `PipeWireEncodedFrameMeta` + new signal `PipeWireEncodedStream::frameMetadata(...)` carrying damage/sequence/pts and size.
- Register the new meta type so cross-thread signal delivery works.

Why it’s safe:
- All changes are additive (no existing symbol changes).
- Damage is opt-in (default off), so existing apps remain unchanged.
- No changes to existing signals or packet layout.

### Private fork strategy (KRdp-only now, upstreamable later)
Recommended approach:
- Keep the patch in `patches/kpipewire/` and apply it to a private KPipeWire fork.
- In KRdp, add a CMake option `KRDP_USE_BUNDLED_KPIPEWIRE`:
  - OFF (default): use system KPipeWire.
  - ON: build and link against a vendored fork in `third_party/kpipewire` (git submodule or subtree).
- Maintain a clean patch series (like the file above) so upstreaming later is straightforward.

Minimal KRdp usage (once patched KPipeWire is in use):
- Before starting the stream: `encodedStream->setDamageEnabled(true);`
- Connect `frameMetadata` and cache the damage region per-frame for transport decisions.

## Build + Run Guide (Private Prefix, No System Interference)
These steps keep the patched KPipeWire isolated from system packages. This is the recommended approach on Kubuntu.

### 1) Build + Install KPipeWire (private prefix)
Private prefix path (recommended):
- `/home/westers/dev/krdp/.deps/kpipewire`

Commands:
```bash
cmake -S /path/to/kpipewire -B /path/to/kpipewire/build \
  -DCMAKE_INSTALL_PREFIX=/home/westers/dev/krdp/.deps/kpipewire \
  -DBUILD_TESTING=OFF
cmake --build /path/to/kpipewire/build
cmake --install /path/to/kpipewire/build
```

### 2) Build KRdp against the private KPipeWire
Point CMake at the private prefix so `find_package(KPipeWire)` resolves there first:
```bash
cmake -S /home/westers/dev/krdp -B /home/westers/dev/krdp/build \
  -DCMAKE_PREFIX_PATH=/home/westers/dev/krdp/.deps/kpipewire
cmake --build /home/westers/dev/krdp/build
```

### 3) Run without touching system packages
At runtime, ensure KRdp loads the private KPipeWire:
```bash
LD_LIBRARY_PATH=/home/westers/dev/krdp/.deps/kpipewire/lib:$LD_LIBRARY_PATH \
  /home/westers/dev/krdp/build/path/to/krdp_binary
```

### 4) Rebuild after system or dependency changes
If Qt/KF6/PipeWire changes (e.g., after `apt upgrade`), rebuild in this order:
1. Rebuild and reinstall KPipeWire in the private prefix.  
2. Reconfigure and rebuild KRdp (to refresh CMake cache and relink).

If builds become inconsistent, delete the build dirs (not the source or prefix) and rebuild:
```bash
rm -rf /path/to/kpipewire/build /home/westers/dev/krdp/build
```

### 5) No interference with system packages (what not to do)
Avoid installing the fork into system paths like `/usr` or `/usr/local`.  
Do **not** replace `libkpipewire6` from apt.  
The private prefix approach keeps your system runtime and desktop components safe.

## ICA/Thinwire and RDPGFX Patterns Worth Borrowing
These are proven techniques to reduce bandwidth while maintaining UI quality.

- ICA/Thinwire uses region classification: transient (video-like) vs. static (text/UI). Transient regions use video codecs; static regions use JPEG/RLE/lossless tiles, with text overlays to keep crispness.
- RDPGFX (AVC) allows region rectangles to crop encoded content (`regionRects`), aligned to 16x16 macroblocks.
- RDPGFX uses surface/tile caches to avoid resending unchanged UI elements.
- RemoteFX has progressive refinement (send low quality first, then improve) to keep responsiveness under congestion.

## Proposed Enhancements For KRdp
These steps are ordered by likely ROI and feasibility given the current PipeWire-based pipeline.

1. Damage-first send decisions
- If we can access damage metadata from PipeWire, only transmit damaged regions. Coalesce nearby rectangles to reduce metadata overhead.
- Align regions to 16x16 blocks if we adopt an RDPGFX-like AVC region mask approach.

2. Region classification (Thinwire-style)
- Track recent damage history for each region/tile.
- Encode transient regions with video (H.264/H.265/AV1), static regions with JPEG/RLE or lossless tile codec.
- Consider text overlay for crisp UI (optional but strong quality gain).

3. Cache / tile reuse
- Add explicit tile cache IDs at protocol level, similar to RDPGFX surface cache operations.
- Maintain cache eviction policy based on usage frequency and age.

4. Progressive refinement under congestion
- Send low-quality deltas first; follow with refinement updates when bandwidth recovers.

## Open Questions To Resolve In Source
- Does `PipeWireEncodedStream::Packet` carry damage metadata, or only encoded frames? If not, do we need to switch to a raw `PipeWireSourceStream` to compute damage locally or to read `SPA_META_VideoDamage`?
- Is the screencast source `Screencasting::Metadata` already forwarding damage? If yes, where is it surfaced in `KPipeWire` APIs?
- Are there existing protocol hooks in KRdp for region rectangles, or would this require extending the protocol and client?

## References (External)
- ext-image-copy-capture-v1: https://wayland.app/protocols/wayland-protocols/440
- wlr-screencopy-unstable-v1: https://hoyon.github.io/wayland-protocol-docs/protocols/wlr_screencopy_unstable_v1.html
- PipeWire video damage metadata (SPA_META_VideoDamage): https://docs.pipewire.org/video-src-fixate_8c-example.html
- Citrix HDX/Thinwire design notes: https://community.citrix.com/tech-zone/design/design-decisions/hdx-graphics/
- RDPGFX AVC region rectangles: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/5f12c20e-2ea1-4ad1-a2a0-019ee3893731
- RDPGFX surface cache PDUs: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/7d40d7c2-5645-46a5-938d-d81c99c04f09
- RemoteFX adaptive graphics overview: https://techcommunity.microsoft.com/blog/microsoft-security-blog/remotefx-adaptive-graphics-in-windows-server-2012-and-windows-8/247454
