# Console/Virtual shared settings coverage (T04–T08)

Date: 2026-09-30. Status: PARTIAL implementation; not a cutover approval.
Plan: [remaining work](../plans/2026-09-30-farside-remaining-work.md).

## Authority and application

System broker configuration owns listeners, TLS, account admission and device
grants. An authenticated user's `~/.config/farsideserverrc` supplies preferences,
read once per connection with that account's filesystem UID/GID. No path from a
client record, another user's config, the broker's HOME or XDG environment is used.
The shared reader rejects unresolved/root identities, failed identity changes,
foreign/nonregular/oversized files, final symlinks and inconsistent reads. The
shared parser rejects the entire recognized preference transaction on invalid
values. Missing/unreadable preferences inherit configured broker defaults.

Recognized preferences neither modify host administration nor expand a host
grant. Standard media requires both host permission and user preference, followed
by standard channel consent. Client device overrides still require authenticated
ownership and reset on ownership loss. Audio priority applies only with enabled
media; selecting it is not consent to capture microphone/camera.

The source implementation applies the shared video/chroma/audio preferences,
Console capture/temporary-output policy, Virtual stock-client policy and worker
wake demand. The dated checkpoints below distinguish implementation, source
verification and still-open native gates. Parsing alone does not claim support. Production broker entry
points enable the reader; socket-free tests inject it and never read Hal's actual
settings. Legacy per-user serving remains in place until all parity gates pass.

## Complete legacy schema inventory

Defaults below describe `krdpserversettings.kcfg`; broker defaults can be explicitly
different. T10 migration must materialize old effective defaults, including values
omitted by KConfig, rather than rely on the destination's defaults. Every preference
currently takes effect on a fresh connection. T08 must disclose this and provide
the required save/reconnect behavior; live reload is not implemented by this slice.

