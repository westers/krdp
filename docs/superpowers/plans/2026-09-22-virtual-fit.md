# Virtual desktop Fit

Goal: Fit changes the retained virtual desktop's actual native resolution and
scale to the selected client screen. It is not client-side scaling. Preserve
the compositor, applications and output identity; retain a successful size on
detach/reconnect. Physical-console resize/restore semantics must not change.
Maintenance guard and automatic crashed-session recovery remain parked.

## Narrow interface

- Advertise optional strict boolean `virtualResize` on a successfully bound,
  PAM-authorized virtual attachment. Older servers without it are unsupported.
- Client request: `type:"virtual-resize", v:1, id:<bounded string>, width:<int>,
  height:<int>, scale:<finite number>`. No client output name, executable,
  compositor socket, PID or session environment is accepted.
- Reply correlates `type/v/id/ok`, with a useful error on refusal. No successful
  reply before fresh mode/scale readback and a matching encoded keyframe.
- Reuse authenticated generation-tagged Resize IPC with the internal fixed
  output selector `virtual-desktop`. Only an explicitly virtual-launched worker
  resolves that selector to its sole private output. Physical mode remains on
  its existing planner and restore lifecycle; never infer isolation from names.
- Bound dimensions to even native pixels, width320..4096, height200..4096,
  scale1..4; preserve the existing encoder limits. Fit must normalize odd local
  native dimensions explicitly and report the actual requested size.
- One outstanding request per binding, monotonic overflow-safe worker request
  IDs, bounded timeout, current owner/handle/manager/control generation checked
  at dispatch and completion. Clear pending client state before detach/rebind
  notifications. Stale replies cannot complete a successor request.

## Worker behavior

Use a separate virtual-specific planner/lifecycle rather than granting the
physical planner permission to touch arbitrary virtual outputs. Read the private
compositor's output snapshot and require exactly one enabled, connected output,
normal orientation. Explicit virtual launch establishes the backend boundary;
KScreen JSON omits custom-mode capability flags, so new modes require verified
addition/readback, not an invented capability field. Generate fixed
kscreen-doctor arguments, never shell commands. Verify the installed custom-mode
syntax and readback behavior from source and a disposable session before coding
assumptions about mode IDs. Custom modes may receive new IDs; geometry and scale
are authoritative. Preserve current refresh rate and output position/identity.

Gate input and release held keys/buttons before discovery, including no-op Fit.
Suppress both frame and output-metadata forwarding throughout resize/recovery.
After readback, explicitly reopen the encoder even when logical geometry did
not change. Only observed producer-thread teardown (`nodeId == 0`) establishes
a replacement capture epoch; an inactive/active pair alone is insufficient for
a producer that was starting. Require that epoch, replacement activation, then
a keyframe whose frame metadata dimensions and scale agree. Frame size is not
decoded payload proof: a new producer can negotiate an old PipeWire format
before a later format update. Require independently verified H.264 payload
dimensions on the candidate too (bounded SPS/PPS/IDR check and clean single-frame
software decode); no metadata-only acknowledgement. This is resize-only work,
not a normal video decode stage. Verify client-decoded dimensions in native
acceptance as well.
An exact no-op still requires trustworthy current geometry; avoid unnecessary
modesets. Keep the previous geometry only for failure/cancellation rollback,
not disconnect restoration after successful completion. Conditional rollback
must not overwrite an independent display change. Bounded failure must surface
honestly, and stale coordinates must not regain input just because a timer ran
out. Detach/rebind during a pending apply cannot generate stale success.

## Client and delivery

Route virtual Fit before the generic host-layout mapping branch. Reuse local
screen native-size/scale calculation and existing Fit action, with disabled
state during a request and an honest unsupported/unattached tooltip. Keep
Scaled and1:1 distinct. Maintain compatibility with physical-console and
ordinary desktop servers. Expose a debug action using the same model dispatch
for native GUI acceptance; no fabricated success or separate test-only path.

Build/test backend first, then client against the fixed contract, with one
combined final review. Preserve unrelated guard worktree edits. Stage matched
broker/worker binaries only in Sol's isolated prefix through the existing
user-run tmux installation workflow. Produce an updated client package and
document any release-packaging limitations rather than claiming it shipped.

## Acceptance

Automated: planner bounds/custom modes/ambiguous output rejection, no-op,
command failure/partial readback/conditional rollback, encoded-keyframe gate,
input safety, current ownership/stale generations/request IDs, detach/rebind,
timeouts and destruction/reentrancy, strict optional capability parsing,
client pending/error UI and unchanged physical behavior. Exercise actual framed
worker IPC, not parser-only tests.

Native Buzz GUI against new disposable Sol virtual desktop: Fit changes decoded
dimensions and actual compositor readback; test multiple sizes and fractional
scale within supported constraints. Check pointer coordinates, an app/content
surviving Fit and disconnect/reconnect, retained geometry, playback/microphone
continuity, repeated Fit/no-op, explicit Stop. Confirm physical session/apps,
outputs and normal Buzz configuration unchanged. Do not SIGTERM the capture
worker to enable logging; seed only a new disposable profile before it starts.
No live implementation or acceptance has occurred at this planning checkpoint.

## Confirmed backend details and bounded mode policy

Installed libkscreen-bin6.6.4 supports
`output.<name>.addCustomMode.<width>.<height>.<refresh-mHz>.full`.
Adding does not select: add, fetch a fresh snapshot, select its fresh mode ID,
then read back again. Never persist IDs across changes. Use the previous
positive refresh rate (mHz integer), full blanking, current output position and
normal orientation. KWin rounds scale to1/120; normalize requested scale before
dispatch and account for Wayland fixed-point conversion/readback. Keyframe
completion must use actual encoded pixel dimensions, not only rounded logical
geometry multiplied by scale.

Reuse matching advertised dimensions/refresh before adding. Keep at most64
advertised modes in the private output: refuse a new distinct mode at that
bound with an explicit error, but allow selecting existing modes. Retain added
modes until this disposable/retained compositor exits. Do not guess custom-list
removal indices from JSON: KScreen omits custom flags/list indices. This bounded
policy avoids introducing a second display configuration SDK or deleting modes
owned by desktop applications. Repeated Fit to the same client screen must not
append duplicates. No arbitrary mode removal or maintenance machinery.

Evidence: libkscreen v6.6.4 src/doctor/doctor.cpp addCustomMode/findMode/setMode
and src/configserializer.cpp; local KWin6.6.6
`/tmp/kwin-source.4zNey9/src/backends/virtual/virtual_output.cpp` applyChanges and
`src/wayland/outputmanagement_v2.cpp` scale handler. The disposable Sol probe
confirmed add-only behavior, selection of fresh mode2 at1920x1080/1.25 and
restoration to1280x720/1. It also exposed an old CPU encoder filter remaining
1280x720 after capture changed to1920x1080, with client decoder errors. Thus a
metadata/keyframe check without guaranteed encoder replacement is insufficient.
Probe evidence is in rdp/evidence/virtual-fit-probe/; new Fit implementation has
not yet been deployed or runtime-accepted.
