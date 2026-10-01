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
remaining preference, listener/TLS/device administration, service UI, migration
and T27 package/rollback gates remain necessary. This component alone does not
complete T08 or the P0 milestone.
