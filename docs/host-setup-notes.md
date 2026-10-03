# Host setup notes

## SDDM greeter environment (Console at the login screen)

Console at the SDDM greeter ("Console worker: screencast failed", seen on Buzz 2026-10-03)
needs the greeter's KWin (user `sddm`) to authorize `/usr/bin/farside-console-worker`
for `zkde_screencast_unstable_v1`. KWin looks the worker up in the `sddm` user's ksycoca
cache. Without `XDG_MENU_PREFIX=plasma-` in the greeter environment, `kbuildsycoca6`
cannot find `applications.menu` (only `plasma-applications.menu` exists) and indexes no
applications, so the lookup fails. A user's Plasma session sets the prefix itself.

What `farside-server` does (`packaging/farside/postinst`, function `farside_greeter_setting`):

- If `/etc/sddm.conf.d` exists, it generates `/etc/sddm.conf.d/30-farside-greeter.conf`
  with `[General]` and only `GreeterEnvironment=`. SDDM replaces a key's value rather than
  appending, so the value is the existing effective one (last `GreeterEnvironment=` seen)
  plus any missing `QT_WAYLAND_SHELL_INTEGRATION=layer-shell` and `XDG_MENU_PREFIX=plasma-`.
  Existing values of those two variables are kept. SDDM reads `*.conf` files in name order
  and later files win (not stated in `man sddm.conf`; from SDDM's behaviour and Sol's
  `10-wayland.conf`, which uses the same section and key).
- It is generated, not a packaged conffile, because SDDM is not a package dependency
  (headless Virtual-only hosts) and `/etc/sddm.conf.d` belongs to SDDM. A file of that name
  without the `# Managed by farside-server` marker is never touched; other packages'
  files are never edited. `purge` removes only the marked file.
- It rebuilds the `sddm` user's ksycoca cache once, best effort and never fatal:
  `runuser -u sddm -- env HOME=<sddm home> LANG=C.UTF-8 XDG_MENU_PREFIX=plasma-
  XDG_DATA_DIRS=/usr/local/share:/usr/share XDG_CONFIG_DIRS=/etc/xdg
  QT_QPA_PLATFORM=offscreen kbuildsycoca6 --noincremental`.
  The `env` call also unsets `SUDO_UID`, `SUDO_GID`, `SUDO_USER` and `SUDO_COMMAND`: under
  `sudo apt` dpkg scripts inherit them, and `kbuildsycoca6` then chowns the cache to
  `SUDO_UID` and writes nothing (seen on Buzz 2026-10-03).
- It never restarts sddm. The setting applies at the next greeter start: reboot, or
  `systemctl restart sddm` when nobody is logged in (this ends all sessions).

Verify: `grep -r GreeterEnvironment /etc/sddm.conf.d`, and
`strings -n4 -eb /var/lib/sddm/.cache/ksycoca6_en_* | grep -c consoleworker` (expect >= 1).
Contract check: `scripts/check-farside-greeter-setting.sh` (also run by `package-farside.sh`).
