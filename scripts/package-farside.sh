#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

# Build the single `farside-server` system deb (normal /usr paths) from committed
# sources. It never installs anything and never touches ~/dev/krdp/build or
# ~/dev/krdp/.deps: both KRdp and the private KPipeWire are built from
# `git archive` exports inside the build directory.
#
# Environment:
#   KRDP_PKG_BUILD_DIR  build directory (default: <checkout>/build-pkg)
#   KPIPEWIRE_SRC       KPipeWire git repository (default: ~/dev/kpipewire)
#   KPIPEWIRE_REF       KPipeWire commit to bundle (default: the pinned one below)
#   JOBS                parallel jobs (default: nproc / 3)
#   KRDP_PKG_ALLOW_DIRTY=1  build HEAD even with uncommitted changes (they are
#                       NOT included; only HEAD is exported)
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=${KRDP_PKG_BUILD_DIR:-$root/build-pkg}
# The build directory is deleted piecemeal below: refuse anything that is not
# clearly a scratch build directory.
build=$(realpath -m -- "$build")
if [[ -z $build || $build == / || $build == "$(realpath -m -- "$HOME")" || $build == "$root" ]] \
    || [[ $build != "$root"/* && $(basename -- "$build") != build-pkg* ]]; then
    echo "refusing unsafe build directory '$build': it must be inside $root or be named build-pkg*" >&2
    exit 1
fi
kpw_src=${KPIPEWIRE_SRC:-$HOME/dev/kpipewire}
# sw-encoders = aud-fix2-dmabuf-egl e31f7e8 (westers/opt-015: v6.6.4 + the KRDP encoder patches,
# OPT-015/OPT-050, + the DmaBufHandler EGL/GBM leak fix, AUD-FIX2 F5) + WS-E software HEVC
# (libx265) and AV1 (libsvtav1) with backend policies, presets and a target bitrate, + libx265
# bitrate/CRF changes in place (AUD-SWENC, 02d475d) + a hidden cursor reported from the
# screencast metadata (FIX-CURSOR, fe44b96; PipeWireCursor::visible) + AV1's own quantiser scale and AV1
# tiles (AV1-Q, fdfa037; setAv1Tiles(), quantizerForQuality()) + Sol HEVC NVENC fallback
# (OPT-046, 9d6b08c). Those libraries come in through
# libavcodec's own Depends (dpkg-shlibdeps: libavcodec62).
kpw_ref=${KPIPEWIRE_REF:-9d6b08c}
jobs=${JOBS:-$(( $(nproc) / 3 ))}
(( jobs >= 1 )) || jobs=1

multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
privdir=/usr/lib/$multiarch/farside

for tool in cmake ninja git patch file dpkg-shlibdeps dpkg-gencontrol dpkg-deb; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

if [[ -n $(git -C "$root" status --porcelain --untracked-files=no) && ${KRDP_PKG_ALLOW_DIRTY:-0} != 1 ]]; then
    echo "uncommitted changes in $root; commit them or set KRDP_PKG_ALLOW_DIRTY=1" >&2
    exit 1
fi

commit=$(git -C "$root" rev-parse HEAD)
revision=${commit:0:7}
kpw_commit=$(git -C "$kpw_src" rev-parse --verify "$kpw_ref^{commit}")
# Reproducible: timestamps and the version come from the commit, not the clock.
export SOURCE_DATE_EPOCH=$(git -C "$root" log -1 --format=%ct "$commit")
project_version=$(sed -n 's/^set(PROJECT_VERSION "\([0-9.]*\)")/\1/p' "$root/CMakeLists.txt")
stamp=$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y%m%d%H%M)
version="$project_version+git$stamp.$revision-1"
export TZ=UTC LC_ALL=C.UTF-8

# Distro hardening/reproducibility flags (-ffile-prefix-map, relro, ...).
eval "$(DEB_BUILD_MAINT_OPTIONS=hardening=+all dpkg-buildflags --export=sh)"

echo "farside-server $version (krdp $commit, kpipewire $kpw_commit), $jobs jobs, in $build"
mkdir -p "$build"

# 1. Private KPipeWire, installed to a scratch prefix. Its libraries carry the
#    package's private RUNPATH so they resolve each other, never the distro's.
# git archive uses the commit timestamp. A new export can be older than objects
# from a previous build, so Ninja's timestamp checks cannot validate that cache.
rm -rf "${build:?}/kpipewire-src" "${build:?}/kpipewire-prefix" "${build:?}/kpipewire-build"
mkdir -p "$build/kpipewire-src"
git -C "$kpw_src" archive "$kpw_commit" | tar -x -C "$build/kpipewire-src"
cmake -S "$build/kpipewire-src" -B "$build/kpipewire-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$build/kpipewire-prefix" \
    -DKDE_SKIP_RPATH_SETTINGS=TRUE -DCMAKE_INSTALL_RPATH="$privdir" \
    -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF \
    -DBUILD_TESTING=OFF
cmake --build "$build/kpipewire-build" -j"$jobs"
cmake --install "$build/kpipewire-build"
kpw_libdir="$build/kpipewire-prefix/lib/$multiarch"
[[ -f "$kpw_libdir/libKPipeWire.so.6" && -f "$kpw_libdir/libKPipeWireRecord.so.6" ]]

# 2. KRdp from the committed tree.
rm -rf "${build:?}/src" "${build:?}/farside-build"
mkdir -p "$build/src"
git -C "$root" archive "$commit" | tar -x -C "$build/src"
cmake -S "$build/src" -B "$build/farside-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DKDE_INSTALL_USE_QT_SYS_PATHS=ON \
    -DKDE_SKIP_RPATH_SETTINGS=TRUE -DCMAKE_INSTALL_RPATH="$privdir" \
    -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
    -DINSTALL_DIAGNOSTIC_PROBES=OFF \
    -DKRDP_BUILD_SYSTEM_PACKAGE=ON \
    -DKPipeWire_DIR="$kpw_libdir/cmake/KPipeWire" \
    -DKRDP_PRIVATE_KPIPEWIRE_LIBDIR="$kpw_libdir"
cmake --build "$build/farside-build" -j"$jobs"

# 3. Stage the package tree the way dpkg-gencontrol/dpkg-shlibdeps expect it.
work="${build:?}/deb"
pkgroot="$work/debian/farside-server"
rm -rf "${work:?}"
mkdir -p "$work/debian"
DESTDIR="$pkgroot" cmake --install "$build/farside-build"

# The pointer observer uses KWin's private plugin ABI: compile its exact SDK and
# pin the installed compositor/library together. Never silently omit it.
kwin_version=$(dpkg-query -W -f='${Version}' libkwin6)
[[ $(dpkg-query -W -f='${Version}' kwin-wayland) == "$kwin_version" ]] || {
    echo 'KWin executable and library versions differ' >&2; exit 1;
}
rm -rf "${build:?}/kwin-sdk" "${build:?}/kwin-capture-build"
mkdir -p "$build/kwin-sdk"
(cd "$build/kwin-sdk" && apt-get download "kwin-dev=$kwin_version")
mapfile -t kwin_sdks < <(find "$build/kwin-sdk" -maxdepth 1 -name '*.deb')
[[ ${#kwin_sdks[@]} == 1 && $(dpkg-deb -f "${kwin_sdks[0]}" Version) == "$kwin_version" ]]
dpkg-deb -x "${kwin_sdks[0]}" "$build/kwin-sdk/root"
cmake -S "$build/src/server/kwin" -B "$build/kwin-capture-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR="lib/$multiarch" \
    -DFARSIDE_KWIN_INCLUDE_DIR="$build/kwin-sdk/root/usr/include/kwin"
cmake --build "$build/kwin-capture-build" -j"$jobs"
DESTDIR="$pkgroot" cmake --install "$build/kwin-capture-build"


# Strip like dh_strip (no -dbgsym package).
while IFS= read -r -d '' file; do
    if file -b "$file" | grep -q 'ELF .*shared object'; then
        strip --remove-section=.comment --remove-section=.note --strip-unneeded "$file"
    else
        strip --remove-section=.comment --remove-section=.note "$file"
    fi
done < <(find "$pkgroot" -type f -exec sh -c 'head -c4 "$1" | grep -q "^.ELF"' _ {} \; -print0)

doc="$pkgroot/usr/share/doc/farside-server"
install -d "$doc"
install -m 0644 "$root/packaging/farside/copyright" "$doc/copyright"
if [[ -s "$root/packaging/farside/lintian-overrides" ]]; then
    install -D -m 0644 "$root/packaging/farside/lintian-overrides" "$pkgroot/usr/share/lintian/overrides/farside-server"
fi
cat >"$work/debian/changelog" <<EOF
farside ($version) resolute; urgency=medium

  * Build of github.com/westers/krdp commit
    $commit,
    private KPipeWire $kpw_commit.

 -- Steve Westers <amiga1.2k@gmail.com>  $(date -u -R -d "@$SOURCE_DATE_EPOCH")
EOF
gzip -9n <"$work/debian/changelog" >"$doc/changelog.Debian.gz"
chmod 0644 "$doc/changelog.Debian.gz"
cp "$root/packaging/farside/control" "$work/debian/control"

install -d -m 0755 "$pkgroot/DEBIAN"
for script in preinst postinst prerm postrm; do
    install -m 0755 "$root/packaging/farside/$script" "$pkgroot/DEBIAN/$script"
done
(cd "$pkgroot" && find etc -type f -printf '/%p\n' | sort) >"$pkgroot/DEBIAN/conffiles"
chmod 0644 "$pkgroot/DEBIAN/conffiles"

# Every ELF file's RUNPATH must be exactly the private directory (or absent):
# a leaked build path would load libraries from this build tree.
mapfile -d '' elves < <(find "$pkgroot" -type f -exec sh -c 'head -c4 "$1" | grep -q "^.ELF"' _ {} \; -print0)
for elf in "${elves[@]}"; do
    runpath=$(readelf -d "$elf" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')
    [[ -z "$runpath" || "$runpath" == "$privdir" ]] || { echo "bad RUNPATH $runpath in $elf" >&2; exit 1; }
done

# Dependencies from the ELF files; the private libraries resolve inside the
# package (via their RUNPATH) and add no dependency.
(cd "$work" && dpkg-shlibdeps -Tdebian/farside-server.substvars -l"$pkgroot$privdir" "${elves[@]}")
printf 'farside:kwinDepends=libkwin6 (= %s), kwin-wayland (= %s)\n' "$kwin_version" "$kwin_version" >>"$work/debian/farside-server.substvars"
(cd "$work" && dpkg-gencontrol -pfarside-server -Pdebian/farside-server -Tdebian/farside-server.substvars)

(cd "$pkgroot" && find . -path ./DEBIAN -prune -o -type f -printf '%P\0' | sort -z \
    | xargs -0 md5sum) >"$pkgroot/DEBIAN/md5sums"
chmod 0644 "$pkgroot/DEBIAN/md5sums"

# Normalise modes and timestamps, then build.
find "$pkgroot" -type d -exec chmod 0755 {} +
find "$pkgroot" -newermt "@$SOURCE_DATE_EPOCH" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
deb="$build/farside-server_${version}_$(dpkg --print-architecture).deb"
rm -f "${build:?}"/farside-server_*.deb
dpkg-deb --root-owner-group -Zxz --build "$pkgroot" "$deb"

# 4. Contract checks.
[[ $(dpkg-deb -f "$deb" Package) == farside-server && $(dpkg-deb -f "$deb" Version) == "$version" ]]
contents=$(dpkg-deb -c "$deb" | awk '{print $6}')
for path in "./usr/lib/$multiarch/qt6/plugins/kwin/plugins/farside-pointer-capture.so" ./usr/bin/farside-server ./usr/bin/farside-console-host ./usr/bin/farside-console-worker \
    ./usr/bin/farside-virtual-host ./usr/bin/farside-virtual-session-entry ./usr/bin/farside-virtual-pam-keeper \
    ./usr/bin/farside-virtual-session-cleanup ./usr/bin/farside-virtual-device-entry \
    ./usr/bin/farside-virtual-guardian ./usr/bin/farside-virtual-guardianctl \
    ".$privdir/libFarsideRdp.so.1" ".$privdir/libKPipeWire.so.6" ".$privdir/libKPipeWireDmaBuf.so.6" \
    ".$privdir/libKPipeWireRecord.so.6" \
    ./usr/lib/systemd/user/app-io.github.westers.farside.server.service \
    ./usr/lib/systemd/system/farside-console-host.service ./usr/lib/systemd/system/farside-virtual-host.service \
    ./usr/lib/systemd/system/farside-virtual-session@.service \
    "./usr/lib/$multiarch/libexec/farside-authentication-helper" \
    ./usr/share/polkit-1/actions/org.farside.authentication.policy \
    "./usr/lib/$multiarch/libexec/farside-host-settings-helper" \
    ./usr/share/polkit-1/actions/org.farside.hostsettings.policy \
    ./usr/lib/systemd/system-preset/00-farside-system.preset ./usr/lib/systemd/user-preset/00-farside.preset \
    ./etc/pam.d/farside-virtual-session \
    ./usr/share/farside/virtual-session/launch-virtual-session.sh \
    ./usr/share/applications/io.github.westers.farside.server.desktop \
    ./usr/share/applications/io.github.westers.farside.consoleworker.desktop \
    ./usr/share/applications/kcm_farside.desktop \
    ./usr/share/icons/hicolor/scalable/apps/io.github.westers.farside.server.svg \
    "./usr/lib/$multiarch/qt6/plugins/plasma/kcms/systemsettings/kcm_farside.so"; do
    grep -qxF -- "$path" <<<"$contents" || { echo "missing from package: $path" >&2; exit 1; }
done
if grep -E -- '-probe$|^\./opt/|/cmake/|/lib[^/]*\.so$|/include/' <<<"$contents"; then
    echo "package contains probes, /opt paths or development files" >&2
    exit 1
fi
if grep -E '^\./(usr/bin/krdp|usr/lib/systemd/(user|system)/.*krdp|usr/share/applications/(org\.kde\.krdp|kcm_krdp)|usr/share/qlogging-categories6/(krdp|kcm_krdp)|usr/share/farside/.*/krdpserverrc|usr/lib/[^/]+/krdp/)' <<<"$contents"; then
    echo "Farside package still installs a KRDP identity" >&2
    exit 1
fi
if grep -qxF './usr/share/icons/hicolor/scalable/apps/io.github.westers.farside.svg' <<<"$contents"; then
    echo 'the server icon path overlaps farside-client' >&2
    exit 1
fi
echo "farside-server system deb: $deb"
