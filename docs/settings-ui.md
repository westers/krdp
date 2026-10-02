# Farside settings: KDE layout conventions

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
