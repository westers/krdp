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

    void unionWiderThanOneEncoderDropsTheList()
    {
        // Two usable monitors whose union no single virtual output could
        // carry (5120 px is past the VA-API surface limit): the list goes,
        // and the advertised 5120x1440 desktop is unusable too -> fallback.
        const auto out = sanitize(Info{QSize(5120, 1440), {{QRect(0, 0, 2560, 1440), true}, {QRect(2560, 0, 2560, 1440), false}}}, QSize(1920, 1080));
        QVERIFY(out.monitors.isEmpty());
        QCOMPARE(out.desktopSize, QSize(1920, 1080));
        // Stacked vertically past the limit as well (two 2160-high panels).
        const auto tall = sanitize(Info{QSize(3840, 4320), {{QRect(0, 0, 3840, 2160), true}, {QRect(0, 2160, 3840, 2160), false}}}, QSize(1920, 1080));
        QVERIFY(tall.monitors.isEmpty());
        QCOMPARE(tall.desktopSize, QSize(1920, 1080));
        // Within the limit in both directions is kept (2 x 1440 high = 2880).
        const auto stacked = sanitize(Info{QSize(2560, 2880), {{QRect(0, 0, 2560, 1440), true}, {QRect(0, 1440, 2560, 1440), false}}}, QSize(1920, 1080));
        QCOMPARE(stacked.monitors.size(), 2);
        QCOMPARE(stacked.desktopSize, QSize(2560, 2880));
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
