# Farside settings (kcm_farside): UX and implementation assessment

**Verdict: Steve's complaint is right, and the cause is structural, not cosmetic.** The page layout copies the
six C++ save scopes (Console host, Virtual host, new-desktop hardware, access policy, personal preferences,
service control) instead of the user's tasks. The result is 5 pages, 5 different Save buttons, 4 Reload
buttons, 2 meanings of "defaults", and 14 buttons/switches on the overview. Every subpage also opens
**empty behind a "Load…" button**. Host and access reads need an administrator password (`auth_admin`, not
cached), so looking at the settings costs up to four password prompts before anything is saved. The KCM also
switches off System Settings' standard Apply/Reset/Defaults and its native page stack and builds its own.
Restyling will not fix this. **Recommendation (design A):** one dashboard root page that already holds
the everyday settings (on/off, address, who can sign in, image quality, my display), plus three pushed pages
(Console, Virtual, My Preferences) that each carry real content. Use the standard KCM
Apply/Reset/Defaults with one authorization per Apply, and read the non-secret settings without a password.
Assessment only: no product code was changed.

- Code reviewed: `src/kcm/ui/*.qml` (13 files, 1,382 lines) and `src/kcm/*.{h,cpp}` at HEAD `e952c3b8`.
  The KCM QML is unchanged since `548fce84`.
- Fresh captures: Buzz, offscreen, software renderer, `org.kde.desktop` style, light theme, private XDG dirs,
  fixture models, no System Settings shell. Saved in `~/dev/rdp/evidence/2026-10-04-settings-ux-assessment/captures/`.
- Wireframe: [2026-10-04-settings-ux-wireframe.svg](2026-10-04-settings-ux-wireframe.svg). A raster preview
  is in the evidence directory.

## Top 10 findings

