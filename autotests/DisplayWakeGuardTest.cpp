// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QSet>
#include <memory>
#include <utility>
#include "DisplayWakeGuard.h"

using namespace Qt::StringLiterals;

class FakeScreenSaver : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")
public:
    bool delayed = false;
    uint sequence = 0;
    int activity = 0;
    int releases = 0;
    QSet<uint> held;
    QList<QPair<QDBusMessage, uint>> pending;
    void answer(const QDBusConnection &bus)
    {
        const auto replies = std::exchange(pending, {});
        for (const auto &reply : replies) QVERIFY(bus.send(reply.first.createReply(QVariant::fromValue(reply.second))));
    }
public Q_SLOTS:
    uint Inhibit(const QString &, const QString &)
    {
        const uint cookie = ++sequence;
        held.insert(cookie);
        if (delayed) { setDelayedReply(true); pending.append({message(), cookie}); }
        return cookie;
    }
    void UnInhibit(uint cookie) { held.remove(cookie); ++releases; }
    void SimulateUserActivity() { ++activity; }
};

class FakePowerDevil : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.Solid.PowerManagement")
public:
    int wakes = 0;
    bool delayed = false;
    QList<QDBusMessage> pending;
    void failPending(const QDBusConnection &bus)
    {
        const auto replies = std::exchange(pending, {});
        for (const auto &request : replies)
            QVERIFY(bus.send(request.createErrorReply(QDBusError::Failed, u"delayed wake failed"_s)));
    }
public Q_SLOTS:
    void wakeup()
    {
        ++wakes;
        if (delayed) { setDelayedReply(true); pending.append(message()); }
    }
};

class DisplayWakeGuardTest : public QObject
{
    Q_OBJECT
    QDBusConnection m_service{u"not-connected"_s};
    QDBusConnection m_client{u"not-connected"_s};
    FakeScreenSaver m_saver;
    FakePowerDevil m_power;
private Q_SLOTS:
    void initTestCase()
    {
        // Run with dbus-run-session on Sol. Refuse a real desktop bus even
        // when someone accidentally sets the marker there.
        QVERIFY(qEnvironmentVariable("FARSIDE_TEST_ISOLATED_BUS") == u"1"_s);
        const auto address = qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS");
        QVERIFY(!address.isEmpty());
        m_service = QDBusConnection::connectToBus(address, u"wake-test-service"_s);
        m_client = QDBusConnection::connectToBus(address, u"wake-test-client"_s);
        QVERIFY(m_service.isConnected() && m_client.isConnected());
        QVERIFY(!m_service.interface()->isServiceRegistered(u"org.freedesktop.ScreenSaver"_s).value());
        QVERIFY(!m_service.interface()->isServiceRegistered(u"org.kde.Solid.PowerManagement"_s).value());
        QVERIFY(m_service.registerService(u"org.freedesktop.ScreenSaver"_s));
        QVERIFY(m_service.registerService(u"org.kde.Solid.PowerManagement"_s));
        QVERIFY(m_service.registerObject(u"/ScreenSaver"_s, &m_saver, QDBusConnection::ExportAllSlots));
        QVERIFY(m_service.registerObject(u"/org/kde/Solid/PowerManagement"_s, &m_power, QDBusConnection::ExportAllSlots));
    }
    void init()
    {
        QTRY_VERIFY(m_saver.held.isEmpty());
        m_saver.delayed = m_power.delayed = false;
        m_saver.activity = m_saver.releases = m_power.wakes = 0;
        QVERIFY(m_saver.pending.isEmpty() && m_power.pending.isEmpty());
    }
    void multipleViewersKeepOneCookie()
    {
        DisplayWakeGuard guard(m_client);
        guard.acquire();
        guard.acquire();
        QTRY_COMPARE(m_saver.held.size(), 1);
        QTRY_COMPARE(m_power.wakes, 1);
        guard.release();
        QTest::qWait(20);
        QCOMPARE(m_saver.held.size(), 1);
        guard.release();
        QTRY_VERIFY(m_saver.held.isEmpty());
        QCOMPARE(m_saver.releases, 1);
        QCOMPARE(m_saver.activity, 0);
    }
    void disabledNeverWakesOrInhibits()
    {
        DisplayWakeGuard guard(m_client);
        guard.setEnabled(false);
        guard.acquire();
        QTest::qWait(30);
        QCOMPARE(m_power.wakes, 0);
        QVERIFY(m_saver.held.isEmpty());
        guard.setEnabled(true);
        QTRY_COMPARE(m_saver.held.size(), 1);
        guard.setEnabled(false);
        QTRY_VERIFY(m_saver.held.isEmpty());
        guard.release();
    }
    void releasedBeforeInhibitReply()
    {
        m_saver.delayed = true;
        DisplayWakeGuard guard(m_client);
        guard.acquire();
        QTRY_COMPARE(m_saver.pending.size(), 1);
        guard.release();
        m_saver.answer(m_service);
        QTRY_VERIFY(m_saver.held.isEmpty());
        QCOMPARE(m_saver.releases, 1);
    }
    void destroyedBeforeInhibitReply()
    {
        m_saver.delayed = true;
        auto guard = std::make_unique<DisplayWakeGuard>(m_client);
        guard->acquire();
        QTRY_COMPARE(m_saver.pending.size(), 1);
        guard.reset();
        m_saver.answer(m_service);
        QTRY_VERIFY(m_saver.held.isEmpty());
        QCOMPARE(m_saver.releases, 1);
    }
    void destroyedAfterCookieArrives()
    {
        auto guard = std::make_unique<DisplayWakeGuard>(m_client);
        guard->acquire();
        QTRY_COMPARE(m_saver.held.size(), 1);
        guard.reset();
        QTRY_VERIFY(m_saver.held.isEmpty());
    }
    void lateWakeFailureCannotTriggerActivityAfterRelease()
    {
        m_power.delayed = true;
        DisplayWakeGuard guard(m_client);
        guard.acquire();
        QTRY_COMPARE(m_power.pending.size(), 1);
        guard.release();
        m_power.failPending(m_service);
        QTRY_VERIFY(m_saver.held.isEmpty());
        QTest::qWait(30);
        QCOMPARE(m_saver.activity, 0);
    }
};
QTEST_GUILESS_MAIN(DisplayWakeGuardTest)
#include "DisplayWakeGuardTest.moc"
