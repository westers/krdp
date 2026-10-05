# Console Fit on a two-monitor host refused: "exceeds capture limits" (2026-10-05)

Status: fix implemented in source (OPT-059, local commit, see the end). Not installed, not live-tested.

## Symptom

Steve, remote on VPN, Buzz (one 1920x1080 monitor) to Hal Console :3391 (two 2560x1440 monitors, 5120x1440 workspace).
Using "Resize Host to This Screen" (Fit) he reports an error that the screen size exceeds encoder limits.

## Evidence

- The refusal text that exists in the code is `resized physical workspace exceeds capture limits`
  (`server/ConsoleResize.h:226-228`). The client shows it as `Could not resize the host: <message>`
  (`krdp-client/src/qml/SessionWindow.qml:109-114`, message stored in `SessionModelControl.cpp:826-827`).
  This is almost certainly what Steve paraphrased. No other "encoder limit" string exists in server, src,
  client or KPipeWire.
- The check (`ConsoleResize.h:195-228`) builds the union of all enabled outputs, with the target output
  at its new pixel size and every other output at its current size and position, and refuses when
  `workspace.width * maxScale > 4096` or the same for height. Positions are preserved, so on Hal the
  union is always at least 2560 + 2560 = 5120 wide, whichever single output is resized, even to 1920x1080.
  Fit on Hal can therefore never pass this check.
- The check runs in the legacy console-resize path (`ConsoleHostController.cpp:1433-1520`, executor
  `ConsoleResizeExecutor.cpp:73`) before any mutation. The visual-topology path is behind
  `KRDP_EXPERIMENTAL_CONSOLE_TOPOLOGY` (default OFF) and is not what Fit uses here.
- The client sends one request per host monitor (`AppLayout.cpp:556`).
- Hal journal (server `e9231e8`, today): the Console connection itself is healthy with the same
  workspace. 17:29:26 `Encoder: codec hevc in hardware`, `Using PipeWire encoder for hevc : 5 backend hardware`,
  `Reset graphics desktop QSize(5120, 1440) with 2 monitor(s): "0,0 2560x1440 primary; 2560,0 2560x1440"`;
  17:34:31 `Retained KScreen readback confirmed 2 independent captured outputs`,
  `Console capture outputs: 2 forwarding true clients 1`. So Console already captures per output
  (two 2560x1440 HEVC hardware encoders, AMD VAAPI renderD128) and sends a 5120 RDPGFX desktop. No encoder
  limit is hit on connect. The 4096 H.264 limit is not hit either (HEVC/AV1 accept up to 8192).
- The journal holds no Fit request or refusal: refusals are only replied to the client, never logged by
  the host. The exact error text on screen is therefore inferred from code, not observed.
  Buzz was not reachable by name (`buzz.local` did not resolve from Hal), so its client log was not read.
- Unrelated earlier lines today: 16:31:12 `Encoder failed ... libx264 5120x1440 Cannot allocate memory`
  (software H.264 of the whole 5120 workspace in an earlier session), and 17:25:42 first worker start
  failed (`screencast failed`, `radeonsi ... init failed`) then recovered at 17:25:54. Worth a separate look.

## Root cause (confidence: high for the code path, medium that this is the exact message Steve saw)

The legacy Fit planner still assumes "the physical worker encodes one workspace surface" (comment at
`ConsoleResize.h:195`, commit `c15aa6cf`, 2026-09-21) and applies the single-surface 4096 limit to the whole workspace
union. Since then the Console worker moved to independent per-output capture and HEVC/AV1, whose real
limits are 4096 per output and 8192 for the RDP atlas (`ConsoleTopologyPlan.h:206`,
`ClientDisplayInfo.h` MaxDimension 4096, MaxDesktopDimension 8192). The check is stale, not an encoder limit.
The message also does not mention the codec or tell the user what to do.

## Regression?

Not a regression from the recent installs. The check is unchanged since 2026-09-21. It is a long-standing
policy gap that surfaces now because this is the first Fit from a one-monitor client to a two-monitor Hal.
Not exercised in the evidence read (Sol test hosts have one console output).

