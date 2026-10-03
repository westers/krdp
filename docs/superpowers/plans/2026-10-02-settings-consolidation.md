# Farside settings consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Steve's standing rule: implementation runs in Sonnet subagents (`model: "sonnet"`), thread name starts `Sonnet5 high - …`, brief includes Goal/Context/Scope/Steps/Definition of Done/Stop conditions/Safety/Report format.

**Goal:** Reduce the Farside KCM from 10 pages to 5 (Overview, Console, Virtual, Who Can Connect, My Preferences) by moving Certificate, New Desktop Hardware and Service Details inline, and restyle the pages to match the approved sketches.

**Architecture:** `BrokerMainPage.qml` keeps its `StackLayout` of persistent pages but drops pages 5–9. The three removed page types become embeddable `ColumnLayout` sections (`BrokerCertificateSection`, `BrokerHardwareSection`, `BrokerServiceDetails`) hosted by the page that owns their state. C++ models are untouched.

**Tech Stack:** Qt 6.10 QML (QQC2, `org.kde.kirigami`, `org.kde.kcmutils`), QtTest/ECM autotests in `autotests/`, offscreen software rendering on Buzz.

**Spec:** `docs/superpowers/specs/2026-10-02-settings-consolidation-design.md` (approved by Steve 2026-10-02). Also read `docs/settings-ui.md` (conventions) and `docs/design/farside-settings-proposal.svg`, `farside-settings-security-proposal.svg` (target look).

## Global Constraints

- Native QQC2/Kirigami (`org.kde.desktop` style), `i18n`/`i18nc` strings, KDE units/fonts/palette; no custom theme, no `qsTr`.
- Do not set a form's `Layout.maximumWidth` from its parent layout's current width (size-negotiation cycle).
- One shared desktop/narrow threshold: `wideMode: width >= Kirigami.Units.gridUnit * 32`; 640 px width must keep fixed footers visible.
- Static editors only: no Repeater bound to the changing `administration.services` list, no timers, no model reconstruction on status events.
- Collapsing an expander must **hide, not unload**: use `visible`, never `Loader`/`active:false`, for any section containing a draft editor.
- Persistent twin `FormLayout`s; avoid transient repeated forms linked as twins.
- `BrokerCertificateSection` keeps the existing local C++ draft (`host.certificateDraft`, `beginCertificateEdit()`, `cancelCertificateEdit()`, `stageCertificateEdit()`); ordinary Restore Defaults still preserves TLS.
- Only one native `Dialogs.FileDialog` may be open at a time (no file dialog from inside a modal).
- Scope of change: `src/kcm/ui/*.qml`, `autotests/Broker*PageTest.cpp`, `autotests/CMakeLists.txt`, docs. No C++ model/schema/backend change. No deployment. No Hal. No `git push` (Steve decides).
- Tests and renders run on **Buzz** in a scratch tree, never on Hal (Hal = this machine, `hal9000`; no daemon-starting tests there). Use `ssh -o BatchMode=yes buzz.local`; never put the `~/dev/rdp/.env` password in argv or logs.
- Commit style: `OPT-053 <imperative subject>` + body; end with the attribution lines from the session reminder.

## Review Focus

