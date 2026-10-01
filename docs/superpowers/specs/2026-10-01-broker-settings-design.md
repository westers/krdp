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
