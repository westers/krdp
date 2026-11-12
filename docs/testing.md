# Running the test suite

The autotests are QtTest/ECM executables registered by name in `autotests/CMakeLists.txt`
(201 tests registered on the `review-fixes` tree, 2026-11-12; the 199 and the Sol measurements below
were taken on `master` 30801020 plus the REVIEW-T1 test changes). This page says how to run them on a
development host and on Sol, which tests need which tools, which cases are expected failures, and
which cases skip on Sol and why. Measured on `master` 30801020 plus the REVIEW-T1 test changes.

## Quick reference

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DBUILD_EXAMPLES=ON -DBUILD_PLASMA_SESSION=ON   # + KPipeWire flags, see CLAUDE.md
cmake --build build -j4
ctest --test-dir build -L unit --output-on-failure      # daemon-free set, safe on a work desktop (Hal)
ctest --test-dir build -L loopback --output-on-failure  # 127.0.0.0/8 only, no daemon, no device node (also safe on Hal)
ctest --test-dir build -N                               # list; add -L <label> or -R <regex>
ctest --test-dir build --output-on-failure --timeout 300 -j1    # everything (Sol only, see below)
```

`BUILD_EXAMPLES=ON` is needed for the `*Loopback*` tests (they use `krdpctl-probe`). Run the suite
one test at a time (`-j1`): several tests start private daemons or listen on sockets.

## Labels

| Label | Meaning | Tests (`review-fixes`, 2026-11-12) |
|---|---|---|
| `unit` | No daemon, no D-Bus connection, no GPU/device node, no listening TCP socket. May spawn our own short-lived helpers or `sleep`, use private sockets under `TMPDIR`, fake tools on `PATH`. Safe on a work desktop. | 165 |
| `loopback` | Listens on 127.0.0.0/8 or runs `krdpctl-probe`/`openssl` against a test server, and starts no daemon and opens no device node. A test that also needs a daemon or a GPU node carries only that stricter label, so `-L loopback` is daemon-free like `-L unit`. | 14 |
| `daemon` | Starts or talks to a daemon: private PipeWire/WirePlumber/dbus-daemon/KWin, the session or system bus, bwrap sandboxes. Never on Hal. | 19 |
| `gpu` | Opens `/dev/dri` or `/dev/nvidia*` (encoder probes, DMA-BUF, VAAPI/NVENC). | 7 |
| `root` | Would need root. No registered test does (cases with a root-only path skip as root). | 0 |
| `gui`, `non-hal`, `pure` | Older labels set next to their tests: offscreen QML page tests, "not on Hal", pure data. Kept. | 4, 4, 1 |

The `unit` list is explicit (`autotests/TestLabels.cmake`), so a new test is not `unit` until someone
has checked what it starts and added it. Every registered test has at least one of `unit`,
`loopback`, `daemon`, `gpu`; `unit` and `loopback` never overlap with another label. Four tests carry
both `daemon` and `gpu` (`DeviceToggleLoopTest`, `RenderAccessTest`, `SoftwareEncodeSessionTest`,
`WorkerEndToEndTest`), so the four counts add up to 205 for 201 tests. `appstreamtest` is
registered by ECM at the top level and labelled `unit` in the root `CMakeLists.txt`. List tests with no label:

```bash
ctest --test-dir build -N | sed -n 's/.*#[0-9]*: //p' | sort > /tmp/all.txt
for l in unit loopback daemon gpu; do ctest --test-dir build -L $l -N | sed -n 's/.*#[0-9]*: //p'; done | sort -u > /tmp/labelled.txt
comm -23 /tmp/all.txt /tmp/labelled.txt
```

### How the labels were derived

Not from names. The full suite was run once on Sol with every test binary wrapped in
`strace -f -e trace=execve,connect,bind,listen,openat` (`cmake -DCMAKE_TEST_LAUNCHER=<script>`, one
trace per test) and the traces were summarised per binary: executables started, Unix and TCP
sockets connected or bound (private temp sockets ignored), and `/dev/dri`, `/dev/nvidia*`,
`/dev/video*`, `/dev/input` opens. 145 of 188 traced binaries did none of those. Findings that
decided a label: `SoftwareEncodeSessionTest` starts a private PipeWire and connects the session and
system bus; `KcmUiTest`, `BrokerSignInPageTest`, `BrokerPreferencesPageTest` connect a bus;
`WorkerEndToEndTest` and `EncoderSupportTest` open GPU nodes even when they then skip;
`VirtualSessionHostControllerTest` binds sockets in the real `/run/user/<uid>/farside-virtual` and
opens `/dev/dri` (so it is `gpu`, not `loopback`); `ConsoleOutputJournalTest` and `ConsoleReleaseLockGuardTest` use stand-ins, not
real `kscreen-doctor`/`kscreenlocker_greet`. Shell-script tests (`dbus-run-session ... bash`) are
not covered by the launcher and are `daemon` by construction, except
`VirtualSessionDesktopSupervisionTest.sh`, which sources only helper functions of
`scripts/virtual-session-desktop.sh` and `SharedHeadersDriftCheck`, which only diffs two source trees
(both `unit`). Tests added after the strace run (REVIEW-S1/S3/S4/P1, GAP-G2/G5) were classified by
reading their sources and, for the two QML page tests `BrokerHostsPageTest`/`BrokerMainPageTest`, by
strace under the CTest environment (no connect other than the fake bus path). `ServerConnectionAdmissionTest`
uses 127.0.0.2.. as extra client addresses: still loopback. Do not run strace for timing: it
slowed `ClipboardFileLargeLoopbackTest` from 46 s to over 480 s and made it time out.

## Environment

| Setting | Why |
|---|---|
| `QT_QPA_PLATFORM=offscreen` | Tests that create a `QGuiApplication` must not need a display. |
| `TMPDIR` private, for example `/tmp/rf-t1` (mode 0700, created first) | `VirtualSessionStorageTest` refuses a root with a group/world-writable ancestor (a stage dir with mode 0775 breaks every fixture; the test skips with a message). |
| `LD_LIBRARY_PATH=<private KPipeWire>/lib/x86_64-linux-gnu:<build>/bin:<build>/lib` | The tests link the private KPipeWire (`97913cc` on Sol). |
| `KRDP_E2E_NVIDIA=1` | Only for `WorkerEndToEndTest` on an NVIDIA host with an accessible render node: binds `/dev/nvidia*` into the private KWin session. Without a usable node the test skips. |
| `KRDP_CLIENT_SRC=<krdp-client checkout>` | `SharedHeadersDriftCheck` compares the hand-copied headers with the client repo; without it (and without `../krdp-client`) it exits 77 and skips. |
| `ulimit -c 0` | Tests such as `TerminateHandlerTest` abort on purpose; do not leave cores. |
| No `FARSIDE_*` / `KRDP_*` set by you | Tests build their own environment. Old `KRDP_*` names are ignored by the product since the Farside rename; the product prints "Old KRDP environment names are ignored" for the test-only `KRDP_E2E_*` variables, which is harmless. |

Never run the suite against real profiles or wallets. `KcmUiTest` and `BrokerPreferences*` read the
account's `~/.config/farsideserverrc` by passwd home (not `XDG_CONFIG_HOME`); they only read it.

### Sol recipe

Sol is the disposable test host (no desktop session; NVIDIA RTX 2070; FreeRDP packages held, never
touch apt). Source by `git archive` of the commit into a stage folder, then:

```bash
S=~/farside-<name>; mkdir -p $S/tmp /tmp/<name>; chmod 700 /tmp/<name>
# private KPipeWire first (see scripts/package-farside.sh for the pinned commit), then:
cd $S && ulimit -c 0 && nice -n 19 ionice -c3 systemd-run --user --scope -p MemoryMax=8G -p CPUQuota=400% \
  cmake -S srv -B bsrv -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DBUILD_EXAMPLES=ON \
        -DBUILD_PLASMA_SESSION=ON -DCMAKE_PREFIX_PATH=$S/kpipewire-prefix \
        -DKPipeWire_DIR=$S/kpipewire-prefix/lib/x86_64-linux-gnu/cmake/KPipeWire
