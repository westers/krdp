# Farside: one task at a time

Date: 2026-10-01. Target agent: `gpt-6.1-sol`.
Status: N01/N02/N03 artifacts complete; Steve explicitly requested client/server
updates on Hal, Buzz and Sol on 2026-10-01. Those installations and basic gates
passed. Remaining N04/N05 integration acceptance is open. Goal remains paused.

## Start here

Steve selects one task below. Complete its stated result, run the checks needed
for that result, record what shipped or remains open, and stop. Do not create or
resume a Goal, execute the whole queue, or start the next task automatically.
The existing Goal remains paused. Superpowers workflows and automatic delegation
are not required. Existing plans, tests, code and evidence remain useful inputs.

**Requested three-host update complete:** Hal, Buzz and Sol have client 0.6.5
and server `2828a7c` (camera readiness follow-up); Sol now has camera and settings-menu fixes `87d8a24`; other hosts remain unchanged. Next tasks to select are the
remaining installed N04/N05 checks. This installation request does not close
their broader acceptance criteria or authorize further tasks.

This file replaces the execution process in the September 30 goal contract and
October 1 release plan. It preserves the T01–T28 feature scope, existing acceptance
requirements and deployment protections. A smaller task can finish without its
parent T-task being complete. New test quantity alone does not count as delivery.

## Current facts to reuse

| Area | Already done | Still missing |
|---|---|---|
| Delivered client | 0.6.5 / `82fe55e` Hal/Buzz/Sol, debug actions OFF; 0.6.3 / `2df38a7` Ace/Cray/Marvin | Remaining N05 monitor acceptance and N06 fleet rollout/tag |
| Delivered server | `87d8a24` Sol, `2828a7c` Hal/Buzz, wire 11, private KPipeWire `9d6b08c`; `4b308bd` other hosts, wire 6 | Installed settings/authorization and broader native parity; remaining fleet/consolidation delivery |
| Settings/auth | Scoped authentication, services, 17 preferences, 25 host/session controls and runtime inspection implemented; `44726890` removes legacy KCM adapters | Real installed password/cancel/save/restart, effective runtime policy and live stock coexistence |
| Monitors/video | Quiet Sol private worker single/two-screen HEVC creation/resize/Fit/decoded restoration passes; client UI and protocol evidence exists | Final packaged Console RDP, broader topology lifecycle; earlier loaded CUDA OOM/fallback is unresolved |
| Discover/locker | Font-cache cause demonstrated and repaired on Sol; Steve confirms Console Discover password entry and ordinary lock/unlock work | Virtual/failure/restart matrix and original lock/output-release race; agent's original failing trace still missing |
| Migration | `d437511c` effective-settings planner and N01 guard included in Hal/Buzz/Sol packages | Durable apply/backup/recovery/rollback, profiles/trust/retained integration; guard deployment to remaining hosts |
| Wallets | N01 automatic copying disabled in installed Hal/Buzz/Sol client/server; no direct wallet operations during deployment | Buzz’s three pending entries still require N16 reconciliation; other hosts retain older retry code |
| Runtime | Steve reports on October 1 that **both Buzz and Sol now have no AI workloads**; both are available for testing. Last measured Sol snapshot had active brokers and SDDM greeter | Recheck current load/login before live work; no AI does not establish a signed-in Console owner or a measured performance result |

The old ledger records **2 done, 15 partial, 10 planned and 1 documentation task
ongoing**. Do not assign arbitrary percentages to partial work or rerun accepted
components just to rebuild a sense of progress.

## Agent instructions for GPT-6.1-sol

1. Read the newest shared snapshot/guardrails, this plan's selected task, and only
   its relevant source/evidence. Open old detailed specs when resolving a design
   question; do not reread all historical logs every turn.
2. Begin with a short statement: result, relevant existing evidence, checks and
   stopping point. Make routine implementation choices without repeated approval.
3. Default to normal/medium reasoning for bounded code/package work; use high
   reasoning for credential authority, wallet races or durable migration design.
   These are workflow recommendations, not measured model-performance claims.
   This document does not change the selected model or global configuration.
4. Use existing helpers/tests. Add coverage for a demonstrated bug or meaningful
   boundary; avoid tests that merely restate implementation and new harnesses that
   duplicate working ones. Fix only the selected task or a demonstrated blocker.
5. Batch independent reads, then edit/build/inspect dependent results sequentially.
   Keep one build running and poll its handle; do not edit its source concurrently.
6. At a failed check, identify the failure and rerun the affected check after its
   fix. Repeated unchanged failure is a reason to report the exact blocker, not
   broaden the feature set. After about 30 minutes without a new concrete result,
   give Steve a short stall report and proposed smaller remaining slice.
