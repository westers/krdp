// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "DisplayWakePolicy.h"
using namespace KRdp;

class DisplayWakePolicyTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void onlyAdmittedStreamingViewersCount()
    {
        const auto policy = displayPolicyFor({{false, true, true}, {true, false, true}, {true, true, false}});
        QVERIFY(policy.active);
        QVERIFY(!policy.wakeEnabled);
        QVERIFY(!displayPolicyFor({{false, true, true}, {true, false, true}}).active);
    }
    void ownerReleaseDoesNotCancelAnEnabledViewer()
    {
        const auto two = displayPolicyFor({{true, true, true}, {true, true, true}});
        const auto viewer = displayPolicyFor({{true, true, true}});
        QCOMPARE(two, viewer);
        QVERIFY(viewer.active && viewer.wakeEnabled);
        const auto disabledViewer = displayPolicyFor({{true, true, false}});
        QVERIFY(disabledViewer.active && !disabledViewer.wakeEnabled);
        QVERIFY(!displayPolicyFor({}).active);
    }
    void releasedRevisionCannotBeReplayed()
    {
        DisplayWakePolicy state;
        QVERIFY(state.accept({1, true, true}));
        QVERIFY(state.accept({2, false, false}));
        QVERIFY(!state.accept({1, true, true}));
        QVERIFY(!state.accept({2, true, true}));
        QVERIFY(!state.accept({0, true, true}));
        QVERIFY(!state.current().active);
        QVERIFY(!state.accept({3, false, true}));
        QVERIFY(state.accept({3, true, false}));
        QVERIFY(state.current().active && !state.current().wakeEnabled);
    }
    void strictWireValidation()
    {
        using namespace ConsoleWorkerWire;
        const DisplayPolicy expected{42, true, true};
        Deframer reader;
        reader.feed(frame(expected));
        const auto record = reader.next();
        QVERIFY(record);
        QCOMPARE(displayPolicy(*record), std::optional<DisplayPolicy>(expected));
        auto malformed = *record;
        malformed.payload.chop(1);
        QVERIFY(!displayPolicy(malformed));
        malformed = *record;
        malformed.payload[8] = char(2);
        QVERIFY(!displayPolicy(malformed));
        malformed.payload[8] = char(0); // Inactive cannot request inhibition/wake.
        QVERIFY(!displayPolicy(malformed));
        malformed = *record;
        for (int i = 0; i < 8; ++i) malformed.payload[i] = char(0);
        QVERIFY(!displayPolicy(malformed));
    }
};
QTEST_GUILESS_MAIN(DisplayWakePolicyTest)
#include "DisplayWakePolicyTest.moc"
