// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-071: the desk-activity pacing, the host-pointer tracker, the record shapes and the desk-device filter. Pure: a fake
// clock, no devices, no daemons.

#include <QTest>

#include "DeskActivity.h"
#include "PhysicalInputWatcher.h"

using namespace KRdp;
using namespace KRdp::DeskActivity;

class DeskActivityTest : public QObject
{
    Q_OBJECT

private:
    static QSet<QString> keySet(const QJsonObject &object)
    {
        const auto keys = object.keys();
        return QSet<QString>(keys.begin(), keys.end());
    }

private Q_SLOTS:
    void firstEventIsSentAtOnce()
    {
        Tracker tracker;
        QVERIFY(!tracker.take(0));
        QCOMPARE(tracker.dueIn(0), qint64(-1));
        tracker.note(1000);
        QCOMPARE(tracker.dueIn(1000), qint64(0));
        QCOMPARE(tracker.take(1000), std::optional<bool>(true));
        QVERIFY(!tracker.take(1000));
        QVERIFY(tracker.live(1000));
    }

    void ratePerSecondNeverExceedsTheCap()
    {
        // A mouse reporting every millisecond for 3 seconds: at most MaxRateHz active records in any second.
        Tracker tracker;
        QList<qint64> sent;
        for (qint64 now = 0; now < 3000; ++now) {
            tracker.note(now);
            if (const auto active = tracker.take(now); active && *active) sent.append(now);
        }
        QVERIFY(sent.size() >= 10);
        QVERIFY(sent.size() <= 12); // 3 s at 4/s, plus the leading edge
        for (qsizetype i = 0; i + MaxRateHz < sent.size(); ++i) {
            QVERIFY2(sent.at(i + MaxRateHz) - sent.at(i) >= 1000, "more than MaxRateHz records inside one second");
        }
    }

    void aBurstGetsATrailingRecord()
    {
        Tracker tracker;
        tracker.note(0);
        QCOMPARE(tracker.take(0), std::optional<bool>(true));
        tracker.note(10); // inside the window: held, not dropped
        QVERIFY(!tracker.take(10));
        QCOMPARE(tracker.dueIn(10), MinIntervalMs - 10);
        QVERIFY(!tracker.take(MinIntervalMs - 1));
        QCOMPARE(tracker.take(MinIntervalMs), std::optional<bool>(true));
    }

    void idleRecordGoesOnceAfterQuiet()
    {
        Tracker tracker;
        tracker.note(0);
        QCOMPARE(tracker.take(0), std::optional<bool>(true));
        QCOMPARE(tracker.dueIn(0), IdleMs);
        QVERIFY(!tracker.take(IdleMs - 1));
        QVERIFY(tracker.live(IdleMs - 1));
        QCOMPARE(tracker.take(IdleMs), std::optional<bool>(false));
        QVERIFY(!tracker.live(IdleMs));
        QVERIFY(!tracker.take(IdleMs + 5000)); // once
        QCOMPARE(tracker.dueIn(IdleMs + 5000), qint64(-1));
        // Activity again starts a new period with an active record at once.
        tracker.note(IdleMs + 6000);
        QCOMPARE(tracker.take(IdleMs + 6000), std::optional<bool>(true));
    }

    void pendingEventPostponesTheIdleRecord()
    {
        Tracker tracker;
        tracker.note(0);
        QCOMPARE(tracker.take(0), std::optional<bool>(true));
        tracker.note(IdleMs - 10);
        // The trailing active record comes first; idle only IdleMs after the last event.
        QCOMPARE(tracker.take(IdleMs), std::optional<bool>(true));
        QVERIFY(!tracker.take(IdleMs));
        QVERIFY(!tracker.take(2 * IdleMs - 11));
        QCOMPARE(tracker.take(2 * IdleMs - 10), std::optional<bool>(false));
    }

    void aBackwardsClockNeverStallsTheTracker()
    {
        Tracker tracker;
        tracker.note(100000);
        QCOMPARE(tracker.take(100000), std::optional<bool>(true));
        tracker.note(5); // the clock went back (suspend/restore of the monotonic source)
        QCOMPARE(tracker.dueIn(5), qint64(0));
        QCOMPARE(tracker.take(5), std::optional<bool>(true));
    }

    void announcedActiveFollowsTheLastRecord()
    {
        Tracker tracker;
        QVERIFY(!tracker.announcedActive());
        tracker.note(0);
        tracker.take(0);
        QVERIFY(tracker.announcedActive());
        tracker.take(IdleMs);
        QVERIFY(!tracker.announcedActive());
        tracker.note(IdleMs + 1);
        tracker.take(IdleMs + 1);
        tracker.reset();
        QVERIFY(!tracker.announcedActive());
        QVERIFY(!tracker.live(IdleMs + 2));
    }

