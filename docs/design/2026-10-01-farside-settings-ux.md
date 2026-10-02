# Farside server settings: assessment and proposed design

**Status:** approved by Steve; implemented and delivered to Sol as `a087e159`.
Follow-up [native capture comparison](2026-10-02-settings-capture-comparison.md)
records the differences found and their corrections. The assessment below
retains its original pre-implementation findings.
**Reviewed:** server source `ec5e2524` (documentation HEAD `505e41b7`), all ten
`src/kcm/ui/*.qml` files, their C++ models, and the four populated settings-tab
fixtures. Earlier Claude source `e0ba4b77` provides a visual precedent.

[Visual proposal](farside-settings-proposal.svg) ·
[Access, certificate and dialog sketches](farside-settings-security-proposal.svg) ·
[QML review](2026-10-01-settings-qml-review.md) ·
[Existing implementation conventions](../settings-ui.md)

The visual is a layout sketch with example values, **not a native Qt rendering
or a report of any host's current state**. This assessment does not establish
live authorization, camera availability, network reachability or performance.

## What the interface should help people do

1. Understand Console versus Virtual and enable the service they want.
2. Find the address to connect to, and see whether the service is running.
3. Decide who can connect and which desktop account they use.
4. Adjust normal video, sound and display behavior without learning backend keys.
5. Resolve an unavailable camera or failed service with a clear next action.
6. Find certificates and hardware tuning when needed, with predictable saves.

Assumptions: native KDE System Settings on a rectangular desktop window;
mouse and keyboard; default English with translated text, RTL and system font
scaling retained. Design first for the existing 900-pixel fixture and support
the existing 640-pixel narrow case. Use the user's KDE font, palette and icons.

## Findings

Here, **Critical** means the editing workflow has a surprising scope or effect;
it does not imply a demonstrated security exploit or certificate-file deletion.

| Priority | Finding and evidence | Required change |
|---|---|---|
| Critical | Host **Defaults** clears all overrides and selects standard certificate paths, even though certificates are on Access. `brokerhostsettings.cpp:194` | Ordinary defaults must preserve TLS. Put certificate reset in the explicit certificate workflow. |
| Critical | The certificate dialog shares the host model; its Save writes unrelated pending host edits too. Closing it leaves staged model changes. `BrokerSignInPage.qml:175`, `brokerhostsettings.cpp:213` | Move certificate editing under its host, with a local cancellable draft and one clearly named host save. |
| Warning | Turning off numeric inheritance uses a clamped empty default. An inherited account Quality becomes an explicit **0** without choosing a value. `BrokerSettingField.qml:25,102` | Customization must require a deliberate value; never substitute the range minimum for an unknown inherited value. |
| Warning | Monitor index and client-created display policies are shown regardless of capture mode. `BrokerPreferencesPage.qml:50` | Show dependent controls only when applicable; preserve inactive values. |
| Warning | Instant service actions, staged host settings and account settings have different activation rules, but the page hierarchy and labels do not make these obvious. | Separate service control from editing and state the activation rule beside each save. |
| Warning | The instant boot-setting action uses a checkbox. `BrokerServiceControls.qml:100` | Use a stable-label switch for an immediate action, with authorization/progress/readback. |
| Warning | Certificate file pickers can open from the certificate modal. `BrokerHostsPage.qml`, `BrokerSignInPage.qml:175` | Put the certificate editor on a page; allow one native file dialog at a time. |
| Warning | Remote-login confirmation is enabled before required password conditions are satisfied; rejection is a general model message. `BrokerSignInPage.qml:240` | Explain and validate name, account and password requirements beside their fields before staging. |
| Opportunity | Headings, forms and disclosures have inconsistent leading/centered alignment and large gaps. | Use one bounded content column, native aligned forms and consistent section spacing. |
| Opportunity | Raw paths, backend labels and generic inheritance controls dominate basic tasks. | Use outcome-oriented labels; place implementation tuning under Advanced. |
| Opportunity | Runtime diagnostics occupy the same editor as normal settings. | Move them to a read-only service-details page with explicit inspection. |
| Opportunity | Virtual uses a second tab strip for settings with separate owners and saves. | Give new-desktop hardware defaults their own clearly scoped child page. |

**Keep the recent refresh fix.** Static editors and systemd events already
address the demonstrated delegate replacement problem. Do not reintroduce
polling, rebuild forms on status changes, or discard drafts on navigation.

## Recommended navigation

