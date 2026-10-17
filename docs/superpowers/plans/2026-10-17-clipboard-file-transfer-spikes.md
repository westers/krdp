# OPT-078 phase 0: spikes, results and the Sol procedures (2026-10-17)

(Copy of `docs/superpowers/plans/2026-10-17-clipboard-file-transfer-spikes.md` on branch `file-clipboard-core`.)

Branches: `file-clipboard-core` (phase 0 counters + phase 1 pure core), `file-clipboard-spike-s1` (the spike code, never to be merged:
`src/ClipboardFileSpike.h` + a 6-line hook in `Clipboard.cpp`, the `krdpctl-probe --clipboard files` mode, `ClipboardFileSpikeLoopbackTest`).
Plan items: S1 = FreeRDP `cliprdr` with the four file flags TRUE; S2 = Wayland `text/uri-list` both ways; baseline = 60 s video + audio numbers (spec 6.4).

## 1. S1 on Hal, loopback (DONE, 2026-10-17, daemon-free)

`KRdp::Server` on 127.0.0.1 with the spike hook, against `krdpctl-probe --clipboard files` (the libfreerdp 3 client library, system build).
The server offers one virtual file (default 5 GiB, generated, nothing on disk); the probe checks SIZE, ranges across and beyond 4 GiB, the tail,
out-of-range and zero-length requests, then pulls 16 MiB in 256 KiB ranges, 4 in flight, verifying a position-dependent byte pattern. Run:

```bash
cd ~/dev/krdp/.work/file-clipboard-spike-s1     # a worktree of file-clipboard-spike-s1, built with BUILD_EXAMPLES=ON
./bw.sh build/bin/ClipboardFileSpikeLoopbackTest          # bwrap: /dev/dri masked, offscreen, private HOME, loopback only
```

Results (8 variants; `SPIKE-S1-PROBE DONE ok=9 bad=0` for: all flags, server without lock, server without stream flag, client without lock. The variants without huge support stop at the client-side refusal below, by design; the 1000-byte variant fails its own large-file checks, as expected):

| Question | Result |
|---|---|
| Server sets `streamFileClipEnabled`, `canLockClipData`, `hasHugeFileSupport` TRUE, `fileClipNoFilePaths` TRUE; what does the client see? | Server caps arrive as `generalFlags=0x3e` (long names, stream, no paths, lock, huge). |
| What does the server context hold after the exchange? | The INTERSECTION with the client's flags: a client offering only `0x2` leaves `streamFileClip=0 noFilePaths=0 canLock=0 huge=0`; client `0x1e` (no huge) leaves `huge=0`. Read the negotiated state from the context in/after `ClientCapabilities`. |
| 64-bit offsets (`nPositionHigh:Low`) with huge on both sides | Work: ranges at 4 GiB-100 (crossing), 4 GiB+12345, 5 GiB-1 (last byte) and the 256 KiB tail all arrive intact. |
| Huge NOT negotiated (server flag off, or client flag off) | The libfreerdp CLIENT refuses to send an offset >= 4 GiB itself (`cliprdr_process_filecontents_response ... error 87`). Whether the SERVER library would also reject such a request from a hostile peer was not tested (the stock client cannot send one), so `RequestValidator` enforces it itself (new `HugeNotNegotiated`); files over 4 GiB are not offered to such a peer. |
| Lock / unlock | `CB_LOCK_CLIPDATA` / `UNLOCK` reach `ClientLockClipboardData` / `ClientUnlockClipboardData`; every later request carries `haveClipDataId=1 clipDataId=77`. Without lock negotiated, requests carry `haveClipDataId=0`. |
| `CB_STREAM_FILECLIP_ENABLED` off | The library still routes FILECONTENTS requests; a stock client would not send them. The server must itself refuse file traffic when the flag was not negotiated. |
| Reads past the end / zero length | Our callback chose to answer FAIL (`CB_RESPONSE_FAIL`); the client reported `failed=1 cb=0`. What Windows `mstsc` sends for the last chunk (does it ever overrun the size?) is NOT known: see S1c. |
| `FILEDESCRIPTORW.cFileName` | Fixed 260 UTF-16 units: a relative path on the wire is at most 259 units. Spec 4.1's 4096 bytes is only the internal ceiling. The encoder returns `NameTooLong`: such an entry must be skipped with a notice. |
| Throughput (Debug build, loopback, pattern generation and verification) | 25-31 MB/s with a window of 4 x 256 KiB. Not a performance number; it shows the pipeline holds. |
| Descriptor parsing | `ClipboardFileDescriptor::decode` parsed the server's real `FileGroupDescriptorW` as sent through the library (1 item, 5 GiB). |

