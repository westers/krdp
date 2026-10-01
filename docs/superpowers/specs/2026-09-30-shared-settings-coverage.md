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

The source implementation currently applies Quality, AdaptiveQuality,
PreferAudioQuality, SoftwareEncoding, Av1Tiles, StandardClientMedia and
VirtualStockClientPolicy and WakeDisplayOnConnect. Remaining parsed preferences are held for their task's
worker integration. Parsing alone does not claim support. Production broker entry
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
| Codec | auto | User preference / both | auto/avc420/avc444 parsed; AVC444 worker parity and application T05 |
| SoftwareEncoding | auto | User preference / both | auto/never/prefer applied to connection encoder policy; T05 live backend parity |
| Av1Tiles | auto | User preference / both | auto/1/2/4/8/16 applied to connection, worker bridge; T05 live headers |
| PreferAudioQuality | false | User preference + owner override / both | Applied/reset; T05 measured audio acceptance |
| Avc444MotionGapMs | 100 | User preference + owner override / both | Complete tuple forwarded/applied to workers on wire8; current-owner partial overrides and reset tested. Actual AVC444/auxiliary bytes and T08 gates remain |
| Avc444RestMs | 150 | User preference + owner override / both | Same shared validated/reset policy; native Console HEVC dormant-policy transfer passes, native auxiliary encoding remains T05 |
| Avc444MaxGapMs | 1500 | User preference + owner override / both | Same shared validated/reset policy; out-of-range/inconsistent merge refused atomically |
| MonitorMode | multi | User preference / Console; Virtual layout is separate | workspace/primary/specific/multi/virtual parsed; capture/layout parity T06 |
| MonitorIndex | 0 | User preference / Console specific capture | Nonnegative index parsed; worker selection T06 |
| VirtualMonitorPolicy | replace | User preference / Console's client-created outputs | replace/extend parsed; restore/local takeover T06, lock race T03 |
| VirtualMonitorLayout | client | User preference / Console's client-created outputs | client/single/physical parsed; retained Virtual topology remains separate; T06 |
| VirtualMonitorFallbackSize | 1920x1080 | User preference / output creation | Even bounded dimensions parsed; hardware limits still enforced at application; T06 |
| VaapiDriverMode | auto | Host device setup plus user backend preference / both | Process/worker setting remains host-controlled; driver choices cannot mutate root broker environment; equivalent T05/T15/T16 |
| WakeDisplayOnConnect | true | User preference / Console physical seat; Virtual own desktop only | Applied through authenticated wire v7 DisplayPolicy, worker session/private bus; Console live enabled/disabled/viewer/failure gates pass; Virtual native and KCM gates remain |
| StandardClientMedia | true | Host ceiling AND user preference AND consent / both | User may opt out; never turns denied host permission on; T11/T12 live media |
| VirtualStockClientPolicy | attach-or-create | User preference / Virtual | Same validated transaction; attach-or-create/refuse applied before stock-client gate |
| CameraLoopbackDevice | empty | Host device grant / both | Existing host CLI path; user config cannot select/grant arbitrary device; equivalent device mapping T04/T08/T11 |
| Users | empty | Host admission / credentials bound to original owner | Root policy maps salted password verifiers to the original owner's OS UID; real RDP alias authentication passes without a forged PAM identity. T08 editing and T10 migration/retained desktop gates remain |
| SystemUserEnabled | false | Host admission / both | Root policy selects independent PAM any/allow-list/disabled routes. Canonical PAM account admission and denial pass on Sol; T10 preserves legacy owner-only/disabled effective values |

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
The paired broker/worker wire is now v8 in source; installed fleet remains v6.
Delayed Inhibit replies release their cookies even after guard destruction,
using the replying service's unique owner rather than a replacement service.

Wire8 also carries complete chroma timing and adaptive auxiliary demand to every
worker encoder. Actual auxiliary capability reports return separately from
backend reports. Console overrides require an admitted current controller;
Virtual requires its current authenticated attachment. Partial timing updates
merge atomically over that owner's current tuple. Control transfer/desktop detach
restore validated user defaults, and old worker reports cannot steer a revoked
bridge. Acceptance acknowledges policy only, not AVC444 availability. Six focused
suites and native Sol Console HEVC/dormant-policy transfer pass. AVC444 codec
preference/actual auxiliary bytes, timing telemetry, native Virtual/multi-screen
adaptive gates and matching KCM remain T05/T08. Evidence
`~/dev/rdp/evidence/2026-09-30-t05-chroma-policy/SUMMARY.md`.

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
