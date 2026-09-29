#!/usr/bin/env bash
# Copy retained-desktop records after the old broker stops. Existing desktop
# units stay running and are adopted by the new broker through their sockets.
set -euo pipefail
[[ $(id -u) == 0 ]]
if systemctl is-active --quiet krdp-virtual-host.service || systemctl is-active --quiet farside-virtual-host.service; then
    echo 'Stop both virtual-host brokers before copying the journal.' >&2
    exit 1
fi
old=/var/lib/krdp/virtual-sessions
new=/var/lib/farside/virtual-sessions
[[ -d $old && ! -L $old ]] || exit 0
install -d -m 0700 /var/lib/farside
if [[ -e $new ]]; then
    # A previous copy is safe to keep. Mixing two independently written
    # journals would make session records ambiguous.
    if [[ -e /var/lib/farside/migrated-journal ]]; then exit 0; fi
    echo "Refusing to merge with an existing $new; inspect it first." >&2
    exit 1
fi
cp -a --no-clobber "$old" "$new"
old_records=$(find "$old" -maxdepth 1 -type f -name '*.json' | wc -l)
new_records=$(find "$new" -maxdepth 1 -type f -name '*.json' | wc -l)
[[ $old_records == "$new_records" ]] || { echo 'Record count changed during the copy.' >&2; exit 1; }
printf 'copied %s records\n' "$new_records" > /var/lib/farside/migrated-journal
chmod 0600 /var/lib/farside/migrated-journal
