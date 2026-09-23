#!/usr/bin/env bash
# Privileged envelope for an explicitly authorized disposable Sol GPU probe.
# Never install this as a service or run it against a physical desktop.
set -euo pipefail
[[ $# == 0 || ( $# == 1 && ( $1 == --multi-worker || $1 == --multi-mixed || $1 == --multi-create || $1 == --multi-add || $1 == --multi-remove || $1 == --multi-resize || $1 == --multi-negative || $1 == --multi-window || $1 == --multi-input || $1 == --multi-drag || $1 == --multi-reposition || $1 == --multi-rdp || $1 == --multi-mixed-rdp || $1 == --multi-resize-rdp || $1 == --layout-campaign ) ) ]]
[[ $EUID == 0 && "$(hostname -s)" == sol ]] || {
    echo 'Run explicitly as root on Sol.' >&2
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
# A single user-run sudo envelope can cover several independently bounded,
# unprivileged private-compositor/RDP probes. No probe is started by root here;
# the agent uses the existing Sol tmux/SSH session while this scoped ACL is
# live. The EXIT trap revokes exactly this grant after 20 minutes or Ctrl-C.
if [[ ${1:-} == --layout-campaign ]]; then
    [[ -z $(ss -H -ltn 'sport = :3396') ]] || { echo 'Private test port 3396 is already occupied.' >&2; exit 1; }
    echo 'Private render access ready for at most 20 minutes; run only isolated Sol probes. Ctrl-C ends it early.'
    timeout --kill-after=5 1200 sleep 1200
    exit
fi
# The graphical probe runs as the ordinary user, never as root. Its own
# timeout is 80 seconds; this outer bound also covers wrapper startup/cleanup.
probe_mode=--plasma-nvidia
probe_limit=100
if [[ $# == 1 ]]; then
    probe_mode=--plasma-multi-worker-nvidia
    [[ $1 != --multi-mixed ]] || probe_mode=--plasma-mixed-worker-nvidia
    [[ $1 != --multi-create ]] || probe_mode=--plasma-multi-create-nvidia
    [[ $1 != --multi-add ]] || probe_mode=--plasma-multi-add-nvidia
    [[ $1 != --multi-remove ]] || probe_mode=--plasma-multi-remove-nvidia
    [[ $1 != --multi-resize ]] || probe_mode=--plasma-multi-resize-nvidia
    [[ $1 != --multi-negative ]] || probe_mode=--plasma-negative-worker-nvidia
    [[ $1 != --multi-window ]] || probe_mode=--plasma-multi-window-nvidia
    [[ $1 != --multi-input ]] || probe_mode=--plasma-multi-input-nvidia
    [[ $1 != --multi-drag ]] || probe_mode=--plasma-multi-drag-nvidia
    [[ $1 != --multi-reposition ]] || probe_mode=--plasma-multi-reposition-nvidia
    probe_limit=150
fi
if [[ ${1:-} == --multi-rdp || ${1:-} == --multi-mixed-rdp || ${1:-} == --multi-resize-rdp ]]; then
    [[ -z $(ss -H -ltn 'sport = :3396') ]]
    host_mode=--multi
    [[ ${1:-} != --multi-mixed-rdp ]] || host_mode=--multi-mixed
    host_environment=()
    [[ ${1:-} != --multi-resize-rdp ]] || host_environment=(KRDP_EXPERIMENTAL_MULTI_RESIZE=1)
    timeout --kill-after=5 210 runuser -u westers -- \
        env "${host_environment[@]}" \
        /home/westers/dev/krdp/build/bin/krdp-virtual-rdp-host-probe "$host_mode" \
        /home/westers/dev/krdp/scripts/probe-virtual-compositor.sh
else
    timeout --kill-after=5 "$probe_limit" runuser -u westers -- \
        bash /home/westers/dev/krdp/scripts/probe-virtual-compositor.sh "$probe_mode"
fi