| Legacy field | Default | Destination scope / applicable type | Current implementation / remaining task |
|---|---|---|---|
| ListenPort | 3389 | Host admin / both, independently | Console 3391, Virtual 3395; custom ports remain supported; T08/T10 migration |
| ListenAddress | all interfaces | Host admin / both | Both CLI listeners; T08 host editing |
| AutogenerateCertificates | true | Host admin / both | Existing TLS helpers; T08/T10 equivalent, destination trust independently verified |
| Certificate | empty | Host admin / both | Independent service TLS, never accepted from user preferences; T08/T10 |
| CertificateKey | empty | Host admin / both | Independent service TLS, never accepted from user preferences; T08/T10 |
| Quality | 75 | User preference / both | Validated 0–100, inherited host default 80 when missing; owner reset retains user's cap |
| AdaptiveQuality | true | User preference / both | Validated Boolean; missing inherits host default false; T05 live recovery |
| Codec | auto | User preference / both | auto/avc420/avc444 applied within actual client caps and current worker availability. Console shared viewers use AVC420; sole admitted controller and current Virtual owner honor saved preference. Runtime auxiliary/software fallback revokes444. Actual AVC444 bytes/adaptive acceptance and T08 remain |
| SoftwareEncoding | auto | User preference / both | auto/never/prefer applied to connection encoder policy; T05 live backend parity |
| Av1Tiles | auto | User preference / both | auto/1/2/4/8/16 applied to connection, worker bridge; T05 live headers |
| PreferAudioQuality | false | User preference + owner override / both | Applied/reset; T05 measured audio acceptance |
| Avc444MotionGapMs | 100 | User preference + owner override / both | Complete tuple forwarded/applied to workers on wire8; current-owner partial overrides and reset tested. Actual AVC444/auxiliary bytes and T08 gates remain |
| Avc444RestMs | 150 | User preference + owner override / both | Same shared validated/reset policy; native Console HEVC dormant-policy transfer passes, native auxiliary encoding remains T05 |
| Avc444MaxGapMs | 1500 | User preference + owner override / both | Same shared validated/reset policy; out-of-range/inconsistent merge refused atomically |
| MonitorMode | multi | User preference / Console; Virtual layout is separate | workspace/primary/specific/multi applied through current-owner wire10; wire11 configured temporary output creation/restore and worker-owned resize/Fit pass native worker gates. Broker/client transaction gates pass separately; real RDP integration and other T06 gates remain |
| MonitorIndex | 0 | User preference / Console specific capture | Current-owner specific capture uses actual QScreen index with its own surface/global origin; unavailable selection fails without whole-workspace fallback. Remaining T06 lifecycle/client gates open |
| VirtualMonitorPolicy | replace | User preference / Console's client-created outputs | Current-owner wire11 policy now creates owned outputs under guard/journal; real private Sol replace/extend and Stop/withdraw exact restore pass. Native local takeover/worker loss/login/failure and T03 lock race remain |
| VirtualMonitorLayout | client | User preference / Console's client-created outputs | Client single/two-output creation, every-screen decode and worker-owned resize/Fit pass on Sol. Revision-bound one-use broker transactions and explicit client capability/ownership/UI gates pass separately; owned projection cannot grant full-layout writes. Native real RDP/mirror/mixed-scale/client interaction and retained persistence remain |
| VirtualMonitorFallbackSize | 1920x1080 | User preference / output creation | Even bounded size and normalized peer fallback transported on wire11; per-surface/single fallback planner tested. Dynamic native limits and lifecycle acceptance remain T06 |
| VaapiDriverMode | auto | Host device setup plus user backend preference / both | Process/worker setting remains host-controlled; driver choices cannot mutate root broker environment; equivalent T05/T15/T16 |
| WakeDisplayOnConnect | true | User preference / Console physical seat; Virtual own desktop only | Applied through authenticated wire v7 DisplayPolicy, worker session/private bus; Console live enabled/disabled/viewer/failure gates pass; Virtual native and KCM gates remain |
| StandardClientMedia | true | Host ceiling AND user preference AND consent / both | User may opt out; never turns denied host permission on; T11/T12 live media |
| VirtualStockClientPolicy | attach-or-create | User preference / Virtual | Same validated transaction; attach-or-create/refuse applied before stock-client gate |
| CameraLoopbackDevice | empty | Host device grant / both | Existing host CLI path; user config cannot select/grant arbitrary device; equivalent device mapping T04/T08/T11 |
| Users | empty | Host admission / credentials bound to original owner | Root policy maps salted password verifiers to the original owner's OS UID; real RDP alias authentication passes without a forged PAM identity. T08 revision-bound helper and scoped sign-in page now pass pure, private root-filesystem and Buzz UI gates; installed authorization/restart, broader T08 and T10 migration/retained desktop gates remain |
| SystemUserEnabled | false | Host admission / both | Root policy selects independent PAM any/allow-list/disabled routes. Canonical PAM account admission and denial pass on Sol; new T08 page/helper edit the two routes separately without exposing verifiers. Installed save/restart and T10 preservation of legacy owner-only/disabled effective values remain |

## Other settings and overrides

The installed Console/Virtual environment files and broker CLI currently also
control worker executable/runtime paths, journal/storage paths, listener/TLS paths,
quality/adaptive/audio defaults, standard media, camera loopback, software encoding,
AV1 tiles and VAAPI process setup. Worker executables and storage/journal paths are
host administration, not user preferences. Experimental topology flags remain
host-only and off by default; a user configuration must not enable them.

Authenticated control overrides include codec/decode preference, audio priority,
per-device consent/reselection, display-control/layout/resize and supported topology
operations. Worker generations and ownership checks remain authoritative; no new
record chooses a configuration UID. Worker loss/rebinding clears previous owner's
overrides and reapplies the new connection's validated defaults.

Display demand is independent of input ownership: Console aggregates eligible,
admitted, streaming viewers and enables waking/inhibition if any of them wants
it. A worker may receive this policy after authenticated Hello, before capture
Ready, so sleeping outputs cannot prevent the wake that makes capture usable.
No input/layout authority is granted before Ready. Strictly increasing policy
revisions reject stale reactivation after release. Virtual sends demand only
for its current authenticated attachment and releases it on revoke. Its worker
uses the private desktop bus; root broker buses are never used for this policy.
The paired broker/worker wire is now v11 in source; installed fleet remains v6.
Delayed Inhibit replies release their cookies even after guard destruction,
using the replying service's unique owner rather than a replacement service.

