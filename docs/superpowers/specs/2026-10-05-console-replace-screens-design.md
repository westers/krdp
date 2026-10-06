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

## 8. S7 amendment (2026-10-06): one-monitor clients, greeter name, picture after a worker replacement

The S7 native matrix (`~/dev/rdp/evidence/2026-10-06-replace-screens-s7/SUMMARY.md`) found D0 to D3. Worker wire
stays at version 13 for all of them.

### 8.1 D0: a client with ONE monitor cannot send the standard block

FreeRDP's client writes TS_UD_CS_MONITOR only when `MonitorCount > 1` (`gcc_write_client_monitor_data`), so a
laptop alone (Buzz, Hal's own client) sends no block and the section 2 gate (permission AND monitor block) never
opened, although the client UI offered "use this computer's monitors". The earlier comment "a one-monitor block
is still a block" in `ConsoleVirtualOutputPolicy.h` was wrong and is corrected there.

**Design as built.** The permission rule, the one attempt per connection, the greeter exclusion and the fail-open
restore are unchanged. The per-connection request now has two equal sources:

1. the standard monitor block (multi-monitor stock clients, `/multimon`; standard RDP first, D3 stays approved);
2. an explicit KRDPCTL request for the gap, `console-screens-request` (contract (h)), sent by the own client:
   `{"type":"console-screens-request","v":1,"requestId":"..","replace":true,"monitors":[{x,y,width,height,primary?}]}`,
   1 to 16 monitors in the client's physical pixels, each 640 to 4096 per side, coordinates within +-32767,
   several monitors need exactly one primary, no overlap and a union within 8192. The reply is
   `{"type":"console-screens-request","v":1,"ok":bool,"message"?,"requestId":"<echo>"}`: `ok:true` only means the
   request was taken; what happens is reported by the existing `console-screens` records.

Advertised as `capabilities.console.screens.request = true`, sent only with the rest of the group (permission
other than Off). An old server has no such flag, so the client does not send and shows a one-line note.

`ConsoleVirtualOutputPolicy::effectiveRequest(block, request)` returns the block when it has monitors (standard RDP
wins and a request is then answered ok with no effect), else the request as a client display (desktop size = the
union, one monitor = a single output of its size), else the connect info unchanged. The gate, the plan and the
worker policy are fed with that value, so a request is exactly a block.

**Timing and races.** KRDPCTL opens at authentication and `capabilities` are the first record, so the request can
only arrive after them; by then the worker usually is already capturing normally for that connection. Three cases,
all through existing paths and no new worker message:

- worker ready and the connection in control (the normal case): the request sets the policy
  (`updateClientDisplayPolicy`, the bridge sends the new `consoleVirtual` config) and arms the lease
  (`armConfiguredConsoleOutputs`) in the same event-loop turn, before any worker reply can be handled; the
  worker's `setEncoderConfig` already starts Replace when the policy flips from disabled to enabled with no plan
  (the same path as a policy change, tested natively in `consoleReplaceRelock`'s fixture);
- not yet in control or no worker yet (a viewer, or a worker restart): the request is stored on the connection and
  the next grant (`syncControlState`) or worker Ready (`armConfiguredConsoleOutputs`) arms it, like a block;
- after the one attempt was used (`replaceAttempted`): a different request is refused with a message
  ("came too late"); the same request is answered ok and does nothing; Replace never re-arms.

Frames the worker produced before the arm are held back by the broker (`m_layoutAwaitingReadback`) until the new
outputs are published, as for a block at connect.

**Alternatives rejected.**
- *RDPEDISP DisplayControl monitor layout*: ambiguous. The client sends a layout after connect for smart sizing and
  window resizes too, in Console "as they are" as well; the server cannot tell "replace my screens" from "my window
  got bigger", and Console Fit semantics (host mode change) differ.
- *Forcing `MonitorCount 2`*: a fake second monitor corrupts the client's real layout and the server's output plan.
- *Patching FreeRDP* so one monitor writes a block: our client links the distro's FreeRDP (3.22 vs 3.31 problem
  already pinned); a fork per fleet host for a gap record is worse than one optional KRDPCTL record.
- *Server guess from `desktopSize != host size`*: any client with a window smaller than the host would turn the
  host's screens off. Consent has to be explicit (D1).

### 8.2 D1: greeter name truncation

`/proc/PID/comm` holds at most 15 characters (`kscreenlocker_g`), so the S3 liveness check against the 19
character name never matched: every release of a locked session concluded "greeter gone", re-locked three times
and logged "could not be re-armed" (~5 s added to the worker exit and to a broker stop). Fixed in
`ConsoleReleaseLock::greeterRunning`: comm matched at the kernel length, argv[0] of cmdline confirms the real name,
only this user's process counts. Native (Sol private fixture, real scan over a fake /proc with the truncated comm):
3 re-lock calls and worker exit at 7.5 to 8.5 s after the release started became 0 calls and 3.0 s.

### 8.3 D2: no picture for ~15 s after a mid-connection worker replacement

Cause (code reading and the row 5 timeline, reproduced in `ConsoleHostControllerTest`): the client still holds the
two indexed RDPGFX surfaces of the ended Replace (`Client::wireLayout`). The replacement worker captures one
output; `outputsReceived` did not start a topology readback (not multi, not a 2->1 transition for the broker, which
had forgotten the old outputs), so `m_topologyAvailable` stayed false and the frame filter rule
`desired.isEmpty() && !m_topologyAvailable && !wireLayout.isEmpty()` skipped every frame. The picture came only when
an unrelated topology answer (Sol: about when the harness used the client window) made the topology available.
The worker itself delivers its first frame on a static screen in ~0.6 s (native probe `consoleFreshWorkerFirstFrame`,
avc420 then hevc as the broker does: 0.2 to 0.6 s), so the KPipeWire/KWin damage hypothesis is not the cause.
Fix: a client that holds surfaces and a single new output count as an independent transition, so the broker reads
the topology back and republishes the layout and a keyframe like after a 2->1 change. The live 15 s on Sol was not
reproduced natively (needs a real Replace, a worker kill and a client with two surfaces); re-run S7 row 5 on the
candidate.

### 8.4 D3: the worker exits and is respawned after every Replace

By design (`finishConsoleCreatorRelease`: after a configured release the worker exits, "replacement proves restored
capture before the next grant"), and the broker treats any worker exit as a failure with a 1000 ms backoff
("failed; retrying in 1000 ms", once per cycle). Measured per cycle (S7 row 2 logs): disconnect, outputs back at
+1.2 s, lease release verified +2.4 s, (before D1) lock check until +7.4 s, worker exit, +1 s backoff, new worker
ready +1.7 s later: a connection in the ~10 s after a disconnect found no worker; with D1 fixed the window is about
5.7 s. A connection with no worker is admitted, waits, and is armed at the next Ready with its own fresh attempt
(unit test `reconnectWhileTheWorkerIsBeingReplacedGetsItsOwnReplaceAttempt`; also first connect after a broker restart,
S7 row 6). Not changed (redesign): keeping the worker alive after the release, or not backing off after a clean exit 0,
would shorten the window; both are proposals for a later item.
