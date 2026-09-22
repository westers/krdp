#!/usr/bin/env bash
# Read-only preflight, not a privileged launcher or a hostile-mutation sandbox.
set -euo pipefail
export PATH=/usr/bin:/bin
unset LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT
fail() { echo "Virtual installation check failed: $*" >&2; exit 1; }
[[ $EUID != 0 ]] || fail 'run as the ordinary administrator account, not root'
[[ $# == 0 ]] || fail 'this acceptance check has a fixed test prefix and takes no arguments'
task_prefix=/opt/krdp-virtual-service-test
task_libdir=$task_prefix/lib/x86_64-linux-gnu

# Check canonical target and every ancestor. System libraries may use trusted
# distro symlinks; code/support inside the prefix has stricter link rules below.
trusted() {
    local path uid mode
    path=$(readlink -e -- "$1") || fail "missing path: $1"
    [[ $path == /* ]] || fail "nonabsolute path: $1"
    while :; do
        read -r uid mode < <(stat -Lc '%u %a' -- "$path")
        [[ $uid == 0 && $mode =~ ^[0-7]+$ ]] || fail "ownership: $path"
        (( (8#$mode & 0022) == 0 )) || fail "writable by group/other: $path"
        [[ $path == / ]] && break
        path=$(dirname -- "$path")
    done
}
[[ $(readlink -e -- "$task_prefix") == "$task_prefix" ]] || fail 'missing or noncanonical prefix'
trusted "$task_prefix"
exec {task_tree_fd}< <(find "$task_prefix" -mindepth 1 -print0)
task_find_pid=$!
while IFS= read -r -d '' path; do
    if [[ -L $path ]]; then
        [[ $path == "$task_libdir/"* ]] || fail "unexpected symlink: $path"
        target=$(readlink -e -- "$path") || fail "broken symlink: $path"
        [[ $target == "$task_libdir/"* && -f $target ]] || fail "escaping symlink: $path"
    elif [[ ! -f $path && ! -d $path ]]; then
        fail "unexpected file type: $path"
    fi
    trusted "$path"
done <&"$task_tree_fd"
exec {task_tree_fd}<&-
wait "$task_find_pid" || fail 'incomplete installation tree traversal'

task_elfs=()
for name in krdp-virtual-host krdp-virtual-session-entry krdp-virtual-pam-keeper \
    krdp-virtual-session-cleanup krdp-virtual-device-entry krdp-virtual-guardian \
    krdp-virtual-guardianctl krdp-console-worker; do
    path=$task_prefix/bin/$name
    [[ -f $path && ! -L $path && -x $path ]] || fail "missing executable: $path"
    task_elfs+=("$path")
done
for name in KRdp KPipeWire KPipeWireDmaBuf KPipeWireRecord; do
    path=$(readlink -e -- "$task_libdir/lib$name.so.6") || fail "missing library: $name"
    [[ $path == "$task_libdir/"* ]] || fail "nonprivate library: $name"
    task_elfs+=("$path")
done
for path in "${task_elfs[@]}"; do
    dynamic=$(readelf -d -- "$path") || fail "ELF metadata: $path"
    while IFS= read -r needed; do
        [[ $needed != */* ]] || fail "path-bearing ELF dependency: $needed"
    done < <(printf '%s\n' "$dynamic" | sed -n '/(NEEDED)/s/.*\[\(.*\)\].*/\1/p')
    runpath=$(printf '%s\n' "$dynamic" | sed -n '/(RUNPATH)/s/.*\[\(.*\)\].*/\1/p')
    [[ $runpath == "$task_libdir" && $dynamic != *'(RPATH)'* ]] || fail "runtime path: $path"
    resolved=$(env -i PATH=/usr/bin:/bin LANG=C ldd -r "$path" 2>&1) || fail "dependency inspection: $path"
    [[ $resolved != *'not found'* && $resolved != *'undefined symbol'* ]] || fail "unresolved dependency: $path"
    while read -r name arrow location rest; do
        if [[ $arrow == '=>' ]]; then
            [[ $location == /* ]] || fail "unresolved library: $name"
            case $name in
                libKRdp.so.*|libKPipeWire.so.*|libKPipeWireDmaBuf.so.*|libKPipeWireRecord.so.*)
                    [[ $location == "$task_libdir/$name" ]] || fail "wrong private library: $location";;
            esac
            trusted "$location"
        elif [[ $name == /* ]]; then
            trusted "$name"
        fi
    done <<< "$resolved"
done
echo 'PASS: 12 installed ELF objects have trusted ownership, private runtime paths and resolved symbols.'
echo 'No service started. PAM policy, unit placement and actual lifecycle/audio acceptance remain separate checks.'
