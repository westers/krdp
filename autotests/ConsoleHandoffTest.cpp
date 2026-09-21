// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "ConsoleHandoff.h"

using namespace KRdp;

namespace
{
ConsoleSeat::Session greeter()
{
    return {QStringLiteral("1"), QStringLiteral("sddm"), QStringLiteral("seat0"), QStringLiteral("wayland"), QStringLiteral("greeter"), QStringLiteral("active"), true, 107};
}

ConsoleSeat::Session user()
{
    return {QStringLiteral("3"), QStringLiteral("westers"), QStringLiteral("seat0"), QStringLiteral("wayland"), QStringLiteral("user"), QStringLiteral("active"), true, 1000};
}
}

class ConsoleHandoffTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void waitsForWorkerBeforeGrantingInput();
    void revokesGreeterBeforeStartingUser();
    void ignoresAStaleWorkerReady();
};

void ConsoleHandoffTest::waitsForWorkerBeforeGrantingInput()
{
    ConsoleHandoff::State state;
    const ConsoleHandoff::Target target = ConsoleHandoff::targetFor({greeter()});
    const auto start = state.select(target);
    QVERIFY(start.startWorker);
    QCOMPARE(start.target, target);
    QVERIFY(!state.inputEnabled());

    const auto ready = state.workerReady(target);
    QVERIFY(ready.grantInput);
    QVERIFY(ready.resetGraphics);
    QVERIFY(ready.requestKeyFrame);
    QCOMPARE(state.activeTarget(), target);
    QVERIFY(state.inputEnabled());
}

void ConsoleHandoffTest::revokesGreeterBeforeStartingUser()
{
    ConsoleHandoff::State state;
    const ConsoleHandoff::Target oldTarget = ConsoleHandoff::targetFor({greeter()});
    const ConsoleHandoff::Target newTarget = ConsoleHandoff::targetFor({greeter(), user()});
    state.select(oldTarget);
    state.workerReady(oldTarget);

    const auto revoke = state.select(newTarget);
    QVERIFY(revoke.revokeInput);
    QVERIFY(revoke.stopWorker);
    QVERIFY(!revoke.startWorker);
    QVERIFY(!state.inputEnabled());

    const auto start = state.workerStopped();
    QVERIFY(start.startWorker);
    QCOMPARE(start.target, newTarget);
    const auto ready = state.workerReady(newTarget);
    QVERIFY(ready.grantInput);
    QVERIFY(ready.resetGraphics);
    QVERIFY(ready.requestKeyFrame);
}

void ConsoleHandoffTest::ignoresAStaleWorkerReady()
{
    ConsoleHandoff::State state;
    const ConsoleHandoff::Target oldTarget = ConsoleHandoff::targetFor({greeter()});
    const ConsoleHandoff::Target newTarget = ConsoleHandoff::targetFor({user()});
    state.select(oldTarget);
    state.select(newTarget);
    state.workerStopped();

    QVERIFY(state.workerReady(oldTarget).empty());
    QVERIFY(!state.inputEnabled());
    QVERIFY(state.workerReady(newTarget).grantInput);
}

QTEST_GUILESS_MAIN(ConsoleHandoffTest)

#include "ConsoleHandoffTest.moc"
