# T10 Console/Virtual consolidation migration

Status: implementation in progress, no fleet migration or route retirement.
Parent: [remaining-work plan](../plans/2026-09-30-farside-remaining-work.md),
T04/T08 scope and T10/T13/T27 acceptance. The earlier rename copy/retry code
remains historical migration input, not consolidation acceptance.

## Final transaction and gates

1. Inventory exact source/destination presence, ownership, permissions, bytes,
   service/worker versions, retained desktops, profiles, wallet results and
   independently observed destination certificate. Resolve the original daemon
   owner from trusted source/account evidence, never an alias or client record.
2. Produce a deterministic dry run with revisions, complete effective settings,
   conflicts and named unresolved actions. Every schema field has a destination
   or an explicit administrator decision. Do not treat an unavailable secret,
   worker feature, unknown profile or unverified certificate as migrated.
3. After parity, authorization and trust gates, create immutable private exact
   backups and a durable transaction journal before writing. Bind commits to
   source/destination presence and complete revisions, recheck identities under
   locks and reject concurrent edits/replacements. Root host/admission writes use
   fixed authorized helpers; account preferences use canonical account identity.
   No path, UID, shell fragment or raw privileged document comes from a client.
4. Publish scope-specific atomic writes with sync and independent readback.
   Persist progress before/after each step so interruption can resume or roll
   back. Verify exact credentials/UID admission and equivalent defaults. Restart
   only explicitly selected brokers with disconnected-client checks. Migrate
   retained journal/namespace ownership without killing an existing desktop.
5. Preserve profile IDs, monitor layouts, custom endpoints and wallet keys. Move
   a Farside legacy profile to Console only after parity and independently verified
   destination trust; never copy trust from a different port. Existing windows
   remain unchanged until voluntarily reopened. Reconcile Buzz's pending secrets
   with counts/reasons, never values. Unknown/custom profiles remain unchanged.
6. Rerun is a verified no-op; rollback restores exact previous presence/bytes,
   permissions, host identities, credentials, profiles, retained records and
   usable previous packages. Refuse rollback over independently changed data.
   T13 retirement and fleet delivery wait for this evidence, Sol/Buzz first and
   Hal last under its work-desktop guardrails.

## Legacy settings planner

The first component inspects caller-supplied bounded private bytes and a trusted
original owner, then prepares a preference document without writing a destination.
It is not an installed migration command, an authorization decision, a backup
engine or a release gate. No startup/KCM hook invokes it automatically.

The legacy schema is embedded from `server/krdpserversettings.kcfg`. Defaults
are read from that exact schema, with complete field coverage checked at runtime
and in tests. The currently deployed f255ee2/4b308bd schema defaults are the
profile being migrated. Its complete sorted name/type/default fingerprint is
pinned; a changed schema fails planning. Schema default changes require a separately identified
historical profile rather than interpreting old omissions through new defaults.
All17 preferences become explicit canonical values, including quality75,
adaptive=true, chroma100/150/1500, Console monitor multi, wake=true and standard
media=true. Canonical output must validate through the production broker parser.

KConfig reads the snapshot in a private temporary file with SimpleConfig/no
globals and locale C, using actual legacy escaping/list semantics. The temporary
directory/file are private and removed on scope exit; no destination, session bus,
secret store, notification or service is touched. Source/destination UTF-8,
NUL/size, structure and recognized-key markers are checked first. Recognized
localized/expanded/deleted or escaped-key forms require resolution and cannot
silently become defaults or expand a root environment. Unknown data stays private.

Source host facts retain the legacy listener, TLS policy/paths, VAAPI setup and
loopback selection. They are not an authorized root configuration, a device grant
or an instruction to reuse :3389/certificate trust. Port3391 remains the intended
Console endpoint; custom endpoints need an explicit mapping. The operator must
reconcile independent broker host defaults/TLS/device policy using T08 controls.

`SystemUserEnabled=true` means an owner-only PAM allow-list; false means disabled.
Neither becomes PAM-any. Every exact, case-sensitive legacy alias remains bound
to that original owner, even when it spells another OS account. Empty/duplicate/
invalid aliases are refused. This component has no passwords/verifiers and does
not claim successful wallet or policy migration. Route selection/merging with
existing independent admission policies remains an explicit later transaction.

Preparation distinguishes absent and existing-empty destinations. Same-file
upgrade mode is explicit trusted transaction input and requires unchanged exact
snapshot bytes; equal bytes never prove file identity or select that mode. The
future writer must prove canonical file identity under locks. This mode preserves effective settings,
unknown bytes, comments, host/credential fields and immutable intent while making
omitted defaults explicit. A separate destination retains its own existing
preferences; conflicting values or locked inheritance fail the entire plan with
key-only reasons. Source locks are copied as per-field locks when appropriate,
without locking unrelated destination fields. All output is independently parsed
and compared, including lock readback. Rerunning against prepared bytes yields
identical bytes. Revisions include owner and complete source/destination presence/
content; filesystem identity and durable backup enforcement belong to the writer.

The prepared document and host/admission facts are private data, never QML or
logs. The public manifest contains only version/revisions/presence/change flags,
counts and fixed pending-action identifiers. It always retains trust, privileged
host/admission, credential, client/retained, backup/publication/readback and rollback
gates; successful preparation is not permission to cut over.

## Source acceptance and remaining implementation

Compare every embedded default/type against the actual generated legacy settings
class and source schema. Exercise all17 explicit preferences through production
readback, partial tuples/defaults, actual KConfig escaping/users/duplicates/locks,
owner-only/disabled admission, aliases that resemble another account, destination
conflict/no-op/presence, malformed/expanded/localized input and private manifests.
Independent full user/host/auth/parser suites remain required. Pure private QCore
tests may run on Hal; native authorization/desktop/wallet tests belong on Sol/Buzz.

Next: durable safe-file backup/journal/apply/rollback; authorized host/admission
and actual secret-store integration; retained journals and client profiles/trust;
native Sol/Buzz connect/rerun/rollback, fleet reports and T27/T13 cutover. This
document preserves all T10 completion gates; component acceptance is PARTIAL.