Use an **overview with native KCM subpages**, rather than adding another sidebar
inside System Settings. The overview answers the connection questions; each
editor answers one configuration question. A normal Back action returns to it.

```text
Farside Remote Desktop                         [overview]
  Console — this computer's desktop             [immediate service controls]
    Configure Console…
      Certificate…                             [local TLS draft → host draft]
      Service Details…                         [read-only/manual inspection]
  Virtual — separate desktops for remote users  [immediate service controls]
    Configure Virtual…
      New Desktop Hardware…                    [separate settings/save]
      Certificate…                             [local TLS draft → host draft]
      Service Details…
  Who Can Connect…                             [one access-policy editor]
  My Preferences…                              [one account editor]
```

Alternatives considered:

| Structure | Assessment |
|---|---|
| One long page | Makes discovery easy but mixes authorization, service actions and several save scopes; too much scrolling. |
| Current four tabs | Retains drafts conveniently, but duplicates service chrome and leaves certificates and host drafts across tabs. |
| Overview plus subpages — recommended | Clear starting point and save ownership; adds one navigation step to detailed tuning. Native Back navigation avoids a second sidebar or nested tabs. |

### Overview

Two compact service sections, Console first. Each has one plain purpose sentence,
running/failed/unknown status, an **Allow connections** switch and a Configure
action. “Start when computer boots” is a separate immediate switch, never part
of a settings draft. Stop/restart first confirms which clients will disconnect.

Show a saved endpoint only from loaded saved settings. If startup inspection
verifies a different endpoint, label both **Saved address** and **Running
address**. Neither is proof of network reachability. Use `Not checked` when
unknown, with an explicit check action; do not invent a connectable hostname
from an all-interface bind address. Make verified values selectable/copyable.
Status refresh remains available without loading administrator configuration.

### Console and Virtual configuration

One introduction: who these defaults affect and when changes apply. Loading
administrator settings remains an explicit action with the normal system
authorization prompt. Afterwards use one aligned form with these groups:

1. **Connection:** listening address and port; Certificate summary/link.
2. **Video:** quality and automatic adjustment.
3. **Sound and devices:** network preference, other RDP app media policy, camera
   readiness. Explain client consent separately from host permission.
4. **Advanced:** encoding policy, AV1 tiles, applicable VA-API policy and the
   Console camera bridge device. No diagnostic wall in the form.

Console links to **My Preferences → Console displays**. Virtual links to
**New Desktop Hardware**, explicitly applying only to newly created desktops.
Do not label Console client-created displays as Virtual sessions.

Keep camera readiness visible: **Not checked**, **Ready**, **Unavailable**, or
**Setup required**, with a reason and an available repair instruction. Unknown
is not success. A bridge path is not a user camera-sharing toggle. Hide the
Virtual loopback editor, preserving its stored value; explain why the Console
bridge option does not apply there. This design must consume existing readiness
metadata where sufficient, and identify any missing checks before promising a
working camera. It does not authorize silent package installation.

### Who Can Connect

One policy editor with Console and Virtual groups and one **Save Access Policy**
action. Each group offers system-account login policy: All eligible accounts,
Selected accounts, or Disabled. Show the selected-account list only for that
mode. Explain that Console requires the appropriate logged-in desktop owner;
Virtual creates or reconnects desktops belonging to the authenticated account.

Remote logins use readable rows: **Remote login → Desktop account**, with Edit
and Remove. Prefer an account selector if verified eligible-account inventory
exists; otherwise use a validated account-name field and explain it. Do not
invent available accounts. Add/edit uses a small native dialog with a local
draft. Its **Add to Policy / Update Policy** action stages only that login;
the page still visibly says Unsaved changes. Removal is staged and reversible.

### My Preferences

Explain once: “For your account. Video and sound apply to Console and Virtual;
display selection applies to Console. Save, then reconnect.” Host permissions
and administrator locks continue to take precedence.

Groups: **Video**, **Console displays**, **Sound and session**, and collapsed
**Advanced / compatibility**. Use a consistent **Use host setting / Custom**
choice. A default is not an explicit false or zero. Display a known default only
when verified for the relevant route; otherwise say **Uses host setting**.
Console and Virtual can have different defaults.

For unknown inherited numbers, choosing Custom opens an editable draft value
without writing a minimum into the model. Require an explicit valid entry or
deliberate slider movement. If an unchanged preview value is offered, name its
source; never present it as the actual host value. Save stays disabled while a
custom value is incomplete. Locked values stay visible and unchanged.

