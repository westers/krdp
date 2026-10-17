# Clipboard file transfer: implementation plan (OPT-078, 2026-10-17)

Spec: `docs/superpowers/specs/2026-10-17-clipboard-file-transfer-design.md`. Starts after the `html-clipboard` branch lands wire v20; this uses v21.
Rules: one branch per phase from master, no pushes without Steve, Sol/Buzz for anything live, Hal gets daemon-free unit tests only.
**[Jev]** marks bounded judgments (classify a log line, check a claim against a quoted PDU table) that follow an evidence step.

## Phase 0: measurement and spikes (branch `file-clip-p0`)
1. Add audio late/underrun counter and per-frame delivery delay histogram to `StatsReporter` (+ unit tests). Needed by acceptance.
2. Spike S1 (Sol): FreeRDP `cliprdr` server with `streamFileClipEnabled`, `canLockClipData`, `hasHugeFileSupport` TRUE against our client and `xfreerdp`; log the negotiated flags, a 5 GiB sparse range read. Evidence file; **[Jev]** classify each flag negotiated / not.
3. Spike S2 (Sol, private KWin): worker reads `text/uri-list` from the Wayland selection; a worker-offered `text/uri-list` to staged paths pastes in Dolphin (measure when Dolphin reads).
4. Baseline video+audio run on Sol/Buzz without transfers (numbers for 6.4).
Done: evidence under `~/dev/rdp/evidence/`, counters merged, S1/S2 verdicts.

## Phase 1: pure core (branch `file-clip-core`)
`ClipboardFileNames.h` (sanitizer), `ClipboardFileDescriptor.h` (codec), `ClipboardFileLimits.h`, `ClipboardFileRequest.h` (validator),
`ClipboardFilePacing.h` (pacer), `ClipboardFileStaging.h` (staging planner/janitor, injectable fs), `ConsoleClipboardPolicy` `files` field.
All unit tests of spec section 9 (pure). No behaviour change. Runs on Hal (daemon-free).

## Phase 2: host to client (branch `file-clip-h2c`), wire v21
Wire records, worker serving (`consoleworker.cpp`), broker `Clipboard.cpp` file callbacks and capability flags, KRDPCTL `clipboard-transfer` record and
capabilities group, server settings `ClipboardFiles`, `ClipboardFileMaxRate`, KCM, systemd unit/env plumbing, pacer wiring (stats feed),
client receive path (staging, progress UI, cancel, `KSystemClipboard` offer). Broker fake-worker test, loopback test. Live on Sol/Buzz: small, 2 GiB, directory,
locked, greeter, Virtual; QoS acceptance (6.4) both LAN and tbf. Needs Sol measurement: yes (pacer constants).

## Phase 3: client to host (branch `file-clip-c2h`)
Client descriptor/serving, host staging with disk guard and notifications, janitor, policy rows (locked deny), `ClipboardFileMinFreeBytes`. Link-wrap failure tests.
Live: same matrix reversed; disk-full case on a small loop mount on Sol; crash cleanup (kill worker mid-copy). Needs Sol measurement: yes.

## Phase 4: client settings and polish (branch `file-clip-ui`)
`Clipboard transfer priority` UI (global + per-connection), Unlimited warning, host-limit display, notifications, docs (`docs/settings-ui.md`, user doc), packaging scripts.
Live: mode comparison table (Auto/Balanced/Fixed/Unlimited) with the 6.4 counters.

## Phase 5 (optional): lazy FUSE or drag and drop
Only if Steve wants it after phase 4 numbers. Separate spec.

## Release gate
All phases: units green on the test list of the build tree, QoS acceptance numbers recorded in HANDOFF Log, paired broker/worker wire v21 (never one half), Sol-first rollout.
