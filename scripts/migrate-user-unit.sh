#!/usr/bin/env bash
# Copy the old user unit's drop-ins and enablement. Never starts or stops a unit.
set -euo pipefail
old=app-org.kde.krdpserver.service
new=app-io.github.westers.farside.server.service
old_dir=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/$old.d
new_dir=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/$new.d
if [[ -d $old_dir ]]; then
    install -d -m 0755 "$new_dir"
    for source in "$old_dir"/*.conf; do
        [[ -f $source && ! -L $source ]] || continue
        target="$new_dir/${source##*/}"
        [[ ! -e $target && ! -L $target ]] || continue
        temp=$(mktemp "$new_dir/.copy.XXXXXX")
        sed -e 's/KRDP_/FARSIDE_/g' -e 's@/etc/krdp/@/etc/farside/@g' \
            -e 's@/usr/bin/krdpserver@/usr/bin/farside-server@g' \
            -e 's@/usr/lib/x86_64-linux-gnu/krdp@/usr/lib/x86_64-linux-gnu/farside@g' "$source" >"$temp"
        chmod --reference="$source" "$temp"
        mv -n "$temp" "$target"
        [[ ! -e $temp ]] || rm -f "$temp"
    done
fi
systemctl --user daemon-reload
if [[ $(systemctl --user is-enabled "$old" 2>/dev/null || true) == enabled ]]; then
    systemctl --user enable "$new"
fi