Not exercised in S1a: the server with a Windows client, FreeRDP `xfreerdp`/`wlfreerdp` as clients (their own caps), a real Dolphin paste, directories
(`FILE_ATTRIBUTE_DIRECTORY` entries), more than one file, `CB_FILECLIP_NO_FILE_PATHS` semantics on a real client.

## 2. S1b/S1c on Sol (NOT RUN; needs Sol + Buzz)

Goal: the same spike with REAL clients and a real LAN. Server on Sol (port 3399, own config, nothing production touched), clients on Buzz.

```bash
# 0. Hal: ship the spike branch source. No secrets in argv; the password file is created on Sol with mode 0600 by hand/the .env tooling.
git -C ~/dev/krdp archive file-clipboard-spike-s1 | ssh -o BatchMode=yes sol 'mkdir -p ~/farside-opt078/src && tar -x -C ~/farside-opt078/src'
# 1. Sol: build libKRdp + the spike test + krdpctl-probe (needs the usual build deps and a KPipeWire; system KPipeWire is enough: nothing here captures video)
ssh -o BatchMode=yes sol 'cd ~/farside-opt078 && cmake -S src -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBUILD_EXAMPLES=ON && nice -n 19 cmake --build build -j6 --target ClipboardFileSpikeLoopbackTest krdpctl-probe'
# 2. Sol: serve (port 3399; not 3389/3391/3395). SPIKE_PASSWORD_FILE holds one line, mode 0600.
ssh -o BatchMode=yes sol 'cd ~/farside-opt078 && SPIKE_SERVE_PORT=3399 SPIKE_USER=alice SPIKE_PASSWORD_FILE=$HOME/farside-opt078/pw SPIKE_SERVE_SECONDS=900 \
  XDG_RUNTIME_DIR=$(mktemp -d) HOME=$(mktemp -d) QT_QPA_PLATFORM=offscreen build/bin/ClipboardFileSpikeLoopbackTest serveForRealClients 2> spike-server.log'   # SPIKE-S1 lines
# 3. Buzz: copy krdpctl-probe (built on Sol or Buzz; Buzz's held FreeRDP 3.22) and run it against Sol, flags as a stock client would offer, then default:
buzz$ ./krdpctl-probe sol 3399 alice - --silent --no-krdpctl --clipboard files --timeout 60 < pwfile     # last: SPIKE-S1-PROBE DONE ok=9 bad=0
buzz$ SPIKE_CLIP_FLAGS=1e ./krdpctl-probe sol 3399 alice - --silent --no-krdpctl --clipboard files --timeout 30 < pwfile
# 4. Buzz: the real clients. Connect each with its clipboard enabled and read the server log's `client-capabilities generalFlags=0x..` and
#    `after-client-caps context: ...` lines. No paste is needed for the flags; wait 10 s after connecting (the file is announced 8 s after the caps).
buzz$ xfreerdp3 /v:sol:3399 /u:alice /cert:ignore +clipboard      # prompts for the password; or wlfreerdp3 / sdl-freerdp3
#    and farside-client (connect to sol:3399, user alice, accept the certificate; see ~/dev/krdp-client for the CLI).
# 5. Optional (a person at Buzz): in the client's session paste the offered file into a file manager (the spike offers one 5 GiB virtual file named spike-big.bin);
#    stop at ~1 GB. The server log then shows the request pattern of a real paste: SIZE first? chunk size? window? ordering? lock id? overruns at the tail?
```