## Complete option placement

All currently supported options remain accessible. This is an inventory of
existing capability, not a proposal to add new codec or multi-GPU features.

### Administrator host settings — 25 scope-field entries

`C` = Console; `V` = Virtual; `N` = new virtual desktop defaults. C and V have
independent models/saves; N has its own save. Ordinary host changes require an
explicit service restart. N changes affect new desktops only.

| Backend key | Scope | Proposed location / label | Native control and behavior |
|---|---|---|---|
| Address | C,V | Connection / Listen on | ComboBox for all interfaces/custom address; validated custom TextField. Keep IPv4/IPv6 semantics explicit. |
| Port | C,V | Connection / Port | Editable SpinBox, 1–65535. |
| Certificate | C,V | Certificate child page / Certificate | Read-only path and metadata; native file picker for import, validated root path for existing material. |
| CertificateKey | C,V | Certificate child page / Private key | Path only; native picker when importing. Never display or copy key contents. |
| Quality | C,V | Video / Image quality | Slider plus editable SpinBox, 0–100; inherit-unit option retains its actual default. |
| AdaptiveQuality | C,V | Video / Adjust quality to the connection | Stable label; ComboBox Default / On / Off so absent and explicit values remain distinct. |
| PreferAudioQuality | C,V | Sound / When the network is busy | ComboBox Default / Keep video sharp / Keep sound smooth. Explain policy, not a guarantee. |
| StandardClientMedia | C,V | Sound / Media with other RDP apps | ComboBox Default / Allow / Block; host permission does not grant camera or microphone consent. |
| CameraLoopbackDevice | C,V | Console Advanced / Camera bridge device | Console validated device path, readiness above; Virtual read-only explanation, existing value preserved. |
| SoftwareEncoding | C,V | Advanced / Encoding policy | ComboBox Default / Automatic / Allow software for best codec / Prefer hardware; map Automatic → `auto`, Prefer hardware → `never`, Allow software for best codec → `prefer`. Hardware preference is not an absolute no-software guarantee. |
| Av1Tiles | C,V | Advanced / AV1 tiles | ComboBox Default / Automatic / 1 / 2 / 4 / 8 / 16; only meaningful when AV1 is negotiated. |
| VaapiDriver | C | Advanced / VA-API driver policy | ComboBox with existing valid modes; does not select an NVIDIA GPU. |
| RenderPci | N | New Desktop Hardware / Available graphics devices | Device checkbox list only with verified inventory, explicit no-grant option; otherwise validated PCI list under Advanced. Namespace access, not per-stream encoder assignment. |
| VaapiDriver | N | New Desktop Hardware / VA-API driver policy | ComboBox; affects new desktops, independent of the Virtual listener's settings. |

### Account preferences — all 17 keys

Every row supports inheritance and preserves administrator locks. Conditional
visibility never clears stored values. Inherited display mode whose effective
value is unknown does not justify guessing which dependent controls apply;
offer explicit customization or a verified default lookup.

| Backend key | Proposed location / label | Control / condition |
|---|---|---|
| Quality | Video / Image quality | Host/custom selector, Slider + SpinBox; explicit custom value, 0–100. |
| AdaptiveQuality | Video / Adjust quality to the connection | ComboBox Host / On / Off. |
| Codec | Video / Color detail | Host / Automatic / Standard color (AVC420) / Full color (AVC444). Explain automatic negotiation; there is currently no forced HEVC/AV1 setting. |
| MonitorMode | Console displays / Share | Host / Whole workspace / Primary display / One display / Separate display streams / Client-created displays. |
| MonitorIndex | Console displays / Display | Only for One display; verified display selector if inventory exists, otherwise bounded index with an explanation that numbering starts at zero. |
| VirtualMonitorPolicy | Console displays / Physical displays | Only for client-created displays: Keep on / Turn off during connection. Explain restore behavior beside Turn off. |
| VirtualMonitorLayout | Console displays / Layout | Only for client-created displays: Client monitors / One display / Physical layout. |
| VirtualMonitorFallbackSize | Console displays / Fallback size | Only for client-created displays; width × height SpinBoxes, even dimensions, 320×200 through 8192×8192; explain use when client monitor data is unavailable. |
| PreferAudioQuality | Sound and session / When the network is busy | Host / Keep video sharp / Keep sound smooth. |
| StandardClientMedia | Sound and session / Media with other RDP apps | Host / Allow / Block; display host permission/consent caveat. |
| WakeDisplayOnConnect | Sound and session / Keep displays awake while connected | Host / On / Off; explicitly does not unlock the screen. |
| SoftwareEncoding | Advanced / Encoding policy | Host / Automatic / Allow software for best codec / Prefer hardware. |
| Av1Tiles | Advanced / AV1 tiles | Host / Automatic / 1 / 2 / 4 / 8 / 16. |
| Avc444MotionGapMs | Advanced / AVC444 color updates during motion | Host/custom SpinBox, 16–5000 ms. |
| Avc444RestMs | Advanced / AVC444 color updates at rest | Host/custom SpinBox, 16–5000 ms. |
| Avc444MaxGapMs | Advanced / AVC444 maximum color-update gap | Host/custom SpinBox, 16–5000 ms. Validate motion ≤ rest ≤ maximum as a group. Unset members use built-in timing values if any override is set. |
| VirtualStockClientPolicy | Advanced / Other RDP apps in Virtual | Host / Attach to or create a desktop / Require Farside session selection. Account ownership rules remain enforced. |

