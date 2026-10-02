# Native pointer capture bridge

The ordinary KWin scripting/D-Bus API does not expose application pointer locks.
This disabled-by-default native plugin observes actual locks, keeping application
request, compositor activation, and Farside's permission separate. Capture workers
load it on their own desktop bus and take a current snapshot. No client means no
Farside constraint override. A controlling client explicitly opts into a renewable
six-second lease; owner loss, bus peer death, timeout or worker shutdown restores
ordinary local behavior. The bridge never injects input or unlocks a desktop.

This plugin uses **KWin 6.6.6 private ABI**, not a stable extension API. Packaging
extracts the matching `kwin-dev`, checks the supported version, and pins `libkwin6`
and `kwin-wayland` to that exact Debian version. A KWin upgrade requires rebuilding
and revalidating this bridge. A missing/incompatible bridge reports unavailable;
the client does not claim capture synchronization or enable its capture action.

`upstream/tabbox/` contains the unmodified GPL headers from
[KWin v6.6.6](https://github.com/KDE/kwin/tree/v6.6.6/src/tabbox), retained with their
copyright/license notices because the distro SDK omits them. They let us read the
real Alt+Tab grab even if it predates plugin loading, rather than infer it from a
window/cursor or duplicate a private class layout. Pointer confinement alone is
never treated as a game lock. Screen locking and Alt+Tab block permission.

`NativePointerCaptureTest` is opt-in and runs only in a hardware-isolated virtual
KWin on Sol. It exercises pre-existing locks, free/captured permission, application
unlock/relock, stale commands, worker generation changes, and lease expiry. Never
run its fake input against a work desktop. See the OPT-054 issue for network and
client intent semantics and remaining hands-on acceptance.