    void deskRecordHasExactlyTheDocumentedKeys()
    {
        // No key code, key name, device, button or coordinate can ever be added without failing this.
        const auto json = record(true, 7, Context::Session);
        QCOMPARE(keySet(json), (QSet<QString>{QStringLiteral("type"), QStringLiteral("v"), QStringLiteral("active"), QStringLiteral("seq"), QStringLiteral("context")}));
        QCOMPARE(json.value(QStringLiteral("type")).toString(), QStringLiteral("desk-activity"));
        QCOMPARE(json.value(QStringLiteral("v")).toInt(), 1);
        QCOMPARE(json.value(QStringLiteral("active")).toBool(), true);
        QCOMPARE(json.value(QStringLiteral("seq")).toInt(), 7);
        QCOMPARE(record(false, 8, Context::Greeter).value(QStringLiteral("context")).toString(), QStringLiteral("greeter"));
        QCOMPARE(record(true, 9, Context::Locked).value(QStringLiteral("context")).toString(), QStringLiteral("locked"));
    }

    void pointerRecordHasExactlyTheDocumentedKeys()
    {
        const auto json = pointerRecord(QPoint(12, 34), true);
        QCOMPARE(keySet(json), (QSet<QString>{QStringLiteral("type"), QStringLiteral("v"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("visible")}));
        QCOMPARE(json.value(QStringLiteral("x")).toInt(), 12);
        QCOMPARE(json.value(QStringLiteral("y")).toInt(), 34);
    }

    void subscribeIsStrict_data()
    {
        QTest::addColumn<QJsonObject>("request");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<bool>("pointer");
        const auto base = [] { return QJsonObject{{QStringLiteral("type"), QStringLiteral("desk-activity-subscribe")}, {QStringLiteral("v"), 1}}; };
        auto with = [&](std::initializer_list<std::pair<QString, QJsonValue>> extra) {
            auto o = base();
            for (const auto &e : extra) o.insert(e.first, e.second);
            return o;
        };
        QTest::newRow("on with pointer") << with({{QStringLiteral("enabled"), true}, {QStringLiteral("pointer"), true}}) << true << true << true;
        QTest::newRow("on without pointer key") << with({{QStringLiteral("enabled"), true}}) << true << true << false;
        QTest::newRow("off") << with({{QStringLiteral("enabled"), false}, {QStringLiteral("pointer"), true}}) << true << false << false;
        QTest::newRow("no enabled") << base() << false << false << false;
        QTest::newRow("enabled not bool") << with({{QStringLiteral("enabled"), 1}}) << false << false << false;
        QTest::newRow("pointer not bool") << with({{QStringLiteral("enabled"), true}, {QStringLiteral("pointer"), QStringLiteral("yes")}}) << false << false << false;
        QTest::newRow("unknown key") << with({{QStringLiteral("enabled"), true}, {QStringLiteral("keys"), true}}) << false << false << false;
        QTest::newRow("wrong version") << with({{QStringLiteral("enabled"), true}, {QStringLiteral("v"), 2}}) << false << false << false;
    }
    void subscribeIsStrict()
    {
        QFETCH(QJsonObject, request);
        QFETCH(bool, valid);
        QFETCH(bool, enabled);
        QFETCH(bool, pointer);
        const auto parsed = parseSubscribe(request);
        QCOMPARE(parsed.has_value(), valid);
        if (valid) {
            QCOMPARE(parsed->enabled, enabled);
            QCOMPARE(parsed->pointer, pointer);
        }
    }

    void workspaceCaptureIsTheIdentityAndPerOutputAddsTheWireOrigin()
    {
        QVERIFY(toDesktop(false, {}, 0, QPoint(100, 50)) == QPoint(100, 50));
        QVector<VideoMonitor> atlas(2);
        atlas[0].geometry = QRect(0, 0, 1920, 1080);
        atlas[1].geometry = QRect(1920, 0, 2560, 1440);
        QVERIFY(toDesktop(true, atlas, 1, QPoint(10, 20)) == QPoint(10, 20));
        QVERIFY(toDesktop(true, atlas, 2, QPoint(10, 20)) == QPoint(1930, 20));
        QVERIFY(!toDesktop(true, atlas, 0, QPoint(10, 20)));
        QVERIFY(!toDesktop(true, atlas, 3, QPoint(10, 20)));
        QVERIFY(toDesktop(true, atlas, 2, QPoint(-5000, 7)) == QPoint(0, 7)); // clamped into the wire range
        QVERIFY(toDesktop(false, {}, 0, QPoint(999999, 0)) == QPoint(MaxCoordinate, 0));
    }

    void pointerIsSentOnChangeAndRateLimited()
    {
        HostPointerTracker tracker;
        QVERIFY(!tracker.take(0));
        tracker.report(1, true, QPoint(10, 10), 0);
        const auto first = tracker.take(0);
        QVERIFY(first && first->visible && first->position == QPoint(10, 10));
        QCOMPARE(tracker.dueIn(0), qint64(-1)); // unchanged: nothing to send
        tracker.report(1, true, QPoint(20, 20), 10);
        QCOMPARE(tracker.dueIn(10), MinPointerIntervalMs - 10);
        QVERIFY(!tracker.take(10));
        const auto second = tracker.take(MinPointerIntervalMs);
        QVERIFY(second && second->position == QPoint(20, 20));
        // Many moves inside one window collapse to the latest.
        for (int i = 0; i < 100; ++i) tracker.report(1, true, QPoint(i, i), MinPointerIntervalMs + 1);
        const auto third = tracker.take(2 * MinPointerIntervalMs);
        QVERIFY(third && third->position == QPoint(99, 99));
        QCOMPARE(tracker.dueIn(2 * MinPointerIntervalMs), qint64(-1));
    }

    void pointerHideWaitsAndCrossingOutputsDoesNotBlink()
    {
        HostPointerTracker tracker;
        tracker.report(1, true, QPoint(5, 5), 0);
        QVERIFY(tracker.take(0));
        // The pointer crosses to output 2: output 1 says "absent" a moment before output 2 says "visible".
        tracker.report(1, false, QPoint(), 100);
        QVERIFY(tracker.dueIn(100) >= PointerHideDelayMs - 1);
        QVERIFY(!tracker.take(110));
        tracker.report(2, true, QPoint(1930, 5), 120);
        const auto moved = tracker.take(200);
        QVERIFY(moved && moved->visible && moved->position == QPoint(1930, 5));
        // A real hide goes after the delay and keeps the last place.
        tracker.report(2, false, QPoint(), 300);
        QVERIFY(!tracker.take(300 + PointerHideDelayMs - 1));
        const auto hidden = tracker.take(300 + PointerHideDelayMs);
        QVERIFY(hidden && !hidden->visible && hidden->position == QPoint(1930, 5));
    }

    void resendMakesTheSameStateDueAgain()
    {
        HostPointerTracker tracker;
        tracker.report(0, true, QPoint(1, 2), 0);
        QVERIFY(tracker.take(0));
        QCOMPARE(tracker.dueIn(100), qint64(-1));
        tracker.resend();
        const auto again = tracker.take(100);
        QVERIFY(again && again->position == QPoint(1, 2));
    }

    void forgettingTheOnlyVisibleSourceHidesThePointer()
    {
        HostPointerTracker tracker;
        tracker.report(1, true, QPoint(3, 4), 0);
        QVERIFY(tracker.take(0));
        tracker.forget(1, 10);
        QVERIFY(!tracker.desired());
        tracker.reset(20);
        QCOMPARE(tracker.dueIn(20), qint64(-1));
    }

    void deskDeviceFilterAcceptsOnlyRealSeat0InputNodes_data()
    {
        QTest::addColumn<QString>("node");
        QTest::addColumn<QString>("syspath");
        QTest::addColumn<QString>("seat");
        QTest::addColumn<bool>("kbd");
        QTest::addColumn<bool>("mouse");
        QTest::addColumn<bool>("accepted");
        const QString real = QStringLiteral("/sys/devices/pci0000:00/0000:00:14.0/usb1/1-3/input/input5/event4");
        QTest::newRow("usb mouse") << QStringLiteral("/dev/input/event4") << real << QString() << false << true << true;
        QTest::newRow("keyboard on seat0") << QStringLiteral("/dev/input/event3") << real << QStringLiteral("seat0") << true << false << true;
        QTest::newRow("uinput virtual device (sunshine, a remote tool)") << QStringLiteral("/dev/input/event9")
            << QStringLiteral("/sys/devices/virtual/input/input9/event9") << QString() << true << true << false;
        QTest::newRow("another seat") << QStringLiteral("/dev/input/event4") << real << QStringLiteral("seat1") << false << true << false;
        QTest::newRow("not an event node (js, mouse0)") << QStringLiteral("/dev/input/mouse0") << real << QString() << false << true << false;
        QTest::newRow("a power button: no keyboard or pointer class") << QStringLiteral("/dev/input/event0") << real << QString() << false << false << false;
    }
    void deskDeviceFilterAcceptsOnlyRealSeat0InputNodes()
    {
        QFETCH(QString, node);
        QFETCH(QString, syspath);
        QFETCH(QString, seat);
        QFETCH(bool, kbd);
        QFETCH(bool, mouse);
        QFETCH(bool, accepted);
        QCOMPARE(PhysicalInputWatcher::isDeskDevice(node, syspath, seat, kbd, mouse, false, false), accepted);
    }

    void onlyRealDeskEventsCountAsActivity()
    {
        input_event event{};
        const auto activity = [&](quint16 type, quint16 code, qint32 value) {
            event.type = type;
            event.code = code;
            event.value = value;
            return PhysicalInputWatcher::isActivity(event);
        };
        QVERIFY(!activity(EV_LED, LED_CAPSL, 1)); // lock-key LED sync writes LEDs; it must not look like the desk
        QVERIFY(!activity(EV_SYN, SYN_REPORT, 0));
        QVERIFY(!activity(EV_MSC, MSC_SCAN, 458756));
        QVERIFY(!activity(EV_KEY, KEY_A, 0)); // key up
        QVERIFY(activity(EV_KEY, KEY_A, 1));
        QVERIFY(activity(EV_REL, REL_X, 3));
        QVERIFY(activity(EV_ABS, ABS_X, 0));
    }
};

QTEST_GUILESS_MAIN(DeskActivityTest)
#include "DeskActivityTest.moc"