Pass/fail (each client, [Jev] may classify the log lines against this table once they are quoted):

| Check | PASS | FAIL |
|---|---|---|
| Probe from Buzz over the LAN | `DONE ok=9 bad=0`, throughput line present | any `bad`, `UNEXPECTED`, or no DONE |
| `client-capabilities` line per real client | present; flags recorded in the evidence file | missing (client does not use the channel) |
| Context after exchange = client AND server flags | `after-client-caps` matches the intersection | any extra flag set |
| Real paste (step 5) | SIZE and RANGE requests arrive; ranges <= the chunk size the client chooses; no request beyond file size | an overrun request (then `RequestValidator` must clamp, see below) or a request this design did not expect |

If a real client over-asks at the tail (cbRequested past the end), set the validator to clamp the tail to the file size (a one-line change, test row exists as `RangePastEnd`) instead of failing; record the observed pattern in the evidence.
Evidence: `~/dev/rdp/evidence/2026-10-17-opt078-s1/` (probe output, `spike-server.log`, a table of flags per client). Stop the server (Ctrl-C of the ssh session or
`SPIKE_SERVE_SECONDS` expiry), remove `~/farside-opt078/pw`, leave no background process.

## 3. S2 on Sol, private KWin (NOT RUN; needs Sol)

Goal: (a) the worker-side reader sees `text/uri-list` (and `application/x-kde-cutselection`) when Dolphin copies files; (b) a `text/uri-list` offered through the
data-control backend (what the worker will do for staged files) pastes in Dolphin, and when Dolphin opens the source files is measured.
Reuse the 2026-10-15 image-wayland rig (private `kwin_wayland --virtual`, dbus-run-session, fake_input injector): scripts in `~/dev/krdp/.work/image-wayland/`
(`start.sh`, `inner.sh`, `stop.sh`, `run.sh`, `sync.sh`, `probe/`); evidence of how it ran: `~/dev/rdp/evidence/2026-10-15-image-wayland-verify`.

```bash
# 0. Hal: copy the rig and apply the clipprobe patch (adds --uri-list PATH [--uri-list PATH ...] and --cut; compile-checked on Hal against the core tree)
cp -r ~/dev/krdp/.work/image-wayland ~/dev/krdp/.work/file-clipboard-s2 && cd ~/dev/krdp/.work/file-clipboard-s2 && patch -p1 < ~/dev/krdp/.work/file-clipboard-core/docs/superpowers/plans/opt078-spikes/s2-clipprobe.patch
sed -i 's#farside-image-wayland#farside-fileclip-s2#g' start.sh stop.sh sync.sh run.sh inner.sh
./sync.sh ~/dev/krdp/.work/file-clipboard-core        # tars server/*.h SelectionReader.cpp src/*.h protocols/*.xml + probe/ to Sol and builds clipprobe, qtprobe, injector2
# 1. Sol: start the private session (its own XDG dirs; nothing of Steve's desktop is touched)
ssh -o BatchMode=yes sol 'bash ~/farside-fileclip-s2/start.sh'              # prints READY
# 2. Sol, inside the session (run.sh NAME TIMEOUT < script runs a script inside it): make a source tree
printf 'hello\n' > $HOME/s2/src/a.txt; truncate -s 700M $HOME/s2/src/big.bin; mkdir -p $HOME/s2/dst
# 3a. READ side: dolphin on src, select a.txt and big.bin, Ctrl+C (injector2: `chord 29 46`), then dump what the selection offers
#     (clipprobe --dump prints every offered text/* and application/* format with its size and read time)
dolphin --new-window $HOME/s2/src &                       # then in injector2: sleep 3000; tap 108 (down); chord 29 30 (Ctrl+A); chord 29 46 (Ctrl+C)
clipprobe --dump --seconds 8 --tag read                   # expect formats incl. text/uri-list, application/x-kde-cutselection; uri-list = file:// URIs of the selection
# 3b. WRITE side: offer the staged paths the way the worker will, paste into dst, and trace when the source is opened
inotifywait -m -e open,access,close_nowrite $HOME/s2/src > $HOME/s2/opens.log &      # or: strace -f -e trace=openat -p $(pidof dolphin)
clipprobe --uri-list $HOME/s2/src/a.txt --uri-list $HOME/s2/src/big.bin --seconds 40 --tag offer &
dolphin --new-window $HOME/s2/dst &                       # focus it; injector2: `chord 29 47` (Ctrl+V)
# 4. Compare: sha256sum $HOME/s2/src/* $HOME/s2/dst/*; note the time of Ctrl+V vs the first `open` in opens.log; `ls -l` of dst DURING the 700 MB copy.
# 5. Stop and clean: stop.sh (moves the run dir aside), kill inotifywait, rm only the files created under $HOME/s2 (guard with ${var:?}).
```

