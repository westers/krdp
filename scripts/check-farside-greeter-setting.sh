#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
# Contract check for the SDDM greeter setting shipped by farside-server (see
# docs/host-setup-notes.md). Runs the real postinst function against a scratch root;
# touches no host path. Used by package-farside.sh and runnable on its own.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
pi=$root/packaging/farside/postinst
po=$root/packaging/farside/postrm
fail() { echo "greeter setting contract FAILED: $*" >&2; exit 1; }

sh -n "$pi" && sh -n "$po" || fail "maintainer script syntax"
grep -q '^# BEGIN farside-greeter-setting$' "$pi" || fail "postinst has no greeter setting function"
grep -q '30-farside-greeter.conf' "$po" || fail "postrm does not remove the greeter drop-in on purge"
grep -q 'kbuildsycoca6 --noincremental' "$pi" || fail "postinst has no sddm cache rebuild"
if sed -n '/BEGIN farside-greeter-setting/,$p' "$pi" | grep -E 'systemctl[^|;]*(restart|stop)[^|;]*sddm|10-wayland.conf' | grep -v '^ *#' | grep -v echo >/dev/null; then
    fail "postinst restarts sddm or edits 10-wayland.conf"
fi

scratch=$(mktemp -d "${TMPDIR:-/var/tmp}/farside-greeter-check.XXXXXX")
trap 'rm -rf "${scratch:?}"' EXIT
sed -n '/^# BEGIN farside-greeter-setting$/,/^# END farside-greeter-setting$/p' "$pi" >"$scratch/fn.sh"
run() { sh -c '. "$1"; farside_greeter_setting "$2"' _ "$scratch/fn.sh" "$1" 2>/dev/null; }
value() { sed -n 's/^GreeterEnvironment=//p' "$1"; }
want() { case ",$(value "$1")," in *",$2,"*) ;; *) fail "$1 lacks $2: $(value "$1")" ;; esac; }

# 1. no SDDM: nothing created
mkdir -p "$scratch/a/etc"; run "$scratch/a"
[ ! -e "$scratch/a/etc/sddm.conf.d" ] || fail "created sddm.conf.d on a host without SDDM"
# 2. Buzz variant (no prefix) plus an extra admin variable: merged, extra kept, foreign file untouched
mkdir -p "$scratch/b/etc/sddm.conf.d"
printf '[General]\nDisplayServer=wayland\nGreeterEnvironment=QT_WAYLAND_SHELL_INTEGRATION=layer-shell,FOO=bar\n' >"$scratch/b/etc/sddm.conf.d/10-wayland.conf"
cp "$scratch/b/etc/sddm.conf.d/10-wayland.conf" "$scratch/b.orig"
run "$scratch/b"
f=$scratch/b/etc/sddm.conf.d/30-farside-greeter.conf
[ -f "$f" ] || fail "drop-in not generated"
want "$f" QT_WAYLAND_SHELL_INTEGRATION=layer-shell; want "$f" XDG_MENU_PREFIX=plasma-; want "$f" FOO=bar
cmp -s "$scratch/b.orig" "$scratch/b/etc/sddm.conf.d/10-wayland.conf" || fail "foreign conffile modified"
grep -qxF '[General]' "$f" || fail "missing [General] header"
[ "$(grep -c '=' "$f")" = 1 ] || fail "drop-in sets more than GreeterEnvironment"
# 3. Sol variant (already complete) and idempotence
mkdir -p "$scratch/c/etc/sddm.conf.d"
printf '[General]\nGreeterEnvironment=QT_WAYLAND_SHELL_INTEGRATION=layer-shell,XDG_MENU_PREFIX=plasma-\n' >"$scratch/c/etc/sddm.conf.d/10-wayland.conf"
run "$scratch/c"; want "$scratch/c/etc/sddm.conf.d/30-farside-greeter.conf" XDG_MENU_PREFIX=plasma-
cp "$scratch/c/etc/sddm.conf.d/30-farside-greeter.conf" "$scratch/c.first"; run "$scratch/c"
cmp -s "$scratch/c.first" "$scratch/c/etc/sddm.conf.d/30-farside-greeter.conf" || fail "not idempotent"
# 4. an administrator's own file without our marker is never overwritten
mkdir -p "$scratch/d/etc/sddm.conf.d"; printf '[General]\nGreeterEnvironment=X=1\n' >"$scratch/d/etc/sddm.conf.d/30-farside-greeter.conf"
run "$scratch/d"; [ "$(value "$scratch/d/etc/sddm.conf.d/30-farside-greeter.conf")" = X=1 ] || fail "overwrote an unmanaged file"
echo "greeter setting contract OK"
