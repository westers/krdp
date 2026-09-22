#!/bin/bash
# Read-only operator diagnostic. Never an admission or provisioning certificate.
set -euo pipefail

if (( $# != 0 )); then
    printf 'No arguments are accepted.\n' >&2
    exit 64
fi
if (( UID != 0 || EUID != 0 )); then
    printf 'Run this read-only inspection as root; it does not change services.\n' >&2
    exit 1
fi

refuse() { printf 'Inspection refused: %s\n' "$1" >&2; exit 1; }
service=unattended-upgrades.service
launcher_pid=$(/usr/bin/systemctl show "$service" -p MainPID --value)
[[ $launcher_pid =~ ^[1-9][0-9]*$ && $launcher_pid != 1 ]] || refuse 'no running launcher'

birth() {
    local row rest
    local -a fields
    IFS= read -r row < "/proc/$launcher_pid/stat" || return 1
    rest=${row##*) }
    read -r -a fields <<< "$rest"
    [[ ${#fields[@]} -ge 20 && ${fields[19]} =~ ^[0-9]+$ ]] || return 1
    printf '%s' "${fields[19]}"
}
launcher_birth=$(birth) || refuse 'launcher birth unavailable'
launcher_mount_ns=$(/usr/bin/readlink "/proc/$launcher_pid/ns/mnt") || refuse 'launcher mount namespace unavailable'
[[ $launcher_mount_ns == "$(/usr/bin/readlink /proc/self/ns/mnt)" ]] || refuse 'launcher mount namespace differs from inspector'
launcher_uids=
while IFS= read -r status_line; do
    if [[ $status_line == Uid:* ]]; then
        read -r _ real_uid effective_uid saved_uid filesystem_uid <<< "$status_line"
        launcher_uids="$real_uid:$effective_uid:$saved_uid:$filesystem_uid"
        break
    fi
done < "/proc/$launcher_pid/status"
[[ $launcher_uids == 0:0:0:0 ]] || refuse 'launcher is not entirely root'
mapfile -d '' -t launcher_args < "/proc/$launcher_pid/cmdline"
[[ ${#launcher_args[@]} == 3
    && ${launcher_args[0]} == /usr/bin/python3
    && ${launcher_args[1]} == /usr/share/unattended-upgrades/unattended-upgrade-shutdown
    && ${launcher_args[2]} == --wait-for-signal ]] || refuse 'launcher arguments differ from audited invocation'

# /proc/PID/environ describes the initial environment region, not necessarily
# Python's later os.environ. Do not print arbitrary values. PATH is diagnostic
# input for candidate selection; for loader/plugin variables print names only.
launcher_path= path_count=0
while IFS= read -r -d '' entry; do
    key=${entry%%=*}
    case "$key" in
        PATH) launcher_path=${entry#*=}; path_count=$((path_count + 1));;
        LD_*|PYTHON*|GLIBC_TUNABLES|APT_CONFIG|UNATTENDED_UPGRADES_PLUGIN_PATH)
            printf 'environment_variable_present=%q\n' "$key";;
    esac
done < "/proc/$launcher_pid/environ"
[[ $path_count == 1 && -n $launcher_path ]] || refuse 'PATH absent, duplicated or empty'
[[ $launcher_path != *$'\n'* && $launcher_path != *$'\r'* ]] || refuse 'unsupported PATH control characters'
printf 'launcher_pid=%s\nlauncher_birth_ticks=%s\ninitialEnvironmentPATH=%q\n' "$launcher_pid" "$launcher_birth" "$launcher_path"

remaining=$launcher_path
selected=
while :; do
    if [[ $remaining == *:* ]]; then
        candidate_dir=${remaining%%:*}
        remaining=${remaining#*:}
        more=1
    else
        candidate_dir=$remaining
        more=0
    fi
    [[ $candidate_dir == /* ]] || refuse 'relative or empty PATH component'
    candidate=$candidate_dir/unattended-upgrade
    if [[ -f $candidate && -x $candidate ]]; then
        selected=$candidate
        break
    fi
    (( more )) || break
done
[[ -n $selected ]] || refuse 'no executable selected'
resolved=$(/usr/bin/readlink -e -- "$selected") || refuse 'selected executable cannot be resolved'
before=$(/usr/bin/stat -Lc '%d:%i:%s:%Y:%Z' -- "$selected") || refuse 'selected executable metadata unavailable'
hash_line=$(/usr/bin/sha256sum < "$selected") || refuse 'selected executable cannot be hashed'
selected_hash=${hash_line%% *}
[[ $selected_hash =~ ^[0-9a-f]{64}$ ]] || refuse 'invalid hash result'
after=$(/usr/bin/stat -Lc '%d:%i:%s:%Y:%Z' -- "$selected") || refuse 'selected executable changed'
[[ $before == "$after" && $resolved == "$(/usr/bin/readlink -e -- "$selected")" ]] || refuse 'selected executable changed during inspection'
printf 'candidate_from_initial_PATH=%q\nresolved_path=%q\nselected_sha256=%s\n' "$selected" "$resolved" "$selected_hash"
selected_mode=$(/usr/bin/stat -Lc '%u:%g:%a' -- "$selected") || refuse 'selected executable final metadata unavailable'
printf 'selected_uid_gid_mode=%s\n' "$selected_mode"

[[ $(birth) == "$launcher_birth"
    && $(/usr/bin/readlink "/proc/$launcher_pid/ns/mnt") == "$launcher_mount_ns"
    && $(/usr/bin/systemctl show "$service" -p MainPID --value) == "$launcher_pid" ]] || refuse 'launcher changed during inspection'
printf 'snapshot_consistent=yes\nselection_view=inspector-filesystem-and-credentials\n'
printf 'identity_check=bracketed-not-pidfd-pinned\nfuture_executable_selection=not-proven\nadmission=not-evaluated\n'
