// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "ConsoleSeat.h"

using namespace KRdp::ConsoleSeat;

class ConsoleSeatTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void choosesGreeterBeforeLogin();
    void prefersPhysicalUserDuringHandoff();
    void ignoresInactiveAndRemoteSessions();
};

void ConsoleSeatTest::choosesGreeterBeforeLogin()
{
    const QList<Session> sessions{{QStringLiteral("1"), QStringLiteral("sddm"), QStringLiteral("seat0"), QStringLiteral("wayland"), QStringLiteral("greeter"), QStringLiteral("active"), true}};
    QCOMPARE(adapterFor(sessions), Adapter::Greeter);
    QCOMPARE(activeSessionId(sessions, Adapter::Greeter), QStringLiteral("1"));
}

void ConsoleSeatTest::prefersPhysicalUserDuringHandoff()
{
    const QList<Session> sessions{
        {QStringLiteral("1"), QStringLiteral("sddm"), QStringLiteral("seat0"), QStringLiteral("wayland"), QStringLiteral("greeter"), QStringLiteral("active"), true},
        {QStringLiteral("3"), QStringLiteral("westers"), QStringLiteral("seat0"), QStringLiteral("wayland"), QStringLiteral("user"), QStringLiteral("active"), true},
    };
    QCOMPARE(adapterFor(sessions), Adapter::PhysicalUser);
    QCOMPARE(activeSessionId(sessions, Adapter::PhysicalUser), QStringLiteral("3"));
    QVERIFY(activeSessionId(sessions, Adapter::VirtualUser).isEmpty());
    QVERIFY(!activeSessionUid(sessions, Adapter::VirtualUser));
}

void ConsoleSeatTest::ignoresInactiveAndRemoteSessions()
{
    const QList<Session> sessions{
        {QStringLiteral("7"), QStringLiteral("westers"), QString(), QStringLiteral("unspecified"), QStringLiteral("user"), QStringLiteral("active"), true},
        {QStringLiteral("8"), QStringLiteral("sddm"), QStringLiteral("seat0"), QStringLiteral("wayland"), QStringLiteral("greeter"), QStringLiteral("closing"), true},
        {QStringLiteral("9"), QStringLiteral("westers"), QStringLiteral("seat0"), QStringLiteral("wayland"), QStringLiteral("user"), QStringLiteral("active"), false},
    };
    QCOMPARE(adapterFor(sessions), Adapter::None);
}

QTEST_GUILESS_MAIN(ConsoleSeatTest)

#include "ConsoleSeatTest.moc"
