# Farside settings: KDE layout conventions

## Approved redesign implementation (2026-10-02)

Steve approved the [complete design](design/2026-10-01-farside-settings-ux.md)
and requested its implementation with the Qt/KDE skills. The native overview
and persistent KCM subpages replace the earlier four tabs. The two SVG sketches
are design references; the new populated native renders are implementation evidence.

## Layout and actions

Updated 2026-10-02 for the ten-to-five page consolidation (evidence
`~/dev/rdp/evidence/2026-10-02-settings-consolidation/`).

- The module has five persistent pages: Overview, Console, Virtual, Who Can
  Connect and My Preferences ([consolidation spec](superpowers/specs/2026-10-02-settings-consolidation-design.md),
  `BrokerMainPage.qml` `pages`). Certificate, New Desktop Hardware and Service
  Details are no longer pages; they are embeddable sections hosted by the page
  that owns their state.
- Overview presents Console and Virtual as plain full-width groups: status,
  immediate connection and boot switches, saved (and verified running) address,
  Configure…, Restart…, Stop… and a Details expander holding the read-only host
  inspection (`BrokerServiceDetails`). Access and personal preferences are the
  fixed footer links. Saved wildcard bindings are described as all interfaces
  and a port, never offered as a connectable address.
- Console and Virtual configuration group Connection (with the inline
  `BrokerCertificateSection`), Video, Sound and devices, then Advanced. The
  Virtual page ends with the inline `BrokerHardwareSection` (GPU grants and
  VA-API policy for newly created desktops) that has its own Restore Defaults,
  Revert Changes and Save Desktop Defaults; the fixed footer saves only the
  Virtual host settings. A status line above the footer names the dirty scope.
  Console display selection links to personal preferences.
- Certificate changes use the dedicated local C++ draft. Cancel and leaving the
  page (Back) discard only that editor. Use Certificate Changes stages only TLS
  fields into the host draft; saving and restarting remain explicit host
  actions. Ordinary Restore Defaults preserves TLS paths, the TLS operation and
  any staged PEM import. Only one native file dialog is ever open because the
  editor expands in place instead of opening a modal.
- Inline expanders (Details, Certificate, Advanced) only toggle `visible`; never
  put a draft editor behind a `Loader` or `active: false`.
- My Preferences keeps account inheritance explicit. Choosing Custom stages an
  incomplete value until the user chooses a number; it does not fabricate zero
  or a minimum. Fallback dimensions must both be supplied. Display-specific
  fields appear only for the corresponding mode and retain hidden values.
- Access separates Console and Virtual system accounts and remote logins.
  Native dialogs stage alias edits; password fields clear when dismissed.
  Removing a login is staged, with Undo. Save applies the access policy and
  offers separate, confirmed service restarts.
- Use native QQC2/Kirigami controls, theme fonts/colors, units, contextual
  i18nc strings and bounded content columns. Multiple forms align through
  persistent twinFormLayouts. Do not constrain a form's Layout.maximumWidth
  to its own parent layout's current width: this creates a size negotiation
  cycle and collapses sections. Sections use one shared desktop/narrow threshold.
- Scoped saves and local Revert/Defaults remain in fixed footers. Reloading a
  modified draft requires an explicit Discard and Reload action. Global KCM
  Apply/Defaults do not authorize or repurpose system/account operations.
- Camera prerequisites remain visible and distinguish saved setup from runtime
  readiness. Device grants describe desktop namespace access, not GPU encoder
  selection. Unsupported Virtual camera sharing is clearly explained.

The implementation preserves the stable refresh behavior and explicit
administrator authorization. Native administrator-prompt and user visual
acceptance remain separate checks; fixture success does not claim those.

## Refresh and object lifetime

The former five-second timers repeatedly assigned a fresh `QVariantList` to
service Repeaters, destroying their delegates and restarting form layout work.
Both timers are removed. The transport subscribes to systemd unit property/job,
unit-file and manager-owner events; user actions still read back actual state.
Manual Refresh remains available. Unrelated unit properties are ignored.

Never bind a widget-creating Repeater to the changing `services` list. The
root page has static Console/Virtual service rows. Service updates must preserve editor identity, focus,
unfinished text, geometry, scroll position and drafts.

Host field objects stay alive during scope switches. Identify fields by both key
and group: VA-API policy occurs in different groups across host/session schemas.
Avoid transient repeated forms linked as twins; Kirigami's deferred twin/label
updates can otherwise run after those objects have been destroyed. Do not pop a
twinned page in the same event-loop turn it was created (a Kirigami `callLater`
then touches the destroyed twin); tests let a pushed page settle first.

## Navigation (OPT-057 S2)

The root (`BrokerMainPage.qml`) is a list page. Console, Virtual, Who Can
Connect and My Preferences are opened with `kcm.push(file, properties)` and left
with the shell's own Back (`kcm.pop()`); there is no custom header, Back button,
history or StackLayout, and no phone duplicate. Pushed pages receive their models
as initial properties (so tests can inject fakes). Unsaved edits live in the
scoped models, so popping loses only view state (scroll, focus). An open
certificate draft is cancelled when the host page is popped, hidden, or
pushes another page. "My Display Preferences" pushes My Preferences on top of
the Console page, so Back returns there.

## KDE references reviewed

