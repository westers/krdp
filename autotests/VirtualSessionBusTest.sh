#!/usr/bin/env bash
# Non-graphical integration test, run INSIDE a disposable dbus-run-session.
set -euo pipefail
names=$(gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ListNames)
if [[ "$names" == *org.kde.plasmashell* || "$names" == *org.freedesktop.systemd1* ]]; then
    echo 'Unexpected shared desktop/user-manager service on private bus' >&2
    exit 1
fi
set +e
result=$(gdbus call --session --dest org.freedesktop.systemd1 --object-path /org/freedesktop/systemd1 --method org.freedesktop.DBus.Peer.Ping 2>&1)
status=$?
set -e
if [[ $status == 0 || "$result" != *org.freedesktop.DBus.Error.AccessDenied* ]]; then
    echo 'Private bus failed to deny user-manager access' >&2
    echo "$result" >&2
    exit 1
fi
# Also ensure a later accidental activation cannot register a manager name.
set +e
result=$(gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.RequestName org.freedesktop.systemd1 0 2>&1)
status=$?
set -e
if [[ $status == 0 || "$result" != *org.freedesktop.DBus.Error.AccessDenied* ]]; then
    echo 'Private bus failed to deny user-manager name ownership' >&2
    exit 1
fi
echo 'Private bus reachable; shared manager calls and name ownership denied'
