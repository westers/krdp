#!/usr/bin/env bash
# User-started privileged envelope for the disposable Sol GPU probe only.
# Never install this as a service or invoke it automatically from the agent.
set -euo pipefail
[[ $# == 0 && $EUID == 0 && "$(hostname -s)" == sol ]] || {
    echo 'Run explicitly as root on Sol, with no arguments.' >&2
    exit 1
}
[[ "$(id -u westers)" == 1000 ]]
exec 9>/run/lock/krdp-sol-render-probe.lock
flock -n 9 || { echo 'Another render probe is active.' >&2; exit 1; }
node=$(realpath /dev/dri/by-path/pci-0000:09:00.0-render)
[[ "$node" =~ ^/dev/dri/renderD[0-9]+$ && -c "$node" ]]
[[ "$(cat /sys/class/drm/"${node##*/}"/device/vendor)" == 0x10de ]]
[[ "$(cat /sys/class/drm/"${node##*/}"/device/device)" == 0x1f02 ]]
before=$(getfacl -ncp "$node")
# Do not take ownership of an existing user grant, or broaden the ACL mask.
if grep -q '^user:1000:' <<<"$before" || ! grep -q '^mask::rw-$' <<<"$before"; then
    echo 'Unexpected ACL; refusing to modify it.' >&2
    exit 1
fi
identity=$(stat -Lc '%d:%i:%t:%T' "$node")
cleanup() {
    local result=$?
    trap - EXIT
    if [[ "$(stat -Lc '%d:%i:%t:%T' "$node")" != "$identity" ]]; then
        echo "Device replaced; manual ACL inspection needed: $node" >&2
        exit 1
    fi
    # Remove ONLY our grant; leave any concurrent sddm/seat ACL changes alone.
    if ! setfacl -n -x u:1000 "$node"; then
        echo "Failed to revoke temporary render access: $node" >&2
        exit 1
    fi
    echo 'Temporary render access revoked.'
    if [[ "$(getfacl -ncp "$node")" != "$before" ]]; then
        echo 'Other device ACL entries changed during test; preserved for inspection.' >&2
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
setfacl -n -m u:1000:rw "$node"
# The graphical probe runs as the ordinary user, never as root. Its own
# timeout is 80 seconds; this outer bound also covers wrapper startup/cleanup.
timeout --kill-after=5 100 runuser -u westers -- \
    bash /home/westers/dev/krdp/scripts/probe-virtual-compositor.sh --plasma-nvidia