## Workaround facts

- Fit cannot succeed on Hal with the current check, so I could not confirm that it works and expect
  it does not. A workaround that passes the check would have to shrink the union below 4096, for example
  both outputs to a combined width of at most 4096 with positions repacked, which Fit does not do.
  Do not rely on it.
- Any successful Fit changes Hal's physical monitor layout (mode/scale of a real output on his work desktop)
  through `kscreen-doctor`. Restore is recorded in the output-restore journal and run on release or crash,
  but windows on that desktop get rearranged while it is applied and when it is restored.

## Fix options (ranked)

1. Replace the single-workspace check with per-output and atlas limits for the console-resize path:
   each output within 4096 (HEVC/AV1) and union within 8192, H.264 only when the negotiated codec requires it.
   Owner: server (`ConsoleResize.h`, plus a test next to the existing ConsoleResize tests). Size: small.
   Risk: low to medium (more layouts reach real KScreen mutation, so the restore journal path matters).
   Test: pure unit cases for 2x2560, 2560+1920, 3x2560 (refuse over 8192), then Buzz client to Sol Console
   with a private two-output compositor; never Hal.
2. Make Fit multi-output aware: when the client has fewer screens than the host, offer
   "Show one host monitor" (client view selection of a per-output surface, no host change) instead of
   changing the physical layout. Owner: client UX plus server. Size: medium. Risk: low for the host.
   This best matches "just works" on a work desktop.
3. Auto-downscale or auto-Fit with consent. Owner: both. Size: large. Risk: highest (changes real monitors).
4. Minimum: change the message to something actionable, naming the real limit and an alternative, and log
   refusals on the host. Owner: server and client. Size: tiny. Risk: none.

Suggest 4 now, 1 next, and 2 as the product answer for two-monitor hosts.

## Tell Steve now

- Do not use Fit on Hal for now; it will be refused and gives him no benefit.
- Hal Console already streams both monitors (5120x1440, HEVC hardware). In the client, use Scaled view
  or the per-monitor view selection to see one monitor on Buzz.
- Tell us the exact text of the error if it differs from `resized physical workspace exceeds capture limits`.

## Addendum: the per-output encoder already exists and is the default (coordinator question)

1. Steve is right. The Console worker captures per output and runs one encoder per screen (commit `bfdc951`,
   wire v10, T06 capture checkpoint). `MonitorCapturePolicy` (`server/MonitorCapturePolicy.h:14-15`) has
   modes Multi, Workspace, Primary, Specific, and the default is `Mode::Multi`.
   `ConsoleHostController::capturePolicy()` (`ConsoleHostController.cpp:2000-2005`) returns that default
   unless the controlling client's codec bridge sets another mode. Same idea as `MonitorMode=multi` from OPT-018.
2. Hal's Console runs multi, not the aggregate workspace. It is not behind `KRDP_EXPERIMENTAL_CONSOLE_TOPOLOGY`
   or `KRDP_EXPERIMENTAL_CONSOLE_VIRTUAL` or a setting; those flags only gate topology/virtual-output writes.
   Hal's journal confirms it: "Console capture outputs: 2" and "Retained KScreen readback confirmed
   2 independent captured outputs", two HEVC hardware encoders, RDPGFX desktop 5120x1440. I did not find a
   selection that skipped multi for Buzz: the single-monitor client got multi too (17:29 and 17:34
   sessions); Buzz simply views a 5120x1440 desktop in a 1920x1080 window. Nothing needs "selecting".
3. Yes, the fix is policy and validation, not a new encoder. The legacy `console-resize` planner
   (`ConsoleResize.h:195-228`) was written for the earlier single-workspace worker and was never updated to
   the per-output reality, so it refuses Fit on a union over 4096. Fix: validate per output (4096) and
   atlas (8192) there (option 1), and for one-monitor clients prefer viewing a single host monitor over
   changing the physical layout (option 2). No encoder or capture-mode change is required.

