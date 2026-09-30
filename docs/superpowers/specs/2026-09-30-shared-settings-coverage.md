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
VirtualStockClientPolicy. Remaining parsed preferences are held for their task's
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
| Avc444MotionGapMs | 100 | User preference + owner override / both | Parsed with whole timing tuple validation; worker policy T05 |
| Avc444RestMs | 150 | User preference + owner override / both | Parsed with whole timing tuple validation; worker policy T05 |
| Avc444MaxGapMs | 1500 | User preference + owner override / both | Parsed with whole timing tuple validation; worker policy T05 |
| MonitorMode | multi | User preference / Console; Virtual layout is separate | workspace/primary/specific/multi/virtual parsed; capture/layout parity T06 |
| MonitorIndex | 0 | User preference / Console specific capture | Nonnegative index parsed; worker selection T06 |
| VirtualMonitorPolicy | replace | User preference / Console's client-created outputs | replace/extend parsed; restore/local takeover T06, lock race T03 |
| VirtualMonitorLayout | client | User preference / Console's client-created outputs | client/single/physical parsed; retained Virtual topology remains separate; T06 |
| VirtualMonitorFallbackSize | 1920x1080 | User preference / output creation | Even bounded dimensions parsed; hardware limits still enforced at application; T06 |
| VaapiDriverMode | auto | Host device setup plus user backend preference / both | Process/worker setting remains host-controlled; driver choices cannot mutate root broker environment; equivalent T05/T15/T16 |
| WakeDisplayOnConnect | true | User preference / Console physical seat; Virtual own desktop only | Parsed; worker session-bus integration T07 |
| StandardClientMedia | true | Host ceiling AND user preference AND consent / both | User may opt out; never turns denied host permission on; T11/T12 live media |
| VirtualStockClientPolicy | attach-or-create | User preference / Virtual | Same validated transaction; attach-or-create/refuse applied before stock-client gate |
| CameraLoopbackDevice | empty | Host device grant / both | Existing host CLI path; user config cannot select/grant arbitrary device; equivalent device mapping T04/T08/T11 |
| Users | empty | Host admission / credentials bound to original owner | Preserve existing custom credentials until an equivalent identity mapping is implemented/tested; T04/T08/T10; do not discard |
| SystemUserEnabled | false | Host admission / both | Brokers currently PAM-enabled; legacy switch/effective owner equivalence and denied-account coverage remain T04/T08/T10 |

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

## Remaining gates

- T04 is partial until account-admission/custom-credential equivalence, complete
  authority tests, reload behavior and live allowed/denied authentication pass.
- T05–T07 must implement the unapplied rows with no unexplained feature loss.
- T08 must wire matching KCM scope/help/reconnect behavior and dual-KCM acceptance.
- T10 must test omitted default values, backups, credentials and independent trust.
- Both brokers must pass live per-user gates on Sol before any package rollout.
  No Hal live tests; no legacy route retirement from this implementation alone.