| # | Sev | Finding (symptom) | Where | Rule |
|---|---|---|---|---|
| 1 | High | Each subpage opens with only an intro and a **Load…** button (one control). Host and access reads require `auth_admin`, so viewing everything takes 4 password prompts (Console, Virtual, New-desktop hardware, Access). My Preferences needs a click even though it needs no authorization. | `BrokerHostsPage.qml:43,51`; `BrokerHardwareSection.qml:24,29`; `BrokerSignInPage.qml:37,47`; `BrokerPreferencesPage.qml:28-30`; `kcmkrdpserver.cpp:25-29`; `server/org.farside.hostsettings.policy.in:8-11` | HIG [Simple by default](https://develop.kde.org/hig/simple_by_default/): "Be welcoming, not demanding", "Optimize common workflows" |
| 2 | High | Pages follow the save scopes, not the tasks. There are 5 Save labels (Save Console Settings…, Save Virtual Settings…, Save Desktop Defaults…, Save Access Policy…, Save Preferences), 2 "defaults" labels (Restore Defaults, Use Host Settings) and 4 Reload buttons. The Virtual page shows **two** Restore/Revert/Reload/Save rows at once (`advanced-2-2.png`). | `BrokerHostsPage.qml:118-127`; `BrokerHardwareSection.qml:73-80`; `BrokerSignInPage.qml:127-135`; `BrokerPreferencesPage.qml:74-82` | HIG [Simple by default](https://develop.kde.org/hig/simple_by_default/): "avoid button overload" |
| 3 | High | The module opts out of the standard KCM buttons (`setButtons(Help)`) and draws its own footers. In System Settings these are expected to sit above the shell's Help-only footer, giving two footer bars. Every other KCM, including stock KRdp (`setButtons(Help \| Apply \| Default)`), uses Apply/Reset/Defaults. | `kcmkrdpserver.cpp:21`; page footers as in #2 | [KQuickConfigModule](https://api.kde.org/kquickconfigmodule.html) (`buttons`, `needsSave`); HIG [Be consistent](https://develop.kde.org/hig/simple_by_default/) |
| 4 | High | KCM navigation is re-implemented: `AbstractKCM`, a custom ToolBar header with its own Back button and title, a `StackLayout` and a JS `history` array, instead of `kcm.push()`. Expected effects: a duplicated title, no System Settings breadcrumb or column mode, a non-standard Back. | `BrokerMainPage.qml:8-56` | [KQuickConfigModule::push](https://api.kde.org/kquickconfigmodule.html); HIG [Layout and navigation](https://develop.kde.org/hig/layout_and_nav/) (PageStack, minimize navigation) |
| 5 | Med | The overview is a launcher with 14 controls per screen and redundancy: the switch and **Stop…** do the same thing, Status / Saved address / Running address repeat one fact, Restart and Stop sit at the top level, and Access and Preferences are reached through footer buttons that look like actions. | `BrokerMainPage.qml:70-101`; `BrokerServiceControls.qml:37-95`; `page-0.png` | HIG [Simple by default](https://develop.kde.org/hig/simple_by_default/): "Show the most important UI elements" |
| 6 | Med | There are three alignment axes. Headings, InlineMessages and buttons sit flush left. Form labels are right-aligned in a middle column. Content is capped at 48 grid units and left-aligned. Sections are separate `Kirigami.Heading` plus separate `FormLayout`s glued together with `twinFormLayouts`, where `FormData.isSection` would do. | `BrokerHostsPage.qml:50-103`; `BrokerPreferencesPage.qml:29-58`; `page-1.png` | [Kirigami FormLayout: Sections and separators](https://develop.kde.org/docs/getting-started/kirigami/components-formlayouts/); HIG [spacing table](https://develop.kde.org/hig/layout_and_nav/) |
| 7 | Med | Wall of text: 16 permanent grey paragraphs (`disabledTextColor`) plus help under fields. The Details section alone has up to 8 paragraphs (`overview-details.png`). Grey "disabled" color for normal information also lowers contrast. | `BrokerPreferencesPage.qml:25,41,42,49,57,59`; `BrokerServiceDetails.qml:51-130`; `BrokerHostsPage.qml:79,105` | HIG [Text and labels](https://develop.kde.org/hig/text_and_labels/); [Status changes](https://develop.kde.org/hig/status_changes/): "Minimize unnecessary status messages" |
| 8 | Med | Jargon and internal terms on first-level pages: "Use unit default", "V4L2 loopback", "device namespace", "VA-API driver policy", "GPU PCI identities", "stage", "draft", "Use Certificate Changes", "Load Administrator Settings…", "Inspect Running Host…", "broker startup arguments". | `BrokerSettingField.qml:27,149`; `BrokerHostsPage.qml:84`; `BrokerCertificateSection.qml:62,107`; `BrokerServiceDetails.qml:92` | HIG [Text and labels](https://develop.kde.org/hig/text_and_labels/): "minimize technical jargon" |
| 9 | Med | Two sources of truth. C++ definitions supply label, group and choices. QML then overrides them with hardcoded key `switch`es and per-page key arrays (C++ "Prefer audio quality" vs QML "When network is busy"). The 170-line field component chooses its widget from the key name. | `BrokerSettingField.qml:21-64`; `BrokerHostsPage.qml:59,72,78,101,114`; `BrokerPreferencesPage.qml:33-69`; `brokerhostsettings.cpp:384-406` | Qt [Best practices: separate UI from business logic, type safety](https://doc.qt.io/qt-6/qtquick-bestpractices.html) |
| 10 | Low | Duplicated or dead QML: 7 near-identical PromptDialogs; `BrokerServicesPage.qml` (138 lines) is unused except by tests; `main_phone.qml` is identical to `main.qml`; `objectName` `<route>RefreshStatus` is defined twice; 108 lines longer than 200 characters; untyped `property var` everywhere; services are addressed by fixed index (`services[0]`/`[1]`). | `BrokerServicesPage.qml`; `BrokerServiceControls.qml:131` and `BrokerServiceDetails.qml:57`; `BrokerHostsPage.qml:48` | Qt [Best practices: type safety, declarative bindings](https://doc.qt.io/qt-6/qtquick-bestpractices.html) |

Further Medium/Low findings are covered in the sections below: certificate metadata always shown (5 rows), a
wall of identical "Use host setting" dropdowns, Access separated from the routes it governs, and a misused
ellipsis on "Edit PCI Identities…".

**What is already right, and should be kept:**
- native QQC2/Kirigami controls and `i18nc` contexts;
- Switches for instant actions (allow connections, boot) and CheckBoxes for staged ones;
- a Slider with a trailing SpinBox for quality;
- PromptDialog confirmation before disconnecting clients;
- `Kirigami.PasswordField`, cleared on close;
- dependent fields hidden without losing their values;
- drafts held in C++ so they survive navigation;
- no polling (systemd events).

These are good foundations; the problems are information architecture and plumbing.

## 1. Inventory

Fixture pages, captured at 900x850 and 640x800. The test harness fixes the window sizes, so the requested
1000x700 is not available.

| Page (file) | Visible interactive controls (default state) | Navigation/expanders | Own save row | Captures |
|---|---|---|---|---|
| Overview (`BrokerMainPage.qml`) | 4 switches, 6 action buttons (Restart…/Stop… ×2, Configure ×2), 2 Details toggles, 2 footer nav buttons = **14** | Configure Console…, Configure Virtual…, Who Can Connect…, My Preferences…, Details ×2 | none | `page-0`, `overview-details`, `narrow-0`, `stop-dialog` |
| Console (`BrokerHostsPage.qml` scope 0) | 6 setting fields (7 widgets) + Change… + Advanced Options + My Display Preferences… + 4 footer = **14**; Advanced adds 5 fields | Advanced, Certificate editor, cross-link to My Preferences | 4 buttons | `page-1`, `advanced-1-*`, `narrow-1` |
| Virtual (`BrokerHostsPage.qml` scope 1 + `BrokerHardwareSection.qml`) | as Console minus camera device, plus GPU checkboxes, Edit PCI Identities…, VA-API, and a **second** save row = **~22** | Advanced, Certificate, Edit PCI Identities… | 4 + 4 buttons | `page-2`, `advanced-2-*`, `narrow-2` |
| Who Can Connect (`BrokerSignInPage.qml`) | 2 combos (+ allow-list field), per login Edit/Remove, 2 Add, 2 help, 3 footer | Add/Edit dialog | 3 buttons | `page-3`, `alias-dialog` |
| My Preferences (`BrokerPreferencesPage.qml`) | 11 combos (all reading "Use host setting"), Advanced toggle, 4 footer; Advanced adds 6 | Advanced | 4 buttons | `page-4`, `advanced-4-*` |
| (orphan) `BrokerServicesPage.qml` | not reachable from the module | – | – | – |

Dialogs:
- PromptDialog instances: stop/restart in 2 service controls, reload ×2 and restart ×2 in the host pages,
  1 hardware reload, 1 preference reload, and reload plus restart in Access.
- Kirigami.Dialog: 1 (remote login).
- FileDialog ×2 per host page.

There are no nested dialogs, which is good. Kirigami.Dialog and FileDialog are never opened from each other.

### Pages and buttons that lead to "one thing", with proposed fate

| # | Element | What you get today | Fate |
|---|---|---|---|
| 1 | **Configure Console…** before load | A sentence plus one button, *Load Administrator Settings…* | Auto-load with an unprivileged read (slice S1). Fold everyday fields into the root page; the rest goes on a populated pushed page. |
| 2 | **Configure Virtual…** before load | The same, plus a **second** *Load Administrator Settings…* for the hardware section | As 1; the hardware section loads with the page, with no separate button. |
| 3 | **Who Can Connect…** | Before load: one button. After load: one combo per route and a short list | **Remove the page.** "Who can sign in" goes on the root page per route; remote logins go on each route page. The policy is per route anyway (`BrokerSignInPage.qml:39-48`). |
| 4 | **My Preferences…** before load | One button, *Load My Preferences* (no authorization involved) | Auto-load on open. Keep a pushed page for the full list, with the two common items on the root page. |
| 5 | **Details** expander (overview) | One *Inspect Running Host…* button plus up to 8 paragraphs | Move into *Advanced → Troubleshooting* on the route page. The result is one line plus "Show details"; drop the boilerplate paragraphs. |
| 6 | **My Display Preferences…** (Console page) | Jumps to another page and scope to show one group | Remove the cross-link. Console display moves to the root page's *My account* section. |
| 7 | **Edit PCI Identities…** (Virtual) | Reveals one text field (the ellipsis is wrong: no dialog opens) | Move the raw field under Advanced and remove the button. |
| 8 | **Change…** certificate | Inline editor (acceptable), but 5 metadata rows are always visible above it | Keep the inline editor. Collapse the metadata into one summary line with a copy button. |
| 9 | Historic Certificate / Service Details / New Desktop Hardware pages (10-page version) | Already folded in `3f45ebdb`..`0a941150` | Done; keep folded. |

## 2. Information architecture

**What users actually change.** From the option map in `2026-10-01-farside-settings-ux.md`:
- *Often*: whether Console and Virtual are on, the address to connect to, who may sign in, image quality, and
  (for the account) which Console display to share.
- *Sometimes*: port and listen address, sound and media policy, the camera, remote logins, start at boot,
  restart.
- *Rarely*: certificate, encoding policy, AV1 tiles, VA-API driver, GPU grants, the camera bridge device,
  AVC444 timings, runtime inspection.

Today the *often* items are spread over **four pages**: the overview, Configure, Who Can Connect and My
Preferences. Three of them need an explicit load first.

**Pages for thin content.** After loading, each route's basic form has only 6 fields. Access has 1-2
controls per route. The full set of first-level settings (about 20 controls) fits on one well-sectioned page
plus Advanced disclosures. The split into pages is driven by the save scopes. That is what Steve describes:
"buttons that take you to new panels with only one thing on them".

**Navigation depth.**
- Common tasks are 1 click plus a password away (Configure, Load, authenticate).
- Who-can-sign-in is 2 clicks plus a password.
- Changing your own Console display from the Console page jumps across pages, which leaves the user unsure
  where they are.

HIG [Layout and navigation](https://develop.kde.org/hig/layout_and_nav/): "The best navigational flow is
nonexistent, because everything is provided to the user as they need it."

**Launcher vs dashboard.** The overview shows status and actions, but no setting a user changes regularly.
It should be the place where the common settings are edited (design A).

**Scope clarity.** The real scopes do matter to users:
- host-wide versus *my account*;
- "needs a service restart" versus "reconnect" versus "new desktops only".

The current UI states these in grey sentences on every page and in footer labels. A dashboard can show them
once per section, using its subtitle ("only affects you · reconnect to apply") and a header InlineMessage
when a restart is pending.

## 3. Layout and style

- **Alignment (finding 6).** Use one centered `Kirigami.FormLayout` per page, with
  `Kirigami.FormData.isSection` rows (or `Kirigami.Separator` sections) for Connection / Picture and sound /
  Service. Headings then line up with the label column. Buttons that act on a field become form rows with
  their own labels, and InlineMessages go in the page header. Every KDE KCM uses this pattern. It removes the
  `twinFormLayouts` cross-references (`BrokerHostsPage.qml:58,72,78,100`) and the hand-tuned
  `Layout.maximumWidth` caps of 24, 28 and 48 grid units.
- **Button hierarchy (findings 2 and 5).**
  - Root level: switches plus at most one "Configure X…" button per route.
  - Restart moves to the route page's Service section and to the restart InlineMessage.
  - Stop goes away, because the switch already does it.
  - Reload goes away (auto-load, and Reset reverts).
  - Revert becomes the shell's Reset; Restore Defaults and Use Host Settings become the shell's Defaults.
- **Inline vs modal.** These choices are right: the inline certificate editor (it avoids a FileDialog nested
  in a dialog, per HIG [Getting input](https://develop.kde.org/hig/getting_input/): "Never use a dialog to
  create more dialogs") and the small login dialog. Keep both.
- **Certificate block.** The 5 always-visible rows (`BrokerCertificateSection.qml:44-56`) are mostly
  paths. Show one line ("Valid until 2030-09-01 · SHA-256 AA:…:AA [copy]") with the paths under "Details".
- **My Preferences wall** (`page-4.png`). Eleven combos all read "Use host setting", so the user cannot see
  what they will actually get. The cause is upstream: preferences cannot read host values, because host reads
  need admin rights (finding 1). Once reads are unprivileged:
  - show "Host setting (80)";
  - use the KCM defaults indicator (`KCM.SettingHighlighter` / `settingStateBinding`) to mark customized
    rows;
  - offer one reset action per field. The inheritance contract stays unchanged: an absent value is still
    distinct from an explicit one.
- **Apply/revert semantics (finding 3).** A standard KCM has one Apply, one Reset and one Defaults. That
  replaces 5 footers, 15 footer buttons and the "Unsaved changes · X only" status lines. Per-scope
  disclosure is still needed in two cases: after Apply when a restart is required (header InlineMessage
  with **Restart Console** / **Restart Virtual**), and when one scope fails (header InlineMessage naming that
  scope, with its draft kept).
- **Narrow width** (`narrow-0..2.png`). There is no clipping at 640 px and the footers fit. With the shell
  footer added, the content area loses about 2 more rows. Removing the page footers recovers about 50 px.

## 4. Implementation (QML/Qt)

- **Navigation (finding 4).** Replace `StackLayout` / `history` / `openPage` / `goBack` / `leave()`
  (`BrokerMainPage.qml:19-56`) with `kcm.push("ConsolePage.qml", {...})` and `KCM.SimpleKCM` subpages. Drafts
  already live in C++ models with `CppOwnership` (`kcmkrdpserver.cpp:22-23`), so popping a page loses no
  data. Only focus and scroll position are lost, which is normal for every KCM. The "certificate editor
  cancels on leave" rule becomes `StackView.onRemoved` / `Component.onDestruction` on the route page.
- **Save model (findings 2 and 3).**
  - `setButtons(Help | Apply | Default)`.
  - `needsSave` is the OR of the six models' `modified` and is updated on `changed()`.
  - `save()` writes the user preferences first (no authorization), then the dirty admin scopes.
  - Use either polkit `auth_admin_keep` on the two actions or one batched helper call, so one Apply means one
    prompt.
  - `defaults()` stages ordinary defaults only for the scopes on the current page (TLS preserved, as today
    in `brokerhostsettings.cpp`). The KCM `Default` button acts module-wide, so this needs to be defined and
    tested.
  - `load()` reads all scopes (finding 1).

  The existing revision, outcome-unknown and TLS-staging protections stay inside the models unchanged.
- **Data-driven fields (finding 9).**
  - Add `control` (`choice|number|slider-number|size|address|text|pci-list`), `min`/`max`/`unit` and
    `advanced` to the C++ definitions. Put the final user-facing labels and choice texts there, once.
  - Reduce `BrokerSettingField` to a dispatcher over small components (`ChoiceField`, `NumberField`,
    `AddressField`, `SizeField`). Pages then iterate definitions by `group`, rather than repeating key arrays.
  - Every setting then keeps UI, validation, help, default and tests in one place, which is the project rule
    in `CLAUDE.md` ("every setting must ship matching UI, validation/help/defaults, tests").
- **Help text (finding 7).** Move per-field help into `Kirigami.ContextualHelpButton` (already used in
  `BrokerSignInPage.qml:117`), or into `Accessible.description` with a short subtitle. Keep visible text only
  where it changes a decision (camera readiness, "new desktops only", "reconnect to apply"). Use
  `Kirigami.Theme.disabledTextColor` only for disabled things.
- **Imperative re-binding.** `checked = Qt.binding(...)` after `onClicked` (`BrokerServiceControls.qml:46,93`,
  `BrokerHardwareSection.qml:34,46`, `BrokerSettingField.qml:99,109,127,129`) is an accepted workaround for
  instant-apply readback, but it is repeated eight times. Wrap it once in a `ReadbackSwitch` / `ReadbackCheckBox`
  component.
- **Typing.** Register the models with `QML_ELEMENT` / `QML_UNCREATABLE` and declare
  `required property BrokerHostSettings host` and similar, instead of `property var`. qmllint can then check
  bindings. The 2026-10-01 review recorded 140 qmllint warnings, many caused by `var`. Replace
  `services[0]/[1]` with `services.console` / `services.virtual` or a `serviceFor(route)` call.
- **Dead and duplicate code.**
  - Delete `BrokerServicesPage.qml` and its test, or fold its fixed-index lesson into the route pages.
  - Make `main_phone.qml` a different layout or remove it.
  - Make one `ConfirmRestartDialog.qml` and one `DiscardAndReloadDialog.qml`.
  - Give unique objectNames.
- **Accessibility and keyboard.**
  - Switches set an `Accessible.name` and FormLayout buddies are set: good.
  - Grey help is not linked to its field (`Accessible.description` is missing).
  - The custom Back ToolButton gives no Alt+Left / Backspace handling; System Settings' own stack does.
  - Remote-login rows use `ItemDelegate` with ToolButtons inside but no single accessible row name.
  - Checked with the fixture renderer only; no screen-reader run.
- **Loader vs hide.** "Hide, never unload draft editors" (`settings-ui.md:39`) is right while drafts live in
  QML. Most drafts already live in C++. With `kcm.push` the remaining QML-only state is the certificate file
  selection (`BrokerCertificateSection.qml:20-22`); move it into the C++ certificate draft.

## 5. Content

| Today | Proposed |
|---|---|
| Allow connections | Remote connections (switch; status and address as the description) |
| Use unit default (all IPv4 interfaces) / Default (Off) | (default) suffix on the real value: "All network interfaces (default)", "Off (default)" |
| Who Can Connect… / System accounts: All eligible accounts | Who can sign in: Everyone with an account / Selected accounts / Nobody |
| Remote login → Desktop account | Keep; section subtitle "Extra sign-in names with their own password" |
| Load Administrator Settings… | (removed; auto-load) |
| Inspect Running Host… / Host Inspection | Advanced → Troubleshooting: "Check Running Service"; result: "Running with the saved settings" or a one-line difference plus "Show details" |
| Camera: setup required — configure a V4L2 loopback device in Advanced | "Camera: not set up" plus "How to set up…" (opens the docs, or expands the Advanced field) |
| Camera: sharing is unavailable in the current Virtual device namespace | "Camera sharing is not available for Virtual desktops yet." |
| GPU PCI identities / Grant no GPU access | New desktops: "Graphics acceleration: [ ] AMD 780M (0000:c5:00.0)…"; raw list under Advanced |
| Use Certificate Changes / staged | "Use This Certificate"; header text "Certificate will change when you apply" |
| Saved address / Running address | One "Address" line; show "Running" only if it differs (warning style) |

The rule behind these: plain outcome labels, the default shown as a value, jargon only under Advanced (HIG
[Text and labels](https://develop.kde.org/hig/text_and_labels/)).

## 6. Recommended design (A): dashboard root plus three content pages

See the wireframe [SVG](2026-10-04-settings-ux-wireframe.svg).

```
Farside Remote Desktop                                      (KCM.SimpleKCM, one FormLayout)
┌ [i] Console settings changed. Restart Console to use them.   [Restart Console] ┐ header, only when pending
                Console  share this computer's screen
    Remote connections:  (●) Running · hal9000.lan:3391  [⧉]
       Who can sign in:  [Everyone with an account ▾]
         Image quality:  ──────●── [80]
                         [Configure Console…  ›]          → kcm.push(ConsolePage)
                Virtual  a separate desktop for each user
    Remote connections:  (●) Running · hal9000.lan:3395
       Who can sign in:  [Selected accounts ▾]   Accounts: [westers, guest]
         Image quality:  ──────●── [80]
                         [Configure Virtual…  ›]          → kcm.push(VirtualPage)
             My account  only affects you · reconnect to apply
       Console display:  [Host setting (whole workspace) ▾]
         Image quality:  [Host setting (80) ▾]
                         [More Preferences…  ›]           → kcm.push(PreferencesPage)
──────────────────────────────── System Settings: [Help] [Defaults]      [Reset] [Apply]

Console (pushed)                         Virtual (pushed)                 My Preferences (pushed)
  Connection: Listen on, Port,            = Console, minus camera device,   Video: quality, adjust, color
    Certificate summary [Change…]          plus section "New desktops":     Console displays: share, display,
  Remote logins: list + Add… (dialog)      Graphics acceleration (GPU          physical, layout, fallback
  Picture and sound: adjust, network        checkboxes), VA-API policy     Sound and session: 3 items
    busy, other-app media, camera state                                    ▸ Advanced: encoding, AV1, AVC444×3,
  Service: Start at boot, Restart…                                          other RDP apps in Virtual
  ▸ Advanced: encoding, AV1 tiles,                                         (host value shown in each
    VA-API, camera bridge device,                                            "Host setting (x)" item; customized
    Troubleshooting                                                          rows marked by SettingHighlighter)
```

- **Pages: 1 root plus 3 pushed** (5 today), and no page holds a single control. The Who Can Connect page is
  dissolved into the route sections. Its single policy file still saves as one scope behind Apply.
- **Demoted to Advanced:** encoding policy, AV1 tiles, VA-API driver, camera bridge device, GPU PCI raw
  list, AVC444 timings, troubleshooting/inspection, certificate paths.
- **Removed:** Stop… (the switch does it), the Reload buttons, the Revert/Restore/Use-Host footers (replaced by
  the shell), the "Unsaved changes · …" lines, the My Display Preferences cross-link, Edit PCI Identities…,
  the duplicate Saved/Running address, and the "Load…" buttons.
- **State and save model:**
  - *Instant, as today:* the Remote connections and Start at boot switches, and Restart (confirmed).
  - *Staged:* everything else, until **Apply**. One Apply writes preferences (no password), then the dirty
    admin scopes with **one** authorization.
  - *Reset* reverts all drafts. *Defaults* stages defaults for the scopes on the current page, with TLS
    preserved.
  - *After Apply:* a header InlineMessage for routes that need a restart. Nothing restarts automatically.
  - *Failure:* the failed scope keeps its draft and an error InlineMessage names it.
  - The existing revision and outcome-unknown guards are unchanged.
- **Product facts preserved:**
  - exactly two routes (3391, 3395);
  - per-user preferences separate from host settings;
  - polkit-gated writes;
  - an explicit TLS operation, with Defaults preserving TLS;
  - "Camera not available for Virtual" shown as unavailable, not hidden as working;
  - every one of the 25 host entries and 17 account keys stays reachable (the inventory in the 2026-10-01
    design still applies; the new locations are listed above).

### Alternative (B): master–detail columns (CHOSEN 2026-10-04; supersedes design A in section 6 and its wireframe)

Set `kcm.columnWidth` so the root list stays as a sidebar beside the pushed page (as in KDE's Users KCM). The
list has 4 rows: Console, Virtual, Who Can Connect and My Preferences. Each route row carries a status
subtitle and a trailing switch. This also ends empty launch pages and keeps navigation always visible. Its
weaknesses: it keeps 4 destinations, keeps Access separate from the routes it governs, and collapses to the
same drill-down at narrow widths. Choose it only if Steve prefers a sidebar look. It needs the same S1–S3
plumbing.

## 7. Migration slices (one at a time; each ends with its check and stops)

| Slice | Change | Done when (testable) |
|---|---|---|
| S0 Decision | **Layout B chosen 2026-10-04 by Steve** (master-detail sidebar; design A, its wireframe and section 6 are superseded by B). DECIDED 2026-10-04 (Steve: "follow KDE best practices on the password issue"): viewing never prompts; only Apply is privileged, as one batched helper call per Apply with polkit `auth_admin_keep` on `org.farside.hostsettings` and `org.farside.authentication`. A new unprivileged read action (`org.farside.hostsettings.read`) is REJECTED (see Decision log). | Decision recorded in this file and the HANDOFF Log (done for the password decision). Layout B recorded (done 2026-10-04). |
| S1 Read without password | No helper read action. `KRDPServerConfig::load()` reads `/etc/krdp/*.conf` (already 0644) plus a root-broker-written 0644 public-metadata snapshot (revision, values, defaults, effective keys, TLS fingerprint/validity, render device ids; no account names, aliases, key/cert paths or verifiers; never anything under `/etc/farside`). Delete the Load buttons for host settings; account names/aliases load once, behind `auth_admin_keep`, when "Who can sign in" is expanded. | A pure test shows the snapshot contains no verifier, key, account-name or path fields and is mode 0644. KCM fixture: every host page is populated on open with zero clicks. Manual on Sol: opening the KCM shows no password prompt; expanding "Who can sign in" prompts at most once. |
| S1b Core-dump hardening | `farside-authentication-helper` sets `PR_SET_DUMPABLE=0` and `RLIMIT_CORE=0` at start, as the host helper already does. Independent of layout A/B; can ship first. | Pure/helper test reads `/proc/self/status` (or `prctl(PR_GET_DUMPABLE)`) and `RLIMIT_CORE` in the helper and finds 0. |
| S2 Native navigation | `kcm.push` pages; remove the StackLayout, custom header, history and `main_phone` duplicate; delete the orphan `BrokerServicesPage.qml`. | `KcmUiTest` loads root and pushes all pages. A test shows certificate-draft cancel on pop. Back works through `kcm.pop()`. grep finds no `StackLayout` / `history` in `src/kcm/ui`. |
| S3 Standard Apply/Reset/Defaults | `setButtons(Help\|Apply\|Default)`; `needsSave` aggregation; `save()` orchestration with one authorization; remove all page footers. | Fixture tests: editing any scope sets `needsSave`; Reset clears all; a failure in one scope keeps that draft and saves the others; the helper is invoked once per Apply for N dirty admin scopes; Defaults preserves TLS. Manual on Sol: opening the KCM shows no password prompt; one prompt per Apply for N dirty scopes; a second Apply within the `auth_admin_keep` window prompts zero times. |
| S4 Master-detail sidebar (layout B) | Set `kcm.columnWidth` so the root list stays as a sidebar beside the pushed page. Four sidebar rows: Console, Virtual, Who Can Connect (kept as its own row, not merged), My Preferences. Each route row (Console, Virtual) has a status subtitle and a trailing enable switch. Collapses to drill-down at narrow width. No page may be a single control. | An inventory test finds an editable control for each of the 25 host entries, 17 preference keys and 4 policy fields by objectName. Test shows 4 sidebar rows, route rows with subtitle and switch, and the wide/narrow switch. Buzz captures at 900 and 640 px; no page has fewer than 3 controls. |
| S5 Data-driven fields | `control`/`min`/`max`/`advanced` in C++ definitions; small field components; `FormData.isSection`; ContextualHelpButton help; typed `required property`. | grep shows no hardcoded key lists or key `switch` in QML. Existing field tests (Quality custom/incomplete, fallback size, address mode) pass. qmllint warning count is below the 140 baseline and recorded. |
| S6 Content and certificate summary | Wording table above; one-line certificate summary; troubleshooting collapsed. | Buzz gallery reviewed by Steve; the strings are in the i18n catalog. |
| S7 Package and accept | Build the server deb; install on Sol only (zero connections); real polkit Apply / cancel / restart. | Real prompt count per Apply = 1; settings read back; the restart InlineMessage works; Steve's visual acceptance. Hal untouched until Steve's go. |

### S3 result and S7 manual checklist (2026-10-04)

S3 landed in source (commit `OPT-057 S3 Use standard Apply, Reset and Defaults`). Behaviour to verify for real on Sol in S7:

- Administrator helper processes per Apply: 1 when only host scopes are dirty (the Console, Virtual and new-desktop
  hardware drafts travel in one `save-batch` request), 1 when only the access policy is dirty, 2 when both are dirty
  (the host helper and the authentication helper are different programs with different polkit actions; the second
  starts only after the first has finished). A single dirty host scope uses the plain request.
- Expected prompts: 1 per Apply. The second helper (access policy) must be silent through `auth_admin_keep`.

S7 checklist (Sol, zero connections, server package from the S3 commit or later; nothing on Hal):

1. Open the panel: no password prompt. Defaults, Reset, Apply visible; Apply and Reset disabled. (Real-shell capture
   shows Defaults enabled even when clean; check it is harmless.)
2. Change one Console field only, Apply: exactly ONE prompt; change read back; the overview shows "Saved. Restart Console".
3. Change a Console field, a Virtual field and a new-desktop hardware field, Apply: exactly ONE prompt for all
   three (journal shows one `farside-host-settings-helper` start); all three read back.
4. Change a Console field AND the access policy, Apply: exactly ONE prompt; the access policy save is silent (second
   helper start in the journal, no second dialog); both read back.
5. Within the keep window (about 5 minutes) change something again and Apply: ZERO prompts. After the window expires,
   the next Apply prompts once.
6. Cancel the dialog: nothing saved, every draft still pending, Apply still enabled, the error names the scope(s);
   a following Apply works.
7. Make one scope refused (edit `/etc/farside/virtual-host.conf` by hand after loading, so the revision is stale):
   the other scopes save, the refused one stays pending and is named in the red message on every page.
8. Reset after a pending change drops all drafts; Defaults on Console/Virtual/preferences keeps the certificate.
9. The restart message offers Restart Console / Restart Virtual; each asks for confirmation; nothing restarts silently.
10. If any step shows more than one prompt for a single Apply, stop and report: the fallback is a single combined
    helper (one more protocol change), not weaker validation.

## 8. Evidence and limits

- **Fresh captures** (`~/dev/rdp/evidence/2026-10-04-settings-ux-assessment/captures/`):
  - pages: `page-0..4`;
  - advanced scroll series: `advanced-1-*`, `advanced-2-*`, `advanced-4-*`;
  - overview Details: `overview-details*`;
  - dialogs: `stop-dialog`, `alias-dialog`;
  - 640 px: `narrow-0..2`.

  How they were produced:
  - `BrokerMainPageTest` (7/7 passed) on Buzz, with a copy of the current `src/kcm/ui` in
    `~/farside-ux-assessment-20261004/`;
  - test binaries and fixtures reused read-only from `~/farside-settings-consolidation/`;
  - offscreen, software renderer, private XDG dirs, no D-Bus daemon, no real profile.

  The other three page tests exited early (their compiled-in page paths point to Hal) and produced no
  images. The Main test covers all five pages.
- **Not verified:**
  - the module inside the real System Settings shell: the double footer and double title (findings 3 and 4)
    are inferred from the API and code, not captured;
  - the real polkit prompt count: it is inferred from the policy file (`auth_admin`, no `_keep`) and from one
    `pkexec` read per model;
  - the empty "Load…" state: it is shown by code (`visible: root.host.loaded`, no auto-load in `load()`), not
    by a capture, because the fixtures pre-load the models;
  - screen-reader behavior; dark theme; RTL.
- **Standards fetched (WCP, 2026-10-04):**
  - HIG [Simple by default](https://develop.kde.org/hig/simple_by_default/),
    [Layout and navigation](https://develop.kde.org/hig/layout_and_nav/),
    [Getting input](https://develop.kde.org/hig/getting_input/),
    [Displaying content](https://develop.kde.org/hig/displaying_content/),
    [Text and labels](https://develop.kde.org/hig/text_and_labels/),
    [Status changes](https://develop.kde.org/hig/status_changes/);
  - [Kirigami form layouts](https://develop.kde.org/docs/getting-started/kirigami/components-formlayouts/),
    [KCM development](https://develop.kde.org/docs/features/configuration/kcm/),
    [KQuickConfigModule API](https://api.kde.org/kquickconfigmodule.html),
    [AbstractKCM](https://api.kde.org/qml-org-kde-kcmutils-abstractkcm.html);
  - Qt [Best practices for QML and Qt Quick](https://doc.qt.io/qt-6/qtquick-bestpractices.html);
  - precedent: stock KRdp `origin/master:src/kcm/ui/main.qml`, which uses `ScrollViewKCM`, a header-action
    switch, header InlineMessages and the standard Apply/Default buttons.
- **Relation to earlier decisions:**
  - `settings-ui.md:54-56` chose "Global KCM Apply/Defaults do not authorize or repurpose system/account
    operations." This assessment recommends revisiting that choice (S0). The scoped footers are the main
    source of the button overload, and the safety goals (no silent restart, TLS preserved, outcome-unknown
    handling) do not depend on per-page Save buttons.
  - The 2026-10-01 "overview plus subpages" design solved save-scope safety but recreated a launcher. Design
    A keeps its safety contract and changes only where things live.

## 9. Verification results (2026-10-04, evidence-only pass; no product source, package or service changed)

Evidence: `~/dev/rdp/evidence/2026-10-04-settings-ux-verification/` (`captures/` named `<n>-<page>-<theme>-<dir>-<WxH>.png`,
`atspi/` full AT-SPI tree dumps, `logs/`, `harness/` scripts). Method: INSTALLED Buzz package (`farside-server 16f6303`),
real `systemsettings kcm_farside` inside a private `dbus-run-session` + `kwin_wayland --virtual` + private XDG dirs, never
the real display. Screenshots via `spectacle`; interaction and tree dumps via AT-SPI (`gi.Atspi`).

| # | Item | Result | Evidence |
|---|---|---|---|
| 1 | Module in the real System Settings shell | **PASS (findings 3 and 4 confirmed).** The title appears three times (window title, shell header, page title bar) on the root and twice on subpages (shell header "Console Settings" plus the page bar "Console Settings"). There is a custom `<` Back button in the page bar. No standard Apply / Reset / Defaults footer exists (`setButtons(Help)`, `kcmkrdpserver.cpp:21`); only the page-local footers and the bottom "Who Can Connect…/My Preferences…" buttons. | `captures/0-root-light-ltr-1000x700.png`, `1-console-light-ltr-1000x700.png` |
| 2 | Real polkit prompt count | **PASS, measured.** A logging-only polkit agent (own D-Bus agent, dismisses every request; polkitd confirms dismissals) was registered for the `systemsettings` process. Clicking through the installed KCM: Console = 1 prompt (`org.farside.hostsettings`), Virtual host = 1, Virtual "New desktop hardware" = 1, Who Can Connect = 1 (`org.farside.authentication`), My Preferences = 0. **Viewing everything = 4 prompts.** Overview "Details > Load Saved Settings…" adds 1 more per route. Multi-scope Save count is DERIVED, not measured (cannot authenticate): one `pkexec` per scope, up to 4 for Console + Virtual + hardware + Access. | `logs/polkit-count-summary.txt`; `captures/1-console-light-ltr-1000x700-after-dismissed-auth.png` |
| 3 | Empty first-open state | **PASS.** Every subpage opens with an intro plus one button, a disabled footer and nothing else (Console, Virtual, Access: "Load Administrator Settings…"; Preferences: "Load My Preferences"). Virtual opens with TWO Load buttons and two button groups (host and New desktop hardware). New defect: the overview Details text reads "Checked at . This is a snapshot…" (empty timestamp). | `captures/{1..4}-*-light-ltr-1000x700.png`, `atspi/atspi-0-root-light-ltr-1000x700.txt` (search "Checked at") |
| 4a | Dark theme | **PASS.** Breeze Dark applies and all five pages stay legible; disabled footer buttons are low contrast but readable. | `captures/*-dark-ltr-1000x700.png` |
| 4b | RTL (`-reverse`) | **PARTIAL.** Layout mirrors correctly, but the Back chevron in the page bar still points left, and the collapsed "Details" chevron points right. Both icons are not mirrored. (English strings inside an RTL layout show leading "..." bidi artefacts; expected without a translation.) | `captures/*-light-rtl-1000x700.png` |
| 4c | Accessibility | **PARTIAL.** AT-SPI exposes 1374 nodes. Among the KCM's checkboxes, radio buttons, combos, spin boxes, sliders and buttons, 0 have an empty accessible name; the only 5 unnamed buttons belong to the System Settings shell (hamburger, search clear, scrollbar-adjacent). Tooltip text is not exposed as a description (only 6 interactive nodes have one). Hidden (not-yet-loaded) subtree contains mislabelled duplicates (for example combo, slider and spin all named "Listen on", and "Fallback width" spin buttons inside the "Port" group). Whether the loaded state exposes the same mislabelled nodes could not be verified because loading needs authentication. No screen reader run. | `atspi/atspi-*-light-ltr-1000x700.txt` |
| 5 | Captures at 1000x700 and 640 wide | **PASS** (1000x700 all pages and themes; 640x700 root, Console, Access, Preferences). At 640 the Console footer clips the Save button (only "S" visible) and, on the overview, "Configure Virtual…" is below the fold; the Virtual page was not reached by the script (not clicked). | `captures/*-640x700.png` |
| 6 | Security facts | see below | |

Security facts (item 6):
- **6a Public read returns (host helper, `hostsettingshelper.cpp:186-218` plus `BrokerHostAdmin.cpp:31`):** `version`, `scope`,
  `revision` (SHA-256 of the whole config file), `values` (overrides), `defaults`, `effective` (all 25 host keys, including
  Address, Port, Certificate and CertificateKey paths, CameraLoopbackDevice, RenderPci), `tls` (state, fingerprint, algorithm,
  notBefore/notAfter, administratorManaged), `cameraLoopback` state, for the session scope `renderDevices` (PCI id, driver,
  render node path), `runtimeVerified`, `application`. Access helper (`BrokerAuthenticationAdmin.cpp:88`): `revision`,
  per route `pam.mode`, `pam.accounts` (local account NAMES), `credentials` (alias plus owner account name). No verifiers, no key bytes.
- **6b Files on Sol (read-only `stat`):** `/etc/farside` root:root 0700 (unreadable to users, contents not listed);
  `/etc/krdp` 0755, `console-host.conf`, `virtual-host.conf`, `virtual-session.conf` 0644, `virtual-test.crt` 0644,
  `virtual-test.key` 0600 root; `/opt/krdp-console/cert/krdp.crt` 0644, `krdp.key` 0600; user files `~/.config/farsideserverrc`
  and `krdpserverrc` 0600 westers. Both polkit policy files 0644 root.
- **6c Disclosure:** an unprivileged read would newly expose the allow-list and alias account names (who may log in remotely),
  certificate and key PATHS, device policy and GPU PCI/render nodes, TLS fingerprint and validity, and a change oracle via `revision`.
  Today the legacy `/etc/krdp/*.conf` host values are already world-readable on Sol, but the new `/etc/farside` tree is deliberately 0700.
  Decide per field; the minimum safe set is service status plus port and fingerprint, with account lists left behind `auth_admin`.
  Also: both helpers take an exclusive non-blocking `flock` (and create the lock file) even for `read`
  (`hostsettingshelper.cpp:328-331`, `authenticationhelper.cpp:72-77`), so a free-to-call read could cause "busy" failures of saves
  (denial of service) and writes to `/etc/farside`; a read action needs `LOCK_SH` or no lock.
- **6d Stdin validation:** strict in both. UID/argc checks (`hostsettingshelper.cpp:274-275`, `authenticationhelper.cpp:32-33`),
  10 s poll deadline plus size cap (`:287`, `:295`; auth `:55`), JSON object only, `version == 1`, operation whitelist, exact key
  count for reads (`:304-305`; auth `:64`), per-scope key whitelist, string-typed values, bounded PEM, 64-hex revision regex
  (`BrokerHostAdmin.cpp:50-80`). Gap: `authenticationhelper.cpp` does not disable core dumps or set `PR_SET_DUMPABLE`
  (the host helper does, `:277`) while it holds passwords in memory.
- **6e `allow_active` reach:** MEASURED on Sol: a process in an SSH session (seatless, remote) gets `auth_admin` for an
  `allow_active=yes` action, but a process started with `systemd-run --user --scope` from that same SSH login (cgroup
  `user@1000.service/app.slice`, no session) is AUTHORIZED, because polkit falls back to the uid's Display session (seat0 session 1286,
  active). So `allow_active` is granted to any same-uid process while that user has an active graphical login. The Virtual
  keeper session is `class=background` with no seat (`krdp-virtual-session.pam`); logind never makes it the user's Display session.
  A Virtual-only user (no physical login) should therefore resolve to no active session (prompt remains); a user who is ALSO on seat0
  would be treated as active. A real Virtual desktop was not created (needs an RDP connection), so this case is NOT verified.

Earlier claims: (a) "4 Load buttons" (S1) is wrong, there are 5 Load/unlock buttons (Console, Virtual host, Virtual hardware,
Access, Preferences) plus 2 Details loads; the 4-prompt statement is correct. (b) Section 8 said findings 3 and 4 were inferred;
they are now observed. (c) Item 1 in section 8 said the "Load…" state came from code only; it is now captured.
Side effect to note: an early run without `QT_QPA_PLATFORM` made systemsettings, spectacle and kactivitymanagerd abort and
apport wrote 3 `.crash` files (westers-owned); moved to `~/farside-ux-verify-20261004/crash-aside/` on Buzz (whoopsie is inactive,
nothing uploaded).

## Decision log

### 2026-10-04: settings-panel password (Steve: "follow KDE best practices on the password issue")

1. Viewing never prompts. The KCM opens populated, with no Load buttons. Host settings are read without privilege from files that are already 0644 (`/etc/krdp/*.conf`) plus a root-broker-written 0644 public-metadata snapshot (revision, values, defaults, effective keys, TLS fingerprint/validity, render device ids). A new polkit read action was REJECTED: the verification (section 9, item 6) showed an unprivileged read would also expose allowed accounts/aliases, certificate/key paths and a change oracle (`revision`), and both helpers take an exclusive lock even for reads, so free reads could block saves.
2. Account names/aliases and anything under `/etc/farside` (root 0700) stay behind admin authorization, loaded once when the user expands "Who can sign in", using `auth_admin_keep`.
3. Only Apply is privileged: standard System Settings Apply/Reset/Defaults, all dirty scopes batched into ONE helper invocation, polkit `auth_admin_keep` on `org.farside.hostsettings` and `org.farside.authentication`. Target: one prompt per Apply, none for viewing.
4. Follow-ups from verification, independent of layout A/B: `farside-authentication-helper` holds passwords in memory without disabling core dumps (slice S1b). Also new defects: overview "Checked at ." with an empty timestamp; Console footer Save clipped at 640 px; RTL chevrons (Back, Details) not mirrored.
5. Not decided: layout A vs B (awaiting Steve); slice S0 layout part and all slices not started. Unverified: a real Virtual-desktop user's polkit `allow_active` outcome (probably prompts, untested); screen-reader run; the multi-scope save prompt count is derived, not measured.

### 2026-10-04: layout (Steve)

Layout **B** (master-detail sidebar) chosen by Steve. Design A (dashboard root), its wireframe and section 6 are superseded. Sidebar rows: Console, Virtual, Who Can Connect, My Preferences; route rows carry a status subtitle and a trailing enable switch; `kcm.columnWidth` master-detail, drill-down at narrow width; no page may be a single control. Slice S4 rewritten accordingly. S5 onward are unchanged except where they refer to the dashboard root.
