# Settings captures compared with the approved design

Reviewed on 2026-10-02 after Steve requested explicit screen captures and a
comparison with the approved sketches. See the standalone gallery at
`~/dev/rdp/evidence/2026-10-02-settings-design-comparison/comparison.html`.
It embeds the references, all ten native pages, dialogs, advanced sections,
certificate import states, narrow hardware page and selected baseline captures.

## Method

The initial captures load the exact stripped KCM module from the Sol-deployed
`a087e159` package. Final revised captures load the exact stripped module from
`ed92e059` / `6.6.80+git202610020550.ed92e05-1` on Buzz.
Both use native KDE controls, a private D-Bus/XDG environment, offscreen software
rendering and populated bounded fixtures. They do not capture Hal's work display
or reveal saved passwords, actual private keys, or private host configuration.
The final packaged main-page capture check (3 cases) and KCM suite (14 cases)
both pass without QML warnings.

The SVGs are layout sketches with example values. Native fonts, theme spacing,
button shapes and switches are expected to differ. These captures show the KCM
content at 900×850 without the surrounding System Settings navigation.

## Comparison and corrections

| Page/state | Comparison with approved design | Correction/outcome |
|---|---|---|
| Overview | Service grids were centered too far from section headings; Access/Preferences links scrolled with content | Leading grids and fixed bottom navigation toolbar |
| Console/Virtual | Correct Connection → Video → Sound/device order; opening Advanced also expanded help throughout basic fields | Help stays scoped to the relevant advanced fields; basic rows remain compact |
| Access | Missing “Remote logins” headings and an empty state | Both services now have the heading; empty lists explain that no remote logins are added |
| Remote-login dialog | Generic title, stacked fields and overflowing account guidance | Scoped Console/Virtual title, native wide form when space allows, wrapped guidance and content padding |
| My Preferences | Correct Video → conditional Console displays → Sound/session order; Advanced changed basic help density | Scoped advanced help; inherited fallback inputs remain hidden until Custom is chosen |
| Console/Virtual Certificate | Four sources and isolated staging matched; successful import lacked the sketch's explicit readiness message | Positive matching-pair message; only certificate metadata is displayed |
| Service details | Inspected wildcard bindings produced a blank Running address row | Explicit “All interfaces · port …”, consistent with the saved binding; no invented connectable address |
| Primary actions | Sketch distinguishes the scoped save/stage action | Native `highlighted` property emphasizes enabled Save/Stage actions |
| New Desktop Hardware | Dedicated page matches companion design's ownership and scope | Inventory/grants and independent save retained; narrow page scrolls with footer accessible |
| Confirmation | Native prompt names the affected service and warns of disconnection | No service operation occurs when dismissed |

All ten pages, both matching-import states, the login/stop dialogs, every
captured advanced section and the narrow hardware render were visually inspected.
The corrected main-page capture/navigation check and compiled KCM suite pass
(3 and 14 cases respectively); earlier backend/draft tests remain valid because
this follow-up changes presentation only.

## Remaining acceptance

Normal installed administrator authorization, Save and explicit Restart require
the existing Sol manual checklist. Steve's visual approval remains separate.
No live camera, codec, network or performance result is inferred from fixtures.

## Delivery state

Corrected source `ed92e059` is committed and pushed. Package `6.6.80+git202610020550.ed92e05-1`
is built and archived read-only under `~/dev/rdp/debs/candidates/server/ed92e05/`
(SHA-256 `e1691038174882cc305e7608bf5a1f99840609fa3a725153bdfbc1aae03f7942`). It is staged on Sol
with the exact prior server/client packages and guarded deployment script in
`/tmp/farside-settings-comparison-release/`. **Not installed:** Sol has an active
Console RDP session. Its installed server remains `a087e159`; all clients and
other hosts remain unchanged. Do not restart the active session.
