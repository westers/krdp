# Sol: remaining installed settings acceptance (N04)

Server `65243df7`, package `6.6.80+git202610020049.65243df-1`.
October 1 read-only baseline: stored and running quality **80**, Console camera
**/dev/video10**, port **3391**, runtime **verified**. Existing package rollback
and helper checks are already accepted; do not repeat them.

## Manual check

Use Sol's System Settings → **Security & Privacy** → **Farside Remote Desktop**
→ Console **Host Settings**. If using server65243df7 or another build missing
the menu category, run `kcmshell6 kcm_farside` in a terminal inside Sol's desktop
to open the installed module directly.
Choose **Console Host**, then **Load Settings…**, using normal KDE administrator
authentication. No password needs to be shared in chat.

1. Stage the camera loopback field as `none`. Click **Save Settings…** and cancel
   the administrator dialog. The draft should remain `none`; **Inspect Running
   Host…** should still report `/dev/video10`. Saved settings must remain unchanged.
   If authorization is cached and no dialog appears, this does not test cancellation;
   wait for the normal authorization to expire before this step.
2. Restore the camera draft to `/dev/video10`. Change **Video quality** from **80**
   to **79**, then **Save Settings…** and authorize. Reload stored settings: quality
   should be **79**. **Inspect Running Host…** should still show **80**; saving
   alone must not restart Console.
3. Open **Console and Virtual Services**, explicitly **Restart…** Console when
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