### Policies, actions and diagnostics

| Item | Location / behavior |
|---|---|
| PAM mode and allowed accounts, independently for C/V | Who Can Connect; policy draft; normal authorization on explicit load/save. |
| Alias name, owning account, new/unchanged password, add/edit/remove | Who Can Connect; local dialog draft → policy draft. Changing owner requires a new password. Removal supports Undo before saving. |
| TLS keep/existing paths/standard paths/import pair | Host Certificate child page. Use clear RadioButtons; only relevant fields visible. Pair validity required before staging. Keep existing is the initial state. |
| Certificate validity/fingerprint and saved paths | Host Certificate page, selectable metadata; distinguish saved material from verified startup material. Unknown remains unknown. |
| Allow connections, start at boot, start/stop/restart | Overview and service-details actions; immediate, authorized, serialized actual readback. Confirm disruption for stop/restart. |
| Status, startup configuration comparison, unit details, failures | Service Details; inspect explicitly, read-only. Saved values, running values and pending values stay separately labelled. |
| Copy address; refresh status; reload saved settings | Different named actions. Reload settings may discard drafts and confirms when needed; status refresh cannot alter them. |
| Save, revert, restore defaults | Scoped footer on the editor, never the overview or read-only diagnostics. Rules below. |

## Editing and dialog contract

| Action | Expected result |
|---|---|
| Save Console / Save Virtual Settings | Save the complete named host draft after validation/authorization, including its visibly staged certificate choice. Never restart automatically. Show saved/restart-needed feedback and an explicit Restart action. |
| Use Certificate Changes | Validate and stage only the local certificate draft into its host; return to the host page. Show the pending certificate change in its Connection summary and save summary. Does not save the host. |
| Cancel certificate edit | Discard only the local TLS edits; preserve unrelated host changes and any previously staged TLS choice. Returning to the host always reveals whether a TLS change remains pending. |
| Restore Defaults on host | Stage ordinary host defaults; preserve TLS paths, mode and imported material. Say what changes. Certificate “Use standard paths” is explicit and separate. |
| Revert Changes | Restore the loaded snapshot for the named draft, without an external reload or save. For host drafts this includes TLS; list that in the scope message. Requires a local revert API for account/access models if absent. |
| Reload Saved Settings… | Explicit external read. Confirm discarding unsaved changes for this scope. Preserve draft on canceled authorization or failed/unknown result. |
| Save New Desktop Defaults | Save N only; existing desktops and listeners remain unchanged. |
| Save Access Policy | Save both policy groups, never host/account preferences. Show that both services require restart and offer separately confirmed restart actions. |
| Save Preferences | Save only this account's allowed overrides; reconnect notice. No administrator prompt or service restart. |
| Use Host Settings | Stage removal of the editable preference overrides only, preserving locks. Clear description distinguishes it from reverting edits. |
| Navigate away with draft | Preserve it; show pending-change markers on overview links. No modal merely for moving between pages. On module close offer explicit discard/cancel where the KCM lifecycle supports interception; verify support before promising a close guard. |

Dialogs are for short consequential decisions or a short login editor, not
whole settings pages. Use native `Kirigami.PromptDialog` for stop/restart and
discard choices, with named actions rather than generic OK. Use
`Kirigami.PasswordField` for passwords, cleared on dismissal; never show a
stored password. Use the system PolicyKit prompt for privileged operations.
TLS import uses one native file dialog at a time from a normal page. Do not add
a wallet picker, wallet reset, wallet migration or secret export.

