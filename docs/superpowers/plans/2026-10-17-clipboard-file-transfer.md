# Clipboard file transfer: implementation plan (OPT-078, 2026-10-17)

Spec: `docs/superpowers/specs/2026-10-17-clipboard-file-transfer-design.md`. Starts after the `html-clipboard` branch lands wire v20; this uses v21.
Rules: one branch per phase from master, no pushes without Steve, Sol/Buzz for anything live, Hal gets daemon-free unit tests only.
**[Jev]** marks bounded judgments (classify a log line, check a claim against a quoted PDU table) that follow an evidence step.

## Phase 0: measurement and spikes (branch `file-clip-p0`; done on `file-clipboard-core` 2026-10-17: counters + S1 loopback; S1 real clients, S2 and the baseline wait for Sol, see the spikes doc)
1. Add audio late/underrun counter and per-frame delivery delay histogram to `StatsReporter` (+ unit tests). Needed by acceptance.
2. Spike S1 (Sol): FreeRDP `cliprdr` server with `streamFileClipEnabled`, `canLockClipData`, `hasHugeFileSupport` TRUE against our client and `xfreerdp`; log the negotiated flags, a 5 GiB sparse range read. Evidence file; **[Jev]** classify each flag negotiated / not.
3. Spike S2 (Sol, private KWin): worker reads `text/uri-list` from the Wayland selection; a worker-offered `text/uri-list` to staged paths pastes in Dolphin (measure when Dolphin reads).
4. Baseline video+audio run on Sol/Buzz without transfers (numbers for 6.4).
Done: evidence under `~/dev/rdp/evidence/`, counters merged, S1/S2 verdicts.

## Phase 1: pure core (branch `file-clip-core`; DONE on `file-clipboard-core` 2026-10-17 except the file-system janitor, which moves to phase 3 with its link-wrap tests)
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

## Phase 5 (follow-up, agreed): lazy FUSE, then drag and drop
Decided by Steve 2026-10-17: FUSE comes after everything else is validated. Separate spec. Spec section 12 records the decisions (locked state applies to new transfers only; defaults; floor).

## Release gate
All phases: units green on the test list of the build tree, QoS acceptance numbers recorded in HANDOFF Log, paired broker/worker wire v21 (never one half), Sol-first rollout.

## Phase 3 server tasks (branch `file-clip-c2h`, based on `rc/0.15.2`; task ledger `.superpowers/sdd/2026-10-21-clip-c2h-server/progress.md`)

Server side only (the client half is a separate branch in `~/dev/krdp-client`; the two meet at standard CLIPRDR and the spec). Wire **v22**
(paired broker/worker). Strict TDD per task, one commit per slice `OPT-078 phase 3: <imperative>`. **[Jev]** marks bounded judgments.

1. Docs: this section and spec section 14 (decisions made while building: wire records, staging root, lock probe, offer rule, notices).
2. `src/ClipboardFilePush.h` (pure): descriptor -> push plan (windows-safe sanitizer, fetchable entries, totals, huge rule, skipped counts).
3. `src/ClipboardFileFetch.h` (pure state machine, fake clock): the broker's windowed FILECONTENTS requester: lock id, window <= 16 (4 stock),
   in-order delivery to the worker, acks, length checks, 3 violations abort, timeouts, unlock on every exit.
4. Wire v22 (`ConsoleWorkerWire.h`): `FilePushOffer`, `FilePushReply`, `FilePushData`, `FilePushAck`, `FilePushState`, `FilePushCancel`;
   `mayStageFiles` gate; `ConsoleWorkerEndpoint` methods and signals; every wire-version pin updated.
5. `server/HostFileStaging.h` (POSIX, header-only): fixed root `$XDG_CACHE_HOME/krdp-clip` (0700, uid/mode verified on every open), `openat`/`unlinkat`
   relative to a dirfd, `O_EXCL|O_NOFOLLOW`, `.part` then rename, disk guard with injectable `statvfs`, janitor (flock liveness probe + `planSweep`).
   Tests against real temp dirs plus `-Wl,--wrap=` failure injection (pwrite ENOSPC, fsync, openat, statvfs, unlinkat).