Wire8 also carries complete chroma timing and adaptive auxiliary demand to every
worker encoder. Actual auxiliary capability reports return separately from
backend reports. Console overrides require an admitted current controller;
Virtual requires its current authenticated attachment. Partial timing updates
merge atomically over that owner's current tuple. Control transfer/desktop detach
restore validated user defaults, and old worker reports cannot steer a revoked
bridge. Acceptance acknowledges policy only, not AVC444 availability. Six focused
suites and native Sol Console HEVC/dormant-policy transfer pass. AVC444
actual auxiliary bytes, timing telemetry, native Virtual/multi-screen
adaptive gates and matching KCM remain T05/T08. Evidence
`~/dev/rdp/evidence/2026-09-30-t05-chroma-policy/SUMMARY.md`.

The later T05 AVC checkpoint applies saved codec preference dynamically within
the client's advertised standard formats and the bound worker's actual probe.
Private HEVC/AV1 retains authority; its AVC fallback uses current preferences
and capabilities. A lost auxiliary encoder or software fallback removes444
availability. Shared Console viewers use420; control release/acquire and final
viewer departure recompute policy. Unbind clears availability, and a replacement
worker applies its own probe. Old format packets cannot be sent with a new
codec ID; changing420/444/v2 requires a header-bearing keyframe. Selection uses
one atomic read snapshot, and queued consumers read current policy. Eight pure
suites and native Sol/Buzz one/two-viewer AVC420 fallback pass. This is a negative
availability gate on Sol, not AVC444 hardware acceptance. Evidence
`~/dev/rdp/evidence/2026-09-30-t05-avc-selection/SUMMARY.md`.

## Configured Console output resize and Fit (T06 source checkpoint)

The verified incomplete Console projection advertises `consoleOwned` together
with `consoleVirtual`, lease lifetime, virtual kind and physical-console owner
for every output. These flags describe creator ownership; connector names alone
grant no authority. Only the current admitted controller with a Ready worker
gets resize/scale capabilities. Viewers get the same inventory without write
capabilities. Add/remove, position, primary and physicalChange stay unavailable.
Physical-layout experiments remain off by default.

Single or multiple owned outputs can preview output-specific resize or Fit with
dependent edge reflow. The broker binds a one-use token to full before inventory,
priorities, topology revision/generation, controller generation and lifetime.
Worker dispatch resolves stable output IDs and edge relations to backend keys.
Successful worker results trigger independent exact topology/capture readback;
wrong/stale result kinds cannot advance a transaction. A partial result stops
forwarding and requires verified output restoration before lease release.

The client requires the explicit ownership capability and exact owner/kind for
all outputs, rejects broad physical/foreign authority, and permits single-output
Fit without physical consent. Pure parser/transaction and Buzz offscreen dialog
gates pass. Five private Sol worker cases pass with decoded resize/Fit frames and
exact restoration. These separate checks do not yet establish real RDP transaction
integration or complete T06/T03/T08/T27 acceptance. Evidence:
`~/dev/rdp/evidence/2026-09-30-t06-owned-transactions/SUMMARY.md`.

The 2026-10-01 private real-RDP attempt verifies PAM, owned single resize/rev2,
fresh decoded HEVC dimensions and exact restoration. Final Fit fails with CUDA
out-of-memory under external AI load; it is not accepted. The software rerun
lost its actual Console owner to signout before capture. The bounded manual
host now aborts on loss of UID1000's physical desktop. Final source builds and
Buzz offscreen dialog9 pass, including a transient capability boolean fix.
Own-client initial standard monitor advertisement remains absent, so native
owned two-screen integration needs independent implementation. No deployment;
T06 and the T05 loaded failure/fallback gates remain open. Evidence:
`~/dev/rdp/evidence/2026-10-01-t06-owned-rdp/SUMMARY.md`.

