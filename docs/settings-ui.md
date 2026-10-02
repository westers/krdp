# Farside settings: KDE layout conventions

## Fresh design proposal (2026-10-01)

Steve requested reassessing all settings, options and dialogs from their user
tasks. The [complete assessment and proposed organization](design/2026-10-01-farside-settings-ux.md)
recommends an overview with native KCM subpages, rather than retaining the four
tabs. It includes the full option map, host/account/access save scopes, local
certificate editing, conditional displays, dialog behavior, and two visual
sketches. [The source/QML review](design/2026-10-01-settings-qml-review.md)
records the confirmed functional findings separately from scanner diagnostics.

This is **not implemented or visually accepted**. The sections below document
the current presentation and earlier brief. For the proposed redesign, the new
document replaces their four-tab, shared certificate-dialog and tri-state
checkbox recommendations. Retain the stable-refresh, persistent-draft, native
KDE, scoped authorization and Hal guardrails. Do not implement or deploy another
layout before Steve reviews the concrete proposal.

Steve requested restoring Claude's earlier KDE presentation on October 1, 2026.
Use `e0ba4b77` as the visual precedent, retaining the current Console/Virtual
models and administrator/account boundaries. Do not restore the obsolete
per-user service controls or legacy configuration adapters.

## Layout

- Persistent Console, Virtual, Access and My Preferences tabs, using native
  `QQC2.TabBar`/`StackLayout` inside `KCM.AbstractKCM`. Each content page uses
  `KCM.SimpleKCM` and keeps its own scroll position and draft alive.
- Console and Virtual show enable/status and known saved/inspected addresses;
  they never present a staged endpoint as the address currently in use. Service
  details offer startup, explicit restart/stop and manual status refresh without
  loading administrator settings. Stopping/restarting confirms disconnection.
- Common host video/device settings come first. Connection/encoding tuning is
  under Advanced. Virtual has separate Connection settings and New desktop
  defaults sections, with independent saves. Console display sharing belongs to
  the account's My Preferences; Console links there explicitly.
- Access contains remote-login permissions and certificate details dialogs.
  Certificates use the existing per-host draft. Dialog saves are labelled Save
  Console/Virtual Settings and explain that other pending host settings are
  included. Certificate Standard Paths/Reset affect only TLS fields, preserving
  other pending settings. Reload confirms discarding the entire host draft.
- One `Kirigami.FormLayout` aligns each editor's labels. Section headings,
  theme colors/fonts and Kirigami spacing are native. Long fingerprints/help
  are bounded; small windows scroll the form while the footer remains visible.
- Quality uses a slider and bounded editable `SpinBox`; ports, monitor indices
  and AVC444 intervals also use bounded spin boxes, with units for intervals.
  Commit numeric input on Enter/focus loss. Model resets/scope changes must update
  the controls; opening/refreshing must never snap or change stored values.
- Boolean fields use tri-state `CheckBox` controls: partial means inherited,
  checked/unchecked means explicit On/Off. Host inheritance shows the unit
  default; unknown inherited account numbers show a dash and Use host setting.
  Other choices use `ComboBox`. Locked account fields remain disabled/preserved.
- Scoped Save/Reset/Defaults actions sit in fixed footers. Save never restarts a
  service. A pending restart notice offers the explicit, confirmed restart.
  Administrator reads/saves remain explicit; opening tabs never invokes a helper
  or silently reads account configuration. Existing global Apply is not repurposed.
- Common fields have short labels. Contextual help is reserved for unfamiliar
  options and locked settings. Per-field reset controls retain space while hidden
  so changes cannot shift neighbouring inputs. Camera prerequisites/unavailability
  stay visible; bridge configuration is not presented as a camera-sharing toggle.

## Refresh and object lifetime

The former five-second timers repeatedly assigned a fresh `QVariantList` to
service Repeaters, destroying their delegates and restarting form layout work.
Both timers are removed. The transport subscribes to systemd unit property/job,
unit-file and manager-owner events; user actions still read back actual state.
Manual Refresh remains available. Unrelated unit properties are ignored.

Never bind a widget-creating Repeater to the changing `services` list. The
remaining direct Services page uses fixed indices; the main page has static
Console/Virtual editors. Service updates must preserve editor identity, focus,
unfinished text, geometry, scroll position and drafts.

Host field objects stay alive during scope switches. Identify fields by both key
and group: VA-API policy occurs in different groups across host/session schemas.
Avoid transient repeated forms linked as twins; Kirigami's deferred twin/label
updates can otherwise run after those objects have been destroyed. StackLayout
pages need explicit zero minimum dimensions so small windows can shrink them
and retain visible footers.

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
