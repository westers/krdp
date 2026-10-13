// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <wayland-util.h>

#include "WheelScale.h"

class WheelScaleTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void oneNotchIsFifteenUnits()
    {
        QCOMPARE(KRdp::fakeInputAxisValue(120), 15.0);
        QCOMPARE(KRdp::fakeInputAxisValue(-120), -15.0);
        QCOMPARE(KRdp::fakeInputAxisValue(0), 0.0);
    }
    void highResDeltasScaleLinearly()
    {
        QCOMPARE(KRdp::fakeInputAxisValue(30), 3.75);
        QCOMPARE(KRdp::fakeInputAxisValue(8), 1.0);
        QCOMPARE(KRdp::fakeInputAxisValue(-255), -31.875);
    }
    void survivesWlFixedWithoutLoss()
    {
        // wl_fixed_t has 1/256 resolution: every 1/8-notch step (15/8 units) is representable.
        for (int d = -240; d <= 240; d += 15) {
            const double v = KRdp::fakeInputAxisValue(d);
            QCOMPARE(wl_fixed_to_double(wl_fixed_from_double(v)), v);
        }
        // The smallest RDP step (1) is 0.125 units, still above wl_fixed resolution (0.0039).
        QCOMPARE(wl_fixed_to_double(wl_fixed_from_double(KRdp::fakeInputAxisValue(1))), 0.125);
    }
};

QTEST_APPLESS_MAIN(WheelScaleTest)
#include "WheelScaleTest.moc"
