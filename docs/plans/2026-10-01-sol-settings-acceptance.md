# Sol: remaining installed settings acceptance (N04)

Server `ed92e059`, package `6.6.80+git202610020550.ed92e05-1`.
October 2 installed readback: stored and running quality **80**, Console camera
**/dev/video10**, port **3391**, runtime **verified**. Existing package rollback
and helper checks are already accepted; do not repeat them.

## Manual check

This package uses an overview with persistent scoped subpages and retains the
event-driven service refresh. Existing accepted camera and
rollback checks need not be repeated.

Use Sol's System Settings → **Security & Privacy** → **Farside Remote Desktop**
→ **Configure Console…** → **Load Administrator Settings…**, using normal KDE administrator
authentication. No password needs to be shared in chat. Image quality is in **Video**; camera bridge is in **Advanced Options**.
Go **Back** to the overview and open Console **Service Details…** for explicit
restart and **Inspect Running Host…**. Returning to configuration preserves drafts. Saving uses the fixed footer's
**Save Console Settings…** button.

1. Stage the camera loopback field as `none`. Click **Save Console Settings…** and cancel
   the administrator dialog. The draft should remain `none`; **Inspect Running
   Host…** should still report `/dev/video10`. Saved settings must remain unchanged.
   If authorization is cached and no dialog appears, this does not test cancellation;
   wait for the normal authorization to expire before this step.
2. Restore the camera draft to `/dev/video10`. Change **Image quality** from **80**
   to **79**, then **Save Console Settings…** and authorize. Reload stored settings: quality
   should be **79**. **Inspect Running Host…** should still show **80**; saving
   alone must not restart Console.
3. Go **Back** to the overview and open Console **Service Details…**, explicitly **Restart…** when
   ready for the connection to disconnect. Reconnect to Sol, reload Console settings,
   and **Inspect Running Host…**: startup quality should now be **79** and agree
   with stored settings. Leave Virtual running.
4. Restore quality **80**, save, explicitly restart Console, reconnect and inspect:
   stored/startup quality **80**, camera **/dev/video10**, runtime agreement.

Report which step passed or the exact error. Cancellation without a write,
authorized save without automatic restart, explicit restart/readback and restoration
complete this remaining N04 check. Keep other settings unchanged.

Automation previously reached the normal dialog but could not enter its password
via accessibility. Root helper success does not replace this manual acceptance.
