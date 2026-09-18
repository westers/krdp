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

    void usableDesktopBounds()
    {
        QVERIFY(usableDesktop(QSize(4096, 4096)));
        QVERIFY(usableDesktop(QSize(8192, 8192)));
        QVERIFY(!usableDesktop(QSize(639, 1080)));
        QVERIFY(!usableDesktop(QSize(1920, 8193)));
        QVERIFY(!usableDesktop(QSize()));
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

    void unionUpTo8192IsKeptForTheClientLayout()
    {
        // Steve's laptop: eDP-1 3072x1728 (a 200% panel, primary) with DP-1
        // 1920x1080 beside it, offset in y. The union is 4992x1956 - past the
        // 4096 per-output VA-API limit, but `VirtualMonitorLayout=client`
        // opens one virtual output (and one VA-API surface) per monitor, each
        // already checked against MaxDimension above, so the union only has
        // to fit the RDP desktop limit (MaxDesktopDimension), not one surface.
        const auto out = sanitize(Info{QSize(4992, 1956), {{QRect(0, 0, 3072, 1728), true}, {QRect(3072, 876, 1920, 1080), false}}}, QSize(1920, 1080));
        QCOMPARE(out.monitors.size(), 2);
        QCOMPARE(out.desktopSize, QSize(4992, 1956));
    }

    void unionOver8192IsDropped()
    {
        // Union height one past MaxDesktopDimension (8192): dropped, and the
        // advertised 4096x8193 desktop is unusable too (height > 4096) -> fallback.
        const auto out = sanitize(Info{QSize(4096, 8193), {{QRect(0, 0, 4096, 4096), true}, {QRect(0, 4097, 4096, 4096), false}}}, QSize(1920, 1080));
        QVERIFY(out.monitors.isEmpty());
        QCOMPARE(out.desktopSize, QSize(1920, 1080));
        // Exactly at the limit is kept.
        const auto atLimit = sanitize(Info{QSize(8192, 2160), {{QRect(0, 0, 4096, 2160), true}, {QRect(4096, 0, 4096, 2160), false}}}, QSize(1920, 1080));
        QCOMPARE(atLimit.monitors.size(), 2);
        QCOMPARE(atLimit.desktopSize, QSize(8192, 2160));
    }

    void singleSizeFallsBackWhenTheUnionExceedsTheEncoderLimit()
    {
        // sanitize() keeps the client-layout list because the union fits the
        // RDP desktop limit, but the one-output paths (VirtualMonitorLayout=
        // single, or a later fallback to one output) cannot open a
        // 4992-wide VA-API surface, so singleSize() still falls back.
        const auto out = sanitize(Info{QSize(4992, 1956), {{QRect(0, 0, 3072, 1728), true}, {QRect(3072, 876, 1920, 1080), false}}}, QSize(1920, 1080));
        QCOMPARE(out.monitors.size(), 2); // client layout: kept
        QCOMPARE(singleSize(out, QSize(1920, 1080)), QSize(1920, 1080)); // single layout: fallback
    }

    void unionWithinTheLimitIsKept()
    {
        const auto out = sanitize(Info{QSize(3840, 1080), {{QRect(0, 0, 1920, 1080), true}, {QRect(1920, 0, 1920, 1080), false}}}, QSize(1920, 1080));
        QCOMPARE(out.monitors.size(), 2);
        QCOMPARE(out.desktopSize, QSize(3840, 1080));
    }

    void absurdPositionsDropTheList()
    {
        // TS_MONITOR_DEF coordinates are INT32; nothing past +-32767 is a
        // monitor position, and a union computed from it could overflow.
        const auto far = sanitize(Info{QSize(3840, 1080), {{QRect(0, 0, 1920, 1080), true}, {QRect(40000, 0, 1920, 1080), false}}}, QSize(1920, 1080));
        QVERIFY(far.monitors.isEmpty());
        const auto negative = sanitize(Info{QSize(3840, 1080), {{QRect(-32768, 0, 1920, 1080), true}, {QRect(0, 0, 1920, 1080), false}}}, QSize(1920, 1080));
        QVERIFY(negative.monitors.isEmpty());
        // The edge itself is fine.
        const auto edge = sanitize(Info{QSize(3840, 1080), {{QRect(-32767, 0, 1920, 1080), true}, {QRect(-32767 + 1920, 0, 1920, 1080), false}}}, QSize(1920, 1080));
        QCOMPARE(edge.monitors.size(), 2);
        QCOMPARE(edge.monitors[0].geometry, QRect(0, 0, 1920, 1080));
    }

    void overlappingMonitorsDropTheList()
    {
        const auto out = sanitize(Info{QSize(3840, 1080), {{QRect(0, 0, 1920, 1080), true}, {QRect(1900, 0, 1920, 1080), false}}}, QSize(1920, 1080));
        QVERIFY(out.monitors.isEmpty());
        // Touching edges are not overlapping.
        const auto touching = sanitize(Info{QSize(3840, 1080), {{QRect(0, 0, 1920, 1080), true}, {QRect(1920, 0, 1920, 1080), false}}}, QSize(1920, 1080));
        QCOMPARE(touching.monitors.size(), 2);
    }

    void singleSizeIsAlwaysUsable()
    {
        // What a single virtual output (VirtualMonitorLayout=single, or the
        // one-output fallback) is asked for: the sanitised desktop size when
        // it is usable, the fallback otherwise.
        QCOMPARE(singleSize(Info{QSize(3840, 1080), {}}, QSize(1920, 1080)), QSize(3840, 1080));
        QCOMPARE(singleSize(Info{QSize(5120, 1440), {}}, QSize(1920, 1080)), QSize(1920, 1080));
        QCOMPARE(singleSize(Info{QSize(), {}}, QSize(1920, 1080)), QSize(1920, 1080));
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