The later 2026-10-01 checkpoint adds explicit client Console monitor
advertisement (`ef692be`), default off and independent of local mapping or
retired assignments. Three pure suites/Buzz form12 and actual two-screen
GUI-to-engine settings pass; closed-port plumbing is not actual RDP acceptance.
After Steve paused AI work, seven private Sol worker cases pass, including
single/two hardware HEVC resize/Fit/decoded frames/exact restoration. Initial
Extend placement reasserts the requested tuple after KWin connector replay;
capture waits for KScreen/Qt agreement. Post-Ready worker failures retain a
bounded reason and fail closed. Per-process render-group fixture access changes
no account/device permission. Sol's physical UID1000 desktop is absent at the
greeter; final real Console RDP and broader T06/T03/T08/T27 gates remain open.
No installed delivery or performance acceptance. Evidence:
`~/dev/rdp/evidence/2026-10-01-t06-monitor-request/SUMMARY.md`.

## Remaining gates

- T04 authentication source gates pass: immutable root policy, explicit alias
  owner identity, canonical PAM admission, rejected credentials and unsafe-file
  startup refusal. T08 administrator editing and T10 legacy credential migration
  and actual retained-desktop equivalence remain unshipped. See the contract below.
- T05–T07 must implement the unapplied rows with no unexplained feature loss.
- T08 must wire matching KCM scope/help/reconnect behavior and dual-KCM acceptance.
- T10 must test omitted default values, backups, credentials and independent trust.
- Both brokers must pass live per-user gates on Sol before any package rollout.
  No Hal live tests; no legacy route retirement from this implementation alone.

## Root authentication contract (T04 source checkpoint)

Both broker entry points load `/etc/farside/authentication.json` before starting
listeners or desktop controllers. `--authentication-policy` selects an explicit
path. It must be a root-owned regular file with no group/other permissions,
at most 65536 bytes, no final symlink and a consistent bounded read. Invalid
Console **or** Virtual policy refuses the complete startup transaction. Schema
version 1 requires exactly `version`, `console`, and `virtual`; each route has
`pam` and `credentials`. Route/credential fields and types are validated, and
duplicate credential aliases or canonical account UIDs are refused. JSON object
keys use Qt JSON's semantics; this does not claim duplicate object-key rejection.

```json
{
  "version": 1,
  "console": {
    "pam": {"mode": "allow-list", "accounts": ["westers"]},
    "credentials": []
  },
  "virtual": {
    "pam": {"mode": "disabled", "accounts": []},
    "credentials": []
  }
}
```

`pam.mode` accepts `any`, `allow-list`, or `disabled`. Only allow-list permits a
nonempty accounts array. Every listed account resolves to a nonroot canonical
OS UID before listening. Existing PAM authentication and account-management
checks still precede admission. Each route can carry up to 128 aliases, each
with exactly `alias`, `owner` and `verifier`. The owner must resolve to a nonroot
OS account; the alias is never interpreted as an account name. The verifier
format is `pbkdf2-sha256$600000$<32 lowercase hex salt>$<64 lowercase hex digest>`.
Salt is 16 random bytes; password UTF-8 bytes use PBKDF2-HMAC-SHA256. Digest
comparison is constant-time; no plaintext password is stored in this policy.

Successful PAM publishes both PAM UID and desktop-owner UID. Successful custom
credentials publish only their explicitly granted desktop-owner UID. Neither
identity exists before authentication or after close. Console seat admission
and all Virtual ownership/control/media paths use this owner identity, never
a UID or username claimed in a client record. Unscoped legacy `Server::users`
credentials cannot bypass an installed broker policy. Each authenticated owner's
preferences come from that owner's filesystem identity, even if the alias
spells another account's name. PAM is attempted first when enabled; an explicit
custom grant is a separate fallback and does not masquerade as PAM authentication.

