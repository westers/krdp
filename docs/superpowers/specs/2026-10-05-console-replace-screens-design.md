# Console "Replace" screens per connection (design amendment, 2026-10-05, OPT-060)

Status: approved by Steve 2026-10-05 (decisions D1 to D5 of
`docs/issues/2026-10-05-console-replace-screens-analysis.md`; D6: the first Hal test happens later, at the
desk on the LAN). This amends `2026-09-21-console-and-session-hosting-design.md` (Console Fit) and
`2026-09-22-remote-monitor-layout-design.md` (consent rule). Server slices S0, S1, S2 and S6 are implemented
with this document; S3 (lock guard), S4/S5 (client and settings UI) and S7/S8 (native matrix) follow.

## 1. Behaviour

Console connections have two ways to show the host's screens:

- **As they are** (default): the host's real monitors are captured one stream each. Nothing on the host changes.
- **Replace** ("use this computer's monitors and turn the host's screens off"): the broker asks the worker for
  KWin virtual outputs sized like the client's monitors (the shipped T06 configured-output path:
  `ConsoleVirtualOutputPlan`, `OutputRestoreJournal`, `PhysicalOutputGuard`), the host's real screens are
  switched off, and they come back when the connection ends or someone uses the host's keyboard or mouse.

Replace is chosen **per connection** and only for the Console route (:3391). Virtual (:3395) is unaffected;
there is no :3389 route.

## 2. Consent rule (settles the 2026-09-22 vs 2026-09-29 contradiction)

2026-09-22 said "connection alone never invokes physical mutation"; the 2026-09-29 inventory and T06 carry
replace-on-connect from a saved preference. Settled:

1. A saved, named, per-connection client choice is the explicit consent (D1). The client shows a banner and a
   one-click "turn the host's screens back on".
2. The host user's permission gates it: `ConsoleScreensPermission = off | ask` (D2, default `ask`, i.e. "When the
   connection asks"). `off` always means normal capture.
3. The per-connection request is the **standard RDP monitor block** in the client's connect data (D3; no wire
   change). A stock `/multimon` client gets the same treatment, which was OPT-041's purpose. A connection with
   no monitor block (`FreeRDP_MonitorCount == 0`) never triggers Replace.
4. Replace needs `Adapter::PhysicalUser`: the SDDM greeter and any non-user state always get plain capture.

Rule: `replace = permission == ask && monitorBlock && adapter == PhysicalUser && no earlier attempt on this
connection`.

Legacy keys: `MonitorMode=virtual` and `VirtualMonitorPolicy=replace` were the old opt-ins. They stay readable:
`VirtualMonitorPolicy` (`replace|extend`) and `VirtualMonitorLayout` still shape the plan (extend is an advanced
value), and a user who had either active choice maps to `ask`, which is also the default, so only an explicit
`ConsoleScreensPermission=off` disables it. `MonitorMode=virtual` is no longer a capture mode and falls back to
multi capture. The KCM replaces the old two controls with the one permission (S5).

## 3. Fit semantics per mode

- **Replace:** "Resize Host to This Screen" resizes the **owned virtual outputs** only (already built, `aec0fbbd`).
  It never changes a real monitor's mode.
- **As they are:** physical Fit (real mode change) remains, with OPT-059 per-output and desktop validation and a
  confirmation. It is refused or unavailable while Replace is active.
- Fit-all (change every real monitor's mode) is dropped (D5); parked on `parked/opt-059-fit-all` in both repos.

## 4. Restore and fail-open requirements

Console must always show a picture. Every unverified step ends in restore plus normal capture, never in a black
screen or a retry loop.

| Event | Requirement |
|---|---|
| Normal disconnect | Restore the snapshot, verify, remove outputs, drop the journal. |
| Client crash / VPN drop | Same, once RDP drops the connection. |
| Worker killed | The next worker replays the journal before capture. If the replay cannot verify, it still captures whatever outputs are lit, keeps the journal for the next attempt and does not Replace; it never exits black (S2c). |
| Broker crash | The worker loses its socket and restores. |
| Reboot / power loss | Virtual outputs vanish; KWin's per-set store relights the real set; the journal replays at the next worker. |
| Creation or replace fails | Restore, verify, then normal capture for the rest of the connection. **One Replace attempt per connection** (latch in the broker, S2b): no re-arm on the next worker Ready. |
| Lock near disconnect (OPT-049) | Out of scope here (S3). |
| Someone at the desk | Restore the screens, remote continues as extend, client is notified (D4). |
| Greeter / SDDM | Never Replace. |
| Client asks "turn screens back on" (S6) | Same path as local reclaim: restore, Replace ends for this connection, normal capture continues. |

## 5. KRDPCTL addition (S6, optional gap record)

Advertised as `capabilities.console.screens = {replace: true, restore: true}` (only when the broker would honour
the corresponding action). Record `console-screens {active, canRestore}` is pushed when Replace becomes
active/inactive. Request `console-screens-restore` (with `requestId`, echoed in its `result`) restores the host's
screens. Old clients ignore both. See `~/dev/rdp/KRDPCTL-V2-CONTRACT.md`.

## 6. Retirement gap record

The :3389 user unit was disabled fleet-wide on 2026-10-03 although inventory row 5 and T06/N17 required every
legacy behaviour to have a replacement and a migration test first. The behaviour (stand-in outputs, host screens
off, restore at disconnect) was lost without a decision. This work restores it inside Console. The safeguard is
`docs/retirement-checklist.md`; a UI label kept across routes must keep its meaning (Console Fit meant a real mode
change from 2026-09-21 while the label stayed "Resize Host to This Screen").

## 7. Not done here

Lock guard (S3), client and KCM UI (S4/S5), Sol installed matrix (S7), two-panel host (S8), first Hal test (S9).
Worker wire stays at version 13: policy and the monitor tuple already travel in it. The client notice for an
unverified recovery would need a worker warning record (a wire change) and is not part of S2.