1. Cancelling the inline certificate draft after other host edits (Port changed) must keep those edits and any previously staged TLS choice → Task 3 test.
2. Collapsing then re-expanding Advanced, Certificate or Service Details must keep unfinished field text, focus-independent draft values and `host.modified` → Task 2/3 tests.
3. Virtual page has two save scopes: editing hardware must enable only *Save Desktop Defaults*, editing host fields only *Save Virtual Settings*; Revert/Restore Defaults must act on their own scope → Task 4 test.
4. A service status event (active→inactive) while the Details expander is open must not recreate its editors or move scroll/geometry → Task 2 test.
5. Leaving the host page (Back) with the certificate section open must cancel the certificate draft (as the old page's Back did) and leave all other drafts untouched → Task 3 test.

---

### Task 1: Scratch tree and green baseline on Buzz

**Files:** none in repo (scratch only).

**Interfaces:** Produces: a reusable build tree `~/farside-settings-consolidation/` on Buzz and the command lines used.

- [ ] **Step 1:** Read `~/dev/rdp/evidence/2026-10-02-settings-design-comparison/{capture.log,rebuild.log,package.log}` and `.../2026-10-01-settings-implementation/SUMMARY.md` to see how the earlier Buzz scratch build/test run was configured (cmake flags, `QT_QPA_PLATFORM=offscreen`, private session bus wrapper, `TMPDIR`). Reproduce, don't invent.
- [ ] **Step 2:** `rsync -a --exclude build --exclude .git --exclude 'kpipewire_*' ~/dev/krdp/ buzz.local:farside-settings-consolidation/src/` (target is a scratch dir created by you; no `--delete` outside it). Configure and build only the targets needed: `BrokerMainPageTest BrokerHostsPageTest BrokerPreferencesPageTest BrokerSignInPageTest BrokerServicesPageTest KcmUiTest`.
- [ ] **Step 3:** Run them: `ctest -R 'BrokerMainPageTest|BrokerHostsPageTest|BrokerPreferencesPageTest|BrokerSignInPageTest|BrokerServicesPageTest|KcmUiTest' --output-on-failure`. Expected: all PASS (record counts). If any fails before edits, stop and report.
- [ ] **Step 4:** Write a helper script on Hal `/tmp/claude-1000/-home-westers-dev-krdp/3b19fcdd-dbe1-4ce6-9c71-98926f83a70c/scratchpad/sync-test.sh` (outside the repo) that rsyncs `src/kcm`, `autotests` to Buzz and rebuilds/runs those tests; later tasks reuse it. No commit.

---

### Task 2: Service details inline in the overview cards

**Files:**
- Create: `src/kcm/ui/BrokerServiceDetails.qml` (content of `BrokerServiceDetailsPage.qml` lines 297–427, as a `ColumnLayout`, no `SimpleKCM`, no title)
- Delete: `src/kcm/ui/BrokerServiceDetailsPage.qml`
- Modify: `src/kcm/ui/BrokerMainPage.qml` (overview cards, `pages`, remove ids `consoleDetails`/`virtualDetails`)
- Modify: `autotests/BrokerHostsPageTest.cpp:57` (component URL), `autotests/BrokerMainPageTest.cpp` (page indices 8/9)

**Interfaces:**
- Produces: `BrokerServiceDetails { required property var host; required property var administration; required property string route; property var navigation; property string hostName }` — exposes the same `objectName`s as before (`inspectHostRuntime`, `hostRuntimeSummary`, `hostRuntimeVerification`, `hostRuntimeStale`, `hostRuntimeMissing`, `hostRuntimeDifferences`, `showHostRuntimeValues`, `runtime_<key>`).
- Consumes: `BrokerServiceControls` (unchanged API: `administration, route, host, navigation, hostName, showDetailsToggle, showBoot, detailsVisible`).

- [ ] **Step 1: Failing test.** In `BrokerMainPageTest::navigationDraftsAndNativePresentation` add after the existing overview checks:

```cpp
// Details live in the overview card and are hidden until expanded.
QObject *detailsButton = item(u"consoleDetailsToggle"_s);
QVERIFY(detailsButton);
QObject *inspect = item(u"inspectHostRuntime"_s);   // first match = console card
QVERIFY(inspect);
QVERIFY(!inspect->property("visible").toBool());
QVERIFY(QMetaObject::invokeMethod(detailsButton, "clicked"));
QVERIFY(inspect->property("visible").toBool());
QCOMPARE(page->property("currentPage").toInt(), 0);   // no page change
```
Remove the two checks that open page 8/9 (`consoleServiceDetails`, `virtualServiceDetails`).
- [ ] **Step 2:** Run `sync-test.sh BrokerMainPageTest`. Expected FAIL (`consoleDetailsToggle` not found).
- [ ] **Step 3: Implement.** Create `BrokerServiceDetails.qml` by moving the body of the old page into a `ColumnLayout { id: root; … }` keeping every function (`fieldNames`, `runtimeState`, `runtimeReason`) and object name. In the overview, replace each `Flow { Configure…, Service Details… }` with:

```qml
Flow {
    Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
    QQC2.Button { objectName: "configureConsole"; text: root.consoleHost.modified ? i18nc("@action:button", "Configure Console… (unsaved)") : i18nc("@action:button", "Configure Console…"); onClicked: root.openPage(1) }
    QQC2.Button { objectName: "consoleRestart"; text: i18nc("@action:button", "Restart…"); enabled: root.administration.services[0].canRestart; onClicked: consoleControls.request("restart") }
    QQC2.Button { objectName: "consoleStop"; text: i18nc("@action:button", "Stop…"); enabled: root.administration.services[0].canStop; onClicked: consoleControls.request("stop") }
    QQC2.Button { objectName: "consoleDetailsToggle"; flat: true; icon.name: consoleDetailsBox.visible ? "arrow-down" : "arrow-right"; text: i18nc("@action:button", "Details"); onClicked: consoleDetailsBox.visible = !consoleDetailsBox.visible }
}
BrokerServiceDetails { id: consoleDetailsBox; visible: false; Layout.fillWidth: true; host: root.consoleHost; administration: root.administration; route: "console"; navigation: root; hostName: root.hostName }
```
Give the existing `BrokerServiceControls` `id: consoleControls` (and `virtualControls`). `BrokerServiceControls` already owns the confirmation `PromptDialog`; make `request()` callable (it is a plain function). Set `showBoot: true` on the controls and remove the duplicate switch/status rows from the details component (it no longer embeds `BrokerServiceControls`). Mirror for Virtual (`virtualRestart`, `virtualStop`, `virtualDetailsToggle`, `virtualDetailsBox`). Delete page entries 8 and 9 from `pages` and the old components; update `BrokerHostsPageTest.cpp:57` to load `BrokerServiceDetails.qml` with `host`/`administration`/`route` properties set.
- [ ] **Step 4:** Run the six tests. Expected PASS.
- [ ] **Step 5 (Review Focus 2, 4):** Add to the same test: type `"3999"` into the Console host Port editor, expand then collapse Details, assert `console.modified()` and the field text unchanged; then drive a fixture service state change (`administration` fixture `activeState` active→inactive via the existing fixture helper used by the services tests) with Details open and assert the `inspectHostRuntime` item pointer is the same object and `scrollY` is unchanged. Run; PASS.
- [ ] **Step 6:** `git add` the changed files; commit `OPT-053 Move service details into overview cards`.

---

### Task 3: Certificate inline in the host pages

**Files:**
- Create: `src/kcm/ui/BrokerCertificateSection.qml` (body of `BrokerCertificatePage.qml` lines 179–281 minus `SimpleKCM`, minus title/footer; stage/cancel buttons inline at the section bottom)
- Delete: `src/kcm/ui/BrokerCertificatePage.qml`
- Modify: `src/kcm/ui/BrokerHostsPage.qml` (Connection group), `src/kcm/ui/BrokerMainPage.qml` (drop pages 5/6, `openPage` special cases, `goBack` cancel hooks move)
- Modify: `autotests/BrokerMainPageTest.cpp:84-90,121-137`, `autotests/BrokerHostsPageTest.cpp:174`

**Interfaces:**
- Produces: `BrokerCertificateSection { required property var host; signal staged(); signal cancelled() }` with `objectName`s `certificateKeep/Existing/Standard/Import`, `selectHostCertificate`, `selectHostPrivateKey`, `inspectHostImport`, `hostImportPreview`, `cancelCertificateEdit`, `stageCertificateEdit`; property `bool open`.
- Consumes: `host.certificateDraft`, `host.beginCertificateEdit(): bool`, `host.cancelCertificateEdit()`, `host.stageCertificateEdit(): bool`, `host.metadata.tls`, `host.metadata.effective`.
- Host page: `function leave()` cancels the certificate draft when the page is left; `BrokerMainPage.goBack()` calls `consolePage.leave()`/`virtualPage.leave()` for the page being left.

- [ ] **Step 1: Failing tests (Review Focus 1, 5).** In `BrokerMainPageTest` replace the page-5 navigation block with:

```cpp
// Inline certificate section: cancel preserves other host edits.
QVERIFY(QMetaObject::invokeMethod(find(consolePage, u"editHostCertificate"_s), "clicked"));
QCOMPARE(page->property("currentPage").toInt(), 1);               // stays on the host page
QObject *standard = item(u"certificateStandard"_s);
QVERIFY(standard && standard->property("visible").toBool());
QVERIFY(QMetaObject::invokeMethod(item(u"certificateStandard"_s), "clicked"));
QVERIFY(QMetaObject::invokeMethod(item(u"cancelCertificateEdit"_s), "clicked"));
QCOMPARE(console.values(), before);                               // Port edit from earlier in the test kept
QCOMPARE(console.tlsMode(), u"keep"_s);
// Stage keeps unrelated edits and sets TLS mode.
QVERIFY(QMetaObject::invokeMethod(find(consolePage, u"editHostCertificate"_s), "clicked"));
QVERIFY(QMetaObject::invokeMethod(item(u"certificateStandard"_s), "clicked"));
QVERIFY(QMetaObject::invokeMethod(item(u"stageCertificateEdit"_s), "clicked"));
QCOMPARE(console.tlsMode(), u"standard"_s);
QCOMPARE(console.values()[u"Port"_s].toString(), u"3401"_s);
// Leaving the page cancels an open, unstaged certificate draft only.
QVERIFY(QMetaObject::invokeMethod(find(consolePage, u"editHostCertificate"_s), "clicked"));
QVERIFY(QMetaObject::invokeMethod(item(u"certificateKeep"_s), "clicked"));
QVERIFY(QMetaObject::invokeMethod(item(u"settingsBack"_s), "clicked"));
QCOMPARE(page->property("currentPage").toInt(), 0);
QCOMPARE(console.tlsMode(), u"standard"_s);                       // earlier staged choice kept
QCOMPARE(console.values()[u"Port"_s].toString(), u"3401"_s);
```
(Adjust variable names to the surrounding test; keep its existing setup that sets Port 3401.) Update the capture loop (`:121-137`) to drop indices 5–9 and instead expand the section before the Console/Virtual screenshots.
- [ ] **Step 2:** Run `BrokerMainPageTest`. Expected FAIL (still opens page 5).
- [ ] **Step 3: Implement.** `BrokerCertificateSection.qml`:

```qml
ColumnLayout {
    id: root
    required property var host
    property bool open: false
    signal staged()
    signal cancelled()
    readonly property var draft: host.certificateDraft
    readonly property var certificate: host.metadata.tls || ({})
    property url certificateFile
    property url privateKeyFile
    property int selectionGeneration: 0
    spacing: Kirigami.Units.smallSpacing
    function begin() { if (host.beginCertificateEdit()) { ++selectionGeneration; certificateFile = ""; privateKeyFile = ""; open = true; } }
    function cancel() { host.cancelCertificateEdit(); ++selectionGeneration; certificateFile = ""; privateKeyFile = ""; open = false; cancelled(); }
    // … certificateState(), saved-certificate FormLayout, source RadioButtons, import form and FileDialogs copied verbatim …
    // The expanded body is wrapped in a ColumnLayout with `visible: root.open` (never a Loader).
    RowLayout {
        visible: root.open
        QQC2.Button { objectName: "cancelCertificateEdit"; text: i18nc("@action:button", "Cancel"); onClicked: root.cancel() }
        Item { Layout.fillWidth: true }
        QQC2.Button { objectName: "stageCertificateEdit"; highlighted: true; text: i18nc("@action:button", "Use Certificate Changes"); enabled: root.draft.canStageCertificate && !root.host.busy && !root.host.outcomeUnknown; onClicked: { if (root.host.stageCertificateEdit()) { root.open = false; root.staged(); } } }
    }
}
```
In `BrokerHostsPage.qml` Connection group: keep the Certificate summary row (status text + `editHostCertificate` button, now `onClicked: certificateSection.begin()` and shown only when `!certificateSection.open`), add `BrokerCertificateSection { id: certificateSection; host: root.host; visible: root.host.loaded && !root.session }` under the form, and add `function leave() { if (certificateSection.open) certificateSection.cancel(); }`. In `BrokerMainPage.qml`, `goBack()` becomes: `if (currentPage === 1) consolePage.leave(); if (currentPage === 2) virtualPage.leave();` before popping history; remove `openPage` branches for 5/6 and the two certificate page entries. Update `BrokerHostsPageTest.cpp:174` to instantiate `BrokerCertificateSection.qml` with `host` and call `begin()`.
- [ ] **Step 4:** Run the six tests. Expected PASS.
- [ ] **Step 5 (Review Focus 2):** Add a check that collapsing via Cancel then re-opening shows `certificateKeep` checked, and that toggling Advanced open/closed does not change `console.modified()`.
- [ ] **Step 6:** Commit `OPT-053 Edit certificates inline on host pages`.

---

### Task 4: New Desktop Hardware inline in the Virtual page

**Files:**
- Create: `src/kcm/ui/BrokerHardwareSection.qml` (the `root.session` branches of `BrokerHostsPage.qml`: lines 85–124 session parts, the PCI checkbox list, `RenderPci`/`VaapiDriver` fields)
- Modify: `src/kcm/ui/BrokerHostsPage.qml` (remove `fixedScope === 2` handling: `session`, `title`, `host` ternary, footer branches; add section at bottom of Virtual page), `src/kcm/ui/BrokerMainPage.qml` (drop `hardwarePage`, index 7)
- Modify: `autotests/BrokerHostsPageTest.cpp` (tests using `fixedScope: 2`), `autotests/BrokerMainPageTest.cpp:151-171`

**Interfaces:**
- Produces: `BrokerHardwareSection { required property var settings /* kcm.virtualSessionSettings */; property var navigation }` with own `RowLayout` of `Restore Defaults`, `Revert Changes`, `Save Desktop Defaults…` (`objectName`s `defaultDesktopHardware`, `discardDesktopHardware`, `saveDesktopHardware`), a scope label "Applies to newly created desktops only", and its own `hostSavedNotice`-style InlineMessage (`desktopHardwareSavedNotice`).
- Consumes: `settings.{loaded,busy,modified,canSave,values,metadata.renderDevices,definitions,error,applicationRequired,setValue,defaults,discard,save,reload}`; `BrokerSettingField`.

- [ ] **Step 1: Failing test (Review Focus 3).** In `BrokerMainPageTest` after opening the Virtual page with both models loaded:

```cpp
QObject *saveHost = find(virtualPage, u"saveHostSettings"_s);
QObject *saveHw = find(virtualPage, u"saveDesktopHardware"_s);
QVERIFY(saveHost && saveHw);
QVERIFY(!saveHost->property("enabled").toBool());
QVERIFY(!saveHw->property("enabled").toBool());
session.setValue(u"VaapiDriver"_s, u"radeonsi"_s);                 // hardware edit
QVERIFY(saveHw->property("enabled").toBool());
QVERIFY(!saveHost->property("enabled").toBool());
QVERIFY(QMetaObject::invokeMethod(find(virtualPage, u"discardDesktopHardware"_s), "clicked"));
QVERIFY(!session.modified());
virtualHost.setValue(u"Port"_s, u"3402"_s);                         // host edit
QVERIFY(saveHost->property("enabled").toBool());
QVERIFY(!saveHw->property("enabled").toBool());
```
(Use the test's existing `session`/`virtualHost` fixture variable names.)
- [ ] **Step 2:** Run. Expected FAIL (`saveDesktopHardware` not found).
- [ ] **Step 3: Implement.** Move the session-only markup into `BrokerHardwareSection.qml`, replacing `root.host` with `root.settings`, `root.session` with `true`. Put the section at the bottom of the Virtual host page under a `Kirigami.Heading { level: 3; text: i18nc("@title:group", "New desktop hardware") }` and a scope label, with `visible: root.fixedScope === 1`. Strip `fixedScope === 2` and `session` from `BrokerHostsPage.qml` (property, title, footer text, `hostField` visibility, summary strings); `showPciEditor` moves into the section. Remove `hardwarePage` and index 7 from `BrokerMainPage.qml`; `pages` is now `[overview, consolePage, virtualPage, accessPage, preferencesPage]`. The Virtual footer's summary label shows `Unsaved changes · Virtual only` / `· New desktop defaults` per dirty scope. Update tests that used scope 2 to drive the section instead.
- [ ] **Step 4:** Run the six tests. Expected PASS.
- [ ] **Step 5:** Commit `OPT-053 Show new-desktop hardware inline on the Virtual page`.

---

### Task 5: Layout pass to match the sketches

**Files:**
- Modify: `src/kcm/ui/BrokerMainPage.qml`, `BrokerHostsPage.qml`, `BrokerSignInPage.qml`, `BrokerPreferencesPage.qml`, `BrokerSettingField.qml`, `BrokerServiceControls.qml`
- Test: existing page tests (footer/width checks at `BrokerMainPageTest.cpp:155-171`), plus screenshots in Task 6

**Interfaces:** Consumes the sections from Tasks 2–4; produces no new API.

Changes (each a visible rule from the spec; verify by render, not just tests):
- [ ] **Step 1:** Content column: replace `Layout.maximumWidth: gridUnit * 38` with `Layout.maximumWidth: Kirigami.Units.gridUnit * 48`, `Layout.fillWidth: true`, left aligned; headings `Kirigami.Heading { level: 2 }` for group titles (service names, Connection, Video…), `level: 4` descriptive text uses `Kirigami.Theme.disabledTextColor` only for secondary help.
- [ ] **Step 2:** Controls fill the form column: in `BrokerSettingField.qml` give ComboBoxes/TextFields/SpinBoxes `Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24` (as in the sketch input width) and the quality slider `Layout.fillWidth: true`; confirm with a 900 px and 640 px render that no horizontal scroll appears.
- [ ] **Step 3:** Overview cards: each service in a `Kirigami.AbstractCard`-free plain group (no decorative cards per spec text "plain full-width group"): heading, one purpose line, switches/status/address form, button row, Details box. Keep `Kirigami.Separator` between services. Keep the existing Copy button; Running address row only when verified.
- [ ] **Step 4:** Spacing: `smallSpacing` inside a group, `largeSpacing` between groups; remove one-off `Layout.topMargin`s.
- [ ] **Step 5:** Status line: add a `QQC2.Label` above each page footer ("Unsaved changes · Console only", "· Virtual only", "· access policy only", "· your account only") bound to the existing `modified`/`tlsMode` state (reuse `hostPendingSummary` text; do not add C++).
- [ ] **Step 6:** Camera banner: shorten messages to one sentence + action, e.g. "Camera: setup required — configure a V4L2 loopback device in Advanced."; type `Kirigami.MessageType.Warning` for setup-required/unavailable, `Positive` for available, `Information` for not-checked. Keep the staged-change and Virtual-unavailable branches.
- [ ] **Step 7:** Who Can Connect / My Preferences: apply Steps 1, 2, 4, 5 only. No logic or label changes beyond heading level.
- [ ] **Step 8:** Run all six tests (width/footer checks must pass at 640 px). Expected PASS.
- [ ] **Step 9:** Commit `OPT-053 Restyle settings pages to the approved layout`.

---

### Task 6: Renders, comparison gallery, docs — then stop for review

**Files:**
- Create: `~/dev/rdp/evidence/2026-10-02-settings-consolidation/` (captures, `build-gallery.py` adapted from `.../2026-10-02-settings-design-comparison/build-gallery.py`, `comparison.html`, `SUMMARY.md`)
- Modify: `docs/settings-ui.md` (replace page-list statements with the 5-page structure), `docs/design/2026-10-01-farside-settings-ux.md` (status line pointing at the consolidation spec), `~/dev/rdp/CLAUDE.md` ("Fixed" line, dated) and `~/dev/rdp/HANDOFF-CODEX.md` Log (newest first)

- [ ] **Step 1:** Using the Task 1 helper, capture on Buzz from the tested QML (offscreen, software, populated fixtures — same method as the earlier gallery): Overview (Details collapsed and expanded), Console (default, Advanced + Certificate expanded, import state), Virtual (hardware and certificate expanded), Who Can Connect (+ alias dialog), My Preferences (+ Advanced), Stop dialog, and 640 px narrow Overview/Console/Virtual.
- [ ] **Step 2:** Build `comparison.html` placing each capture beside the matching sketch crop. Record theme/fixture differences honestly (fixture images, software rendering, not a live desktop).
- [ ] **Step 3:** Update docs listed above; write `SUMMARY.md` with exactly what passed (test counts per suite), what was not exercised (real administrator prompt, live Sol, installed package, Hal), and the gallery path.
- [ ] **Step 4:** Confirm `git status` shows only intended files; commit docs `OPT-053 Record settings consolidation evidence`.
- [ ] **Step 5:** **Stop.** Report to Steve with the gallery path. Do not package, install, push, or start step C.

---

## Self-review notes

- Spec coverage: navigation 10→5 (Tasks 2–4); overview cards with Copy/Running address/Details/pending markers (Tasks 2, 5; pending markers already exist on the bottom buttons and are kept); host page groups/camera banner/status line (Task 5); inline certificate with local draft (Task 3); inline hardware with its own scope (Task 4); constraints carried over (Global Constraints); testing/acceptance gallery (Task 6); non-goals respected.
- `BrokerServicesPage.qml` (and `BrokerServicesPageTest`) is not part of the main page and is out of scope; leave it untouched.
- Names used across tasks: `BrokerServiceDetails`, `BrokerCertificateSection` (`begin()`, `cancel()`, `open`, `staged`, `cancelled`), `BrokerHardwareSection`, `leave()` on the host page, object names listed in each task's Interfaces.
