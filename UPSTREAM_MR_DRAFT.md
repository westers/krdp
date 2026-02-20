## Title
KRDP: damage-aware streaming refinements, congestion adaptation, and H264 Main preference

## Summary
This series improves interactive quality and stability for KRDP sessions under
real network and desktop workloads:

- prioritize freshest frames under load and bound queue depth
- coalesce damage rectangles and classify activity tiles
- pair encoded packets with damage metadata and recover on metadata gaps
- add progressive refinement after motion settles
- adapt frame rate and per-rect quality under congestion
- negotiate AVC444/AVC420 safely based on client caps and local capabilities
- prefer H264 Main when advertised by PipeWire stream backends
- include local build/install docs and debug markers for validation

## Scope
Branch: `westers/krdp:master`
Notable commits (latest first):

- `a66a8e9` Throttle progressive refinement frames with cooldown
- `a76f28c` video: adapt frame rate and quantization to congestion
- `af80d2e` video: prefer H264 Main profile when available
- `a55442f` patches: track kpipewire damage-metadata patch
- `9d291e9` docs: document runtime tuning behaviors and debug markers
- `33c54f5` video: add progressive refinement after motion settles
- `3c1ff9b` video: classify damage regions by tile activity
- `63cd49f` session: pair packets with damage metadata using short wait
- `105ef62` video: prioritize freshest frame and bound queue depth
- `9b3d24f` video: gate AVC444 flags on local encoder capability
- `355293f` video: align caps confirm with negotiated codec support
- `66fe94b` video: add AVC444 negotiation scaffold with AVC420 fallback
- `90654e9` video: refresh aggressively under rapid motion
- `ec6e2e5` video: favor full refresh under high-motion partial updates
- `caa65de` video: react faster to congestion in frame-rate control
- `fea50af` video: adapt quality by damage rect size
- `f166970` video: coalesce damage rectangles before RDP metadata
- `9e45c8c` video: prioritize freshest frames with bounded queue
- `7a5c880` docs: add local build install and systemd config guide
- `8be18ee` server: fix plasma input mapping and damage-aware updates
- `81531ed` cmake: allow building with Qt 6.9.2

GitHub reference:
`https://github.com/westers/krdp`

## Validation
- Built locally with Qt 6.9.2.
- Tested with standard RDP clients over KRDP server sessions.
- Verified input handling, pointer mapping, and session reconnect behavior.
- Monitored logs for damage-metadata desync and queue overflow markers.

## Notes
- This shell cannot authenticate to `invent.kde.org`, so MR creation must be
  done from a session with KDE GitLab credentials.
