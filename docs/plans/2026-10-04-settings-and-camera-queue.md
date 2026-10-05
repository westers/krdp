# Queue after OPT-057 (settings redesign) — 2026-10-04

Ordered. One item at a time, each ends with its check and a stop. Steve picks when each starts unless marked "running".
Source of items: Steve's live test of the Sol panel on 2026-10-04 and the agents' reviews.

| # | Item | Status | Done when | Notes |
|---|---|---|---|---|
| Q1 | Settings polish round 2: port shown as "3391" (no grouping), "Saved. Restart to use the new settings" clears after the restart, Custom preference defaults valid (no "Invalid preference" on switching to Custom) | DONE (2026-10-04; candidate `62f34e4`) | Suites at or above baseline, real-shell captures viewed, candidate deb archived (not installed) | Evidence `evidence/2026-10-04-settings-s8/` |
| Q2 | Install Q1 candidate on Sol | DONE (2026-10-04 23:40 CDT, Sol `62f34e4`) | Gated install (zero connections, apt simulation, holds, backup, rollback) | Same procedure as S7 |
| Q3 | (PENDING, Steve) Steve's manual checks from `evidence/2026-10-04-settings-s7/STEVE-CHECKLIST.md`: one prompt per Apply, zero on a second Apply in the keep window, at most one for Who Can Connect, restart flow | Steve | Results recorded in HANDOFF Log | Steve already confirmed layout, restart reconnect |
| Q4 | Camera/microphone state after login (needs-session code, device-availability push, client "waiting" state and auto-retry) | DONE 2026-10-05: server `2bbd793` + client 0.6.8 (`99a43bf`) installed Sol, Buzz, Hal; Steve's post-login live check pending (see issue doc) | Issue doc test plan T1-T8 pass, then Sol server + Buzz client live check | Paired wire change: server + client ship together; doc `docs/issues/2026-10-04-camera-state-after-login.md`. Client repo `~/dev/krdp-client`. Do not touch Hal live |
| Q5 | Settings layout tightening: Service block as one compact row, no duplicated status, side-by-side labels in the detail pane (or wider minimum pane), Virtual sidebar icon outline, elided Virtual subtitle, fingerprint wrap at 640 px | DONE 2026-10-05: `2bbd793d`, installed with Q4 (Steve's visual go pending) | Recaptured in real shell (light, dark, wide, narrow), reviewed against HIG | From S7 REVIEW.md items 1-6; Steve said he likes the settings now, so judge by his go |
| Q6 | Hal deployment of the new panel/server | DONE (2026-10-04 23:45 CDT, Hal `62f34e4`, no live test) | Zero RDP connections on Hal, Hal last in gated rollout | Hal still `a8c7d6f`; Steve also owes the AV1 test for candidate `16f6303`. Never test live on Hal without his OK |
| Q7 | Push the OPT-057 commits (`72b1b93f`..) to github master | DONE (2026-10-04 CDT, Steve approved) | Fast-forward push | Nothing pushed yet |
| Q8 | Housekeeping: HANDOFF entries for Q1/Q4, `research.md` OPT-057 line (file has other uncommitted edits, do not stage blindly), scratch dirs on Buzz (`~/farside-s*-20261004`, `~/farside-ux-*-20261004`) | queued | Old scratch moved aside, docs current | Safe-delete rules apply |

Not on this queue (separate decisions): the 19 baseline-failing test functions, wave 2/3 robustness (OPT-055), Sol WirePlumber wedge repair, core cleanup, cray disk, GPU-selection and performance items in the older plans.
