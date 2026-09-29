#!/usr/bin/env bash
# FARSIDE-VLOCK: a virtual desktop's lock screen cannot authenticate (no_new_privs
# blocks the setgid unix_chkpwd), so the launcher puts immutable lock-off config in
# the desktop's XDG_CONFIG_DIRS. Check that it wins over a retained profile that
# turned locking on, and that the launcher installs it.
# Usage: VirtualSessionLockConfigTest.sh <kreadconfig6> <locked-config dir> <launch-virtual-session.sh>
set -euo pipefail
kreadconfig=$1 locked=$2 launcher=$3
scratch=$(mktemp -d)
trap 'rm -rf -- "${scratch:?}"' EXIT
mkdir -p "$scratch/profile" "$scratch/empty"
printf '[Daemon]\nAutolock=true\nLock=true\nLockOnResume=true\nLockOnStart=true\n' >"$scratch/profile/kscreenlockerrc"
printf '[KDE Action Restrictions]\naction/lock_screen=true\n' >"$scratch/profile/kdeglobals"

read_key() { # <config dirs> <file> <group> <key>
    env -i LANG=C.UTF-8 HOME="$scratch" XDG_CONFIG_HOME="$scratch/profile" XDG_CONFIG_DIRS="$1" \
        "$kreadconfig" --file "$2" --group "$3" --key "$4"
}
expect() { # <config dirs> <expected> <file> <group> <key>
    local got
    got=$(read_key "$1" "$3" "$4" "$5")
    if [[ $got != "$2" ]]; then
        echo "FAIL: $3 [$4] $5 = '$got' with XDG_CONFIG_DIRS=$1, expected '$2'" >&2
        exit 1
    fi
}
# Control: without the locked config the profile's settings are what KConfig reads.
expect "$scratch/empty" true kscreenlockerrc Daemon Autolock
expect "$scratch/empty" true kdeglobals 'KDE Action Restrictions' action/lock_screen
# With it, the immutable groups override the retained profile.
for key in Autolock Lock LockOnResume LockOnStart; do
    expect "$locked" false kscreenlockerrc Daemon "$key"
done
expect "$locked" false kdeglobals 'KDE Action Restrictions' action/lock_screen
# The launcher requires the files and copies them into the desktop's config dirs.
grep -q 'locked-config/kscreenlockerrc locked-config/kdeglobals' "$launcher"
grep -q 'cp "$support/locked-config/kscreenlockerrc" "$support/locked-config/kdeglobals" "$runtime/config-defaults/"' "$launcher"
grep -q 'XDG_CONFIG_DIRS="$runtime/config-defaults"' "$launcher"
echo 'Lock screen is off and immutable inside virtual desktops'