## Fix implemented (OPT-059, 2026-10-05)

What changed (validation and messaging only; Fit still applies the same physical lease with consent and restore):

- `server/ConsoleResize.h`: the single-surface union check (`workspace * maxScale > 4096`) is replaced by
  two limits, now named constants: `MaxOutputDimension = 4096` (per output; hardware H.264 stops there,
  and a 4096 output pads to at most 4096 in every codec, so no codec-specific padding term is needed) and
  `MaxDesktopDimension = 8192` (whole RDPGFX desktop). They equal `ClientDisplayInfo.h` MaxDimension /
  MaxDesktopDimension and `ConsoleTopologyPlan.h:206`; no conflicting constants were found. The desktop
  extent is still logical union times the largest scale, as before. Positions are preserved and overlap is
  still refused (so growing DP-1 past x=2560 on Hal is refused as overlap, correctly).
- Refusal text now names the limit and output: "Output DP-1 would be 4400x2400, which is over the 4096-pixel
  limit of the video encoder for this connection" and "The desktop would be WxH after resizing X, which is
  over the 8192-pixel limit of the remote desktop; resize or move another output first".
- `server/ConsoleHostController.cpp/.h`: every console-resize refusal (broker pre-checks, worker plan
  errors via `finishResize`, physical-topology resize replies) is logged with `qWarning` ("Console resize
  refused: ...") through a member `LogThrottle` (3 lines per 10 s, suppressed count appended). The broker also
  rejects an oversized request with the specific encoder-limit message instead of "invalid physical resize request".
- Client: no change needed. It shows the server's `message` unmodified (`SessionWindow.qml:109`);
  the wire allows up to 1024 characters.

Tests (`autotests/ConsoleResizeTest.cpp`, 12 -> 19 test rows, all pass): Hal layout fit to 1920x1080 and
1280x720 on both outputs, HDMI-A-1 to 3840x2160, DP-1 growing over its neighbour refused as overlap,
unchanged 5120-wide union accepted (regression), scale-only change, three monitors (7680 wide accepted),
four monitors (10240 refused, message checked), 4400x2400 output refused with the exact message and 4096x2160
accepted, desktop beyond 8192 refused. One existing case (`rejectsOverlapAndOversizedDesktop`, was
`...Workspace`) encoded the old limit with a neighbour at x=4000; it now uses x=7000 (past 8192).
ConsoleResizeExecutorTest and ConsoleResizeSessionTest pass unchanged.

Not verified: no live Fit on Hal, Sol or Buzz; the controller log lines are not compiled in the old main
`build/` tree (its private KPipeWire headers are stale, `PipeWireCursor::visible` missing, so
ConsoleHostControllerTest cannot be rebuilt there); they compile in the package build. The
`KRDP_EXPERIMENTAL_CONSOLE_TOPOLOGY` Fit path (default OFF) still reports a generic "conflicts with the
verified layout" for plan failures. Fit from a one-monitor client to a two-monitor host still changes the
physical layout of one output; "show one host monitor" (option 2) remains the better product answer.

### Live-check steps for Steve

1. Sol server + Buzz client first (Sol has one console output, so this covers the unchanged path and the
   refusal text): install the candidate on Sol only when nobody is connected, connect Buzz, try Fit to a size
   above 4096 is not offerable from the client, so just confirm a normal Fit still works and restores on
   disconnect. `journalctl -u krdp-console-host | grep "Console resize refused"` shows any refusal.
2. A two-output check needs a two-output Console host; use a Sol private compositor with two outputs or a
   cray/ace host only with Steve's go. Expect Fit on one output to apply and release restore both.
3. Hal only with Steve's explicit go, nobody else connected and a 2-minute restore plan: before Fit note
   `kscreen-doctor -o`; Fit from Buzz; on any trouble disconnect the client (the restore journal restores
   on release) or run `kscreen-doctor output.DP-1.mode.<id> output.HDMI-A-1.mode.<id>` from Hal's own desktop;
   `krdpserver --restore-outputs` is for the old user service only.
