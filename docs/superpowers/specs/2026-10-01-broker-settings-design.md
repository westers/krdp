# Console/Virtual settings administration (T08)

Status: implementation in progress; source gates do not authorize deployment.
Parent plan: [T01–T28](../plans/2026-09-30-farside-remaining-work.md).

## Required final page

The Farside page manages Console and Virtual, with their own system service
status/start/stop/restart/autostart and listen/TLS information. Stock KDE KRDP
retains its separate module, config, certificates and service. Legacy user-daemon
settings remain migration input until T10/T13; the page must not present them as
effective Console/Virtual administration.

Per-user video/chroma/audio, Console capture/temporary-output and wake, plus
Virtual stock-client preferences use the shared validated model and save only
recognized preference keys. Saving them never rewrites host admission, TLS,
listeners or device grants. Defaults/invalid values/save/reload and restart or
reconnect requirements have working bindings and explicit scope/help.

Host settings require normal administrator authorization. System service
operations use systemd's system bus and its policy. Authentication policy editing
uses a small fixed-purpose pkexec helper, installed at a fixed absolute path
under root-owned libexec with its own policy action. No caller-supplied file path,
shell fragment, environment expansion or arbitrary service command is accepted.
The helper uses pkexec's fixed-path action and disables the internal text agent;
dialog cancellation (126) and denied authorization (127) preserve pending edits.
This behavior and the sanitized execution environment are specified by
[polkit's upstream manual](https://raw.githubusercontent.com/polkit-org/polkit/master/docs/man/pkexec.xml).

## Shared preference transaction

The editor uses the canonical account-home `~/.config/farsideserverrc`, sharing
UserConfiguration's UID resolver and BrokerUserSettings' lexical/value parser.
Process HOME/XDG, another user's file, the system host environment or a QML path
cannot select the production target. Merely opening the module does not load or
write preferences; the scoped page loads explicitly. Tests inject a scratch
directory through C++ only.

The public model exposes the seventeen recognized preference fields, locked-key
metadata and translated field choices/help. Raw configuration and legacy/host/
credential keys never reach QML. A missing value means host inheritance, rather
than a guessed legacy default; Console and Virtual may have different defaults.
Individual missing AVC444 timing values use the production parser's built-in
tuple when any timing override exists. All timing fields unset inherits the
broker's chroma policy. Save and Reset to host settings are explicit page actions;
preferences apply on reconnect and never broadcast KConfig reload notifications.

Edits preserve unrecognized/host/legacy/comment/localized/expanded entries and
unrelated sections. Duplicate editable fields are replaced once in the last
General section. Key/group immutable markers are respected; defaults remove only
unlocked preferences. All submitted fields/types/line boundaries and the entire
result validate through the same production parser. Invalid staged values stay
visible for correction and disable save. Invalid existing recognized values or
unsafe files fail loading without guessing a replacement.

Save requires an unchanged complete document/existence snapshot, not only matching
preference fields. Cooperating writers take a safe per-directory user lock.
Bounded nonblocking regular-file reads reject foreign owners/final symlinks/FIFO/
oversized or inconsistent reads; unsafe file/directory/lock permissions fail.
Safe existing user-owned config-directory symlinks remain compatible with the
broker reader. An inspected directory descriptor anchors atomic user0600 writes;
sync and independent parsed readback precede the saved/reconnect notice. Failure
after commit is reported as saved-but-unverified, never as unchanged.

Full parser/editor storage tests plus actual populated Buzz controls must prove
all seventeen bindings, actual text/choice edits, invalid/stale save refusal,
cancelled discard, reload, locked/default inheritance and preservation of host
and secret-bearing unrelated entries. This does not complete the legacy main
page cutover, host administration, native worker or package/deployment gates.

## Typed host documents and administration contract

`BrokerHostSettings` defines twelve Console fields, eleven Virtual host fields
and two Virtual-session fields. Exact environment names and defaults match the
shipped system unit templates. Console keeps port3391 until migration; its
command-line default3389 is separate. Adding explicit address/port Environment
defaults to the Console template makes removing those file overrides meaningful
without moving the legacy listener. No installed unit is changed by this source
checkpoint. Missing RenderPci means no GPU grant; it never means all devices.

Each scope has one fixed filename under `/etc/farside`: console-host.conf,
virtual-host.conf or virtual-session.conf. The editor submits only typed public
keys, never an environment fragment, write path, executable, shell or service
name. The bounded UTF-8 parser follows EnvironmentFile quoting/continuation and
last-assignment semantics, preserves unknown administrator bytes and excludes
them from public metadata. Unterminated quotes/continuations, invalid characters,
invalid recognized values and wrong scopes fail closed. Canonical booleans in an
existing file must match the actual brokers' accepted text; drafts normalize
before serialization. Desired overrides are complete, omitted fields inherit
unit defaults, and unchanged effective overrides preserve the original bytes.
All affected duplicate statements are replaced or removed in full, including
multiline values. This is not shell expansion. Semantics were checked against
[upstream systemd documentation](https://raw.githubusercontent.com/systemd/systemd/main/man/systemd.exec.xml)
and its [EnvironmentFile parser](https://raw.githubusercontent.com/systemd/systemd/main/src/basic/env-file.c).

The pure snapshot's `effective` map describes shipped defaults plus that
document. It does not establish installed runtime state: systemd drop-ins and
other EnvironmentFiles can override it. Final administration must inspect the
actual fixed units and expose a differing/custom unit configuration explicitly,
without claiming that a successful file save already applied to a running host.

The remaining fixed-purpose root helper must implement the following contract:

1. A bounded stdin read/save request chooses only the three named scopes; an
   expected revision covers presence and the complete file. Public replies expose
   only recognized values, revision and bounded TLS/device metadata. Unknown
   entries, key material and raw files never reach QML or diagnostics.
2. Inspect safe root-owned `/etc/farside` and files, take a fixed safe per-scope
   lock, recheck the complete revision and directory identity, validate the full
   candidate, atomically replace that one file root0600 through its directory
   descriptor, sync and independently read back. Scope saves are separate; never
   imply a transaction across all three files. Denial/cancel/invalid/stale input
   preserves installed bytes and pending UI edits. Post-commit errors explicitly
   report a saved-but-unverified result.
3. TLS path edits select existing safe root-administered, bounded, matching PEM
   pairs. Validate parents, targets, unencrypted keys and actual public metadata
   without prompting or exporting a key. Unchanged administrator-managed symlinks
   and supported custom paths retain their existing broker semantics. A new
   arbitrary missing path cannot authorize the broker to overwrite a root file.
4. Certificate import carries bounded certificate/key bytes over stdin after
   caller-side file reading; it never gives the helper an arbitrary input/write
   filename. Stage both files in a new exclusive generation beneath a fixed root
   TLS-import subtree, inspect the pair, then atomically publish both configured
   paths in that scope's single environment file. Final root-owned symlink aliases
   mark imported material administrator-managed for ensureHostCertificate, so
   automatic renewal never replaces an imported certificate with a self-signed
   one. Reject mismatched/encrypted/expired imports; show fingerprint/expiry and
   renewal responsibility. Clean only uncommitted generations. Preserve old
   generations while any running host can still reference them.
5. Returning to the standard TLS paths changes only those two configured paths,
   preserves existing files/fingerprints and accurately reports whether their
   current material is generated or administrator-managed. Explicit regeneration
   needs its own reviewed operation; ordinary Defaults/Save cannot regenerate.
6. Camera-loopback paths require actual device-type/loopback checks. Virtual
   render grants remain canonical distinct PCI identities resolved by the current
   production device authority; no renderD index, arbitrary device path, global
   ACL/account change or unsupported GPU backend/selection feature is introduced.
   VA-API driver policy retains auto/off/radeonsi/iHD/i965 behavior.

Host listener/TLS/video/media changes require an explicit separate restart of
the relevant broker, with client-disconnection confirmation. Virtual-session
render/driver changes apply to newly created desktop namespaces; restarting the
broker cannot change a retained desktop's device grants. Explain this in the UI
without terminating existing desktops. User preferences apply on reconnect and
cannot rewrite these host policies. The host UI must include every whitelisted
field, defaults, validation, load/save/discard flows and actual populated Buzz
evidence. Source helper/TLS/device tests, installed authorization/cancel/restart,
both auth scopes, live stock coexistence, migration and package/rollback delivery
remain required gates after the pure format component.

## Host writer source checkpoint

The fixed `farside-host-settings-helper` now implements the three-scope root
filesystem transaction. Read requests contain version1/operation/read/scope;
Save contains version1/operation/save/scope/revision/complete values and optional
TLS operation. No argv selection or caller write path exists. Its own installed
PolicyKit action uses auth_admin and the exact libexec path; the client must use
pkexec with the internal text agent disabled. UI wiring and real installed dialog
acceptance are still open.

TLS keep preserves the current effective paths; changing them requires explicit
existing/standard/import. Existing requires both safe current matching paths.
Standard and Import omit the two path fields. Import carries bounded PEM strings
on stdin, validates unencrypted matching current material and selects its own new
private generation. Root-managed symlink aliases preserve imported renewal
ownership. Standard removes both overrides and preserves all existing files.
Root-owned unsafe parents/targets, custom missing/non-TLS material, stale state
and mixed operation/path injection fail without config writes. Standard missing
material may be generated by the broker at its next explicit restart; it is
never generated by Save. Future-dated material is refused rather than described
as automatically renewable by the current broker.

File snapshots/revisions include scope, presence and complete bytes. Root-owned
safe ancestor/file/lock checks, nonblocking bounded regular reads, cooperating
flock, directory identity and post-staging revision checks precede a descriptor-
anchored root0600 atomic save, sync and independent readback. Selected TLS files
are rechecked before and after publication. Failure before config publication
cleans only the newly staged generation; after publication, material stays and
errors explicitly report saved-but-unverified. Core dumps are disabled and the
input/private-key byte buffers are cleared; key material never enters argv,
environment, metadata or diagnostics.

Read-only `VirtualGpuDevices::resolve` is shared with the actual device entry,
preserving its supported NVIDIA/AMD/Intel device identity/driver/node checks.
Changed nonempty grants require complete currently supported PCI device sets;
empty means no grant and applies to new desktop namespaces. No actual device
open, global account/ACL change or encoder selection policy is introduced.
Camera selection checks an existing root V4L2 loopback with read-only QUERYCAP.
Console worker user access is still separately required; this setting grants no
OS permission. Sol has no loopback module/device, so positive Console loopback
hardware acceptance remains open. Virtual's current device namespace has no
loopback node grant: a new non-none selection is refused and metadata explicitly
marks it unavailable. Existing typed manual values can be preserved during
unrelated edits, without claiming they work. Standard PipeWire camera delivery
is separate. Assign Virtual loopback parity/native worker checks to T05/T08;
never drop them or call them completed because an unavailable flag exists.

Every reply labels broker-restart versus new-desktops and runtimeVerified=false.
It describes stored configuration, not installed/drop-in/running settings. The
host UI, actual fixed-unit/running-field readback, explicit restart round trips,
installed authorization/cancel/password, legacy main-page cutover, migration,
stock coexistence and package/rollback delivery remain required. Seven complete
pure/boundary suites and twenty-two isolated Sol root filesystem cases accept
this component; no installed state changes. Evidence:
`~/dev/rdp/evidence/2026-10-01-t08-host-writer/SUMMARY.md`.

## Host page source checkpoint

The KCM now owns three lazy models with immutable Console, Virtual and
Virtual-session scopes. Opening the plugin/page never starts authorization or
reads host files. Explicit Load/Save uses the fixed pkexec executable, disabled
text agent and fixed host helper path; executable/timing injection exists only
in C++ tests. Drafts and checked import material stay separate across sections.
All25 typed fields have choices/text controls, unit defaults, staging validation,
discard/reload confirmation and broker-restart/new-desktop help. Defaults reset
TLS paths explicitly without regenerating material or touching authentication.

Local file selection carries only URLs to the unprivileged model. Nonblocking
FD inspection rejects FIFO/device/oversized/changing files before bounded reads;
the shared parser checks current matching unencrypted PEM. C++ keeps the import
buffers private and clears them on replacement/discard/success/destruction.
Readable metadata contains only the public certificate fingerprint/algorithm/
dates. A newly accepted file selection invalidates the previous checked pair.
Helper requests use stdin, never argv/environment or caller-controlled root
input paths. Standard and Import remove both path fields; Existing includes both.

Replies are bounded and validated against the immutable scope, full typed
defaults/effective map, revision and public metadata schema. Stderr/arbitrary
error text is never forwarded to QML. Normal cancellation/denial preserves
pending edits. Crash, timeout, malformed saved response and post-publication
verification failure preserve drafts and require a reload before another save;
they never claim the file is unchanged. Process lifetime gates prevent concurrent
requests. File saving never restarts either host or modifies an existing desktop.

The page names stored state explicitly. RuntimeVerified remains false; actual
installed units/drop-ins/running-field readback is still independent work.
Virtual loopback is disabled with a reason, while an existing override can be
reset without being presented as functional. Namespace loopback parity and
positive Console device acceptance remain T05/T08. GPU PCI fields control access
to new desktop namespaces; encoder selection remains T15/T16.

Seven complete pure suites, four Buzz populated-page cases and sixteen actual
compiled KCM cases pass, with width640/scope/cancel/invalid/default/import flow
and readable native-style screenshots. Corrected fixture compile and target-name
errors are retained. No installed state changed. Main legacy cutover, runtime
readback, installed pkexec/password/cancel/restart into both admission scopes,
native worker/camera gates, T10 migration/live stock coexistence and T27 package/
rollback/fleet delivery remain; T08/P0 is PARTIAL. Evidence:
`~/dev/rdp/evidence/2026-10-01-t08-host-ui/SUMMARY.md`.

## Runtime inspection contract

Explicit Inspect Running Host uses the same fixed authorized helper with a new
read-only operation. It never starts/stops/reloads a service. Only the Console
and Virtual fixed unit names are permitted. Session grants retain new-desktop
scope; one broker process cannot prove every retained namespace changed.

Read the system manager's unique owner and query that owner for the fixed unit's
loaded Unit/Service properties. Inspect actual merged Environment, ordered
EnvironmentFiles, ExecStart, fragment/drop-in paths and NeedDaemonReload. Honor
EnvironmentFile order and UnsetEnvironment; PassEnvironment, PAM/login
environment, unsupported expansion/commands and incomplete known fields cannot
be guessed from source defaults. Projection describes the loaded unit and
current files, rather than rereading possibly newer unit text as loaded state.
Root-owned safe bounded regular reads preserve unknown environment bytes privately;
arbitrary command/environment text never becomes public metadata. Other files,
drop-ins, literal/custom settings or fixed-file omission remain visibly custom.
Missing required files, unsafe/invalid values and unknown mapping fail projection.

For an active broker, pin MainPID with pidfd before inspecting /proc. Require
root real/effective/saved/fs identities, exact unit control group, stable process
start ticks and executable inode matching the trusted installed fixed broker.
Parse only recognized explicit command-line options. Do not infer absent fields
from a different source version's CLI defaults. Recheck pidfd liveness, process
identity/argv, manager owner and all relevant unit/process properties after reads;
replacement, exit, manager reload or stale file identity fails that sample.
Deleted/replaced binaries and custom commands are explicitly unverifiable.

Public versioned data contains scope, unit/state, PID, bounded known startup
values, current unit projection, missing/differing field names and fixed reason
codes. It distinguishes missing/inactive/partial/custom/different/verified/stale/
denied/unavailable state and stored file revision. A mismatch does not disable
safe editing; it explains why Save is not Apply. Running values mean verified
startup arguments, not live per-user overrides, successful listening, loaded TLS
fingerprint or supported media behavior. Current stored TLS metadata remains
separate from a certificate already loaded by a process. No raw arguments,
unrecognized env values, authentication verifiers or key bytes reach QML/logs.

Pure mapping/identity/public-schema tests run with injected data on Hal. Private
typed manager/replacement/denial fixtures and actual read-only installed process
agreement run on Sol. Populated runtime/difference/unavailable GUI evidence runs
on Buzz. Task acceptance and delivery still require the installed password/cancel/save ->
explicit restart -> runtime/auth-scope round trip and all other release gates.
Property types follow [systemd's D-Bus interface](https://raw.githubusercontent.com/systemd/systemd/main/man/org.freedesktop.systemd1.xml).

### Runtime inspection source checkpoint (2026-10-01, unshipped)

Fixed read-only helper operation, private typed unit/environment/process reader
and explicit page inspection now implement this contract. Safe regular file
identity includes both the lexical entry and resolved target across repeated
reads, including replacements with identical bytes. Manager/unit ownership and
metadata are checked again after final file/process probes. Public schema rejects
contradictory verification flags/states as well as private or mistyped fields.

Eight complete pure suites pass; Sol native16 covers typed queries, root file
safety, drop-ins/custom literals, omission/inactive/reload/denial, changing file,
process/executable/manager identity and actual installed Console/Virtual startup
agreement. The exact helper passes both installed read-only operations and the
previous22 isolated root writer checks without changing installed files or PIDs.
Buzz page13 and compiled KCM16 pass, with nine populated runtime states, width640,
preserved drafts/TLS/unknown-save state and readable warning screenshots. Fixture
compile/type/revision/widget errors were corrected and retained as evidence.
All test handles are terminal and own remote fixture directories removed.

Main legacy cutover, installed normal authorization/restart into both admission
scopes, native worker/camera parity, T10 migration/live stock coexistence and T27
package/rollback/fleet gates remain independent work. Virtual loopback remains
T05/T08; unavailable is not acceptance. T08/P0 PARTIAL, goal ACTIVE. Evidence:
`~/dev/rdp/evidence/2026-10-01-t08-runtime/SUMMARY.md`.

## System service controls

The service model addresses only `farside-console-host.service` and
`farside-virtual-host.service` on systemd's system bus. The existing stock/legacy
user-manager coexistence adapter stays separate. Opening the service page reads
state without starting/stopping anything; read-only polling updates externally
changed state. Unknown, missing, masked and invalid units never enable a start
or restart action. Boot startup distinguishes persistent `enabled` from
`enabled-runtime`, and static/indirect/generated states do not offer a writable
boot checkbox.

Start/stop/restart use the fixed manager methods, `replace`, and normal systemd
PolicyKit authorization with D-Bus interactive authorization allowed. Subscribe
before queueing and match the exact returned job path/unit. `JobRemoved` can
precede the method reply; retain bounded early completions. A queued method
reply alone is not success. Failed/cancelled/timeout jobs, manager replacement
and authorization denial keep an explanation and require actual state readback.
Operation generations prevent a late callback from completing a newer request.

Enable/disable use persistent unit-file operations (`runtime=false`, no force),
followed by explicit manager Reload and fresh status. If files change but Reload
fails, preserve that explanation while displaying actual file state. Startup
changes never imply that the service is currently running. The actual service
manager API and authorization are documented in
[systemd's upstream D-Bus manual](https://raw.githubusercontent.com/systemd/systemd/main/man/org.freedesktop.systemd1.xml).
Stop/restart needs an explicit in-page confirmation that clients will disconnect.
The model serializes operations and blocks repeats until readback completes.

Pure state/lifetime/denial tests, private Sol D-Bus API/job-order tests and Buzz
populated UI evidence accept this component only. Installed-policy real
authorization and broker restart acceptance remain T08/T27 gates.

## Authentication transaction

- Read the fixed `/etc/farside/authentication.json` using the same safe reader
  as the brokers. Absence retains their existing PAM-any default. Invalid or
  unsafe policy fails closed; editing never guesses a replacement.
- Return a versioned snapshot containing revision, Console/Virtual PAM modes,
  canonical OS account names and alias/owner metadata. Never return password
  verifiers, salts or passwords to QML or command output.
- Save receives the expected revision and complete two-route desired metadata
  through bounded stdin JSON. No secrets in argv, environment, logging or errors.
  Each route selects any/allow-list/disabled; aliases explicitly select a valid
  non-root owner account. Existing alias verifiers are retained only for the
  same route/alias/owner UID. New or rebound aliases require a new password.
- Build the whole candidate and validate with the production broker parser
  before writing. Unknown fields, malformed JSON/types, duplicate aliases or OS
  identities, invalid accounts, password/verifier injection and stale revisions
  fail without changes. Passwords become PBKDF2 verifiers inside the helper.
- Serialize cooperating writers with a root-owned directory lock; use an
  atomic root-owned0600 file replacement. The parent and current file must be
  safe/root-owned with no symlink. Cancellation/authorization failure occurs
  before helper mutation. Failed validation preserves the full original policy.
- Saving admission does not restart services automatically. Show that both
  brokers must restart to load it; restart is an explicit separate action.

## Acceptance and remaining scope

Pure transaction tests must prove distinct Console/Virtual scopes, preservation
and rotation of aliases, no accidental owner rebinding, exact stale rejection,
default-absence semantics and no secret material in public replies/errors.
Bounded Sol scratch helper/file tests must prove unsafe file/directory rejection,
atomic round trip, root0600 mode and invalid/stale unchanged-file behavior.
Production installed policy is never the test write target.

The real KCM needs populated Buzz screenshots and save/reload/cancel/restart
round trips to the intended brokers, plus live stock/Farside coexistence. All
remaining preference, listener/TLS/device administration, main-page cutover, migration
and T27 package/rollback gates remain necessary. This component alone does not
complete T08 or the P0 milestone.