Policy and account-resolution snapshots are installed before listening and
cannot be changed while listening or registered peers remain. Administrator
edits/account changes require broker restart; an existing stream retains its
snapshot. User preferences remain read on fresh authenticated connections.

**Missing-default and migration boundary:** An absent *implicit default* policy
preserves the currently deployed brokers' PAM-any-nonroot admission and has no
custom aliases. An absent explicitly selected file fails startup. This default
is not the legacy per-user migration policy: T10 must materialize
`SystemUserEnabled=true` as an allow-list containing the original daemon owner,
or `false` as disabled. Every migrated legacy custom user must retain that
original owner's UID and exact alias/password semantics. Do not resolve the
alias as its owner or use `any` to migrate owner-only admission. Preserve old
wallet/config/routes until T10 backups, destination trust and rollback gates
pass. T08 must provide matching privileged editing/help/restart behavior before
this source checkpoint is packaged.

Sol source acceptance: 20 policy tests and 11 real RDP loopback entries passed;
three existing authority suites passed. Buzz → bounded scratch Console :3397
accepted PAM and a custom alias with HEVC first frames and snapshots, while an
empty PAM allow-list rejected the valid OS password. A separate bounded :3398
fixture uses the production Virtual controller/transport and an empty private
registry with no desktop launcher: it accepts alias owner UID1000 with no PAM
identity and separately accepts real PAM UID1000; disabled PAM and wrong
passwords are refused. This proves authentication/control acceptance, not a
retained Virtual desktop migration. Both production CLI entry points reject
explicit missing, nonroot-owned, group-readable, malformed, symlink and FIFO
policies before listening. Installed services/desktops remain unchanged; scratch
credentials/TLS copies/desktop permission entry were removed. Evidence:
`~/dev/rdp/evidence/2026-09-30-t04-auth-scopes/SUMMARY.md`.

Wire9 additionally returns complete per-output chroma costs, including auxiliary
max-gap and stage min/avg/max. Current generation/exact codec and binding checks
reject stale reports. Primary/per-output producers participate; complete bounded
rate-limited logs and measured encode time survive without stats subscription.
Ten focused suites pass. Actual444/native timing and T08 gates remain; evidence
`~/dev/rdp/evidence/2026-09-30-t05-chroma-costs/SUMMARY.md`.

Wire10 carries Console capture preferences under the current control generation.
Selected capture readback is a projection explicitly distinct from complete
physical topology. It cannot grant full-layout editing; workspace payloads retain
one aggregate surface, independent mode retains per-output surfaces. Retained
Virtual capture/layout remains separate. Five real-worker Sol cases and focused
broker/input/authority checks pass; client-created Console outputs, lifecycle,
limits, full retained persistence and matching T08 acceptance remain T06/T08.
Evidence `~/dev/rdp/evidence/2026-09-30-t06-capture/SUMMARY.md`.

Wire11 configured Console output lifecycle is now implemented in source. Nine
focused suites, five strengthened private Sol worker cases and 61 corrected
Virtual controller cases pass. Separate KScreen queries verify fixture baseline
enable/disable and exact restoration; every screen decodes. Connector aliases
are test-only, so physical hardware behavior is not accepted. Native takeover,
failure/worker loss/login, mixed-scale client interaction, resize/Fit, retained
persistence and T03/T08/package gates remain. Evidence
`~/dev/rdp/evidence/2026-09-30-t06-virtual-lifecycle/SUMMARY.md`.

Configured Console owned output-specific resize/Fit is now implemented at the
worker boundary, including single output, fractional scale, dependent reflow
and large global Extend origins. Complete foreign-output preservation and
fresh per-output decoding/restoration pass in five private Sol native cases;
four pure suites pass. Recreation restores requested modes even when KWin
replays an old connector configuration. Broker preview/commit, client ownership
capability validation and UI integration remain; this does not expose or ship
the feature yet. Native physical hardware/mixed-scale client/persistence and
T03/T08/package gates remain. Evidence
`~/dev/rdp/evidence/2026-09-30-t06-owned-resize/SUMMARY.md`.
