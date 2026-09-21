// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "ConsoleResizeExecutor.h"

using namespace KRdp;
class ConsoleResizeExecutorTest : public QObject
{
    Q_OBJECT
    QByteArray snapshot(QString mode = QStringLiteral("1"), double scale = 1.25)
    {
        return QStringLiteral(R"({"outputs":[{"name":"DP-3","connected":true,"enabled":true,"currentModeId":"%1","scale":%2,"modes":[
            {"id":"1","refreshRate":60,"size":{"width":1920,"height":1080}},
            {"id":"2","refreshRate":60,"size":{"width":1280,"height":720}}]}]})").arg(mode).arg(scale).toUtf8();
    }
private Q_SLOTS:
    void unsupportedAndNoopDoNotRunModeCommands()
    {
        int commands = 0;
        QString error;
        ConsoleResizeExecutor executor(nullptr, [&](auto args, auto reply) {
            ++commands;
            QCOMPARE(args, QStringList{QStringLiteral("-j")});
            reply(true, snapshot());
        });
        connect(&executor, &ConsoleResizeExecutor::finished, this, [&](auto, const QString &message) { error = message; });
        QVERIFY(executor.resize(QStringLiteral("DP-3"), {1536, 864}, 1));
        QVERIFY(!error.isEmpty());
        QVERIFY(executor.resize(QStringLiteral("DP-3"), {1920, 1080}, 1.25));
        QVERIFY(error.isEmpty());
        QCOMPARE(commands, 2);
    }

    void restoresAndVerifiesOriginal()
    {
        const auto plan = ConsoleResize::plan(snapshot(), QStringLiteral("DP-3"), {1280, 720}, 1);
        QStringList arguments;
        ConsoleResizeExecutor::Reply pending;
        ConsoleResizeExecutor executor(nullptr, [&](auto args, auto reply) { arguments = args; pending = reply; });
        int completions = 0;
        connect(&executor, &ConsoleResizeExecutor::finished, this, [&](auto, const QString &error) { QVERIFY(error.isEmpty()); ++completions; });
        QVERIFY(executor.restore(plan));
        std::exchange(pending, {})(true, snapshot(QStringLiteral("2"), 1));
        QCOMPARE(arguments, plan.restore);
        std::exchange(pending, {})(true, {});
        QCOMPARE(completions, 0);
        std::exchange(pending, {})(true, snapshot());
        QCOMPARE(completions, 1);
    }

    void appliesAndVerifiesWithoutBlocking()
    {
        QStringList arguments;
        ConsoleResizeExecutor::Reply pending;
        ConsoleResizeExecutor executor(nullptr, [&](auto args, auto reply) { arguments = args; pending = reply; });
        int completions = 0;
        connect(&executor, &ConsoleResizeExecutor::finished, this, [&](auto, const QString &error) { QVERIFY(error.isEmpty()); ++completions; });
        QVERIFY(executor.resize(QStringLiteral("DP-3"), {1280, 720}, 1));
        QVERIFY(executor.busy());
        QVERIFY(!executor.resize(QStringLiteral("DP-3"), {1920, 1080}, 1));
        QCOMPARE(arguments, QStringList{QStringLiteral("-j")});
        std::exchange(pending, {})(true, snapshot());
        QCOMPARE(arguments.first(), QStringLiteral("output.DP-3.mode.2"));
        std::exchange(pending, {})(true, {});
        QCOMPARE(completions, 0); // Process exit alone is not success.
        QCOMPARE(arguments, QStringList{QStringLiteral("-j")});
        std::exchange(pending, {})(true, snapshot(QStringLiteral("2"), 1));
        QCOMPARE(completions, 1);
        QVERIFY(!executor.busy());
    }
    void partialApplyRollsBackOnlyChangedFields()
    {
        QStringList arguments;
        ConsoleResizeExecutor::Reply pending;
        ConsoleResizeExecutor executor(nullptr, [&](auto args, auto reply) { arguments = args; pending = reply; });
        QString failure;
        connect(&executor, &ConsoleResizeExecutor::finished, this, [&](auto, const QString &error) { failure = error; });
        QVERIFY(executor.resize(QStringLiteral("DP-3"), {1280, 720}, 1));
        std::exchange(pending, {})(true, snapshot());
        std::exchange(pending, {})(false, {});
        std::exchange(pending, {})(true, snapshot(QStringLiteral("2"), 1.25)); // Only mode changed.
        QCOMPARE(arguments, QStringList{QStringLiteral("output.DP-3.mode.1")});
        std::exchange(pending, {})(true, {});
        std::exchange(pending, {})(true, snapshot());
        QVERIFY(failure.contains(QStringLiteral("original mode restored")));
        QVERIFY(!executor.busy());
    }
    void restorePreservesLocalChanges()
    {
        const auto plan = ConsoleResize::plan(snapshot(), QStringLiteral("DP-3"), {1280, 720}, 1);
        int commands = 0;
        ConsoleResizeExecutor executor(nullptr, [&](auto args, auto reply) {
            ++commands;
            QCOMPARE(args, QStringList{QStringLiteral("-j")});
            reply(true, snapshot(QStringLiteral("2"), 1.5)); // A person changed scale.
        });
        QVERIFY(executor.restore(plan));
        QCOMPARE(commands, 1);
        QVERIFY(!executor.busy());
    }
    void lateReplyAfterDestructionIsIgnored()
    {
        ConsoleResizeExecutor::Reply pending;
        auto *executor = new ConsoleResizeExecutor(nullptr, [&](auto, auto reply) { pending = reply; });
        QVERIFY(executor->resize(QStringLiteral("DP-3"), {1280, 720}, 1));
        delete executor;
        pending(true, snapshot());
    }
};
QTEST_GUILESS_MAIN(ConsoleResizeExecutorTest)
#include "ConsoleResizeExecutorTest.moc"
