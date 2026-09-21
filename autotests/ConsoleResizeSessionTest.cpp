// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "ConsoleResizeSession.h"
#include <utility>

using namespace KRdp;
class ConsoleResizeSessionTest : public QObject
{
    Q_OBJECT
    QByteArray snapshot(bool resized = false)
    {
        return QStringLiteral(R"({"outputs":[{"name":"DP-3","connected":true,"enabled":true,"currentModeId":"%1","scale":1,"modes":[
            {"id":"1","refreshRate":60,"size":{"width":1920,"height":1080}},
            {"id":"2","refreshRate":60,"size":{"width":1280,"height":720}}]}]})").arg(resized ? 2 : 1).toUtf8();
    }
    const ConsoleWorkerWire::Resize request{11, 1, QStringLiteral("DP-3"), QSize(1280, 720), 1};
    const ConsoleWorkerWire::Outputs resized{{{QStringLiteral("DP-3"), QRect(0, 0, 1280, 720), 1, true}}};
private Q_SLOTS:
    void waitsForMatchingKeyframe()
    {
        int calls = 0;
        ConsoleResizeSession session(nullptr, [&](auto, auto reply) { reply(true, ++calls == 1 ? snapshot() : snapshot(true)); });
        QList<ConsoleWorkerWire::ResizeResult> results;
        connect(&session, &ConsoleResizeSession::result, this, [&](auto result) { results.append(result); });
        session.setControl({1, true});
        session.request(request);
        QVERIFY(!session.inputAllowed());
        QCOMPARE(results.size(), 0);
        session.captured(resized, false);
        session.captured({{{QStringLiteral("DP-3"), QRect(0, 0, 1920, 1080), 1, true}}}, true);
        QCOMPARE(results.size(), 0);
        session.captured(resized, true);
        QCOMPARE(results.size(), 1);
        QVERIFY(results.first().error.isEmpty());
        QVERIFY(session.inputAllowed());
    }

    void controlLossDuringApplyRestoresInsteadOfSucceeding()
    {
        ConsoleResizeExecutor::Reply pending;
        QStringList args;
        ConsoleResizeSession session(nullptr, [&](auto arguments, auto reply) { args = arguments; pending = reply; });
        QList<ConsoleWorkerWire::ResizeResult> results;
        connect(&session, &ConsoleResizeSession::result, this, [&](auto result) { results.append(result); });
        session.setControl({1, true});
        session.request(request);
        std::exchange(pending, {})(true, snapshot());
        session.setControl({2, false});
        std::exchange(pending, {})(true, {}); // In-flight apply completed after revocation.
        std::exchange(pending, {})(true, snapshot(true));
        QCOMPARE(results.size(), 1);
        QVERIFY(!results.first().error.isEmpty());
        QCOMPARE(args, QStringList{QStringLiteral("-j")});
        std::exchange(pending, {})(true, snapshot(true));
        QCOMPARE(args.first(), QStringLiteral("output.DP-3.mode.1"));
        std::exchange(pending, {})(true, {});
        std::exchange(pending, {})(true, snapshot());
        QVERIFY(session.changing());
        QVERIFY(!session.inputAllowed());
        session.setControl({3, true}); // New owner cannot use old coordinates during recovery.
        QVERIFY(!session.inputAllowed());
        session.captured(resized, true);
        QVERIFY(!session.inputAllowed());
        session.captured({{{QStringLiteral("DP-3"), QRect(0, 0, 1920, 1080), 1, true}}}, true);
        QVERIFY(!session.changing());
        QVERIFY(session.inputAllowed());
    }

    void stopWaitsForOutstandingApplyAndRestore()
    {
        ConsoleResizeExecutor::Reply pending;
        ConsoleResizeSession session(nullptr, [&](auto, auto reply) { pending = reply; });
        int stopped = 0;
        connect(&session, &ConsoleResizeSession::stopped, this, [&](const QString &error) { QVERIFY(error.isEmpty()); ++stopped; });
        session.setControl({1, true});
        session.request(request);
        std::exchange(pending, {})(true, snapshot());
        session.stop();
        QCOMPARE(stopped, 0);
        std::exchange(pending, {})(true, {});
        std::exchange(pending, {})(true, snapshot(true));
        QCOMPARE(stopped, 0);
        std::exchange(pending, {})(true, snapshot(true));
        std::exchange(pending, {})(true, {});
        std::exchange(pending, {})(true, snapshot());
        QCOMPARE(stopped, 1);
        QVERIFY(!session.inputAllowed());
    }

    void refusesStaleGenerationWithoutRunningCommands()
    {
        int commands = 0;
        ConsoleResizeSession session(nullptr, [&](auto, auto) { ++commands; });
        int refused = 0;
        connect(&session, &ConsoleResizeSession::result, this, [&](auto result) { QVERIFY(!result.error.isEmpty()); ++refused; });
        session.setControl({2, true});
        session.request(request);
        QCOMPARE(commands, 0);
        QCOMPARE(refused, 1);
    }

    void losingControlWhileWaitingForCaptureCancelsSuccess()
    {
        int commands = 0;
        ConsoleResizeSession session(nullptr, [&](auto, auto reply) {
            ++commands;
            reply(true, commands == 1 || commands == 6 ? snapshot() : snapshot(true));
        });
        QList<ConsoleWorkerWire::ResizeResult> results;
        connect(&session, &ConsoleResizeSession::result, this, [&](auto result) { results.append(result); });
        session.setControl({1, true});
        session.request(request);
        QCOMPARE(results.size(), 0);
        session.setControl({2, false});
        QCOMPARE(results.size(), 1);
        QVERIFY(!results.first().error.isEmpty());
        session.captured(resized, true); // Late frame cannot acknowledge the cancelled request.
        QCOMPARE(results.size(), 1);
        QCOMPARE(commands, 6);
    }
};
QTEST_GUILESS_MAIN(ConsoleResizeSessionTest)
#include "ConsoleResizeSessionTest.moc"