Saving shows progress and preserves drafts on rejection. Place invalid-field
explanations near the fields and disable confirmation until local validation
passes; the backend still validates authoritatively. Show actionable errors
and stale/unknown outcomes persistently. Unknown outcomes require readback,
not a success toast or automatic retry.

## Native layout and implementation rules

- One bounded leading content column; aligned labels/inputs within each native
  `Kirigami.FormLayout`. Do not center standalone headings in the viewport.
- `smallSpacing` within related controls, `largeSpacing` between groups. Use
  normal KDE text sizes and palette roles; no decorative cards or custom theme.
- Native navigation delegates, switches, ComboBoxes, editable SpinBoxes,
  sliders, InlineMessages and fixed scoped footers. Standard focus indicators,
  accessible names/buddies, logical keyboard order, selectable/copyable details.
- On narrow widths let forms stack and the content scroll; footers must remain
  usable. Allow translation expansion, RTL and larger fonts. No hardcoded
  primary-label widths, clipped errors or horizontal scrolling of whole forms.
- Keep models and unfinished editor input alive across Back navigation and
  service events. Stable identities, no polling and no Repeaters bound to the
  changing service list. Event status updates must not move focus or geometry.
- Treat certificate editing, scoped defaults, numeric customization and local
  revert as functional changes requiring model support where necessary. QML
  rearrangement alone cannot fix their save semantics.

## Suggested delivery order — one task at a time

This is a design proposal, not a new Goal or an automatic execution queue.

| Task | What to implement | Done means |
|---|---|---|
| U1 Safe draft behavior | Separate ordinary defaults from TLS; local certificate draft/staging/cancel; deliberate numeric customization; scoped local revert. | One focused pure/model check proves ordinary defaults preserve TLS, TLS cancel preserves host edits, and inheritance never silently stages zero. Existing authorization/revision protections retained. |
| U2 Overview and host pages | Native overview/Back navigation, common forms, service details, independent new-desktop hardware page; certificate workflow from U1. | Populated native KDE renders on Buzz show overview/host/certificate/narrow states; Steve accepts visual organization. One event-update check preserves focus, unfinished text, draft and geometry. |
| U3 Preferences and access | All 17 keys mapped, conditional displays, plain labels, required login fields and native password control, policy restart feedback. | Focused checks cover conditional visibility without value loss, locked/inherited values and login staging/cancel. One visual review covers these pages and dialogs; every inventory row reachable. |
| U4 Install and acceptance | Build/package reviewed product changes and install on selected authorized hosts. | One real authorization cancel/save/readback/explicit restart check on Buzz/Sol, preservation checks required by deployment policy, and Steve's visual acceptance. Stop after the selected delivery; do not start unrelated tests. |

Do not fix 144 scanner style findings as a prerequisite to the redesign. They
are review inputs, not 144 confirmed bugs. Do not repeat camera, codec,
performance or full fleet tests unless a changed behavior gives a concrete
reason. No live Hal testing. No wallets or real configuration were accessed
for this assessment.

## Basis and evidence

- [KDE: simple by default](https://develop.kde.org/hig/simple_by_default/) and
  [layout/navigation](https://develop.kde.org/hig/layout_and_nav/) support task
  priority, grouping and familiar navigation.
- [KDE: getting input](https://develop.kde.org/hig/getting_input/) distinguishes
  immediate switches from staged checkboxes, stable labels, native password and
  file controls, validation and avoiding nested dialogs.
- [KDE: status changes](https://develop.kde.org/hig/status_changes/) and
  [text/labels](https://develop.kde.org/hig/text_and_labels/) guide actionable
  feedback and plain wording.
- [Kirigami forms](https://develop.kde.org/docs/getting-started/kirigami/components-formlayouts/)
  and [KCM development](https://develop.kde.org/docs/features/configuration/kcm/)
  are the native implementation references.
- Source review includes `brokerhostsettings`, `brokerpreferences`,
  `brokerauthenticationsettings`, `brokerservices`, and the KCM model ownership.
  Raw lint evidence: `~/dev/rdp/evidence/2026-10-01-settings-design-review/`.
  Fixture images: `~/dev/rdp/evidence/2026-10-01-settings-tabs/tabs-{0,1,2,3}.png`.
- Applied Qt UI Design, QML and QML Review skills. Six focused review passes
  were performed by the current agent, following the standing no-delegation
  rule; no claim of independent reviewers or a model switch.
