# OPT-041 Client-Sized Virtual Monitors (Phases A + B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `MonitorMode=virtual` makes krdpserver present the connecting client's own screen layout — one KWin virtual output per client monitor at the client's exact size, physical monitors switched off for the duration by default (`replace`) and restored on disconnect — with client input landing on the virtual output(s).

**Architecture:** The client's advertised desktop size and monitor list are read from FreeRDP's peer settings after the capabilities exchange and handed (queued) to `SessionController`, which builds one `PlasmaScreencastV1Session` per client monitor in virtual-monitor mode. Each virtual session resolves the `QScreen` KWin adds for its output and tracks that geometry for input mapping (fixing today's hardcoded `(0,0)`). A controller-owned `PhysicalOutputGuard` snapshots the physical layout with `kscreen-doctor -j`, applies the policy (`replace`: disable physicals, position virtuals; `extend`: reconcile) once the virtual output(s) exist, and restores the snapshot before the virtual outputs are torn down, with a state file for crash recovery. Phase B reuses Plan 3's multi-surface machinery (`SessionWrapper::setSessions` with a layout, `VideoStream::setMonitorLayout`, `SurfaceLayout`), fed by the virtual outputs' real geometries.

**Tech Stack:** C++20 / Qt 6.10 (QProcess, QScreen, QJsonDocument, QStandardPaths), KF6 Config (kcfg), FreeRDP 3 server settings (`FreeRDP_DesktopWidth/Height`, `FreeRDP_MonitorCount`, `FreeRDP_MonitorDefArray`), `zkde_screencast_unstable_v1.stream_virtual_output` (already wired), `kscreen-doctor` 6.6 as a child process, QtTest.

**Spec:** `docs/superpowers/specs/2026-09-17-opt-041-virtual-monitor-design.md` (approved 2026-09-17; `replace` is the default). Spike evidence: `.superpowers/sdd/spike-opt-041-virtual-monitor/report.md`.

## Global Constraints

- Plasma Wayland only (`--plasma` = `PlasmaScreencastV1Session`); the portal session never gets virtual mode (log a warning and fall back to `workspace`).
- Existing modes `workspace|primary|specific|multi` stay byte-for-byte unchanged in behaviour; the static `--virtual-monitor WxH@S` CLI flag keeps working as today (fixed size, no policy).
- The live service on port 3389 (`app-org.kde.krdpserver.service`, Steve's desktop) is never restarted, never connected to, and never reconfigured by an agent. Every test uses a throwaway instance: `export XDG_CONFIG_HOME=/tmp/krdp-v/config WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 QT_QPA_PLATFORM=wayland DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus`, a `$XDG_CONFIG_HOME/krdpserverrc` written with a plain `printf` (NEVER `kwriteconfig6 --notify`: it makes every running krdpserver, the live one included, reload), and `~/dev/krdp/build/bin/krdpserver --plasma --port 3392 -u krdptest -p krdptest --certificate ~/.local/share/krdpserver/krdp.crt --certificate-key ~/.local/share/krdpserver/krdp.key`.
- Before any test that adds or removes outputs or disables a physical monitor: `ss -tnp | grep -E ':(3389|3391|3392) ' | grep ESTAB` must show nothing on 3389/3391 (Steve connected); if it does, wait up to 10 minutes, then report BLOCKED. Never run `kscreen-doctor --dpms …`. Every test path — including failures — ends with `kscreen-doctor output.DP-1.enable output.HDMI-A-1.enable output.DP-1.position.0,0 output.HDMI-A-1.position.2560,0 output.DP-1.priority.1 output.HDMI-A-1.priority.2` and a check that `kscreen-doctor -o` shows only DP-1 at 0,0 and HDMI-A-1 at 2560,0, both enabled. Never disable DP-1 outside the guard's own code path under test.
- Client machine: Steve's laptop `buzz.local` (production; ssh as `westers@buzz.local`, host name `hal9000.local` from there). Deployed own client `~/.local/bin/krdp-client` (`--headless --host --port --user --password --seconds`; GUI mode with `--debug-actions` for pointer readback), `sdl-freerdp3` 3.22 (`/gfx:AVC420 /cert:ignore`, `/multimon` for Phase B), Remmina. Never `rsync --delete`, never install/remove packages, no sudo.
- VA-API encode limit 4096 px per side per output (`KRdp::MultiLayout::MaxEncodeDimension`); client sizes outside 640..4096 per side fall back to `VirtualMonitorFallbackSize`.
- Virtual output naming: the session requests name `krdp-m<i>-<W>x<H>`; KWin exposes it as `QScreen::name() == "Virtual-krdp-m<i>-<W>x<H>"` (verified in Task 3 Step 3; if KWin's prefix differs, the prefix constant in `OutputSnapshot.h` and `PlasmaScreencastV1Session.cpp` is the one place to change, and the report must say so). Stable names per (index, size) — ruling over spec §6's "unique per connection" first try: KWin keys remembered setups on the present output set, so stable names bound the setups list to the number of distinct client sizes and the guard re-asserts the physical state on every connect anyway (`applyReplace` / `reconcileExtend`).
- Threads: FreeRDP peer callbacks (`onCapabilities`) run on the session thread; `SessionController`, sessions, `PhysicalOutputGuard` and everything touching `QScreen` run on the main thread; cross only with `Qt::QueuedConnection`.
- Tests: QtTest with `QTEST_GUILESS_MAIN`, added to `autotests/CMakeLists.txt` as `add_executable(<Name>Test <Name>Test.cpp)` + `target_link_libraries(<Name>Test PRIVATE Qt6::Test Qt6::Core)` + `add_test(NAME <Name>Test COMMAND <Name>Test)` (pure headers include from `src/`/`server/` via the existing `target_include_directories`). `ctest --test-dir build --output-on-failure` passes at the end of every task; `cmake --build build -j16` produces zero warnings in the files touched.
- Commits on `master`, message ending with `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`; no amend/rebase/stash/push. `git status` clean except the pre-existing untracked `kpipewire_6.6.3*` files.
- Logging: `qCInfo/qCWarning(KRDP)` inside `src/` (libKRdp), plain `qInfo()/qWarning()` inside `server/` (matches the existing files). No per-frame log lines above debug.

---

## File structure

| File | Responsibility |
|---|---|
| `src/ClientDisplayInfo.h` (new, pure) | `KRdp::ClientDisplay::Info` (client desktop size + monitors), `sanitize()`, `virtualMonitorName()`, `placement()` |
| `autotests/ClientDisplayInfoTest.cpp` (new) | tests for the above |
| `src/RdpConnection.{h,cpp}` | read the client's display info in `onCapabilities()`, expose `clientDisplayInfo()`, emit `clientDisplayInfoReceived()` |
| `src/AbstractSession.h` | `outputGeometryChanged(QRect)` + `virtualOutputUnresolved()` signals, `outputGeometryResolved()` |
| `src/PlasmaScreencastV1Session.{h,cpp}` | resolve the virtual output's `QScreen`, track its geometry, gate input until resolved |
| `server/OutputSnapshot.h` (new, pure) | parse `kscreen-doctor -j`, build `kscreen-doctor` argument lists, compare snapshots, JSON state file (de)serialisation |
| `autotests/OutputSnapshotTest.cpp` (new) | tests for the above |
| `server/PhysicalOutputGuard.{h,cpp}` (new) | run `kscreen-doctor`, snapshot / applyReplace / reconcileExtend / positionOutputs / restore, state file, crash recovery |
| `server/SessionController.{h,cpp}` | virtual mode: deferred session build on client display info, policy application, teardown order, one connection at a time, Phase B layout |
| `server/main.cpp`, `server/krdpserversettings.kcfg` | `MonitorMode=virtual`, `VirtualMonitorPolicy`, `VirtualMonitorLayout`, `VirtualMonitorFallbackSize`, `--restore-outputs`, startup crash recovery |
| `README.md`, `research.md`, `~/dev/rdp/CLAUDE.md` | docs |

---

### Task 1: Pure client display model (`ClientDisplayInfo.h`)

**Files:**
- Create: `src/ClientDisplayInfo.h`
- Test: `autotests/ClientDisplayInfoTest.cpp`
- Modify: `autotests/CMakeLists.txt` (append the three lines for the new test)

**Interfaces:**
- Consumes: `KRdp::VideoMonitor { QRect geometry; bool primary; }` from `src/SurfaceLayout.h`; `KRdp::MultiLayout::MaxEncodeDimension` (4096) from `server/MultiLayout.h` is NOT included (server/ is not visible to src/): the header defines its own `MaxDimension = 4096` with a comment pointing at `MultiLayout::MaxEncodeDimension`.
- Produces:
  ```cpp
  namespace KRdp::ClientDisplay {
  constexpr int MinDimension = 640;
  constexpr int MaxDimension = 4096;
  constexpr int MaxMonitors = 16;
  struct Info { QSize desktopSize; QVector<VideoMonitor> monitors; bool operator==(const Info &) const = default; };
  inline bool usable(const QSize &size);
  inline QString virtualMonitorName(int index, const QSize &size);   // "krdp-m0-1920x1080"
  inline Info sanitize(Info info, const QSize &fallback);            // see Step 3
  inline QVector<QRect> placement(const QVector<VideoMonitor> &monitors, const QPoint &anchor);
  }
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// autotests/ClientDisplayInfoTest.cpp
// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "ClientDisplayInfo.h"

using namespace KRdp;
using namespace KRdp::ClientDisplay;

class ClientDisplayInfoTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void usableBounds()
    {
        QVERIFY(usable(QSize(640, 640)));
        QVERIFY(usable(QSize(4096, 4096)));
        QVERIFY(!usable(QSize(639, 1080)));
        QVERIFY(!usable(QSize(1920, 4097)));
        QVERIFY(!usable(QSize()));
    }

    void nameEncodesIndexAndSize()
    {
        QCOMPARE(virtualMonitorName(0, QSize(1920, 1080)), QStringLiteral("krdp-m0-1920x1080"));
        QCOMPARE(virtualMonitorName(1, QSize(1920, 1280)), QStringLiteral("krdp-m1-1920x1280"));
    }

    void sanitizeFallsBackOnUnusableDesktop()
    {
        const auto out = sanitize(Info{QSize(100, 100), {}}, QSize(1920, 1080));
        QCOMPARE(out.desktopSize, QSize(1920, 1080));
        QVERIFY(out.monitors.isEmpty());
    }

    void sanitizeKeepsUsableDesktop()
    {
        const auto out = sanitize(Info{QSize(2560, 1440), {}}, QSize(1920, 1080));
        QCOMPARE(out.desktopSize, QSize(2560, 1440));
    }

    void singleMonitorListIsDropped()
    {
        // One monitor carries no more information than the desktop size.
        const auto out = sanitize(Info{QSize(1920, 1080), {{QRect(0, 0, 1920, 1080), true}}}, QSize(1920, 1080));
        QVERIFY(out.monitors.isEmpty());
        QCOMPARE(out.desktopSize, QSize(1920, 1080));
    }

    void twoMonitorsAreNormalisedToOrigin()
    {
        // Laptop panel left, portable monitor right, as a client advertises with negative coordinates.
        const auto out = sanitize(Info{QSize(3840, 1280), {{QRect(-1920, 0, 1920, 1080), false}, {QRect(0, -100, 1920, 1280), true}}}, QSize(1920, 1080));
        QCOMPARE(out.monitors.size(), 2);
        QCOMPARE(out.monitors[0].geometry, QRect(0, 100, 1920, 1080));
        QCOMPARE(out.monitors[1].geometry, QRect(1920, 0, 1920, 1280));
        QVERIFY(out.monitors[1].primary);
        QCOMPARE(out.desktopSize, QSize(3840, 1280)); // union of the monitors, not the advertised size
    }

    void monitorsWithoutExactlyOnePrimaryAreDropped()
    {
        const auto none = sanitize(Info{QSize(3840, 1080), {{QRect(0, 0, 1920, 1080), false}, {QRect(1920, 0, 1920, 1080), false}}}, QSize(1920, 1080));
        QVERIFY(none.monitors.isEmpty());
        const auto two = sanitize(Info{QSize(3840, 1080), {{QRect(0, 0, 1920, 1080), true}, {QRect(1920, 0, 1920, 1080), true}}}, QSize(1920, 1080));
        QVERIFY(two.monitors.isEmpty());
    }

    void unusableMonitorDropsTheWholeList()
    {
        const auto out = sanitize(Info{QSize(6016, 1080), {{QRect(0, 0, 1920, 1080), true}, {QRect(1920, 0, 4096 + 1, 1080), false}}}, QSize(1920, 1080));
        QVERIFY(out.monitors.isEmpty());
        QCOMPARE(out.desktopSize, QSize(1920, 1080)); // 6016 is unusable too -> fallback
    }

    void tooManyMonitorsDropsTheList()
    {
        Info info{QSize(4096, 1080), {}};
        for (int i = 0; i < MaxMonitors + 1; ++i) {
            info.monitors.push_back({QRect(i * 640, 0, 640, 640), i == 0});
        }
        QVERIFY(sanitize(info, QSize(1920, 1080)).monitors.isEmpty());
    }

    void placementTranslatesByAnchor()
    {
        const QVector<VideoMonitor> monitors{{QRect(0, 100, 1920, 1080), false}, {QRect(1920, 0, 1920, 1280), true}};
        const auto rects = placement(monitors, QPoint(5120, 0));
        QCOMPARE(rects.size(), 2);
        QCOMPARE(rects[0], QRect(5120, 100, 1920, 1080));
        QCOMPARE(rects[1], QRect(7040, 0, 1920, 1280));
        QCOMPARE(placement(monitors, QPoint(0, 0))[0], QRect(0, 100, 1920, 1080));
    }
};

QTEST_GUILESS_MAIN(ClientDisplayInfoTest)

#include "ClientDisplayInfoTest.moc"
```

Append to `autotests/CMakeLists.txt`:
```cmake
add_executable(ClientDisplayInfoTest ClientDisplayInfoTest.cpp)
target_link_libraries(ClientDisplayInfoTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ClientDisplayInfoTest COMMAND ClientDisplayInfoTest)
```

- [ ] **Step 2: Run, expect failure**

Run: `cmake --build build -j16 --target ClientDisplayInfoTest 2>&1 | tail -3`
Expected: compile error, `ClientDisplayInfo.h: No such file or directory`.

- [ ] **Step 3: Implement `src/ClientDisplayInfo.h`**

```cpp
// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

#include "SurfaceLayout.h"

namespace KRdp
{
/**
 * What an RDP client says about its own display, and the pure rules that turn
 * it into virtual-monitor requests (OPT-041). No Qt GUI, no FreeRDP: the
 * connection fills Info from the peer settings and the controller consumes it.
 */
namespace ClientDisplay
{
/** Below this a desktop is unusable (RDP clients send 0x0 when they do not know). */
constexpr int MinDimension = 640;
/** Per-output VA-API surface limit; the same value as MultiLayout::MaxEncodeDimension. */
constexpr int MaxDimension = 4096;
/** RDPGFX allows 16 monitors; so does MultiLayout::MaxMonitorCount. */
constexpr int MaxMonitors = 16;

struct Info {
    /** TS_UD_CS_CORE desktopWidth/desktopHeight. */
    QSize desktopSize;
    /**
     * TS_UD_CS_MONITOR entries in the client's own coordinate space (after
     * sanitize(): translated so the union's top-left is (0,0)). Empty when the
     * client advertised none or the list was unusable; then desktopSize is the
     * only request.
     */
    QVector<VideoMonitor> monitors;

    bool operator==(const Info &other) const = default;
};

inline bool usable(const QSize &size)
{
    return size.width() >= MinDimension && size.width() <= MaxDimension && size.height() >= MinDimension && size.height() <= MaxDimension;
}

/**
 * The name requested from KWin for client monitor \a index at \a size. Stable
 * per (index, size) on purpose: KWin remembers output arrangements keyed on the
 * set of present outputs, so a stable name keeps that list bounded and lets the
 * server re-assert the physical layout on every connect instead of chasing a
 * new identity each time. KWin exposes the output as "Virtual-<name>".
 */
inline QString virtualMonitorName(int index, const QSize &size)
{
    return QStringLiteral("krdp-m%1-%2x%3").arg(index).arg(size.width()).arg(size.height());
}

/**
 * Apply the size rules: an unusable desktop becomes \a fallback; a monitor list
 * is kept only when it has 2..MaxMonitors entries, every entry is usable and
 * exactly one is primary, in which case it is translated to a (0,0) origin and
 * the desktop size becomes the union's size (what RDPGFX will be told anyway).
 */
inline Info sanitize(Info info, const QSize &fallback)
{
    const bool listUsable = info.monitors.size() >= 2 && info.monitors.size() <= MaxMonitors
        && std::all_of(info.monitors.cbegin(), info.monitors.cend(), [](const VideoMonitor &m) {
               return usable(m.geometry.size());
           })
        && std::count_if(info.monitors.cbegin(), info.monitors.cend(), [](const VideoMonitor &m) {
               return m.primary;
           }) == 1;
    if (!listUsable) {
        info.monitors.clear();
        if (!usable(info.desktopSize)) {
            info.desktopSize = fallback;
        }
        return info;
    }

    QRect unionRect;
    for (const auto &monitor : std::as_const(info.monitors)) {
        unionRect |= monitor.geometry;
    }
    for (auto &monitor : info.monitors) {
        monitor.geometry.translate(-unionRect.topLeft());
    }
    info.desktopSize = unionRect.size();
    return info;
}

/** KWin-global logical rects for sanitised \a monitors placed with their union's top-left at \a anchor. */
inline QVector<QRect> placement(const QVector<VideoMonitor> &monitors, const QPoint &anchor)
{
    QVector<QRect> rects;
    rects.reserve(monitors.size());
    for (const auto &monitor : monitors) {
        rects.push_back(monitor.geometry.translated(anchor));
    }
    return rects;
}
}
}
```

Add `#include <algorithm>` and `#include <utility>` at the top (for `std::all_of`, `std::count_if`, `std::as_const`).

- [ ] **Step 4: Run, expect pass**

Run: `cmake --build build -j16 --target ClientDisplayInfoTest && ctest --test-dir build -R ClientDisplayInfoTest --output-on-failure`
Expected: `1/1 Test #N: ClientDisplayInfoTest ... Passed`, 10 test functions, no warnings.

- [ ] **Step 5: Commit**

```bash
git add src/ClientDisplayInfo.h autotests/ClientDisplayInfoTest.cpp autotests/CMakeLists.txt
git commit -m "client display: pure model of the client's advertised desktop and monitors (OPT-041)"
```

---

### Task 2: Read the client's display info in `RdpConnection`

**Files:**
- Modify: `src/RdpConnection.h` (public API + signal), `src/RdpConnection.cpp` (`onCapabilities()` at ~608-631, the `Private` struct)

**Interfaces:**
- Consumes: `KRdp::ClientDisplay::Info` (Task 1).
- Produces:
  ```cpp
  /** The display the client asked for in its connect data. Valid once clientDisplayInfoReceived() fired; a copy, safe from any thread. */
  ClientDisplay::Info clientDisplayInfo() const;
  /** Emitted on the session thread, once per connection, at the end of the capabilities exchange. Connect with Qt::QueuedConnection. */
  Q_SIGNAL void clientDisplayInfoReceived();
  ```

- [ ] **Step 1: Add the API**

In `src/RdpConnection.h`: `#include "ClientDisplayInfo.h"`; in the public section after `Clipboard *clipboard() const;` add the two declarations above (with their doc comments).

In `src/RdpConnection.cpp`, in `class RdpConnection::Private` add:
```cpp
    std::mutex clientDisplayMutex;
    ClientDisplay::Info clientDisplay;
```
(add `#include <mutex>`), and the getter:
```cpp
ClientDisplay::Info RdpConnection::clientDisplayInfo() const
{
    std::lock_guard lock(d->clientDisplayMutex);
    return d->clientDisplay;
}
```

- [ ] **Step 2: Fill it in `onCapabilities()`**

Just before the final `return true;` of `RdpConnection::onCapabilities()`:
```cpp
    ClientDisplay::Info info;
    info.desktopSize = QSize(int(freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth)), int(freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight)));
    const auto monitorCount = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
    for (UINT32 i = 0; i < monitorCount; ++i) {
        const auto *monitor = static_cast<const rdpMonitor *>(freerdp_settings_get_pointer_array(settings, FreeRDP_MonitorDefArray, i));
        if (!monitor) {
            break;
        }
        info.monitors.push_back(VideoMonitor{
            .geometry = QRect(monitor->x, monitor->y, monitor->width, monitor->height),
            .primary = monitor->is_primary != 0,
        });
    }
    {
        std::lock_guard lock(d->clientDisplayMutex);
        d->clientDisplay = info;
    }
    qCInfo(KRDP) << "Client display: desktop" << info.desktopSize << "monitors" << info.monitors.size()
                 << "monitorLayoutPdu" << freerdp_settings_get_bool(settings, FreeRDP_SupportMonitorLayoutPdu);
    Q_EMIT clientDisplayInfoReceived();
```
`rdpMonitor` (`x, y, width, height, is_primary`) is in `freerdp/settings_types.h`; `freerdp_settings_get_pointer_array(settings, id, offset)` in `freerdp/settings.h`.

- [ ] **Step 3: Build and observe on a test instance**

Run: `cmake --build build -j16 2>&1 | grep -E "warning|error" ; echo build-done`
Expected: no warnings/errors.

Start a test instance on :3392 (Global Constraints recipe, `MonitorMode=specific`), connect once from buzz: `ssh westers@buzz.local 'WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 ~/.local/bin/krdp-client --headless --host hal9000.local --port 3392 --user krdptest --password krdptest --seconds 8'`, then `grep "Client display" /tmp/krdp-v/server.log`.
Expected: one line like `Client display: desktop QSize(1920, 1080) monitors 0 monitorLayoutPdu true` (the own client sends its screen or 1920x1080 fallback; Remmina would show its configured resolution). Stop the instance (`kill $(cat /tmp/krdp-v/server.pid)`).

- [ ] **Step 4: Run the full test suite, commit**

Run: `ctest --test-dir build --output-on-failure | tail -3` → all pass.
```bash
git add src/RdpConnection.h src/RdpConnection.cpp
git commit -m "rdp: read the client's desktop size and monitor layout after capabilities (OPT-041)"
```

---

### Task 3: Virtual output geometry resolution and input gating in `PlasmaScreencastV1Session`

**Files:**
- Modify: `src/AbstractSession.h` (two signals + one virtual), `src/PlasmaScreencastV1Session.h` (private helpers), `src/PlasmaScreencastV1Session.cpp` (`Private` members ~298-310, ctor ~320-340, `setupScreencastRequest()` ~410-530, `sendEvent()` ~627-650, `outputGeometry()` ~766)

**Interfaces:**
- Produces (on `AbstractSession`):
  ```cpp
  /** KWin-global logical rect of this session's captured output changed (virtual outputs: first known some time after start). */
  Q_SIGNAL void outputGeometryChanged(const QRect &geometry);
  /** The virtual output requested from KWin never showed up as a QScreen (5 s). Input stays gated; the stream may still run. */
  Q_SIGNAL void virtualOutputUnresolved();
  /** False only for a virtual-monitor session whose QScreen has not been found yet. */
  virtual bool outputGeometryResolved() const { return true; }
  ```
- Consumes: `QScreen::name()` of the virtual output = `QStringLiteral("Virtual-") + virtualMonitor()->name`.

- [ ] **Step 1: Verify KWin's naming (evidence for the constant)**

With no client on 3389/3391 (Global Constraints), start a test instance with `--virtual-monitor 1280x720@1` on :3392, connect the own client headless for 20 s from buzz, and while connected run `kscreen-doctor -o | grep -A1 Virtual`.
Expected: `Output: 3 Virtual-1280x720@1 …` — the prefix is `Virtual-` followed by the requested name verbatim. If it is anything else, use what you see for `VirtualOutputPrefix` below and say so in the report. Stop the instance; confirm `kscreen-doctor -o` shows only DP-1 and HDMI-A-1.

- [ ] **Step 2: Signals on `AbstractSession`**

In `src/AbstractSession.h`, next to the existing `Q_SIGNAL`s (`clipboardDataChanged` etc.), add the two signals and the virtual method from Interfaces (with `#include <QRect>`).

- [ ] **Step 3: Private state and helpers in the Plasma session**

`src/PlasmaScreencastV1Session.cpp`, in `Private` (after `QTimer streamRestartTimer;`):
```cpp
    // Virtual-monitor target only: the QScreen KWin created for our request.
    // Input is mapped through logicalRect, which for a virtual output is only
    // known once that screen exists (KWin places it, we do not).
    QString virtualScreenName;
    QPointer<QScreen> virtualScreen;
    bool virtualGeometryResolved = false;
    QMetaObject::Connection virtualScreenAddedConnection;
    QMetaObject::Connection virtualScreenRemovedConnection;
    QTimer virtualScreenTimer;
    bool loggedGatedInput = false;
```
Constants at file scope with the other `constexpr int`s:
```cpp
// How long a requested virtual output may take to show up as a QScreen. The
// spike saw it within one screencast round trip; 5 s is generous.
constexpr int VirtualScreenTimeoutMs = 5000;
const QLatin1String VirtualOutputPrefix("Virtual-");
```
In the constructor, after the `streamRestartTimer` setup:
```cpp
    d->virtualScreenTimer.setSingleShot(true);
    connect(&d->virtualScreenTimer, &QTimer::timeout, this, [this]() {
        qCWarning(KRDP) << "Virtual output" << d->virtualScreenName << "did not appear within" << VirtualScreenTimeoutMs << "ms; input stays gated";
        Q_EMIT virtualOutputUnresolved();
    });
```
Private member functions (declare in `src/PlasmaScreencastV1Session.h` under `private:`):
```cpp
    void watchForVirtualScreen();
    bool adoptVirtualScreen(QScreen *screen);
    void updateVirtualGeometry(const QRect &geometry);
```
Definitions:
```cpp
void PlasmaScreencastV1Session::watchForVirtualScreen()
{
    const auto vm = virtualMonitor();
    d->virtualScreenName = VirtualOutputPrefix + vm->name;
    d->virtualGeometryResolved = false;
    d->virtualScreen = nullptr;
    disconnect(d->virtualScreenAddedConnection);
    disconnect(d->virtualScreenRemovedConnection);

    d->virtualScreenRemovedConnection = connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
        if (screen != d->virtualScreen) {
            return;
        }
        // KWin dropped our output (stream closed, or a stale same-name output
        // going away before the new one arrives). Wait for it to come back.
        qCInfo(KRDP) << "Virtual output" << d->virtualScreenName << "removed; waiting for it to reappear";
        d->virtualScreen = nullptr;
        d->virtualGeometryResolved = false;
        d->virtualScreenTimer.start(VirtualScreenTimeoutMs);
    });
    d->virtualScreenAddedConnection = connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
        adoptVirtualScreen(screen);
    });

    for (auto *screen : qGuiApp->screens()) {
        if (adoptVirtualScreen(screen)) {
            return;
        }
    }
    d->virtualScreenTimer.start(VirtualScreenTimeoutMs);
}

bool PlasmaScreencastV1Session::adoptVirtualScreen(QScreen *screen)
{
    if (!screen || screen->name() != d->virtualScreenName || d->virtualScreen == screen) {
        return false;
    }
    d->virtualScreenTimer.stop();
    d->virtualScreen = screen;
    d->virtualGeometryResolved = true;
    connect(screen, &QScreen::geometryChanged, this, [this, screen](const QRect &geometry) {
        if (screen == d->virtualScreen) {
            updateVirtualGeometry(geometry);
        }
    });
    updateVirtualGeometry(screen->geometry());
    return true;
}

void PlasmaScreencastV1Session::updateVirtualGeometry(const QRect &geometry)
{
    if (geometry.isEmpty() || d->logicalRect == geometry) {
        return;
    }
    // logicalRect is KWin-global (input mapping); the monitor layout handed
    // to the RDP side stays local to this output, as for a physical one.
    d->logicalRect = geometry;
    d->monitorLayout = {
        VideoMonitor{
            .geometry = QRect(QPoint(0, 0), geometry.size()),
            .primary = true,
        },
    };
    qCInfo(KRDP) << "Virtual output" << d->virtualScreenName << "resolved at" << geometry;
    Q_EMIT outputGeometryChanged(geometry);
}

bool PlasmaScreencastV1Session::outputGeometryResolved() const
{
    return !virtualMonitor() || d->virtualGeometryResolved;
}
```
(`outputGeometryResolved` is declared `bool outputGeometryResolved() const override;` in the header's public section.)

- [ ] **Step 4: Hook the virtual branch of `setupScreencastRequest()`**

Right after the existing line `qCDebug(KRDP) << "Using virtual monitor stream" << vm->name << "logical rect" << d->logicalRect;` add `watchForVirtualScreen();`. The provisional `targetLogicalRect = QRect(QPoint(0, 0), vm->size)` stays (it sizes the stream); `logicalRectChanged` in the recreate decision must not be tripped by KWin's placement: change that line to
```cpp
    const bool logicalRectChanged = (target != Private::StreamTarget::Virtual) && (d->logicalRect != targetLogicalRect);
```
and only assign `d->logicalRect = targetLogicalRect;` when `!(target == Private::StreamTarget::Virtual && d->virtualGeometryResolved)` (a resolved virtual session keeps the real rect across a no-op refresh).

- [ ] **Step 5: Gate input until resolved**

At the top of `PlasmaScreencastV1Session::sendEvent()` after the `isActive()` check:
```cpp
    if (virtualMonitor() && !d->virtualGeometryResolved && event->type() == QEvent::MouseMove) {
        // Until KWin tells us where the virtual output sits, a pointer position
        // would be mapped onto (0,0) - Steve's real primary monitor. Drop it.
        if (!d->loggedGatedInput) {
            d->loggedGatedInput = true;
            qCInfo(KRDP) << "Dropping pointer motion until the virtual output geometry is known";
        }
        return;
    }
```
Reset `d->loggedGatedInput = false;` inside `adoptVirtualScreen()`.

- [ ] **Step 6: Build, unit tests, then the input acceptance on the test instance**

`cmake --build build -j16 2>&1 | grep -E "warning|error"` → nothing. `ctest --test-dir build` → all pass.

Acceptance (the spike's follow-up, which FAILED before this task): no client on 3389/3391. Start :3392 with `--virtual-monitor 1920x1080@1`. From buzz run the GUI client with the pointer debug action the Task 5 client implementer added (`ssh westers@buzz.local '~/.local/bin/krdp-client --help | grep -i debug'` for the exact syntax; it moves the remote pointer to client pixel (1234,567) and the run opens a window on Steve's laptop for ~40 s — say so in the report). While connected, read KWin's cursor position with a KWin script (`~/dev/krdp/.superpowers/sdd/spike-opt-041-virtual-monitor/` has the script the spike used; reuse it).
Expected: server log has `Virtual output "Virtual-1920x1080@1" resolved at QRect(5120,0 1920x1080)`; cursor reads `(6354, 567)` = 5120+1234, not `(1234, 567)`. Stop the instance; `kscreen-doctor -o` shows only the two physical outputs.

- [ ] **Step 7: Commit**

```bash
git add src/AbstractSession.h src/PlasmaScreencastV1Session.h src/PlasmaScreencastV1Session.cpp
git commit -m "plasma session: map input through the virtual output's real geometry (OPT-041)"
```

---

### Task 4: Pure output snapshot model (`OutputSnapshot.h`)

**Files:**
- Create: `server/OutputSnapshot.h`
- Test: `autotests/OutputSnapshotTest.cpp`
- Modify: `autotests/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  namespace KRdp::OutputSnapshot {
  const QLatin1String VirtualPrefix("Virtual-");
  struct Output { QString name; bool enabled = false; QPoint position; int priority = 0; QSize size; bool operator==(const Output &) const = default; };
  struct Placement { QString name; QPoint position; };     // name = KWin name incl. prefix
  inline bool isVirtual(const QString &name);
  inline QVector<Output> parse(const QByteArray &kscreenJson, QString *error = nullptr);  // connected outputs only
  inline QVector<Output> physicalOnly(const QVector<Output> &outputs);
  inline QRect enabledUnion(const QVector<Output> &outputs);
  inline QStringList replaceArgs(const QVector<Output> &physical, const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName);
  inline QStringList positionArgs(const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName, int firstPriority);
  inline QStringList restoreArgs(const QVector<Output> &physical);
  inline bool matches(const QVector<Output> &snapshot, const QVector<Output> &current);   // physical names/enabled/position/priority equal
  inline QByteArray toJson(const QVector<Output> &outputs);
  inline QVector<Output> fromJson(const QByteArray &json);
  }
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// autotests/OutputSnapshotTest.cpp
// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "OutputSnapshot.h"

using namespace KRdp::OutputSnapshot;

namespace
{
// Trimmed from a real `kscreen-doctor -j` on hal9000 (2026-09-17), plus one virtual output.
const QByteArray kscreenJson = R"({
  "features": 3,
  "outputs": [
    {"id": 1, "name": "DP-1", "enabled": true, "connected": true, "priority": 1, "pos": {"x": 0, "y": 0}, "size": {"width": 2560, "height": 1440}, "scale": 1, "type": 14},
    {"id": 2, "name": "HDMI-A-1", "enabled": true, "connected": true, "priority": 2, "pos": {"x": 2560, "y": 0}, "size": {"width": 2560, "height": 1440}, "scale": 1, "type": 6},
    {"id": 3, "name": "Virtual-krdp-m0-1920x1080", "enabled": true, "connected": true, "priority": 3, "pos": {"x": 5120, "y": 0}, "size": {"width": 1920, "height": 1080}, "scale": 1, "type": 0},
    {"id": 4, "name": "DP-2", "enabled": false, "connected": false, "priority": 0, "pos": {"x": 0, "y": 0}, "size": {"width": 0, "height": 0}, "scale": 1, "type": 14}
  ],
  "screen": {"currentSize": {"height": 1440, "width": 7040}}
})";
}

class OutputSnapshotTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void parsesConnectedOutputs()
    {
        QString error;
        const auto outputs = parse(kscreenJson, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(outputs.size(), 3); // DP-2 is not connected
        QCOMPARE(outputs[0], (Output{QStringLiteral("DP-1"), true, QPoint(0, 0), 1, QSize(2560, 1440)}));
        QCOMPARE(outputs[1], (Output{QStringLiteral("HDMI-A-1"), true, QPoint(2560, 0), 2, QSize(2560, 1440)}));
        QCOMPARE(outputs[2].name, QStringLiteral("Virtual-krdp-m0-1920x1080"));
    }

    void parseReportsGarbage()
    {
        QString error;
        QVERIFY(parse("not json", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(parse(R"({"outputs": 5})", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void physicalFilterAndUnion()
    {
        const auto physical = physicalOnly(parse(kscreenJson));
        QCOMPARE(physical.size(), 2);
        QVERIFY(!isVirtual(physical[0].name));
        QVERIFY(isVirtual(QStringLiteral("Virtual-krdp-m0-1920x1080")));
        QCOMPARE(enabledUnion(physical), QRect(0, 0, 5120, 1440));
        auto oneOff = physical;
        oneOff[1].enabled = false;
        QCOMPARE(enabledUnion(oneOff), QRect(0, 0, 2560, 1440));
    }

    void replaceArgsDisablePhysicalsAndPlaceVirtuals()
    {
        const auto physical = physicalOnly(parse(kscreenJson));
        const auto args = replaceArgs(physical,
                                      {{QStringLiteral("Virtual-krdp-m0-1920x1080"), QPoint(0, 0)}, {QStringLiteral("Virtual-krdp-m1-1920x1280"), QPoint(1920, 0)}},
                                      QStringLiteral("Virtual-krdp-m1-1920x1280"));
        const QStringList expected{
            QStringLiteral("output.Virtual-krdp-m1-1920x1280.priority.1"),
            QStringLiteral("output.Virtual-krdp-m1-1920x1280.position.1920,0"),
            QStringLiteral("output.Virtual-krdp-m0-1920x1080.priority.2"),
            QStringLiteral("output.Virtual-krdp-m0-1920x1080.position.0,0"),
            QStringLiteral("output.DP-1.disable"),
            QStringLiteral("output.HDMI-A-1.disable"),
        };
        QCOMPARE(args, expected);
    }

    void replaceArgsSkipAlreadyDisabledPhysicals()
    {
        auto physical = physicalOnly(parse(kscreenJson));
        physical[1].enabled = false;
        const auto args = replaceArgs(physical, {{QStringLiteral("Virtual-krdp-m0-1920x1080"), QPoint(0, 0)}}, QStringLiteral("Virtual-krdp-m0-1920x1080"));
        QVERIFY(!args.contains(QStringLiteral("output.HDMI-A-1.disable")));
        QVERIFY(args.contains(QStringLiteral("output.DP-1.disable")));
    }

    void positionArgsForExtend()
    {
        const auto args = positionArgs({{QStringLiteral("Virtual-krdp-m0-1920x1080"), QPoint(5120, 100)}, {QStringLiteral("Virtual-krdp-m1-1920x1280"), QPoint(7040, 0)}},
                                       QStringLiteral("Virtual-krdp-m1-1920x1280"),
                                       3);
        const QStringList expected{
            QStringLiteral("output.Virtual-krdp-m1-1920x1280.priority.3"),
            QStringLiteral("output.Virtual-krdp-m1-1920x1280.position.7040,0"),
            QStringLiteral("output.Virtual-krdp-m0-1920x1080.priority.4"),
            QStringLiteral("output.Virtual-krdp-m0-1920x1080.position.5120,100"),
        };
        QCOMPARE(args, expected);
    }

    void restoreArgsReenableExactly()
    {
        auto physical = physicalOnly(parse(kscreenJson));
        physical[1].enabled = false; // Steve had HDMI off before the session: it stays off
        const auto args = restoreArgs(physical);
        const QStringList expected{
            QStringLiteral("output.DP-1.enable"),
            QStringLiteral("output.DP-1.position.0,0"),
            QStringLiteral("output.DP-1.priority.1"),
            QStringLiteral("output.HDMI-A-1.disable"),
        };
        QCOMPARE(args, expected);
    }

    void matchesIgnoresVirtualsAndSize()
    {
        const auto all = parse(kscreenJson);
        const auto physical = physicalOnly(all);
        QVERIFY(matches(physical, all)); // extra virtual output in "current" is fine
        auto moved = physical;
        moved[1].position = QPoint(0, 1440);
        QVERIFY(!matches(physical, moved));
        auto off = physical;
        off[0].enabled = false;
        QVERIFY(!matches(physical, off));
        auto resized = physical;
        resized[0].size = QSize(1920, 1080);
        QVERIFY(matches(physical, resized));
    }

    void jsonRoundTrip()
    {
        const auto physical = physicalOnly(parse(kscreenJson));
        QCOMPARE(fromJson(toJson(physical)), physical);
        QVERIFY(fromJson("garbage").isEmpty());
    }
};

QTEST_GUILESS_MAIN(OutputSnapshotTest)

#include "OutputSnapshotTest.moc"
```

CMake: `add_executable(OutputSnapshotTest OutputSnapshotTest.cpp)` + `target_link_libraries(OutputSnapshotTest PRIVATE Qt6::Test Qt6::Core)` + `add_test(NAME OutputSnapshotTest COMMAND OutputSnapshotTest)`. If `autotests/CMakeLists.txt` does not already add `${CMAKE_SOURCE_DIR}/server` to the include path (check how `MultiLayoutTest` finds `MultiLayout.h`), do it the same way.

- [ ] **Step 2: Run, expect failure** — `cmake --build build -j16 --target OutputSnapshotTest 2>&1 | tail -3` → `OutputSnapshot.h: No such file or directory`.

- [ ] **Step 3: Implement `server/OutputSnapshot.h`**

```cpp
// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace KRdp
{
/**
 * Pure model of KWin's output arrangement as `kscreen-doctor -j` reports it,
 * and the `kscreen-doctor` argument lists that change it (OPT-041). No
 * process is run here; PhysicalOutputGuard does that.
 */
namespace OutputSnapshot
{
/** KWin names screencast-created outputs "Virtual-<requested name>". */
const QLatin1String VirtualPrefix("Virtual-");

struct Output {
    QString name;
    bool enabled = false;
    QPoint position;
    int priority = 0;
    QSize size;

    bool operator==(const Output &other) const = default;
};

struct Placement {
    /** KWin output name, prefix included. */
    QString name;
    QPoint position;
};

inline bool isVirtual(const QString &name)
{
    return name.startsWith(VirtualPrefix);
}

/** Connected outputs from `kscreen-doctor -j`. Empty + \a error set on malformed input. */
inline QVector<Output> parse(const QByteArray &kscreenJson, QString *error = nullptr)
{
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(kscreenJson, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        if (error) {
            *error = QStringLiteral("kscreen-doctor output is not a JSON object: %1").arg(parseError.errorString());
        }
        return {};
    }
    const auto outputsValue = doc.object().value(QLatin1String("outputs"));
    if (!outputsValue.isArray()) {
        if (error) {
            *error = QStringLiteral("kscreen-doctor output has no \"outputs\" array");
        }
        return {};
    }

    QVector<Output> outputs;
    for (const auto &entry : outputsValue.toArray()) {
        const auto object = entry.toObject();
        if (!object.value(QLatin1String("connected")).toBool(false)) {
            continue;
        }
        const auto pos = object.value(QLatin1String("pos")).toObject();
        const auto size = object.value(QLatin1String("size")).toObject();
        outputs.push_back(Output{
            .name = object.value(QLatin1String("name")).toString(),
            .enabled = object.value(QLatin1String("enabled")).toBool(false),
            .position = QPoint(pos.value(QLatin1String("x")).toInt(), pos.value(QLatin1String("y")).toInt()),
            .priority = object.value(QLatin1String("priority")).toInt(),
            .size = QSize(size.value(QLatin1String("width")).toInt(), size.value(QLatin1String("height")).toInt()),
        });
    }
    if (error) {
        error->clear();
    }
    return outputs;
}

inline QVector<Output> physicalOnly(const QVector<Output> &outputs)
{
    QVector<Output> physical;
    for (const auto &output : outputs) {
        if (!isVirtual(output.name)) {
            physical.push_back(output);
        }
    }
    return physical;
}

/** Bounding rect of the enabled outputs; invalid when none is enabled. */
inline QRect enabledUnion(const QVector<Output> &outputs)
{
    QRect rect;
    for (const auto &output : outputs) {
        if (output.enabled) {
            rect |= QRect(output.position, output.size);
        }
    }
    return rect;
}

/**
 * Give the virtual outputs their places and priorities (primary first at
 * \a firstPriority, the rest in list order after it).
 */
inline QStringList positionArgs(const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName, int firstPriority)
{
    QStringList args;
    int priority = firstPriority;
    auto emit = [&](const Placement &placement) {
        args << QStringLiteral("output.%1.priority.%2").arg(placement.name).arg(priority++);
        args << QStringLiteral("output.%1.position.%2,%3").arg(placement.name).arg(placement.position.x()).arg(placement.position.y());
    };
    for (const auto &placement : virtualOutputs) {
        if (placement.name == primaryVirtualName) {
            emit(placement);
        }
    }
    for (const auto &placement : virtualOutputs) {
        if (placement.name != primaryVirtualName) {
            emit(placement);
        }
    }
    return args;
}

/**
 * `replace` policy in one kscreen-doctor invocation: the virtual outputs take
 * the top priorities and their positions, then every physical output that is
 * currently enabled is disabled. One invocation so KWin never sees a
 * configuration without an enabled output.
 */
inline QStringList replaceArgs(const QVector<Output> &physical, const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName)
{
    QStringList args = positionArgs(virtualOutputs, primaryVirtualName, 1);
    for (const auto &output : physical) {
        if (output.enabled) {
            args << QStringLiteral("output.%1.disable").arg(output.name);
        }
    }
    return args;
}

/** Put the physical outputs back exactly as the snapshot had them. */
inline QStringList restoreArgs(const QVector<Output> &physical)
{
    QStringList args;
    for (const auto &output : physical) {
        if (!output.enabled) {
            args << QStringLiteral("output.%1.disable").arg(output.name);
            continue;
        }
        args << QStringLiteral("output.%1.enable").arg(output.name);
        args << QStringLiteral("output.%1.position.%2,%3").arg(output.name).arg(output.position.x()).arg(output.position.y());
        args << QStringLiteral("output.%1.priority.%2").arg(output.name).arg(output.priority);
    }
    return args;
}

/** Every physical output of \a snapshot is present in \a current with the same enabled/position/priority (size and virtual outputs ignored). */
inline bool matches(const QVector<Output> &snapshot, const QVector<Output> &current)
{
    for (const auto &wanted : snapshot) {
        if (isVirtual(wanted.name)) {
            continue;
        }
        const auto it = std::find_if(current.cbegin(), current.cend(), [&wanted](const Output &candidate) {
            return candidate.name == wanted.name;
        });
        if (it == current.cend() || it->enabled != wanted.enabled || (wanted.enabled && (it->position != wanted.position || it->priority != wanted.priority))) {
            return false;
        }
    }
    return true;
}

inline QByteArray toJson(const QVector<Output> &outputs)
{
    QJsonArray array;
    for (const auto &output : outputs) {
        array.push_back(QJsonObject{
            {QLatin1String("name"), output.name},
            {QLatin1String("enabled"), output.enabled},
            {QLatin1String("x"), output.position.x()},
            {QLatin1String("y"), output.position.y()},
            {QLatin1String("priority"), output.priority},
            {QLatin1String("width"), output.size.width()},
            {QLatin1String("height"), output.size.height()},
        });
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

inline QVector<Output> fromJson(const QByteArray &json)
{
    const auto doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) {
        return {};
    }
    QVector<Output> outputs;
    for (const auto &entry : doc.array()) {
        const auto object = entry.toObject();
        outputs.push_back(Output{
            .name = object.value(QLatin1String("name")).toString(),
            .enabled = object.value(QLatin1String("enabled")).toBool(false),
            .position = QPoint(object.value(QLatin1String("x")).toInt(), object.value(QLatin1String("y")).toInt()),
            .priority = object.value(QLatin1String("priority")).toInt(),
            .size = QSize(object.value(QLatin1String("width")).toInt(), object.value(QLatin1String("height")).toInt()),
        });
    }
    return outputs;
}
}
}
```
(`#include <algorithm>` for `std::find_if`.)

- [ ] **Step 4: Run, expect pass** — `ctest --test-dir build -R OutputSnapshotTest --output-on-failure` → Passed, 9 functions.

- [ ] **Step 5: Commit**

```bash
git add server/OutputSnapshot.h autotests/OutputSnapshotTest.cpp autotests/CMakeLists.txt
git commit -m "server: pure kscreen output snapshot model and command builders (OPT-041)"
```

---

### Task 5: `PhysicalOutputGuard` — apply and restore the physical layout with kscreen-doctor

**Files:**
- Create: `server/PhysicalOutputGuard.h`, `server/PhysicalOutputGuard.cpp`
- Modify: `server/CMakeLists.txt` (`target_sources(krdpserver PRIVATE main.cpp SessionController.cpp DisplayWakeGuard.cpp PhysicalOutputGuard.cpp)`), `server/main.cpp` (`--restore-outputs` option + startup recovery; see Step 3)

**Interfaces:**
- Consumes: `KRdp::OutputSnapshot::*` (Task 4).
- Produces:
  ```cpp
  class PhysicalOutputGuard : public QObject {
      Q_OBJECT
  public:
      explicit PhysicalOutputGuard(QObject *parent = nullptr);
      ~PhysicalOutputGuard() override;                 // restore() if a snapshot is still held
      static QString stateFilePath();                  // $XDG_STATE_HOME/krdp-server/physical-outputs.json (app name is krdp-server, not krdpserver)
      static bool restoreFromStateFile();              // true = nothing to do or restored+verified
      bool available() const;                          // kscreen-doctor found in PATH
      bool snapshot();                                 // read + remember the physical outputs, write the state file
      bool hasSnapshot() const;
      QVector<KRdp::OutputSnapshot::Output> physicalOutputs() const;
      bool applyReplace(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
      bool positionOutputs(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
      bool reconcileExtend();                          // re-enable physicals KWin's remembered setup switched off
      bool restore();                                  // blocking restore + verify; one async retry after 2 s; deletes the state file on verified success
  Q_SIGNALS:
      void restored(bool verified);
  };
  ```

- [ ] **Step 1: Write the header**

```cpp
// server/PhysicalOutputGuard.h
// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include "OutputSnapshot.h"

/**
 * Switches Steve's physical monitors off while a virtual-monitor session runs
 * (`VirtualMonitorPolicy=replace`) and puts them back exactly as they were,
 * using `kscreen-doctor` as a child process (OPT-041).
 *
 * Every mutation is one kscreen-doctor invocation followed by a read-back and
 * comparison against the snapshot, so a failure is a logged fact, not a
 * silent wrong layout. The snapshot is also written to a state file before
 * anything is disabled; restoreFromStateFile() replays it after a crash (at
 * server start and via `krdpserver --restore-outputs`).
 *
 * All calls are synchronous on the main thread (a kscreen-doctor run takes
 * well under a second); the only asynchronous part is the single retry a
 * failed restore schedules.
 */
class PhysicalOutputGuard : public QObject
{
    Q_OBJECT
public:
    explicit PhysicalOutputGuard(QObject *parent = nullptr);
    ~PhysicalOutputGuard() override;

    static QString stateFilePath();
    static bool restoreFromStateFile();

    bool available() const;
    bool snapshot();
    bool hasSnapshot() const;
    QVector<KRdp::OutputSnapshot::Output> physicalOutputs() const;

    bool applyReplace(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
    bool positionOutputs(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
    bool reconcileExtend();
    bool restore();

Q_SIGNALS:
    void restored(bool verified);

private:
    static bool run(const QStringList &args, QByteArray *output = nullptr);
    static QVector<KRdp::OutputSnapshot::Output> current(QString *error = nullptr);
    static bool restoreSnapshot(const QVector<KRdp::OutputSnapshot::Output> &physical);

    QVector<KRdp::OutputSnapshot::Output> m_physical;
    bool m_held = false;
    bool m_retryScheduled = false;
};
```

- [ ] **Step 2: Write the implementation**

```cpp
// server/PhysicalOutputGuard.cpp
// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PhysicalOutputGuard.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

using namespace KRdp::OutputSnapshot;
using namespace Qt::StringLiterals;

namespace
{
constexpr int KscreenTimeoutMs = 5000;
constexpr int RestoreRetryMs = 2000;
const QString KscreenDoctor = u"kscreen-doctor"_s;
}

PhysicalOutputGuard::PhysicalOutputGuard(QObject *parent)
    : QObject(parent)
{
}

PhysicalOutputGuard::~PhysicalOutputGuard()
{
    if (m_held) {
        qWarning() << "Physical outputs still replaced at shutdown; restoring";
        restore();
    }
}

QString PhysicalOutputGuard::stateFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::StateLocation) + u"/physical-outputs.json"_s;
}

bool PhysicalOutputGuard::available() const
{
    return !QStandardPaths::findExecutable(KscreenDoctor).isEmpty();
}

bool PhysicalOutputGuard::run(const QStringList &args, QByteArray *output)
{
    QProcess process;
    process.setProgram(KscreenDoctor);
    process.setArguments(args);
    process.start();
    if (!process.waitForFinished(KscreenTimeoutMs)) {
        process.kill();
        qWarning() << "kscreen-doctor" << args.join(u' ') << "timed out";
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qWarning() << "kscreen-doctor" << args.join(u' ') << "failed:" << process.readAllStandardError().trimmed();
        return false;
    }
    if (output) {
        *output = process.readAllStandardOutput();
    }
    return true;
}

QVector<Output> PhysicalOutputGuard::current(QString *error)
{
    QByteArray json;
    if (!run({u"-j"_s}, &json)) {
        if (error) {
            *error = u"kscreen-doctor -j failed"_s;
        }
        return {};
    }
    return parse(json, error);
}

bool PhysicalOutputGuard::snapshot()
{
    QString error;
    const auto outputs = current(&error);
    if (outputs.isEmpty()) {
        qWarning() << "Cannot snapshot the physical outputs:" << error;
        return false;
    }
    m_physical = physicalOnly(outputs);
    if (m_physical.isEmpty()) {
        qWarning() << "No physical outputs to snapshot";
        return false;
    }

    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::StateLocation));
    QFile file(stateFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Cannot write" << file.fileName() << ":" << file.errorString() << "- a crash would leave the outputs as the session leaves them";
    } else {
        file.write(toJson(m_physical));
    }
    qInfo() << "Physical outputs snapshot:" << m_physical.size() << "outputs, enabled union" << enabledUnion(m_physical);
    return true;
}

bool PhysicalOutputGuard::hasSnapshot() const
{
    return !m_physical.isEmpty();
}

QVector<Output> PhysicalOutputGuard::physicalOutputs() const
{
    return m_physical;
}

bool PhysicalOutputGuard::applyReplace(const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName)
{
    if (!hasSnapshot()) {
        qWarning() << "applyReplace without a snapshot; refusing to touch the physical outputs";
        return false;
    }
    const auto args = replaceArgs(m_physical, virtualOutputs, primaryVirtualName);
    if (!run(args)) {
        // kscreen may refuse the combined change (priorities colliding with
        // outputs that are being disabled in the same config); apply it as
        // two steps instead, virtual outputs first so an enabled output always exists.
        qInfo() << "Combined replace refused; applying in two steps";
        if (!run(positionArgs(virtualOutputs, primaryVirtualName, 1)) || !run(replaceArgs(m_physical, {}, QString()))) {
            return false;
        }
    }
    m_held = true;

    const auto after = current();
    const bool anyPhysicalEnabled = std::any_of(after.cbegin(), after.cend(), [](const Output &o) {
        return !isVirtual(o.name) && o.enabled;
    });
    if (anyPhysicalEnabled) {
        qWarning() << "replace policy applied but a physical output is still enabled:" << after;
        return false;
    }
    qInfo() << "Physical outputs replaced by" << virtualOutputs.size() << "virtual output(s)";
    return true;
}

bool PhysicalOutputGuard::positionOutputs(const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName)
{
    // Virtual outputs sit behind the physical ones in priority (extend keeps
    // Steve's primary as the primary).
    const int firstPriority = int(m_physical.size()) + 1;
    return run(positionArgs(virtualOutputs, primaryVirtualName, firstPriority));
}

bool PhysicalOutputGuard::reconcileExtend()
{
    if (!hasSnapshot()) {
        return false;
    }
    const auto now = current();
    if (now.isEmpty()) {
        return false;
    }
    if (matches(m_physical, now)) {
        return true;
    }
    // KWin replayed a remembered arrangement for this output set (it does that
    // when the same set of outputs reappears); put the physical ones back.
    qInfo() << "Physical outputs drifted after the virtual output appeared; re-applying the snapshot";
    return run(restoreArgs(m_physical)) && matches(m_physical, current());
}

bool PhysicalOutputGuard::restoreSnapshot(const QVector<Output> &physical)
{
    if (physical.isEmpty()) {
        return true;
    }
    if (!run(restoreArgs(physical))) {
        return false;
    }
    return matches(physical, current());
}

bool PhysicalOutputGuard::restore()
{
    if (!hasSnapshot()) {
        return true;
    }
    const bool verified = restoreSnapshot(m_physical);
    if (verified) {
        m_held = false;
        QFile::remove(stateFilePath());
        qInfo() << "Physical outputs restored";
        Q_EMIT restored(true);
        return true;
    }

    const QString recovery = u"kscreen-doctor "_s + restoreArgs(m_physical).join(u' ');
    if (!m_retryScheduled) {
        m_retryScheduled = true;
        qCritical() << "Physical outputs NOT restored; retrying in" << RestoreRetryMs << "ms. Manual recovery:" << recovery;
        QTimer::singleShot(RestoreRetryMs, this, [this]() {
            m_retryScheduled = false;
            restore();
        });
    } else {
        qCritical() << "Physical outputs still not restored after the retry. Run:" << recovery << "(or krdpserver --restore-outputs)";
        Q_EMIT restored(false);
    }
    return false;
}

bool PhysicalOutputGuard::restoreFromStateFile()
{
    QFile file(stateFilePath());
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot read" << file.fileName() << ":" << file.errorString();
        return false;
    }
    const auto physical = fromJson(file.readAll());
    if (physical.isEmpty()) {
        qWarning() << "State file" << file.fileName() << "is unreadable; removing it";
        file.remove();
        return false;
    }
    qWarning() << "A previous krdpserver left the physical outputs replaced (state file present); restoring" << physical.size() << "outputs";
    if (!restoreSnapshot(physical)) {
        qCritical() << "Restore from" << file.fileName() << "failed. Run: kscreen-doctor" << restoreArgs(physical).join(u' ');
        return false;
    }
    file.remove();
    qInfo() << "Physical outputs restored from the state file";
    return true;
}
```
Notes for the implementer: `QStandardPaths::StateLocation` exists since Qt 6.7 and resolves to `~/.local/state/krdp-server` (as built: the app name set in `main.cpp` is `krdp-server`, not `krdpserver` as this note originally said). `operator<<(QDebug, Output)` is needed for the two `qWarning() << after` lines — add a small `inline QDebug operator<<(QDebug dbg, const KRdp::OutputSnapshot::Output &o)` at the bottom of `OutputSnapshot.h` (name, enabled, position, priority) with `#include <QDebug>`. `qWarning() << virtualOutputs.size()` is fine as-is.

- [ ] **Step 3: `--restore-outputs` and startup recovery in `server/main.cpp`**

Add the option next to `--virtual-monitor`:
```cpp
        {u"restore-outputs"_s, u"Re-enable the physical outputs a crashed virtual-monitor session left disabled, then exit."_s},
```
Right after `parser.process(application); about.processCommandLine(&parser);`:
```cpp
    if (parser.isSet(u"restore-outputs"_s)) {
        return PhysicalOutputGuard::restoreFromStateFile() ? 0 : 1;
    }
```
And after the `SessionController controller(...)` construction:
```cpp
    // A crash with MonitorMode=virtual/replace leaves the state file behind;
    // a clean session deletes it. Nothing to do in the common case.
    PhysicalOutputGuard::restoreFromStateFile();
```
(`#include "PhysicalOutputGuard.h"`.)

- [ ] **Step 4: Build and exercise the state-file path (no display change needed for the first check)**

`cmake --build build -j16 2>&1 | grep -E "warning|error"` → nothing.

Recovery test WITHOUT disabling anything (a hand-written state file that already matches reality is a no-op restore that still exercises the code; as built the state directory is `~/.local/state/krdp-server`, not `~/.local/state/krdpserver`): `mkdir -p ~/.local/state/krdp-server && printf '[{"name":"DP-1","enabled":true,"x":0,"y":0,"priority":1,"width":2560,"height":1440},{"name":"HDMI-A-1","enabled":true,"x":2560,"y":0,"priority":2,"width":2560,"height":1440}]' > ~/.local/state/krdp-server/physical-outputs.json && ./build/bin/krdpserver --restore-outputs; echo "exit $?"; ls ~/.local/state/krdp-server/`
Expected: log `restoring 2 outputs` then `Physical outputs restored from the state file`, exit 0, the file is gone, `kscreen-doctor -o` unchanged.

Real toggle test (Global Constraints apply — no client on 3389/3391): with the same file but `"enabled":true` for HDMI-A-1 while HDMI-A-1 is actually disabled: `kscreen-doctor output.HDMI-A-1.disable; sleep 2; <write file>; ./build/bin/krdpserver --restore-outputs; sleep 2; kscreen-doctor -o | grep -A2 HDMI`
Expected: HDMI-A-1 enabled again at 2560,0 priority 2. If not, run the manual recovery command from the Global Constraints and report the failure.

- [ ] **Step 5: Full tests, commit**

`ctest --test-dir build --output-on-failure | tail -3` → all pass.
```bash
git add server/PhysicalOutputGuard.h server/PhysicalOutputGuard.cpp server/OutputSnapshot.h server/CMakeLists.txt server/main.cpp
git commit -m "server: PhysicalOutputGuard replaces/restores physical outputs via kscreen-doctor, with crash recovery (OPT-041)"
```

---

### Task 6: `MonitorMode=virtual` Phase A — one client-sized virtual output, replace/extend, teardown order

**Files:**
- Modify: `server/krdpserversettings.kcfg` (3 new keys), `server/main.cpp` (`normalizedMonitorMode`, mode dispatch ~250-290, `applyRuntimeConfig` ~303-335, startup summary), `server/SessionController.h` (new enums/setters/members), `server/SessionController.cpp` (`SessionWrapper` ~62-490, `refreshDisplayConfiguration()` ~666, `buildSessions()` ~962, `onNewConnection()` ~1010, new `buildVirtualSessions()`)

**Interfaces:**
- Consumes: `RdpConnection::clientDisplayInfo()` / `clientDisplayInfoReceived()` (Task 2); `AbstractSession::outputGeometryChanged` / `virtualOutputUnresolved` / `outputGeometryResolved()` / `started()` (Task 3); `PhysicalOutputGuard` (Task 5); `ClientDisplay::sanitize/virtualMonitorName` (Task 1); `OutputSnapshot::VirtualPrefix` (Task 4).
- Produces (on `SessionController`):
  ```cpp
  enum class VirtualPolicy { Replace, Extend };
  enum class VirtualLayout { Client, Single };
  void setVirtualMode(bool enabled);               // MonitorMode=virtual; takes effect for the next connection
  bool virtualMode() const;
  void setVirtualPolicy(VirtualPolicy policy);
  void setVirtualLayout(VirtualLayout layout);
  void setVirtualFallbackSize(const QSize &size);
  static VirtualPolicy parseVirtualPolicy(const QString &text);   // "extend" -> Extend, anything else -> Replace
  static VirtualLayout parseVirtualLayout(const QString &text);   // "single" -> Single, anything else -> Client
  static std::optional<QSize> parseSize(const QString &text);     // "1920x1080"
  ```
  Phase B (Task 7) extends `buildVirtualSessions()`; this task builds exactly one session whatever the client's monitor list says (`VirtualLayout::Client` with monitors is logged as "multi-output virtual layout lands in Task 7" and treated as `Single`).

- [ ] **Step 1: Config keys**

`server/krdpserversettings.kcfg`, after `MonitorIndex`:
```xml
    <entry name="VirtualMonitorPolicy" type="String">
      <label>With MonitorMode=virtual: replace (switch the physical outputs off while a client is connected) or extend (keep them)</label>
      <default>replace</default>
    </entry>
    <entry name="VirtualMonitorLayout" type="String">
      <label>With MonitorMode=virtual: client (one virtual output per client monitor) or single (one output at the client's desktop size)</label>
      <default>client</default>
    </entry>
    <entry name="VirtualMonitorFallbackSize" type="String">
      <label>With MonitorMode=virtual: output size used when the client advertises no usable size (WIDTHxHEIGHT)</label>
      <default>1920x1080</default>
    </entry>
```
Also change the `MonitorMode` label to `Display target mode (workspace, primary, specific, multi, virtual)`.

- [ ] **Step 2: Mode plumbing in `main.cpp`**

`normalizedMonitorMode()`: add
```cpp
    if (mode.compare(u"virtual"_s, Qt::CaseInsensitive) == 0) {
        return u"virtual"_s;
    }
```
In the `else` branch of the mode dispatch (the one that computes `multiRequested`), before `controller.setMonitorIndex(monitorIndex);`:
```cpp
        const bool virtualRequested = !parser.isSet(u"monitor"_s) && normalizedMonitorMode(config->monitorMode()) == u"virtual"_s;
        if (virtualRequested && !parser.isSet(u"plasma"_s)) {
            qWarning() << "MonitorMode=virtual needs --plasma (the portal session cannot create outputs); using workspace";
        }
        controller.setVirtualPolicy(SessionController::parseVirtualPolicy(config->virtualMonitorPolicy()));
        controller.setVirtualLayout(SessionController::parseVirtualLayout(config->virtualMonitorLayout()));
        controller.setVirtualFallbackSize(SessionController::parseSize(config->virtualMonitorFallbackSize()).value_or(QSize(1920, 1080)));
        controller.setVirtualMode(virtualRequested && parser.isSet(u"plasma"_s));
```
and make `multiRequested` also require `!virtualRequested`. Extend the `streamTarget` chain: `if (controller.virtualMode()) { streamTarget = u"virtual:%1 (%2)"_s.arg(config->virtualMonitorLayout(), config->virtualMonitorPolicy()); } else if (controller.multiMonitorEnabled()) …`.

In `applyRuntimeConfig`, inside `if (!monitorPinnedByCli)`, before `setMultiMonitorEnabled`:
```cpp
            const bool virtualRequested = normalizedMonitorMode(config->monitorMode()) == u"virtual"_s && plasmaSession;
            controller.setVirtualPolicy(SessionController::parseVirtualPolicy(config->virtualMonitorPolicy()));
            controller.setVirtualLayout(SessionController::parseVirtualLayout(config->virtualMonitorLayout()));
            controller.setVirtualFallbackSize(SessionController::parseSize(config->virtualMonitorFallbackSize()).value_or(QSize(1920, 1080)));
            controller.setVirtualMode(virtualRequested);
            controller.setMultiMonitorEnabled(!virtualRequested && normalizedMonitorMode(config->monitorMode()) == u"multi"_s);
```
(`plasmaSession = parser.isSet(u"plasma"_s)` captured by the lambda.) Add `virtualPolicy/virtualLayout` to the `Runtime config applied:` log line.

- [ ] **Step 3: Controller state and parsers (`SessionController.h/.cpp`)**

Header: the enums, setters, statics from Interfaces; members
```cpp
    bool m_virtualMode = false;
    VirtualPolicy m_virtualPolicy = VirtualPolicy::Replace;
    VirtualLayout m_virtualLayout = VirtualLayout::Client;
    QSize m_virtualFallbackSize{1920, 1080};
    PhysicalOutputGuard m_outputGuard;
    void buildVirtualSessions(SessionWrapper *wrapper);
```
Declare `m_outputGuard` right after `m_displayWakeGuard` (i.e. BEFORE `m_wrappers`, same reason as the comment there: it must outlive the wrappers that restore into it), and make the destructor explicit so the order is not left to member order alone:
```cpp
SessionController::~SessionController() noexcept
{
    // Wrappers restore the physical outputs through m_outputGuard; tear them
    // down while the guard is still alive.
    m_wrappers.clear();
}
```
(`#include "PhysicalOutputGuard.h"`, `#include "ClientDisplayInfo.h"`.)

Rulings recorded here for the reviewer: (1) the policy is applied when every session is `streamActive()` and resolved, not on "first frame sent" as spec §4.5 words it — the controller cannot observe frames without touching `VideoStream`, and an active stream on a resolved output is the same moment for practical purposes; (2) a virtual output removed by KWin mid-session waits for it to reappear (Task 3) instead of ending the session as spec §4.7 says — the closed-stream recovery path already re-requests the output, and ending the connection would turn a compositor hiccup into a disconnect; (3) the geometry timeout is 5 s, not the spec's 2 s. Setters store the value; `setVirtualMode()` logs `"MonitorMode=virtual" << (enabled ? "on" : "off") << "- applies to the next connection"` only when the value changes. Parsers:
```cpp
SessionController::VirtualPolicy SessionController::parseVirtualPolicy(const QString &text)
{
    return text.trimmed().compare(u"extend"_s, Qt::CaseInsensitive) == 0 ? VirtualPolicy::Extend : VirtualPolicy::Replace;
}
SessionController::VirtualLayout SessionController::parseVirtualLayout(const QString &text)
{
    return text.trimmed().compare(u"single"_s, Qt::CaseInsensitive) == 0 ? VirtualLayout::Single : VirtualLayout::Client;
}
std::optional<QSize> SessionController::parseSize(const QString &text)
{
    const QRegularExpression rx(uR"(^\s*(\d+)\s*x\s*(\d+)\s*$)"_s);
    const auto match = rx.match(text);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    const QSize size(match.capturedView(1).toInt(), match.capturedView(2).toInt());
    return KRdp::ClientDisplay::usable(size) ? std::optional(size) : std::nullopt;
}
```
`refreshDisplayConfiguration()`: change the guard to `if (m_virtualMode || m_virtualMonitor.has_value()) { return; }` — in virtual mode the physical outputs come and go by design and the virtual sessions track their own screen.

- [ ] **Step 4: `SessionWrapper` virtual-session state**

Add to `SessionWrapper` (public members next to `layout`):
```cpp
    // MonitorMode=virtual bookkeeping; see SessionController::buildVirtualSessions().
    PhysicalOutputGuard *outputGuard = nullptr;
    SessionController::VirtualPolicy virtualPolicy = SessionController::VirtualPolicy::Replace;
    QVector<KRdp::OutputSnapshot::Placement> virtualPlacements; // intended KWin positions, one per session
    QString virtualPrimaryName;
    bool policyApplied = false;
    bool ownsPhysicalLayout = false; // this wrapper took the snapshot and must restore it
```
and the method that both Phase A and B call once every virtual session is started and resolved:
```cpp
    /**
     * Called whenever a virtual session starts or resolves its geometry. Once
     * every session is both, apply the policy exactly once.
     */
    void maybeApplyVirtualPolicy()
    {
        if (policyApplied || !outputGuard || sessions.empty()) {
            return;
        }
        const bool allReady = std::all_of(sessions.cbegin(), sessions.cend(), [](const std::unique_ptr<KRdp::AbstractSession> &session) {
            return session->streamActive() && session->outputGeometryResolved();
        });
        if (!allReady) {
            return;
        }
        policyApplied = true;
        if (virtualPolicy == SessionController::VirtualPolicy::Replace) {
            if (outputGuard->applyReplace(virtualPlacements, virtualPrimaryName)) {
                ownsPhysicalLayout = true;
            } else {
                qWarning() << "replace policy could not be applied; continuing as extend";
                outputGuard->reconcileExtend();
            }
        } else {
            outputGuard->reconcileExtend();
        }
    }
```
`~SessionWrapper()` becomes:
```cpp
    ~SessionWrapper() override
    {
        // Restore BEFORE the sessions (and with them the virtual outputs) go
        // away, so KWin never has zero enabled outputs and windows migrate
        // back onto the physical monitors.
        if (ownsPhysicalLayout && outputGuard) {
            outputGuard->restore();
        }
        holdDisplayWake(false);
    }
```
`streamActive()` is the existing `AbstractSession` accessor (true once the encoded stream runs).

- [ ] **Step 5: Deferred build and the virtual session (`SessionController.cpp`)**

`onNewConnection()`: at the top,
```cpp
    if (m_virtualMode) {
        const bool busy = std::any_of(m_wrappers.cbegin(), m_wrappers.cend(), [](const std::unique_ptr<SessionWrapper> &w) {
            return w && w->connection && !w->sessions.empty();
        });
        if (busy) {
            qWarning() << "MonitorMode=virtual serves one connection at a time; refusing a second client";
            newConnection->close(KRdp::RdpConnection::CloseReason::None);
            return;
        }
    }
```
then create the wrapper as today but replace the unconditional `buildSessions(wrapper.get());` with
```cpp
    if (m_virtualMode) {
        // The client's desktop size is only known after the capabilities
        // exchange (session thread); build once it arrives.
        connect(newConnection, &KRdp::RdpConnection::clientDisplayInfoReceived, wrapper.get(), [this, wrapper = wrapper.get()]() {
            buildVirtualSessions(wrapper);
        }, Qt::QueuedConnection);
    } else {
        buildSessions(wrapper.get());
    }
```
New method:
```cpp
void SessionController::buildVirtualSessions(SessionWrapper *wrapper)
{
    if (!wrapper || !wrapper->connection || !wrapper->sessions.empty()) {
        return;
    }
    const auto info = KRdp::ClientDisplay::sanitize(wrapper->connection->clientDisplayInfo(), m_virtualFallbackSize);
    if (!m_outputGuard.available()) {
        qWarning() << "kscreen-doctor not found; MonitorMode=virtual runs as extend without layout control";
    } else if (!m_outputGuard.snapshot()) {
        qWarning() << "Could not snapshot the physical outputs; MonitorMode=virtual runs as extend";
    }
    const bool canReplace = m_virtualPolicy == VirtualPolicy::Replace && m_outputGuard.hasSnapshot();

    wrapper->outputGuard = &m_outputGuard;
    wrapper->virtualPolicy = canReplace ? VirtualPolicy::Replace : VirtualPolicy::Extend;
    wrapper->policyApplied = false;
    wrapper->virtualPlacements.clear();

    if (m_virtualLayout == VirtualLayout::Client && !info.monitors.isEmpty()) {
        qInfo() << "Client advertises" << info.monitors.size() << "monitors; multi-output virtual layout lands in Task 7, using one output of" << info.desktopSize;
    }

    std::vector<std::unique_ptr<KRdp::AbstractSession>> sessions;
    auto session = makeSession();
    const auto name = KRdp::ClientDisplay::virtualMonitorName(0, info.desktopSize);
    session->setVirtualMonitor(KRdp::VirtualMonitor{name, info.desktopSize, 1.0});
    session->setMonitorIndex(0);
    wrapper->virtualPrimaryName = KRdp::OutputSnapshot::VirtualPrefix + name;
    wrapper->virtualPlacements.push_back({wrapper->virtualPrimaryName, QPoint(0, 0)});

    connect(session.get(), &KRdp::AbstractSession::started, wrapper, &SessionWrapper::maybeApplyVirtualPolicy);
    connect(session.get(), &KRdp::AbstractSession::outputGeometryChanged, wrapper, [wrapper](const QRect &) {
        wrapper->maybeApplyVirtualPolicy();
    });
    connect(session.get(), &KRdp::AbstractSession::virtualOutputUnresolved, wrapper, [wrapper]() {
        qWarning() << "Virtual output unresolved: pointer input stays gated and the replace policy is not applied";
        wrapper->policyApplied = true; // never apply on an unresolved output
    });
    if (m_quality.has_value() && !m_adaptiveQuality) {
        session->setVideoQuality(m_quality.value());
    }
    sessions.push_back(std::move(session));

    qInfo() << "MonitorMode=virtual: one output" << info.desktopSize << "policy" << (canReplace ? "replace" : "extend");
    wrapper->setSessions(std::move(sessions), MonitorLayout{});
}
```
Note: the `started`/`outputGeometryChanged`/`virtualOutputUnresolved` connections are made before `setSessions()` and are NOT part of `m_sessionConnections` (they die with the session object).

- [ ] **Step 6: Build, unit tests**

`cmake --build build -j16 2>&1 | grep -E "warning|error"` → nothing; `ctest --test-dir build` → all pass.

- [ ] **Step 7: Acceptance on a test instance (Global Constraints apply; no client on 3389/3391)**

Config: `printf '[General]\nMonitorMode=virtual\nVirtualMonitorPolicy=replace\nSystemUserEnabled=false\nQuality=80\nAdaptiveQuality=false\n' > $XDG_CONFIG_HOME/krdpserverrc`, start on :3392 WITHOUT `--virtual-monitor`. Baseline `kscreen-doctor -o > /tmp/krdp-v/baseline.txt`.

(a) **replace**: from buzz, own client headless 40 s. While connected, at ~10 s: `kscreen-doctor -o` → `Virtual-krdp-m0-1920x1080` enabled at `0,0` priority 1, DP-1 and HDMI-A-1 `disabled`; server log has `Client display: desktop QSize(1920, 1080)`, `Virtual output "Virtual-krdp-m0-1920x1080" resolved at QRect(...)`, `Physical outputs replaced by 1 virtual output(s)`; `ls ~/.local/state/krdp-server/physical-outputs.json` exists (as built: `krdp-server`, not `krdpserver`). After the client exits: within 5 s `Physical outputs restored`, `kscreen-doctor -o` equals the baseline (`diff`), the state file is gone, the virtual output is gone.
(b) **input**: repeat with the GUI client's pointer debug action (a window opens on buzz ~40 s): KWin cursor readback equals `(1234, 567)` + the virtual output's origin as logged in `resolved at` (with replace it is `(0,0)`, so `(1234,567)` on the VIRTUAL output — confirm via the log that the output sits at 0,0 and DP-1 is disabled at that moment).
(c) **extend**: `VirtualMonitorPolicy=extend` (plain rewrite of the file; restart the test instance — no `--notify`), own client headless 30 s: physical outputs stay enabled throughout, the virtual output appears to the right (`5120,0`), log shows no `replaced`; on exit the layout equals the baseline.
(d) **second client refused**: while a headless run is connected, start a second (`--seconds 10`); server log `refusing a second client`, the second exits non-zero, the first keeps streaming.
(e) **SIGTERM mid-session (replace)**: during a headless run, `kill -TERM $(cat /tmp/krdp-v/server.pid)`; expected log `Physical outputs restored` (controller destroyed → wrapper destructor → restore; or the guard's own destructor), `kscreen-doctor -o` equals the baseline. If the physicals are still disabled: run `./build/bin/krdpserver --restore-outputs`, then the manual command, and report the gap.
(f) Steve's acceptance (do NOT run it yourself; write the exact steps in the report): Remmina profile to `hal9000.local:3392` at 1920x1080 → his real desktop fills the laptop screen, windows migrate, monitors come back on disconnect.

Stop the instance; final `diff baseline.txt <(kscreen-doctor -o)` empty.

- [ ] **Step 8: Commit**

```bash
git add server/krdpserversettings.kcfg server/main.cpp server/SessionController.h server/SessionController.cpp
git commit -m "server: MonitorMode=virtual - one client-sized virtual output with replace/extend policy (OPT-041 Phase A)"
```

---

### Task 6c: Console takeover — give the monitors back when a person uses hal9000 (plan amendment, Steve 2026-09-17)

**Decision (Steve):** when someone uses the console while a `replace` session holds the outputs, the physical monitors come back immediately and the remote session CONTINUES in `extend` mode (the virtual output stays, parked to the right of the physical desktop). Triggers: (1) local pointer motion detected from the screencast cursor metadata, (2) a "Restore my monitors" action on the krdpserver tray icon, (3) a global keyboard shortcut (KGlobalAccel, default `Meta+Ctrl+Alt+R`; KF6::GlobalAccel headers are installed).

**Files:**
- Create: `server/TakeoverDetector.h` (pure), `autotests/TakeoverDetectorTest.cpp`
- Modify: `server/SessionController.{h,cpp}` (`SessionWrapper::onCursorUpdate`, input path, `releasePhysicalOutputs()`, SNI menu, KGlobalAccel action), `server/PhysicalOutputGuard.{h,cpp}` (preamble fixes), `server/CMakeLists.txt` (`KF6::GlobalAccel`), `autotests/CMakeLists.txt`

**Preamble commit first (Task 6 re-review follow-ups), `server/PhysicalOutputGuard.cpp`:** `reconcileExtend()` sets `m_held = true` when its re-apply does not verify; `snapshot()` refuses (returns false, logs) while `m_held` or when `enabledUnion()` of the physical outputs is invalid; the state file is written by `applyReplace()` (just before the first mutation), not by `snapshot()`, so an extend crash never restores anything; `restoreFromStateFile()`'s live-owner skip also prints the manual recovery command; `SessionController::rebuildSessions()` skips a virtual-mode wrapper that has no sessions yet (its deferred build is pending). Build + ctest green, commit `server: guard/controller follow-ups from the Task 6 re-review`.

**Interfaces:**
- Consumes: `PipeWireCursor{position, hotspot, texture}` from `AbstractSession::cursorUpdate` (position in the captured output's pixels), `AbstractSession::outputGeometry()` (KWin-global logical rect, Task 3), the wrapper's input path (`InputHandler::inputEvent` → `AbstractSession::sendEvent`), `SessionController::releasePhysicalOutputs()` (Task 6), `PhysicalOutputGuard::positionOutputs()`.
- Produces:
  ```cpp
  namespace KRdp::Takeover {
  constexpr int DistanceThresholdPx = 24;      // cursor moved further than any injected move could explain
  constexpr int QuietWindowMs = 300;           // no injected pointer motion this recently
  constexpr int ArmDelayMs = 2000;             // ignore the first samples after the policy applied
  struct Detector {
      void armed(qint64 nowMs);                                  // call when the replace policy applied
      void injected(const QPoint &globalLogical, qint64 nowMs); // every pointer motion the server injects
      bool observed(const QPoint &globalLogical, qint64 nowMs); // every cursor metadata sample; true = local motion
  };
  }
  ```
  `observed()` returns true once (then latches `fired`) when: armed for ≥ ArmDelayMs, at least one `injected()` has happened, `nowMs - lastInjectedMs > QuietWindowMs`, and `manhattanLength(observed - lastInjected) > DistanceThresholdPx`.

- [ ] **Step 1: Failing test** — `autotests/TakeoverDetectorTest.cpp` (QTEST_GUILESS_MAIN): `notBeforeArmDelay` (armed at 0, injected (100,100)@100, observed (900,900)@500 → false); `injectedMotionIsNotLocal` (armed 0, injected (100,100)@3000, observed (110,105)@3050 → false: within threshold); `quietWindowSuppresses` (injected (100,100)@3000, observed (800,800)@3100 → false: too soon after an injection); `localMotionFires` (injected (100,100)@3000, observed (800,800)@3400 → true); `firesOnce` (after true, another far sample → false); `needsOneInjection` (armed 0, observed (800,800)@5000 with no injected → false).
- [ ] **Step 2: run, expect failure. Step 3: implement `server/TakeoverDetector.h` to those rules. Step 4: run, expect pass.**
- [ ] **Step 5: Wire it in `SessionWrapper`:** a `KRdp::Takeover::Detector takeover;` member; `armed()` when `maybeApplyVirtualPolicy()` applied `replace`; in the single-session input path (the `sendEvent` connection for virtual wrappers) route through a lambda that records `injected(globalLogical)` — compute the same mapping `PlasmaScreencastV1Session::sendEvent()` uses: normalised position × (logicalSize−1) + `outputGeometry().topLeft()` (expose a small `AbstractSession::mapToGlobal(const QPointF &local) const` helper for that); in `onCursorUpdate()` compute `outputGeometry().topLeft() + cursor.position / devicePixelRatio` and call `observed()`; on true → `qInfo() << "Console activity detected; restoring the physical outputs (session continues in extend mode)"` then `QMetaObject::invokeMethod(controller, &SessionController::releasePhysicalOutputs, Qt::QueuedConnection)`.
- [ ] **Step 6: `releasePhysicalOutputs()` completes the takeover:** for every virtual wrapper that `ownsPhysicalLayout`: `guard.release()` (restores), then park/position the virtual output(s) at the extend anchor (right edge of the restored physical union, `positionOutputs()` with the wrapper's placements re-anchored) so nothing overlaps DP-1, set `virtualPolicy = Extend`, `ownsPhysicalLayout = false`; the sessions' `geometryChanged` tracking (Task 3) keeps input correct. Log one line. Idempotent (second call is a no-op).
- [ ] **Step 7: Tray action + global shortcut:** in `SessionController`'s SNI menu add `i18n("Restore my monitors")` → `releasePhysicalOutputs()` (enabled only while a wrapper owns the layout; update on apply/release); register a `KGlobalAccel` action `restore-physical-outputs` (component `krdpserver`, default `Meta+Ctrl+Alt+R`) → same slot. `target_link_libraries(krdpserver PRIVATE KF6::GlobalAccel)` and `find_package` component in the top-level CMakeLists.
- [ ] **Step 8: Build, ctest, commit** `server: console takeover - local pointer motion, tray action or shortcut restore the monitors and keep the session (OPT-041 Task 6c)`.
- [ ] **Step 9 (deferred, Steve's go required — it toggles the monitors):** test instance :3392 `MonitorMode=virtual`, headless client from buzz for 90 s; at ~20 s move the real mouse on hal9000: expect `Console activity detected`, `Physical outputs restored`, virtual output re-parked at 5120,0, the client keeps streaming (frames continue), `kscreen-doctor -o` shows physicals enabled + virtual at 5120,0; on disconnect the layout equals the baseline. Repeat with the tray action and with the shortcut instead of the mouse. Steve does the mouse part himself if no agent can (fake input is not available to scripts).

---

### Task 7: Phase B — one virtual output per client monitor

**Files:**
- Modify: `server/SessionController.cpp` (`buildVirtualSessions()`, `SessionWrapper`: new `onVirtualGeometryResolved()` / `adoptActualVirtualLayout()`), `server/SessionController.h`

**Interfaces:**
- Consumes: `ClientDisplay::placement()` (Task 1), `OutputSnapshot::enabledUnion()`/`positionArgs` via `PhysicalOutputGuard::positionOutputs()` (Tasks 4/5), Plan 3's multi machinery: `SessionWrapper::setSessions(sessions, layout)` with `MonitorLayout{monitors (KWin pixel rects), names, scale=1.0}`, `VideoStream::setMonitorLayout()`, `SurfaceLayout::originOf()` in `onInputEvent()`, `correctSurfaceSize()`.
- Produces: nothing new outside the controller.

- [ ] **Step 1: Multi-output build in `buildVirtualSessions()`**

Replace the "lands in Task 7" branch with a real one. After the snapshot/policy preamble:
```cpp
    const bool multiOutput = m_virtualLayout == VirtualLayout::Client && info.monitors.size() >= 2;
    if (!multiOutput) {
        // ... the Phase A single-session body from Task 6, unchanged ...
        return;
    }

    // Intended KWin placement: the client's layout, anchored at (0,0) when the
    // physical outputs are about to be switched off, or to the right of them.
    const QRect physical = m_outputGuard.hasSnapshot() ? KRdp::OutputSnapshot::enabledUnion(m_outputGuard.physicalOutputs()) : QRect();
    const QPoint anchor = (canReplace || !physical.isValid()) ? QPoint(0, 0) : QPoint(physical.x() + physical.width(), 0);
    const auto rects = KRdp::ClientDisplay::placement(info.monitors, anchor);

    std::vector<std::unique_ptr<KRdp::AbstractSession>> sessions;
    MonitorLayout layout;
    layout.scale = 1.0;
    for (qsizetype i = 0; i < info.monitors.size(); ++i) {
        const auto &monitor = info.monitors.at(i);
        auto session = makeSession();
        const auto name = KRdp::ClientDisplay::virtualMonitorName(int(i), monitor.geometry.size());
        const QString kwinName = KRdp::OutputSnapshot::VirtualPrefix + name;
        session->setVirtualMonitor(KRdp::VirtualMonitor{name, monitor.geometry.size(), 1.0});
        session->setMonitorIndex(int(i));
        wrapper->virtualPlacements.push_back({kwinName, rects.at(i).topLeft()});
        if (monitor.primary) {
            wrapper->virtualPrimaryName = kwinName;
        }
        layout.monitors.push_back(KRdp::VideoMonitor{.geometry = rects.at(i), .primary = monitor.primary});
        layout.names.push_back(kwinName);

        connect(session.get(), &KRdp::AbstractSession::started, wrapper, &SessionWrapper::maybeApplyVirtualPolicy);
        connect(session.get(), &KRdp::AbstractSession::outputGeometryChanged, wrapper, [wrapper](const QRect &) {
            wrapper->maybeApplyVirtualPolicy();
        });
        connect(session.get(), &KRdp::AbstractSession::virtualOutputUnresolved, wrapper, [this, wrapper]() {
            qWarning() << "A virtual output never appeared; falling back to a single virtual output";
            wrapper->policyApplied = true;
            QMetaObject::invokeMethod(this, [this, wrapper]() { rebuildAsSingleVirtual(wrapper); }, Qt::QueuedConnection);
        });
        if (m_quality.has_value() && !m_adaptiveQuality) {
            session->setVideoQuality(m_quality.value());
        }
        sessions.push_back(std::move(session));
    }
    qInfo().noquote() << QStringLiteral("MonitorMode=virtual: %1 outputs mirroring the client layout, policy %2, anchor %3,%4: %5")
                             .arg(sessions.size())
                             .arg(canReplace ? u"replace"_s : u"extend"_s)
                             .arg(anchor.x())
                             .arg(anchor.y())
                             .arg(layoutSummary(layout.monitors));
    wrapper->setSessions(std::move(sessions), layout);
```
`rebuildAsSingleVirtual()` (declare `void rebuildAsSingleVirtual(SessionWrapper *wrapper);` in the header; add `bool forceSingleVirtual = false;` to `SessionWrapper`):
```cpp
void SessionController::rebuildAsSingleVirtual(SessionWrapper *wrapper)
{
    if (!wrapper || !wrapper->connection || wrapper->forceSingleVirtual) {
        return;
    }
    wrapper->forceSingleVirtual = true;
    // setSessions() replaces the vector wholesale, but buildVirtualSessions()
    // refuses to build over live sessions: drop them first (their virtual
    // outputs go away with them; the physical layout was never touched
    // because the policy is only applied once every output resolved).
    wrapper->sessions.clear();
    buildVirtualSessions(wrapper);
}
```
and the `multiOutput` condition in `buildVirtualSessions()` becomes `m_virtualLayout == VirtualLayout::Client && info.monitors.size() >= 2 && !wrapper->forceSingleVirtual`.

- [ ] **Step 2: Policy application positions the outputs, then adopts what KWin actually did**

In `SessionWrapper::maybeApplyVirtualPolicy()`, the `Extend` branch becomes
```cpp
            if (virtualPlacements.size() > 1) {
                outputGuard->positionOutputs(virtualPlacements, virtualPrimaryName);
            }
            outputGuard->reconcileExtend();
```
(`applyReplace` already positions.) Then, in BOTH branches, after the guard call, schedule one verification: `QTimer::singleShot(1000, this, &SessionWrapper::adoptActualVirtualLayout);`
```cpp
    /**
     * KWin may refuse or adjust the requested positions. Whatever it did is
     * what the RDP layout must describe, or pointer input lands off by the
     * difference: read the sessions' resolved geometries back and, if they
     * differ from the layout the stream was reset with, re-publish.
     */
    void adoptActualVirtualLayout()
    {
        if (layout.isEmpty() || !connection) {
            return;
        }
        QVector<KRdp::VideoMonitor> actual = layout.monitors;
        for (size_t i = 0; i < sessions.size() && qsizetype(i) < actual.size(); ++i) {
            const auto geometry = sessions[i]->outputGeometry();
            if (geometry.isValid()) {
                actual[qsizetype(i)].geometry = geometry;
            }
        }
        if (actual == layout.monitors) {
            return;
        }
        qWarning().noquote() << QStringLiteral("KWin placed the virtual outputs differently than requested; adopting: %1").arg(layoutSummary(actual));
        layout.monitors = actual;
        connection->videoStream()->setMonitorLayout(actual);
    }
```
`setMonitorLayout()` (Plan 3) clears the queue and triggers a surface reset with the new `TS_MONITOR_DEF`, and `onInputEvent()` reads `layout.monitors` on every event, so input follows.

- [ ] **Step 3: Build, unit tests** — zero warnings; `ctest` green.

- [ ] **Step 4: Acceptance (Global Constraints; Steve's portable monitor must be connected to buzz — ask via the report if `ssh westers@buzz.local 'WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 kscreen-doctor -o | grep -c Output'` prints 1)**

Config as Task 6 (`replace`, `VirtualMonitorLayout=client`), instance on :3392.
(a) `sdl-freerdp3` from buzz: `ssh westers@buzz.local 'WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 timeout 60 sdl-freerdp3 /v:hal9000.local:3392 /u:krdptest /p:krdptest /gfx:AVC420 /cert:ignore /multimon'` (opens full-screen windows on BOTH of Steve's screens for 60 s — say so). Server log: `Client display: desktop QSize(3840, 1280) monitors 2` (or whatever buzz advertises), `2 outputs mirroring the client layout`, two `resolved at` lines, `Physical outputs replaced by 2 virtual output(s)`; `kscreen-doctor -o` while connected: two `Virtual-krdp-m…` outputs at the client's relative positions, physicals disabled; `Reset graphics desktop … with 2 monitor(s)` whose rects equal the client's sanitised rects (this is the OPT-040 case: the client's own layout is what it gets back). After exit: baseline restored.
(b) Own client headless (advertises one monitor): Phase A path, one output — proves the fallback.
(c) `extend` variant of (a): physicals stay on, virtuals positioned at `5120,0` and `5120+…`, priorities 3 and 4.
(d) Steve's acceptance steps for the report: Remmina "use all monitors" / `sdl-freerdp3 /multimon` → each laptop screen shows one remote monitor at native size; pointer exact on both; monitors back afterwards.

- [ ] **Step 5: Commit**

```bash
git add server/SessionController.h server/SessionController.cpp
git commit -m "server: MonitorMode=virtual mirrors the client's monitor layout with one virtual output each (OPT-041 Phase B)"
```

---

### Task 8: Documentation and hand-off

**Files:**
- Modify: `README.md` (the `MonitorMode` section), `research.md` (OPT-041 entry + status; OPT-040 note), `docs/superpowers/specs/2026-09-17-opt-041-virtual-monitor-design.md` (§6 ruling: stable names + reconcile), `~/dev/rdp/CLAUDE.md` (config line, gotchas)

- [ ] **Step 1: README**

In the `MonitorMode` paragraph add `virtual`: what it does (client-sized virtual outputs, `replace`/`extend`, `VirtualMonitorLayout`, `VirtualMonitorFallbackSize`), the switch command `kwriteconfig6 --file krdpserverrc --group General --key MonitorMode virtual --notify` (takes effect at the next connection), the rollback (`specific`), the crash recovery (`krdpserver --restore-outputs`, or `kscreen-doctor output.DP-1.enable output.HDMI-A-1.enable`), and that virtual mode serves one connection at a time.

- [ ] **Step 2: research.md**

Add `OPT-041 DONE <date> — client-sized virtual monitors (Phases A+B): …` in the status list with the acceptance evidence from Tasks 6/7 reports, the known limits (dpr fixed at 1.0, one connection at a time, KWin remembered-setups entry per distinct client size, Phase C live resize pending), and mark OPT-040 as `superseded for MonitorMode=virtual (client layout is mirrored by construction); still open for MonitorMode=multi`.

- [ ] **Step 3: Spec §6 and CLAUDE.md**

Spec §6: replace the "unique per connection" paragraph with the stable-name + reconcile ruling (Global Constraints wording). `~/dev/rdp/CLAUDE.md`: in the `~/.config/krdpserverrc` line add `virtual` with its three keys; in Gotchas add: "MonitorMode=virtual/replace switches DP-1 and HDMI-A-1 OFF while a client is connected; if krdpserver dies mid-session run `krdpserver --restore-outputs`"; in the system section note that `kscreen-doctor` is a runtime dependency of virtual mode.

- [ ] **Step 4: Commit and report the deployment steps (do not deploy)**

```bash
git add README.md research.md docs/superpowers/specs/2026-09-17-opt-041-virtual-monitor-design.md
git commit -m "docs: OPT-041 virtual monitors done; MonitorMode=virtual, policies, recovery"
```
In the report, list for Steve: (1) `ss -tnp | grep ':3389' | grep ESTAB` must be empty, (2) `systemctl --user restart app-org.kde.krdpserver.service`, (3) `kwriteconfig6 --file krdpserverrc --group General --key MonitorMode virtual --notify`, (4) connect from buzz with Remmina at 1920x1080, (5) rollback command.