- [Human Interface Guidelines](https://develop.kde.org/hig/)
- [Simple by default](https://develop.kde.org/hig/simple_by_default/)
- [Layout and navigation](https://develop.kde.org/hig/layout_and_nav/)
- [Displaying content and inline help](https://develop.kde.org/hig/displaying_content/)
- [Getting input](https://develop.kde.org/hig/getting_input/)
- [Status changes](https://develop.kde.org/hig/status_changes/)
- [Text and labels](https://develop.kde.org/hig/text_and_labels/)
- [Accessibility](https://develop.kde.org/hig/accessibility/)
- [Kirigami form layouts](https://develop.kde.org/docs/getting-started/kirigami/components-formlayouts/)
- [KCM development](https://develop.kde.org/docs/features/configuration/kcm/)

Use the existing scoped widget/KCM fixtures and visually inspect their rendered
pages on Buzz. Actual administrator-dialog acceptance remains the separate N04
manual check; UI fixture success does not substitute for it. No Hal live testing.

Before delivering a layout change, capture the actual packaged QML pages and
relevant dialog/expanded/empty/populated states, then compare them with the
approved design. Fix demonstrated layout differences and retain a reviewable
comparison gallery. Passing widget tests alone does not establish visual
agreement. Record theme and fixture-state differences explicitly. The
[October 2 comparison](design/2026-10-02-settings-capture-comparison.md)
documents the first complete comparison with the approved sketches.

## Qt skills and design review (2026-10-01)

Steve requested installing and using The Qt Company's skills after rejecting
the current presentation. Installed for Codex under `~/.codex/skills/`:

- `qt-ui-design` 1.1: screen organization and visual/interaction audits.
- `qt-qml` 1.2: native QML implementation conventions.
- `qt-qml-review` 1.0: scoped, read-only implementation review, with its
  checklist and linter included.

Upstream: <https://github.com/TheQtCompanyRnD/agent-skills>. Read the applicable
installed SKILL.md before use. These are separate from Superpowers; do not
resume the old Goal or replace the selected-task workflow. Skill installation
does not switch the running model. Automatic discovery is available next turn;
the design and coding instructions were read and applied manually this turn.

KDE HIG, Kirigami, KCM conventions and Steve's instructions take precedence over
generic skill defaults. In particular:

- Keep KDE's selected font, theme, icon set and Kirigami units. Do not introduce
  the design skill's custom typography scale, assumed Linux font or fixed
  16-pixel minimum into the KCM.
- Keep KDE's `i18n`/`i18nc` translation conventions rather than replacing them
  with `qsTr`. Keep native controls styled by `org.kde.desktop`.
- Persistent draft editors must survive service events and tab changes. Generic
  advice to unload optional content must not reintroduce destroyed editors,
  refresh jitter, lost focus or unsaved text.
- Scope implementation review to the changed components and concrete risks;
  follow the standing no-delegation and focused-verification instructions.

### Initial audit and proposed next presentation

Reviewed the populated `2026-10-01-settings-tabs/tabs-{0,3}.png` fixtures and
current host/preferences/field/service code, alongside Claude's `e0ba4b77`
main and VideoAudioPage source. These are fixture images, not live Sol captures.
No fresh rendering, product edits or deployment occurred for this audit.

Warnings:

1. The leading title/description, centered forms/headings and leading Service
   details control create disconnected visual groups. Use one bounded content
   column with consistent heading, form and disclosure alignment.
2. Video, Displays and Audio headings have large surrounding gaps. Use
   `smallSpacing` within groups and `largeSpacing` between groups; remove the
   one-button Displays section and place its preference link with the scope
   explanation or related display settings.
3. Basic booleans expose three-state inheritance with changing checkbox text
   (`Use unit default (Off)` / `Enabled`). Show a stable feature label and a
   clearly named default/on/off choice where inheritance is editable. Preserve
   the distinction between an absent override and explicit false.
4. My Preferences presents disabled numeric controls and repeated inheritance
   controls before explaining the shared default behavior. Explain the scope
   once, distinguish inherited and customized values consistently, and leave
   unknown inherited values unknown.
5. Labels such as `Prefer audio quality` and `Allow standard client media`
   describe configuration fields. Restore the earlier outcome-oriented choices:
   busy-network video/sound preference and media sharing with other RDP apps.

Proposed Console order: compact enable/status/address group; video quality and
automatic adjustment; audio/media sharing with a camera availability message;
an Advanced disclosure for listener/encoder/bridge configuration; consistent
service actions and the existing scoped save footer. Keep the four persistent
tabs and separate Console/Virtual/account authority. Camera device paths and
runtime inspection remain available under the appropriate advanced/details
section.

Before the next product edit, review a concrete visual proposal using KDE
settings references and this brief. Acceptance requires Steve's visual review
and a populated native-style render, in addition to only the affected draft,
inheritance and stable-refresh checks. Functional fixture success alone does
not establish that the design is acceptable.

## Reading settings without a password (OPT-057 S1)

- Opening the panel never prompts. Host settings (Console, Virtual host, new-desktop hardware) are read from a
  root-written, world-readable public-metadata snapshot, `/var/lib/farside-public/<scope>.json` (mode 0644, directory
  0755): revision, non-path values/defaults/effective keys, TLS state/fingerprint/validity, camera bridge state and GPU
  render device ids. It never holds account names or aliases, verifiers, certificate or key paths or key material, and is
  never derived from anything the reader can influence. `farside-host-settings-helper` writes it after each successful
  save and, via `ExecStartPre=-... --publish <scope>`, at every broker start. `BrokerHostSettings::refresh()` is the
  unprivileged reader; `reload()` remains the authoritative helper read.
- Because the TLS paths are hidden, a save that keeps TLS preserves the saved path overrides in the helper.
- My Preferences loads from the user's own file on open. Account names and aliases (administrator protected) load once,
  through the helper, when "Who can sign in" is expanded. Both polkit actions use `auth_admin_keep` for `allow_active`.
- A hand edit of `/etc/farside/*.conf` shows in the panel only after the broker restarts or the next save (the helper
  refuses a save whose revision is stale).