7. Finish with result, source/package/host, verification and any material open
   limitation. Update the compact record below and append a short shared handoff.
   Create a separate design document only for an unresolved design risk.
8. Stop at the selected task's boundary. A question about status does not authorize
   the next task. Ask only for necessary information or a real blocked decision;
   an elapsed wait is never permission.

GPT-6.1 Sol supports medium and high reasoning; the recommendation above is our
project policy. [Official model reference](https://developers.openai.com/api/docs/models/gpt-6.1-sol).

### Verification limits

- Before testing, name the concrete risk and smallest check that distinguishes a
  working result from a broken one. Usually one affected suite and one relevant
  installed/live flow are sufficient for a bounded change.
- Reuse evidence when relevant source, package dependency and environment behavior
  have not changed. A new documentation commit does not invalidate binary tests.
  Previously injected checks do not replace missing installed integration.
- Expand only for an observed failure, affected shared dependency or an explicit
  acceptance requirement. Do not rerun the full suite, all codec combinations or
  entire fleet after every edit. Required original matrices are divided among
  named tasks below and run once against the accepted candidate where applicable.
- A source-only task ends at source acceptance. An artifact task ends at a verified
  package. A deployment task ends at installed and running version checks. Do not
  hold all three hostage to every future feature, or call one equivalent to another.
- Unavailable Windows/camera/Console-owner checks get an exact task/version/host/
  steps/pass-criteria entry. Stop the selected task if that check is essential;
  Steve chooses what to do next. Leave its parent partial. Do not silently waive
  the check or automatically move to another task.
- Historical failing traces remain open diagnosis evidence; they do not require
  deliberately breaking a repaired live desktop or repeating idle captures.
  T01 stays partial until its original diagnostic requirement is satisfied or
  Steve explicitly revises it. It need not prevent unrelated accepted releases.
- Record external CPU/GPU/VRAM/AI load for live performance-sensitive work. Quiet
  functional success, performance acceptance and loaded recovery are separate.
  No new benchmark infrastructure is needed for a simple correctness change.

## Protections that apply to every task

- **Wallets:** QtKeychain chooses the desktop backend; Farside selects application
  namespaces, not a wallet name. Retain system wallet selection and disabled
  plaintext fallback. No wallet daemon reset/reconfiguration, forced unlock,
  secret values in logs/arguments/manifests, or plaintext exports. Preserve old
  stores and keys. Do not claim read-then-write is atomic or copy a live wallet
  file and call it a consistent backup. N01 may finish with automatic migration
  disabled if safe insertion cannot be guaranteed; full migration stays pending.
- **Hal:** Steve's work desktop; never an agent live GUI/RDP/D-Bus/PipeWire/output/
  GPU benchmark target. Pure injected checks and isolated builds are allowed.
  Deployment is already authorized after non-Hal gates, with zero incoming RDP
  before restart, preserved work processes/settings/certificates/windows/holds,
  package simulation and Hal last. Check all active Farside listener ports.
- **Hosts:** Testing is authorized between Buzz and Sol; both currently have no
  AI workloads per Steve's October 1 update. Use Sol as server and Buzz as client
  by default; reverse roles for a specific codec/device case when useful.
  Recheck login/session/load/connection state
  before live tests. Only stop own fixtures. Do not kill retained desktops to
  force an upgrade or stop Steve's AI work without authorization.
- **Trust/auth:** Preserve strict certificate identity, IDs/custom endpoints,
  explicit Console/Virtual types, PAM/owner authority and separate stock KRDP.
  No automatic trust transfer from :3389 to :3391 or broadened admission.
- **Repositories:** Preserve existing AGENTS.md/repository CLAUDE.md/research drift,
  tarballs and worktrees. No amend/rebase/stash or edits under `~/.claude/`.
  Server commits push to `github/master`, client to `origin/main`; do not push
  private KPipeWire. Use pinned private KPipeWire `9d6b08c` for release builds.
- Keep hosts' apt holds; use existing `.env` credentials privately when needed.
  No unrelated package upgrades. Keep working rollback packages read-only.

## Immediate queue: each row is a separate selected task

Instructions and stopping points below are deliberately bounded. An artifact or
test-host installation is useful progress but does not close the full parent.
Only start a row after Steve selects it and its prerequisite result exists.

### N01 — Protect both wallet retry paths (T10)

**DONE in source, 2026-10-01.** Client `41b6cd9`; server commit recorded in the
shared handoff. Automatic password copying removed because QtKeychain has no
create-only write. Two focused migration suites pass (client 7/server 8 Qt cases);
full affected client/server executables compile. Neither migration suite links
QtKeychain; normal explicit storage and server read fallback remain. Evidence:
`~/dev/rdp/evidence/2026-10-01-n01-wallet-guards/SUMMARY.md`.
Now included in Hal/Buzz/Sol installed packages; other hosts await deployment.

**Do:** Inspect client `src/core/LegacyMigration.cpp` and server
`server/FarsideMigration.cpp`. Remove blind startup/diagnostic copying. Preserve
source entries and pending metadata; accept an identical destination, retain a
different/locked/unavailable/uncertain one. Require actual safe create semantics;
if QtKeychain cannot guarantee them, keep absent entries pending rather than
invent a universal wallet transaction layer. Normal explicit Remember Password
behavior must keep working. Default wallet selection remains delegated.

**Check:** Injected source/destination absent/same/different/unavailable cases,
write failure/uncertainty and concurrent destination change; prove startup and
diagnostics cannot clobber values. No real wallet access for these tests.

**Stop/done:** Both paths guarded or automatic retry safely disabled, affected
targets compile, focused checks pass, originals and metadata preserved. Commit
both repositories as needed. Buzz's three passwords remain a separate N16 result.

### N02 — Produce the client 0.6.4 candidate (T06/T27)

**DONE as an artifact, 2026-10-01.** Source `afed6fbbe3efb72a4a9631fa6eaa5dc388b991bd`
committed/pushed; clean detached build, RelWithDebInfo, debug actions OFF.
Candidate: `~/dev/rdp/debs/candidates/client/0.6.4/farside-client-0.6.4-Linux.deb`.
SHA256: `afa933d2e6db35ac1e70c4b49feafab0385ef78a92102f757784afcea519f8f2`.
Metainfo, package paths/modes/licenses and all seven ELF dependencies pass.
FreeRDP dependency floor increases from 3.0.0 to 3.11.1; Buzz/Sol held 3.31+h264.2
satisfies it. Both apt simulations propose only client 0.6.3 → 0.6.4, no removals
or dependency upgrades; installed 0.6.3 and all holds unchanged. Exact archived
0.6.3 rollback hash `b76401ce9786f18ed8e2fcf3b0f73f51d042a65c3bd39c0b63a25a572456ddfd`
verified and archive unchanged. No candidate launch, install, tag, real wallet
access or performance claim. N01 tests reused. Evidence:
`~/dev/rdp/evidence/2026-10-01-n02-client-candidate/SUMMARY.md`.
T06/T27 remain partial; N03 proposed, not started.

Subsequently installed on Hal/Buzz/Sol at Steve's explicit three-host request;
see the deployment evidence below. The preceding paragraph records the original
N02 artifact completion boundary.

Prerequisite N01. **Do:** Freeze `ef692be` plus N01/release fixes; update actual
Farside version/metainfo/README, build committed sources with debug actions OFF.
Archive exact 0.6.3 rollback; verify hash/dependencies and Sol/Buzz apt simulation.
Avoid launching against real profiles during artifact checks.

**Stop/done:** Candidate `.deb`, source/version/hash, intended package changes and
rollback recorded. No live acceptance or fleet delivery claimed. No further UI work.

### N03 — Produce the paired server candidate (T04–T08/T27)

**DONE as an artifact, 2026-10-01.** Clean `df517e8` source,
`scripts/package-farside.sh`, pinned KPipeWire `9d6b08c`, jobs 3; no main build or
private library source changes. Package `6.6.80+git202610012028.df517e8-1`, SHA256
`4d938f5b4ba2b4ca374974aeb31123f54e1b70c676606318cfd0783fb0aeb383`, archived at
`~/dev/rdp/debs/candidates/server/df517e8/`. Contract/RUNPATH/dependency/helper/
policy/KCM/unit/PAM checks pass; all three apt simulations change only the two
requested packages. Exact prior server/client rollback artifacts preserved.
Subsequent installation explicitly requested by Steve; original parent gates remain.

Prerequisite N01. **Do:** Freeze `d437511c` plus guard/package fixes in a new clean
checkout; `scripts/package-farside.sh`, pinned library, bounded jobs. Check ELF/
RUNPATH, helpers/policies, KCM resources, units/PAM/conffiles and apt/path conflicts.
Archive exact per-host rollback; do not touch Hal's old build or dependencies.

**Stop/done:** Concrete paired broker/worker package and rollback artifacts exist,
dependencies and Sol/Buzz simulations pass. Fix only packaging failures; no install.

### N04 — Install and prove server rollback on Sol (T08/T27)

**Settings-panel blocker resolved, 2026-10-01:** Steve could not find Farside in
System Settings. `87d8a24a` restores the missing category; clean server87d8a24
installed on Sol, with exact preservation and startup readback accepted. The module
belongs under **Security & Privacy → Farside Remote Desktop**; direct launcher
`kcmshell6 kcm_farside` also works on earlier packages. Manual cancellation/save/
explicit restart acceptance is still pending. Evidence:
`~/dev/rdp/evidence/2026-10-01-kcm-discovery/SUMMARY.md`.

**PARTIAL, 2026-10-01.** Sol install → exact server/client rollback → return
passes with active broker/current-worker binary checks. Existing settings,
certificates, profiles, holds and physical greeter PIDs preserved. Installed
helpers/policies/KCM resources verified. Actual KCM cancel/save/explicit restart/
readback authorization flow still open. No pre-existing retained desktop at cutover.

**October 1 follow-up:** The actual installed KCM loads on Sol's signed-in
Console desktop and invokes the production authorization path. Automation cannot
enter the KDE password field: its accessibility interface exposes Text, not
EditableText. Initial attempts timed out before any settings save; the remaining
Farside challenge was cancelled and temporary accessibility enablement restored.
No agent restart or policy bypass. The camera report exposed missing server
loopback setup; the installed helper saved only Console `CameraLoopbackDevice`
with revision validation/TLS keep, an explicit restart at zero connections applied
it, and runtime inspection verified `/dev/video10`. This proves helper/save/runtime
behavior, **not** normal KCM authentication acceptance. Existing rollback evidence
is reused. Evidence: `~/dev/rdp/evidence/2026-10-01-n04-installed-settings/SUMMARY.md`.

**Remaining manual check (Sol, installed 87d8a24):** Read-only inspection on
October 1 confirms stored and running quality `80`, camera `/dev/video10`,
port `3391`, and runtime `verified`. Use [the short manual checklist](2026-10-01-sol-settings-acceptance.md).
Steve has selected this manual check. Open Farside Host Settings,
load Console with normal KDE administrator authentication. Stage loopback `none`,
cancel its Save authorization, and verify the draft remains while saved/runtime
`/dev/video10` is unchanged. Restore draft `/dev/video10`; make a reversible
quality change, authorize Save, verify runtime still has the previous quality,
and explicitly restart Console when its connection can be interrupted. Inspect
runtime for the saved quality, then restore the original quality through the same
flow. Pass requires cancellation without a write, an authorized scoped save,
explicit restart and matching runtime readback; do not change TLS, wallets or
Virtual settings. These are the only remaining N04 acceptance steps.

Prerequisite N03. **Do:** Inventory active connections/retained workers; schedule
safe restart without discarding desktops. Install candidate; verify running
broker/worker identities and policies. From Buzz, exercise actual KCM cancellation,
scoped save, explicit restart and readback. Demonstrate rollback to the recorded
baseline and return to candidate, preserving configuration/trust/retained state.

**Stop/done:** Exact test-host installed/running candidate and rollback pass; normal
privileged chain works. If existing workers prevent safe replacement, stop with
the exact scheduling requirement. Sol remains a test candidate, not fleet delivery.

### N05 — Accept the packaged client on Buzz/Sol (T06/T09/T27)

**PARTIAL, 2026-10-01.** Packaged client 0.6.4 authenticated/disconnected from
Sol Console and Virtual; Console application received HEVC with hardware encode
and Intel VAAPI decode. Sol client rollback/return passes. Private client profiles
and bus prevent wallet activation; gates request no monitor changes or devices.
Virtual connection created one new retained desktop, then normal systemd stop
cleaned that test desktop; durable audit records retained. Virtual stream/monitor
resize/Fit/opt-in/reconnect/restore and full remembered-password checks still open.
Sol physical seat remains SDDM greeter; no signed-in Console owner.

Prerequisites N02/N04. **Do:** Test actual package: Console/Virtual, custom ports,
current installed server behavior and new advertised capabilities, explicit monitor
opt-in, owned one/two-screen resize/Fit, decoded frames, reconnect and exact restore.
Reuse pure/UI evidence; verify profiles/trust/remembered-password behavior without
automatic migration. Perform client rollback/return once.

**Stop/done:** Included client changes work on actual RDP, metadata/window preservation
and rollback pass. Missing Sol physical owner is a named prerequisite, not a cue
to run more unit tests. Failures get only the needed fix and affected rerun.

### N06 — Release/deploy the accepted client (T27)

**PARTIAL deployment, 2026-10-01.** At Steve's explicit request, Hal/Buzz/Sol
client0.6.4 and serverdf517e8 installed; Hal last, no live test. Existing Hal/Buzz
user :3389 and Sol Console/Virtual routes retained. No full acceptance/tag/fleet
release claim; Ace/Cray/Marvin unchanged. Goal remains paused. Evidence for this
bounded three-host update: `~/dev/rdp/evidence/2026-10-01-three-host-update/SUMMARY.md`.

Prerequisite N05. **Do:** Publish version/tag/source, archive the verified package,
install intended hosts preserving holds/profiles/trust and existing windows; Hal last.

**Stop/done:** Every intended host has the exact package/version result or a named
incomplete target. Existing windows stay open until voluntary relaunch. No server
restart or full P0 completion claim. This is the first planned fleet deliverable.

### N07 — Finish Discover authorization acceptance (T01/T02)

Prerequisite N04. **Do:** Reuse the demonstrated font-cache repair and Steve's
Console success. Finish Console/Virtual normal, incorrect-password/cancel,
second-requester and restart matrix against the candidate; use a reversible operation.
Do not resume idle debugger windows without a current reproducible crash.

**Stop/done:** T02's three normal attempts per mode and failure/restart behavior
pass with one healthy agent per desktop and no policy bypass; diagnostic changes
cleaned up. Original failing-agent stack remains explicitly separate T01 evidence.

### N08 — Close the locker/output-release race (T03)

**Do:** On Sol/disposable desktop, exercise the original lock/disconnect/restore
ordering. Inspect only implicated teardown code if it fails. Do not suppress
Console locking or use Virtual lock policy as a workaround.

**Stop/done:** Original ten controlled interleavings preserve greeter, lock state
and exact outputs; login/owner release restores layout. Existing normal lock
confirmation is reused; any Hal-specific check is Steve-owned and recorded.

### N09 — Finish remaining monitor lifecycle parity (T06)

**Do:** Reuse N05 and quiet-worker evidence. Complete missing workspace/primary/
selected/multi modes, physical mirror/mixed scale/pointer/drag, takeover/login,
worker failure/recovery and retained layout persistence. Split by a named remaining
case when selecting this task; do not rerun accepted N05 resize/Fit without a reason.

**Stop/done:** The selected case has actual captured-picture/input/restore proof;
T06 closes only when its coverage table has no unexplained lost mode or open case.
Supported limits are validated before applying an unsupported request.

### N10 — Finish video/audio policy parity (T05)

**Do:** Select one missing matrix case at a time: actual AVC444/chroma timing,
software/hardware/AV1 tiles, ownership replacement/reset, quality recovery, or
consented playback/mic/duplex priority. Reuse codec policy tests. Investigate loaded
CUDA OOM only with measured allocation/load evidence, not an assumed AI cause.

**Stop/done:** Requested policy matches actual bytes/backend and control readback;
audio priority has its required controlled comparison, stale ownership is cleared.
Full T05 remains partial until all applicable legacy controls are accounted for.

### N11 — Finish Virtual wake/inhibition (T07)

**Do:** Complete Virtual namespace/inhibitor policy using existing worker guard.
Reuse accepted Console DPMS/multi-viewer/failure evidence; verify any changed path.

**Stop/done:** Virtual activity affects only its desktop, teardown releases cookies,
and neither mode unlocks a locked Console. Installed settings apply as documented.

### N12 — Finish camera and redirected media (T11)

**User acceptance, 2026-10-01:** Steve reports “it worked” after server65243df7
delivery and the Hal→Sol BRIO trace. The reported Console camera failure is
resolved. No further camera retest is needed for that issue. Separate app-specific,
switching/LED/bidirectional-call and Virtual requirements remain open; N12 is partial.

**PARTIAL, 2026-10-01 BRIO/PipeWire fix delivered to Sol:** `c48db896` caps
publication at 30 fps (BRIO first MJPEG mode is 640×480/120 fps; Camera filters
out that fixed rate). `765ced19` negotiates full RGBA frame buffers and bounds
chunk writes; previous source supplied empty chunks to a linked app. Native
Camera's normal portal discovers corrected source and starts synthetic preview;
actual source before/after reproduces rejection/empty buffers and corrected capture.
Installed test caught stale Console worker object in reused package cache;
`65243df7` clears both build caches, clean package installed Sol only and then
**30 complete nonzero actual camera frames** pass Buzz→Sol→PipeWire. Client
0.6.5 sufficient; Hal/Buzz servers stay2828a7c. Intermediate765ced1 is marked
REJECTED, not a deployable candidate. Settings/certs/profiles/holds/desktop/client
PIDs preserved; only Sol server package changes. Source prerequisites4 pass and
native/capture evidence reused, no wider matrix. User camera confirmation was
subsequently received (above); switching/LED/call/Virtual requirements remain open. Evidence:
`~/dev/rdp/evidence/2026-10-01-camera-app-debug/SUMMARY.md`.

**PARTIAL, 2026-10-01 readiness fix delivered:** server `2828a7c` and client0.6.5
`82fe55e` installed on Sol/Buzz/Hal. A missing or invalid Console bridge disables
sharing with setup instructions; worker producer failures are errors. Virtual
sharing is disabled with its namespace explanation. Installer does not automatically
install/configure loopback packages. Focused source/model/menu checks and installed
Buzz → Sol 10-frame V4L2/PipeWire provider/portal-presence checks pass. Native
Camera/Meet preview remains open: Sol session1286 was locked; GNOME Camera launched
behind the lock suspended its stream. Preserve the lock; Steve reconnects/unlocks,
enables camera and reopens Camera/refreshes Meet, leaving connection open if it
fails. Record no-device/blank/error and trace that active connection. This is not
proof the lock caused his earlier failure. Source fixes and transport acceptance
are delivered; no full call/Virtual camera/LED acceptance. Evidence:
`~/dev/rdp/evidence/2026-10-01-camera-readiness/SUMMARY.md`.

**PARTIAL, 2026-10-01 user-reported Console camera fix:** Sol had no V4L2 devices
or loopback module despite the Hal client reporting mapping. Installed the five
new loopback/DKMS/tool packages without upgrades/removals, configured persistent
`/dev/video10` with `exclusive_caps=1` and normal desktop `uaccess` permissions,
and applied the Console path through the installed helper plus explicit restart.
Runtime path, device identity, preserved Virtual/TLS settings and apt holds pass.
Hal must reconnect, enable mapping and reopen Camera/refresh Meet to check device
visibility and live video. No real Hal-camera frames or call accepted yet; camera
off means this exclusive device advertises output rather than capture. See
[Console camera setup](../console-camera.md). That setup-only checkpoint preceded the readiness release above.

**Do:** Select the missing Console or Virtual case: namespace/device grants,
on/off/reselect, second camera, LED or external bidirectional call. Reuse existing
JPEG/150-frame/demand-stop evidence. No new camera transport implementation unless
a case demonstrates a defect. Record unavailable hardware/human checks exactly.

**Stop/done:** Selected mode/device consent, usable media, switching and teardown
pass; T11 closes only after its remaining real call/device requirements are met.

### N13 — Standard-client compatibility (T12)

**Do:** Installed Wayland FreeRDP/Remmina on Buzz: Console lock/sign-in, Virtual
attach/create/reconnect, clipboard/monitors/media consent. Record actual capabilities;
no private extensions before handshake. Windows mstsc is a separate available-host case.

**Stop/done:** Versioned matrix distinguishes pass/unsupported/untested. Unsupported
needs a real protocol/client reason. Missing Windows access stays visible; no repeat
of completed Farside-client cases or blanket standard-client compatibility claim.

### N14 — Installed settings and stock coexistence (T04/T08)

**Do:** Reuse N04's installed auth chain and source UI coverage. Verify remaining
stored/applied/restart settings reach the worker and actual stock/Farside KCMs only
change their own files/services. Install stock only on a disposable/non-Hal target.

**Stop/done:** Every current supported field has a working binding and scope;
cancel/invalid settings preserve state, separate services/config/trust/secrets stay
independent. Future GPU selectors remain assigned to N21, not invented here.

### N15 — Implement durable settings/profile migration (T10)

**Do:** Extend the accepted planner, not replace it. Add verified file identity,
private exact backups, stale checks, durable journal, atomic apply/readback,
interruption recovery and rollback refusal over independent edits. Preserve IDs,
layouts/custom endpoints/retained records and independently verify destination TLS.
Keep wallet mutation separate behind N01; dry run cannot access it.

**Stop/done:** Private fixture dry-run/apply agreement, no-op rerun, interrupted
recovery and exact usable rollback pass. No real fleet migration in this task.

### N16 — Migrate Sol/Buzz safely (T10)

Prerequisites N15 and relevant parity N07–N14. **Do:** Inventory/dry run, preserve
originals, apply verified mappings and reconnect/rollback on test hosts. Reconcile
Buzz's three entries only through accepted non-clobbering semantics and a consistent
encrypted backup. If unavailable, retain pending state and give exact reentry steps;
never export values. Existing windows keep their routes until voluntarily reopened.

**Stop/done:** Host/profile/trust/layout/credential results and rollback recorded.
Unresolved passwords/custom profiles remain named exceptions; not full T10 closure.

### N17 — Retire the redundant Farside route (T13)

Prerequisite T02–T12 acceptance and verified migration. **Do:** Cut over one test
host first; remove obsolete Farside per-user unit/listener/code/UI/package branches,
retaining genuine shared code and migration inputs. Console :3391 avoids stock :3389.

**Stop/done:** Clean install/upgrade exposes only Console/Virtual Farside routes,
stock remains independent, no feature/credential/layout is lost, rollback works.
Do not retire the old route just to make a package look finished.

### N18 — Deliver the accepted server/consolidation fleet release (T27/T28)

**Do:** Package the accepted source; reuse earlier unchanged artifact/integration
evidence, check affected dependencies and migration. Gate Intel/AMD where relevant,
deploy remaining hosts, Hal last under its preservation/zero-connection rules.

**Stop/done:** Per-host package/running worker/listener/integrity/migration result,
rollback and remaining exceptions recorded. P0 closes only when T01–T13 and its
delivery requirements are satisfied; a successful fleet install alone is insufficient.

## Later queue: choose one after consolidation delivery

All rows retain their original detailed acceptance criteria. Each selected row
gets one concrete case if too large for a normal task; completed cases accumulate.

| Next task | Original | Instructions | Done / focused proof |
|---|---|---|---|
| N19 NVIDIA HEVC | T14 | Reuse quiet worker/Virtual motion; finish Console motion, two-screen, resize/keyframes/replacement/failure and NVDEC on an authorized host | Both modes sustain valid HEVC and recover/fail clearly; truthful context/backend. Performance remains N30; Hal live NVDEC is Steve's check |
| N20 Stable GPU inventory | T15 | PCI identity → current node/CUDA ordinal, actual-context per-codec probes and runtime telemetry | Reordering preserves the chosen GPU; restricted/unavailable devices and Sol AV1 are not falsely advertised. VAAPI, NVENC/NVDEC, software; QSV/Vulkan are available only if implemented/proven |
| N21 GPU selectors | T16 | Server authority/defaults plus client encode/decode choice; one device per stream, independent screen assignments | Save/reconnect/readback and actual telemetry agree; unavailable/fallback reason clear. Preserve Hal AMD auto and Sol NVIDIA HEVC; no arbitrary cross-vendor frame splitting |
| N22 NVIDIA AVC420 | T17 | Implement/probe NVENC/NVDEC AVC420, shared selection/capability/packaging | Actual keyframe/motion/resize/reconnect/failure with Farside and available standard client; never relabel 420 as 444 |
| N23 NVIDIA AV1 | T18 | Eligible hardware only; dimensions/render size/tiles/context/keyframes | Valid visible motion/resize/one-two-screen/fallback on authorized eligible hardware; Hal-only gates stay partial, no agent live Hal benchmark |
| N24 DrKonqi | T20 | One bounded current reproduction/observation; identify reporter vs crashing application; fix only demonstrated cause | Original trigger plus 30-minute normal observation has no loop; one ordinary report still works. No current recurrence means diagnostic evidence remains partial, no endless reporter polling |
| N25 Startup cleanup | T21 | Product fixes already shipped; verify normal fresh launch after Steve voluntarily restarts stale Hal Codex parent | No obsolete preload inherited, real certificate warnings still correct; do not kill work processes or rerun all delivered QML/certificate tests |
| N26 Independent decode | T22 | Per-surface bounded workers/queues, Qt thread boundaries, generation/ack/cleanup | Delayed stream does not stall peer; bounded memory/order, peer p95 within original 10% single-stream limit under matched load |
| N27 HEVC tuning | T23 | One fixed text/motion/image corpus across supported backends; compare before changing defaults | At matched quality within 1dB PSNR, 10% bitrate or p95 improvement plus text/frame-budget acceptance; otherwise keep defaults and record no optimization delivered |
| N28 Sessions page | T24 | Authorized list/resume/disconnect/end, stable ownership, populated/offline/empty states | Listing opens no desktop; End confirms and targets only authorized desktop; stale/restart/keyboard behavior works |
| N29 Client UX | T25 | Update old UX design to exactly Console/Virtual, service/account/trust boundaries and discovery separate from attach | Real add/edit/open/create/resume/disconnect/end flows, narrow/wide/light/dark, keyboard/errors; integrate N28, preserve migration and no third type |
| N30 Performance/visual | T26 | Fixed quiet then representative recorded load, 1080p30 plus two-screen/text/resize, 60fps only when claimed; scoped network throttle on non-Hal | Original 5-minute fps/p95/bounded-queue targets, loaded recovery and 6Mbit/s slow-link criteria; AV1 bitrate/headers and human cursor/mapping/drag/Hal checks explicit |

## Original-task accounting

This table is the compact working record. Update only affected rows with commit,
package/host and evidence links. Existing detailed evidence stays in the old ledger;
do not duplicate its long history here or close a task by renaming it.

| Original task | Current state | Next work |
|---|---|---|
| T01 PolicyKit diagnosis | Partial; font-cache evidence, original agent trace missing | N07 diagnosis if reproducible; no indefinite idle capture |
| T02 Discover | Partial; Steve's Console success accepted | N07 |
| T03 Locker race | Partial; normal lock works | N08 |
| T04 Config/auth scopes | Partial; source accepted, unshipped | N04/N14/N16 |
| T05 Video/audio | Partial; source/live component evidence | N10 |
| T06 Monitors | Partial; source/private quiet worker evidence | N05/N09 |
| T07 Wake | Partial; Console component accepted | N11 |
| T08 Settings/coexistence | Partial; scoped source UI accepted | N04/N14 |
| T09 Client types | Delivered 0.6.3 | Reuse; changed paths only in N05 |
| T10 Migration | Partial; planner and N01 source guard accepted, unshipped | N15/N16; N01 done |
| T11 Camera/media | Partial; transport/demand evidence | N12 |
| T12 Standard clients | Planned | N13 |
| T13 Legacy retirement | Planned; old route retained | N17 |
| T14 NVIDIA HEVC | Partial; worker/Virtual motion evidence | N19 |
| T15 GPU identity | Planned | N20 |
| T16 GPU selection | Planned | N21 |
| T17 NVIDIA AVC420 | Planned | N22 |
| T18 NVIDIA AV1 | Planned; Sol hardware ineligible | N23 |
| T19 Health explanations | Delivered 0.6.3 | Reuse; thresholds remain unchanged |
| T20 DrKonqi | Partial; chronology, no current loop proven | N24 |
| T21 Startup/dialogs | Partial; product fixes delivered, stale Hal parent cleanup | N25 |
| T22 Independent decode | Planned | N26 |
| T23 HEVC tuning | Planned | N27 |
| T24 Sessions page | Planned | N28 |
| T25 UX | Planned | N29 |
| T26 Performance/visual | Partial; functional/load snapshots only | N30 |
| T27 Releases | Partial; N02/N03 artifacts complete; client0.6.5/server2828a7c Hal/Buzz/Sol installed, other hosts unchanged | Remaining N04/N05 integration, N06 fleet/tag and N18 consolidation; later deliveries |
| T28 Documentation | Ongoing | This plan + short affected-row/handoff updates per task |

### Prompt for a fresh task

```text
Use GPT-6.1-sol for Farside task NXX in
docs/plans/2026-10-01-farside-task-plan.md.
Read the newest shared instructions and only the relevant task/source/evidence.
Reuse accepted work. Complete this task's result with focused verification.
Preserve wallets, Hal's work desktop, credentials, trust and rollback.
Fix only this task or a demonstrated blocker; no automatic Goal or delegation.
Report the result, exact versions/hosts, checks and open limitations; update the
affected progress rows and a short handoff, then stop. Do not start another task.
```

## References and precedence

- Product requirements/acceptance: [original T01–T28 plan](../superpowers/plans/2026-09-30-farside-remaining-work.md).
- Prior diagnosis/release assessment: [October 1 assessment](../superpowers/plans/2026-10-01-farside-release-and-completion.md).
- Detailed accepted evidence: `.superpowers/sdd/2026-09-30-farside-remaining-work/progress.md`
  and `~/dev/rdp/evidence/`; old checkpoints are historical, not automatic next actions.
- Original specs remain references for Console/Virtual inventory, migration, broker
  settings and NVIDIA; `~/dev/rdp/CLIENT-UX-REDESIGN.md` needs sole-Console revision.
- Steve's newest instruction to work task by task supersedes old Goal persistence,
  automatic next-task and formal review loops. Hal/wallet protections remain.
  Changing a product acceptance requirement needs an explicit recorded decision;
  optimizing execution does not silently drop features or mark partial work done.

Initial planning checkpoint: no wallet access, candidate build/install, host restart,
model/configuration change or Goal activation is part of creating this document.