6. `server/HostFileStager.h` (worker): background single-thread writer, acks, progress (4/s), offer rule (<= 256 MiB after completion, else
   after the first finished top-level item, refreshed at the end), echo suppression, notifications; `HostClipboard` backend gains `writeUriList`/`clearUriList`.
7. Broker `Clipboard.cpp`: FormatList with FileGroupDescriptorW, lock, descriptor request, hostile-descriptor checks, fetcher wiring, FILECONTENTS
   response/lock callbacks, counters, `clipboard-files` notices (additive `direction` field).
8. Console and Virtual glue (`ClientFileRelay`), planner input `clientToHostFileStarted`, KRDPCTL `clipboard.files.clientToHost`.
9. Setting `ClipboardFileMinFreeBytes` (kcfg is not involved: broker host keys), KCM row, env plumbing, units, conf examples, docs/settings-ui.md.
10. Reverse loopback: `krdpctl-probe --clipboard push` (a libfreerdp client that copies files) against a real Server + Clipboard + stager.
11. Sol/Buzz live checklist (below); test run; cleanup pass.

### Phase 3 live checklist (Sol server, Buzz client; never Hal)

Candidate packages from `file-clip-c2h` (server) and the client branch, gated install. Staging is `~/.cache/krdp-clip/c-<hex>/<seq>/` (0700) on the host.

| # | Case | Steps | Pass |
|---|---|---|---|
| 1 | small | Buzz: copy `report.txt` (1 KB) in Dolphin, Sol Dolphin: Ctrl+V | byte-identical (`sha256sum`), host notification "Receiving 1 file", offered at once |
| 2 | folder | copy a tree (20 files, one empty dir, one empty file, a `CON.txt`, a name with a trailing dot) | tree identical except the renamed Windows-hostile names; empty entries present |
| 3 | 700 MB | copy one file | host notification, progress on Buzz, offer appears after the first file is complete, paste identical; video/audio unaffected (6.4 counters) |
| 4 | 2 GiB | copy one file, LAN, then `tc tbf rate 20mbit` | completes; rate near the floor while video is active; Unlimited fails the QoS criteria by design |
| 5 | locked deny | `loginctl lock-session` on Sol (Sol only), then copy on Buzz | nothing staged, journal `files client to host disabled (reason=locked-new-transfer)`, no notification of receipt |
| 6 | lock mid-transfer | start the 2 GiB copy, then lock Sol | the transfer completes, the offer appears, then policy flips to deny for new ones |
| 7 | greeter | SDDM greeter on Sol, copy on Buzz | nothing staged (text still pastes) |
| 8 | Virtual | same as 1-3 on :3395 | identical; staging under the virtual user's `~/.cache/krdp-clip` |
| 9 | disk full | mount a 3 GiB loop fs on `~/.cache/krdp-clip` (Sol only, as root, undo after) and copy 2 GiB (reserve default 2 GiB), then set `FARSIDE_CONSOLE_CLIPBOARD_FILE_MIN_FREE=0` explicit 200 MiB reserve and copy 2.5 GiB while filling the fs from another shell | refused up front with "Not enough space"; mid-way fill aborts, `.part` removed, one failure notification, no core |
| 10 | kill worker mid-copy | `kill -9` the Sol console worker during the 2 GiB copy | broker restarts a worker, the stale `c-*` dir is gone at the next worker start (flock probe), no `.part` left, Buzz shows failure |
| 11 | disconnect mid-copy | drop Buzz during the copy | staging removed within 1 s |
| 12 | replace | copy A, then copy B before A finishes | A's staging removed, only B offered |
| 13 | hostile | (client agent's probe) descriptor with `..\x`, `C:\x`, 100001 entries | refused, session stays up, journal shows reasons only |
| 14 | stock client smoke | `xfreerdp3 +clipboard` copy a file on Buzz, paste on Sol | record what works (own client first) |
| 15 | logs | `journalctl -u krdp-console-host -u ... | grep -c <canary file name>` | 0 |
Afterwards: cores (`coredumpctl list --since -1h`), stale `~/.cache/krdp-clip/`, loop mounts, `tc` qdisc removed.
