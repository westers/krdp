# OpenMW mouse capture over Farside RDP (OPT-054)

## Report and observed state

Steve reports Morrowind through OpenMW on Sol Console: he should be able to
move and look around, but cannot. He disconnected and deliberately left the
game running. The preceding connection was Hal (`192.168.48.93`) → Sol:3391.
Read-only inspection found OpenMW PID322976 and an Xwayland window with
`WM_CLASS=openmw`, `_NET_WM_PID=322976`, `_NET_WM_STATE_FOCUSED`.
No keys, mouse motion, game commands, screenshots or game configuration changes
were injected during diagnosis. Focus does not prove active gameplay versus a
paused/menu state, or the earlier client's keyboard focus.

Server logs contain absolute `Global pointer motion` positions reaching
`QPointF(1919,752)` on a 1920×1080 captured desktop. This demonstrates a finite
coordinate path, not continuous relative mouse input. No input drop/control
failure was established from the bounded log inspection.

## Confirmed implementation gaps

- Client `SessionView.qml` uses an ordinary MouseArea and explicitly allows the
  pointer to leave the window. `SessionModel::sendPointer()` maps/clamps movement
  into desktop coordinates. `SessionEngine` sends `freerdp_input_send_mouse_event`.
- Server `InputHandler::initialize()` has no `RelMouseEvent` callback.
- Plasma session pointer motion is injected through `pointer_motion_absolute`;
  the existing cursor-settle timer also injects absolute position nudges.
- Client pointer-position PDUs are deliberately ignored. They therefore do not
  provide a recentering fallback for applications that warp their pointer.
- The September 16 client design §5.5 excluded RDP relative mouse input; its
  September 17 amendment retained free pointer movement for normal desktop use.

Missing capture/relative motion is a concrete limitation for game mouse-look.
**Keyboard movement failure is not yet diagnosed.** It needs its own local
focus/key-delivery/game-state check; do not claim relative mouse support alone
fixes movement keys or infer an encoder/performance cause.

## Proposed implementation, as one task

1. Add an explicit **Capture Mouse** action for the active session view. Ordinary
   desktop use retains free pointer movement. Do not infer capture from cursor
   hiding, which terminal/text applications also use. Show a clear release hint
   using the existing host-key convention; preserve Escape for game menus.
2. Use native Wayland pointer-constraints and relative-pointer protocols on the
   client, following the existing ShortcutInhibitor lifecycle pattern. Bind
   native capability availability; prevent duplicate grabs, release on focus
   loss, modal UI, view removal, disconnect and unsupported protocol state.
   Handle fractional movement without per-event rounding loss; do not scale
   relative deltas through the video viewport or clamp them to monitor edges.
3. Use standard RDP relative-mouse capability negotiation and signed delta
   events, not a new private input channel. Advertise only routes that can
   deliver relative motion. Keep normal absolute input and unsupported routes
   explicit; no misleading successful capture against an unsupported server.
4. Preserve relative semantics through admission/control ownership, broker
   worker serialization and both Console/Virtual workers. Inject KWin relative
   `pointer_motion`; support portal relative motion where applicable or report
   unavailable. Never reinterpret a delta as an absolute coordinate. Suppress
   absolute cursor-settle nudges while game capture is active, with defined
   state reset when capture ends. Reject stale workers/owners and preserve
   existing pressed-key/button cleanup.
5. Verify keyboard focus and WASD key down/up delivery separately. Release all
   held keys/buttons when capture or the session ends. Capture must not consume
   movement keys, game Escape, or leave the host key held.

FreeRDP **3.22.0**, already pinned on Buzz, implements relative event send/receive
and the input capability bit. No FreeRDP package upgrade is needed just to add
this path. Sources:
[relative events](https://github.com/FreeRDP/FreeRDP/blob/3.22.0/libfreerdp/core/input.c),
[input capability negotiation](https://github.com/FreeRDP/FreeRDP/blob/3.22.0/libfreerdp/core/capabilities.c).
KDE's installed `fake-input.xml` has separate relative `pointer_motion` and
absolute `pointer_motion_absolute` requests.

## Done means

- Buzz → Sol Console OpenMW: hold W to move, release W to stop; turn repeatedly
  through 360° in both directions without an edge stop, jump or persistent spin.
- Game buttons and Escape/menu behavior work; the host release action restores
  ordinary local pointer movement immediately. Focus loss, dialogs and reconnect
  leave no grab or held input behind. Ordinary multi-window desktop pointer
  movement and drag behavior remain intact.
- Console and Virtual share the relative input path; capability-unavailable
  cases have an accurate explanation. Game capture is explicitly selected.
- One focused delta/ownership/lifecycle gate and one native Buzz→Sol game check
  suffice; reuse accepted codec/camera/TLS evidence. Steve checks Hal manually.
- Paired client/server packages are delivered with zero incoming RDP before
  restarts, preservation of the running game/configuration/desktop, and rollback.
  User confirms game behavior; no gameplay acceptance from packet receipt alone.

This is a scoped implementation proposal, not a new Goal, automatic queue run,
or a claim that gaming support is already implemented.
