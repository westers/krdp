# Farside settings consolidation (step B) — design

**Status:** approved in conversation by Steve on 2026-10-02; written spec awaiting review.
**Scope:** presentation and navigation of the Farside KCM (`src/kcm/ui/Broker*.qml`) only.
Step C (per-row inheritance cleanup in My Preferences) is a separate later spec.

## Intent

Steve likes the layout in the approved sketches (`docs/design/farside-settings-proposal.svg`,
`farside-settings-security-proposal.svg`) but the shipped native pages diverge: forms are narrow
and left-aligned with large empty areas, headings are weak, and ten pages sit behind eight
launcher buttons, several of them nearly empty. Success: fewer, fuller pages that look like the
sketches, with every current option still reachable and no change to save/draft/authorization
behavior.

## Non-goals

No new settings, backend, schema or model-semantics changes. No change to authorization,
revisions, TLS-draft isolation, scoped saves, or the no-polling/static-editor rules in
`docs/settings-ui.md`. No inheritance-control rework (step C). No Hal testing; no deployment
before Steve accepts populated renders.

## Navigation (10 pages → 5)

```
Overview ── Console   (Advanced + Certificate inline)
         ├─ Virtual   (Advanced + Certificate + New-desktop hardware inline)
         ├─ Who Can Connect
         └─ My Preferences
```

Removed as pages: Console/Virtual Certificate, New Desktop Hardware, Console/Virtual Service
Details. Their content moves inline as below. Back navigation, scoped footers and the
Add Remote Login / Stop / Restart dialogs are kept.

## Components

**Overview** (`BrokerMainPage.qml`): bounded full-width card per service (Console, Virtual):
purpose line, status, *Allow connections* and *Start when computer boots* switches (immediate
actions), saved address with Copy (plus a separate *Running address* row only when startup
inspection verifies a different endpoint; never invent a connectable host from a wildcard bind),
actions *Configure…*, *Restart…*, *Stop…* (existing confirmation dialogs) and an expandable
*Details* row containing the read-only inspection and *Inspect Running Host…* (replaces
`BrokerServiceDetailsPage`/`BrokerServicesPage`). Below the cards: *Who Can Connect…* and
*My Preferences…*, each showing a pending-changes marker when its draft is dirty.

**Host pages** (`BrokerHostsPage.qml`, Console and Virtual): one bounded column, one persistent
twin `FormLayout`, controls filling the column, stronger section headings, `smallSpacing` within
and `largeSpacing` between groups. Groups: Connection (listen address, port, Certificate summary
row), Video, Sound and devices, collapsed Advanced. Camera readiness is a single-sentence
InlineMessage with the repair action. A status line ("Unsaved changes · Console only") sits above
the existing fixed scoped footer.

**Certificate inline section** (replaces `BrokerCertificatePage.qml`): the Connection group shows
status, expiry and a selectable fingerprint. A *Certificate…* disclosure expands the four-way
source choice and import fields in place (not in a modal, so only one native file dialog is ever
open). It keeps the existing local C++ draft: section Cancel discards only the TLS edits; *Use
Certificate Changes* stages only TLS fields into the host draft. Host Save/Restore Defaults/Revert
semantics are unchanged (ordinary defaults still preserve TLS).

**New Desktop Hardware inline section** (Virtual page only, replaces its page): GPU grants and
VA-API policy for newly created desktops, keeping its own *Save Desktop Defaults* action and an
explicit scope label; the footer states which scope each action saves.

**Who Can Connect / My Preferences**: same layout treatment (bounded column, headings, spacing,
status line); no logic changes. The "Use host setting" dropdowns stay until step C.

## Constraints carried over

- Native QQC2/Kirigami (`org.kde.desktop`), `i18n`/`i18nc`, KDE units/fonts; no custom theme.
- Do not constrain a form's `Layout.maximumWidth` to its parent layout width (size-negotiation
  cycle). One shared desktop/narrow threshold; 640 px narrow case must keep footers usable.
- Static editors; no Repeater bound to the changing services list; no timers; drafts, focus and
  unfinished text survive service events and navigation. Avoid transient repeated forms linked as twins.
- Inline expanders must not destroy draft editors when collapsed (hide, don't unload).

## Testing and acceptance

- Update page-name/route expectations in `BrokerMainPageTest`, the Hosts/Preferences/SignIn
  fixtures and `KcmUiTest`; remove tests for deleted pages only after their behavior is covered inline.
- Add one check: cancelling the inline certificate draft preserves other pending host edits and a
  previously staged TLS choice.
- Run only the affected suites; no daemon-starting tests on Hal.
- Capture populated renders from the packaged QML on Buzz for: overview (collapsed/expanded
  Details), Console, Virtual (with inline hardware and certificate expanded), Who Can Connect,
  My Preferences, the dialogs, and the 640 px narrow case. Lay them beside the sketches in a
  comparison gallery; record theme/fixture differences.
- Acceptance = Steve's visual review of that gallery. Installed authorization and deployment remain
  separate gated steps (Sol/Buzz only unless Steve says otherwise).

## Delivery

One implementation plan, executed by a Sonnet agent with a fixed Definition of Done, ending at the
gallery for review. Step C is specified separately after that review.