Pass/fail:

| Check | PASS | FAIL |
|---|---|---|
| (a) `--dump` after Dolphin's Ctrl+C | `text/uri-list` present with `file://` URIs of exactly the selection, and `application/x-kde-cutselection` present ("0" for copy, "1" after Ctrl+X) | no uri-list offered, or URIs not file:// |
| (a) read time | `text/uri-list` read in < 100 ms (it is a few hundred bytes) | > 1 s (the worker would have to read it off the main loop) |
| (b) Dolphin pastes the worker-style offer | `dst/a.txt` and `dst/big.bin` byte-identical to `src` (sha256) | missing/short files |
| (b) when Dolphin opens the sources | NO `open` of a source file before Ctrl+V; copy starts within 2 s of it | opens at offer time (then staged files must be complete before the offer: spec 3.2 offer rule applies to everything) |
| (b) partial files | a source path that does not exist yet (simulate: offer a path that is created 3 s after Ctrl+V) -> record Dolphin's behaviour (error dialog? retry? skip?) | n/a: record, it decides the "offer after the first file complete" rule |

## 4. Baseline video + audio numbers (spec 6.4; NOT RUN; needs Sol server + Buzz client; BLOCKED on the counters being in the Sol build)

The new counters are in `file-clipboard-core` (stats `audio`, `video.sendDelayMs`, `link.sendQueueAtSendKiB`). The Sol Console/Virtual host must run a
build containing them: that is a candidate package built and installed by the normal gated procedure (not part of this task).

```bash
# Buzz, against the Sol Console route (:3391) or Virtual (:3395) with a 1080p60 test pattern + audio playing in the session (the synthetic scene used by the OPT-062 runs).
# Subscribe to stats at 1 Hz for 60 s; krdpctl-probe prints every record, stats-samples included, as `reply: {...}` lines. stats.json = {"type":"stats","action":"subscribe","rateHz":1}
./krdpctl-probe sol 3391 USER - --silent --raw stats.json --timeout 75 < pwfile > baseline.log     # run 1: no transfer
./krdpctl-probe sol 3391 USER - --silent --raw stats.json --timeout 75 < pwfile > withfile.log     # run 2: same scene while a 2 GiB file moves (phase 2/3 code)
uv run --no-project python -I ~/dev/krdp/docs/superpowers/plans/opt078-spikes/analyze-stats.py   # on branch file-clipboard-core withfile.log baseline.log     # exit 0 = every criterion PASS
```

`analyze-stats.py` implements the 6.4 criteria: framesSent rate within 10% of baseline; p99 send queue at send <= 256 KiB; p99 frame delay <= baseline + 40 ms;
audio late + overflow + producer gaps <= baseline + 2 and no gap > 100 ms. It was tested only on synthetic input (good / bad runs). Verify the `--raw` stats
subscription once with a 5 s dry run before the long ones (the `reply:` prefix and the `stats` record shape come from `krdpctl-probe`'s source and StatsRequest.h,
not from a live run). Repeat once under `tc tbf rate 20mbit burst 32kbit latency 400ms` on Sol's NIC (`sudo`; remove the qdisc afterwards; never on a production route while a user is connected).
Acceptance for the pacer's constants (floor, +5% step, 50% share) is these runs; the unit tests only prove the control loop's shape.
