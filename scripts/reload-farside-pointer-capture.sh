#!/bin/bash
# Reload our bridge without restarting KWin. QPluginLoader keeps old libraries
# resident: an immutable, content-versioned filename is required after upgrade.
set -euo pipefail
[[ $(id -u) == 0 ]]
desktop_uid=${1:?desktop UID required}
[[ $desktop_uid =~ ^[0-9]+$ && $desktop_uid != 0 ]]
desktop_user=$(getent passwd "$desktop_uid" | cut -d: -f1)
[[ -n $desktop_user ]]
idle() { [[ -z $(ss -Htn state established '( sport = :3389 or sport = :3391 or sport = :3395 )') ]]; }
idle || { echo 'Incoming RDP active; refusing bridge reload.'; exit 4; }
query() { runuser -u "$desktop_user" -- env -u LD_PRELOAD XDG_RUNTIME_DIR="/run/user/$desktop_uid" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$desktop_uid/bus" qdbus6 "$@"; }
source=/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/qt6/plugins/kwin/plugins/farside-pointer-capture.so
[[ -f $source && ! -L $source && $(stat -c %u "$source") == 0 ]]
digest=$(sha256sum "$source" | cut -d' ' -f1)
plugin="farside-pointer-capture-${digest:0:12}"
target="${source%/*}/$plugin.so"
if [[ -e $target || -L $target ]]; then
 [[ -f $target && ! -L $target && $(stat -c %u "$target") == 0 ]]
 cmp "$source" "$target"
else
 install -o root -g root -m 0644 "$source" "$target"
fi
[[ $(sha256sum "$target" | cut -d' ' -f1) == "$digest" ]]
loaded=$(query org.kde.KWin /Plugins org.kde.KWin.Plugins.LoadedPlugins)
before='{}'
if grep -Eq '^farside-pointer-capture(-[0-9a-f]{12})?$' <<< "$loaded"; then
 before=$(query org.kde.KWin /org/kde/KWin/FarsidePointerCapture org.farside.PointerCapture1.Snapshot)
 python3 -c 'import json,sys; assert not json.loads(sys.argv[1])["leased"]' "$before"
fi
idle || { echo 'Incoming RDP active; refusing bridge reload.'; exit 4; }
while IFS= read -r name; do
 if [[ $name =~ ^farside-pointer-capture(-[0-9a-f]{12})?$ ]]; then
  query org.kde.KWin /Plugins org.kde.KWin.Plugins.UnloadPlugin "$name"
 fi
done <<< "$loaded"
[[ $(query org.kde.KWin /Plugins org.kde.KWin.Plugins.LoadPlugin "$plugin") == true ]]
after=$(query org.kde.KWin /org/kde/KWin/FarsidePointerCapture org.farside.PointerCapture1.Snapshot)
python3 -c 'import json,sys; a=json.loads(sys.argv[1]); assert a["supported"] and not a["leased"]; b=json.loads(sys.argv[2]); assert not b or a["epoch"]!=b["epoch"]' "$after" "$before"
# Verify the running compositor maps this exact file, not a cached old library.
python3 - "$desktop_uid" "$target" <<'PY'
import pathlib,sys
uid=int(sys.argv[1]); target=pathlib.Path(sys.argv[2]); inode=target.stat().st_ino
found=False
for proc in pathlib.Path('/proc').iterdir():
    if not proc.name.isdecimal(): continue
    try:
        if proc.stat().st_uid!=uid or (proc/'comm').read_text().strip()!='kwin_wayland': continue
        matches=[line for line in (proc/'maps').read_text().splitlines() if str(target) in line]
        if matches:
            assert all(int(line.split()[4])==inode and '(deleted)' not in line for line in matches)
            found=True
    except (FileNotFoundError,ProcessLookupError): pass
assert found, 'Compositor did not map the installed bridge'
print('Verified active compositor maps the content-versioned installed bridge.')
PY
printf 'Loaded bridge: %s\nSnapshot: %s\n' "$plugin" "$after"