nice -n 19 ionice -c3 systemd-run --user --scope -p MemoryMax=8G -p CPUQuota=400% cmake --build bsrv -j4 -- -k 0
TMPDIR=/tmp/<name> QT_QPA_PLATFORM=offscreen KRDP_E2E_NVIDIA=1 LD_LIBRARY_PATH=... \
  timeout -s KILL 3600 ctest --test-dir bsrv --output-on-failure --timeout 300 -j1
```

A full run takes about 10 minutes (about 20 before `WorkerEndToEndTest` started to skip on Sol);
`ctest -L unit` about 3 minutes of test time. Check afterwards: no leftover
`pipewire|wireplumber|kwin_wayland|dbus-run-session` of yours, `coredumpctl list --since -1h`
(`TerminateHandlerTest` shows SIGABRT entries with no stored core), and remove the stage folder.

## Tools the conditional tests need

A test is only registered when CMake finds its tools; the audio shell tests exit 77 (skip) when the
environment cannot run them.

| Test(s) | Needs |
|---|---|
| `VirtualSessionBusTest` | `dbus-run-session`, `gdbus`, `bash` |
| `VirtualSessionLockConfigTest` | `kreadconfig6`, `bash` |
| `VirtualSystemBusLauncherTest`, Broker page/model tests | `python3`, `bash` |
| `PipeWireMicrophoneTest`, `DeviceToggleLoopTest`, `DeviceLoopbackTest` | `pipewire` (private daemon) |
| `VirtualSessionAudioGraphTest` | `pipewire`, `pw-dump`, `jq`, `timeout`, `bash` |
| `PipeWireMicrophonePcmTest`, `PipeWireMicrophoneInUseTest` | + `wireplumber`, `pw-cat`, `dbus-run-session` |
| `VirtualSessionAudioPcmTest` | + `ffmpeg` |
| `VirtualSessionAudioPulseTest` | + `paplay`, `pactl` |
| `WorkerEndToEndTest` | `bwrap`, `dbus-daemon`, `pipewire`, `wireplumber`, `kwin_wayland`, `kbuildsycoca6`; a render node the user can open; optional `ffplay` (motion) |
| `*LoopbackTest`, `RdpConnectionInitTest` | `krdpctl-probe` (`BUILD_EXAMPLES=ON`), `openssl` |
| `SystemdNotifyTest` (2 cases) | built with libsystemd (`libsystemd-dev`); skips otherwise |
| camera cases of `ConsoleHostControllerTest` | a v4l2loopback device at `/dev/video10` (else the "unsupported" refusal is asserted) |

## Expected failures (real product defects)

Marked with `QEXPECT_FAIL` at the exact assertion. They XPASS, and so fail loudly, once the product
is fixed: delete the marker then.

| Test | Defect | Where |
|---|---|---|
| `LegacySettingsMigrationTest`: 8 functions (`omittedDefaultsMatchActualGeneratedLegacySettings`, `completeSettingsAndHostFactsAgreeWithRealLegacyAndBrokerReaders`, `escapedListsAndAliasesNeverResolveAsTheirOwner`, `sameFileMaterializesPartialDefaultsAndPreservesPrivateBytes`, `duplicateAndEscapedValuesUseActualKConfigSemantics`, `locksArePreservedForSameFileAndSeparateDestinations`, `conflictingOrLockedDestinationFailsWholePlan`, `identityPresenceAndCompleteRevisionAreBound`) | OPT-066 added `ClientQualityMax` to the kcfg (2026-10-11) without classifying it in the migration; the coverage guard refuses every input ("Legacy schema coverage changed"). Nothing in production calls the migration yet, so the effect is latent. | `server/LegacySettingsMigration.cpp:121`, `server/BrokerUserSettings.cpp:126` |

## Cases that skip, and why

Checked on Sol 2026-11-12 (`SKIP_RETURN_CODE 77` tests and `QSKIP` inside tests).

| Test | Skip reason on Sol | Where it runs |
|---|---|---|
| `WorkerEndToEndTest` (all rows) | `/dev/dri/renderD128` is `root:render` with an ACL for `sddm`; the user has no logind session and is not in `render`, so the private KWin cannot composite (all 101 rows died with "screencast failed"). | A host where the user can open the render node (Hal/cray/Buzz with a session). Run it on Sol after `loginctl`/group changes, which are Steve's call. |
| `SystemdNotifyTest` (2 cases) | no `libsystemd-dev` | hosts with libsystemd |
| `DeviceToggleLoopTest::cameraLoopbackToggleLoop` | stale: `PipeWireCamera::start()` refuses the regular-file stand-in since OPT-053 `2828a7c4`; needs a real v4l2loopback or an injectable preflight | none until reworked |
| `KcmUiTest::standardBarStateFollowsEveryDraft`, `cleanLoadAnnouncesDefaultsToTheShell` | the account has a saved `~/.config/farsideserverrc`, which the module reads by passwd home | an account without saved preferences |
| `RenderAccessTest::probeInSandboxFindsTheHostsHardware` | `bwrap` cannot create a sandbox on Sol (existing skip) | hosts where unprivileged user namespaces work |
| `ConsoleHostControllerTest::greeterRefusesCameraAndMicrophoneWithNeedsSession` | no v4l2loopback (existing skip) | host with `/dev/video10` loopback |

`VirtualSessionStorageTest::guardianOwnsProfileAcrossChildExec` builds its runtime root under `/tmp`
(a long `TMPDIR` would overflow the 107-byte guardian socket path) and skips only if that path
still does not fit.

## Results on Sol (2026-11-12)

| Run | Tree | Registered | Passed | Failed | Not run |
|---|---|---|---|---|---|
| baseline | `30801020`, unmodified | 199 | 182 | 16 (153 failing test functions/rows) | 1 (`DeviceToggleLoopTest` did not build) |
| after, run B | REVIEW-T1 (before the last test-only fix) | 199 | 199 | 0 | 0 |
| after, run C | REVIEW-T1 (same) | 199 | 199 | 0 | 0 |
| after, run D (final tree) | REVIEW-T1 + guardian-socket fix | 199 | 199 | 0 | 0 |

`ctest -L unit`: 161 tests, 190 s on `master` plus REVIEW-T1 (165 on the `review-fixes` tree). Counts include tests that pass with skipped cases (ctest does not
count a QtTest `SKIP` as skipped; only exit 77 does).
