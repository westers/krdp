// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QElapsedTimer>
#include <csignal>
#include <cerrno>
#include <unistd.h>
#include "VirtualSessionGuardian.h"
using namespace KRdp;
using namespace Qt::StringLiterals;

class VirtualSessionGuardianTest : public QObject
{
    Q_OBJECT
    const QByteArray token{32, 'x'};
    QJsonObject message(const QString &session, const QString &op = u"status"_s)
    {
        return {{u"v"_s, 1}, {u"id"_s, u"request-1"_s}, {u"op"_s, op},
            {u"session"_s, session}, {u"token"_s, QString::fromLatin1(token.toHex())}};
    }
    QJsonObject exchange(QLocalSocket &socket, const QJsonObject &message)
    {
        socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        if (!QTest::qWaitFor([&] { return socket.canReadLine(); }, 2000)) return {};
        return QJsonDocument::fromJson(socket.readLine()).object();
    }
private Q_SLOTS:
    void executableTerminationStopsChild_data()
    {
        QTest::addColumn<int>("signal"); QTest::addColumn<bool>("ignore");
        QTest::newRow("TERM graceful") << int(SIGTERM) << false;
        QTest::newRow("INT graceful") << int(SIGINT) << false;
        QTest::newRow("TERM escalation") << int(SIGTERM) << true;
    }
    void executableTerminationStopsChild()
    {
        if (!getuid()) QSKIP("Guardian requires a nonroot desktop UID");
        QFETCH(int, signal); QFETCH(bool, ignore);
        QTemporaryDir runtime; QVERIFY(runtime.isValid());
        QTemporaryFile credential(runtime.filePath(u"credential.XXXXXX"_s));
        QVERIFY(credential.open()); QCOMPARE(credential.write(token), qint64(32));
        QVERIFY(credential.flush()); credential.close();
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto address = runtime.filePath(u"guardian.sock"_s);
        const auto ready = runtime.filePath(u"ready"_s), exited = runtime.filePath(u"exited"_s);
        QProcess process;
        process.setStandardInputFile(credential.fileName());
        process.start(QCoreApplication::applicationDirPath() + u"/krdp-virtual-guardian"_s,
            {u"--session"_s, id, u"--socket"_s, address, u"--token-fd"_s, u"0"_s, u"--"_s,
                QCoreApplication::applicationDirPath() + u"/GuardianSignalChild"_s, ready, exited,
                ignore ? u"ignore"_s : u"graceful"_s});
        QVERIFY(process.waitForStarted(3000));
        const auto cleanup = qScopeGuard([&] {
            if (process.state() != QProcess::NotRunning) {
                process.terminate();
                if (!process.waitForFinished(9000)) { process.kill(); process.waitForFinished(3000); }
            }
        });
        QJsonObject state;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            QFile readiness(ready);
            if (!readiness.open(QIODevice::ReadOnly)) return false;
            state = QJsonDocument::fromJson(readiness.readAll()).object();
            return state.contains(u"blocked"_s) && state.contains(u"pid"_s);
        })(), 3000);
        QVERIFY(state.contains(u"blocked"_s)); QVERIFY(!state.value(u"blocked"_s).toBool());
        const auto childPid = pid_t(state.value(u"pid"_s).toInteger()); QVERIFY(childPid > 1);
        // QProcess still owns this unreaped guardian, so its PID cannot be reused.
        QCOMPARE(process.state(), QProcess::Running);
        const auto guardianPid = pid_t(process.processId()); QVERIFY(guardianPid > 1);
        QElapsedTimer elapsed; elapsed.start();
        QCOMPARE(::kill(guardianPid, signal), 0);
        QVERIFY(process.waitForFinished(9000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit); QCOMPARE(process.exitCode(), 0);
        QCOMPARE(QFile::exists(exited), !ignore);
        if (ignore) QVERIFY(elapsed.elapsed() >= 4500);
        else QVERIFY(elapsed.elapsed() < 4500);
        // Observation only: never signal a child by this possibly stale PID.
        QCOMPARE(::kill(childPid, 0), -1); QCOMPARE(errno, ESRCH);
        QVERIFY(!QFile::exists(address));
    }
    void invalidLauncherIncarnationDoesNotSpawn()
    {
        QTemporaryDir runtime;
        const auto session = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto address = runtime.filePath(u"guardian.sock"_s);
        const QStringList invalid{u"not-a-uuid"_s, u"00000000-0000-0000-0000-000000000000"_s,
            u"{12345678-1234-1234-1234-123456789abc}"_s, u"12345678-1234-1234-1234-123456789ABC"_s};
        for (const auto &instance : invalid) {
            VirtualSessionGuardian guardian;
            QVERIFY(!guardian.start(getuid(), session, token, address,
                {u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}}, nullptr, instance));
            QCOMPARE(guardian.phase(), u"absent"_s);
            QCOMPARE(guardian.processId(), qint64(0));
            QVERIFY(!QFile::exists(address));
        }
    }
    void brokerDisconnectAndReconnectRetainsChild()
    {
        QTemporaryDir runtime;
        VirtualSessionGuardian guardian;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto address = runtime.filePath(u"guardian.sock"_s);
        QString error;
        QVERIFY2(guardian.start(getuid(), id, token, address, {u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}}, &error), qPrintable(error));
        QTRY_COMPARE(guardian.phase(), u"running"_s);
        const auto pid = guardian.processId();
        QVERIFY(pid > 0);
        {
            QLocalSocket broker;
            broker.connectToServer(address);
            QTRY_COMPARE(broker.state(), QLocalSocket::ConnectedState);
            const auto reply = exchange(broker, message(id));
            QVERIFY(reply.value(u"ok"_s).toBool());
            QCOMPARE(reply.value(u"state"_s), QJsonValue(u"running"_s));
            QVERIFY(!reply.contains(u"captureReady"_s));
            QVERIFY(!reply.contains(u"token"_s));
            broker.abort();
        }
        QTest::qWait(50);
        QCOMPARE(guardian.processId(), pid);
        QCOMPARE(guardian.phase(), u"running"_s);
        QLocalSocket nextBroker;
        nextBroker.connectToServer(address);
        QTRY_COMPARE(nextBroker.state(), QLocalSocket::ConnectedState);
        const auto reply = exchange(nextBroker, message(id));
        QVERIFY(reply.value(u"ok"_s).toBool());
        auto stop = message(id, u"stop"_s);
        stop.insert(u"instance"_s, reply.value(u"instance"_s));
        QVERIFY(exchange(nextBroker, stop).value(u"ok"_s).toBool());
        QTRY_COMPARE(guardian.phase(), u"exited"_s);
        QCOMPARE(guardian.processId(), qint64(0));
    }
    void executableRejectsInvalidExplicitInstance()
    {
        QTemporaryDir runtime;
        QTemporaryFile credential(runtime.filePath(u"token.XXXXXX"_s));
        QVERIFY(credential.open());
        QCOMPARE(credential.write(token), qint64(32));
        QVERIFY(credential.flush());
        credential.close();
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto address = runtime.filePath(u"guardian.sock"_s);
        for (const auto &instance : QStringList{QString(), u"not-a-uuid"_s,
                u"00000000-0000-0000-0000-000000000000"_s}) {
            QProcess process;
            process.setStandardInputFile(credential.fileName());
            process.start(QCoreApplication::applicationDirPath() + u"/krdp-virtual-guardian"_s,
                {u"--session"_s, id, u"--instance"_s, instance, u"--socket"_s, address,
                    u"--token-fd"_s, u"0"_s, u"--"_s, u"/usr/bin/sleep"_s, u"60"_s});
            QVERIFY(process.waitForFinished(3000));
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(process.exitCode(), 1);
            QVERIFY(!QFile::exists(address));
        }
    }
    void badTokenAndStaleInstanceCannotStopChild()
    {
        QTemporaryDir runtime;
        VirtualSessionGuardian guardian;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto address = runtime.filePath(u"guardian.sock"_s);
        QVERIFY(guardian.start(getuid(), id, token, address, {u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}}));
        QTRY_COMPARE(guardian.phase(), u"running"_s);
        for (bool badToken : {true, false}) {
            QLocalSocket socket;
            socket.connectToServer(address);
            QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
            auto stop = message(id, u"stop"_s);
            stop.insert(u"instance"_s, badToken ? guardian.incarnation() : QUuid::createUuid().toString(QUuid::WithoutBraces));
            if (badToken) stop.insert(u"token"_s, QString(64, QLatin1Char('0')));
            const auto reply = exchange(socket, stop);
            QVERIFY(reply.contains(u"ok"_s));
            QVERIFY(!reply.value(u"ok"_s).toBool());
            QCOMPARE(guardian.phase(), u"running"_s);
        }
    }
    void refusesExistingSocketAndInheritedEnvironment()
    {
        QTemporaryDir runtime;
        VirtualSessionGuardian first, second;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto address = runtime.filePath(u"guardian.sock"_s);
        VirtualSessionSupervisor::Launch launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        QVERIFY(first.start(getuid(), id, token, address, launch));
        QVERIFY(!second.start(getuid(), id, token, address, launch));
        launch.environment = QProcessEnvironment(QProcessEnvironment::InheritFromParent);
        QVERIFY(!second.start(getuid(), id, token, runtime.filePath(u"other.sock"_s), launch));
    }
    void independentExecutableSurvivesBrokerSocketLoss()
    {
        QTemporaryDir runtime;
        QTemporaryFile credential(runtime.filePath(u"token.XXXXXX"_s));
        QVERIFY(credential.open());
        QCOMPARE(credential.write(token), qint64(32));
        QVERIFY(credential.flush());
        credential.close();
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto plannedInstance = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto address = runtime.filePath(u"guardian.sock"_s);
        QProcess guardian;
        guardian.setStandardInputFile(credential.fileName());
        guardian.start(QCoreApplication::applicationDirPath() + u"/krdp-virtual-guardian"_s,
            {u"--session"_s, id, u"--instance"_s, plannedInstance, u"--socket"_s, address,
                u"--token-fd"_s, u"0"_s, u"--"_s, u"/usr/bin/sleep"_s, u"60"_s});
        QVERIFY(guardian.waitForStarted());
        QTRY_VERIFY(QFile::exists(address));
        QString instance;
        {
            QLocalSocket first;
            first.connectToServer(address);
            QTRY_COMPARE(first.state(), QLocalSocket::ConnectedState);
            const auto reply = exchange(first, message(id));
            QVERIFY(reply.value(u"ok"_s).toBool());
            instance = reply.value(u"instance"_s).toString();
            QCOMPARE(instance, plannedInstance);
            first.abort();
        }
        QCOMPARE(guardian.state(), QProcess::Running);
        QLocalSocket replacement;
        replacement.connectToServer(address);
        QTRY_COMPARE(replacement.state(), QLocalSocket::ConnectedState);
        const auto status = exchange(replacement, message(id));
        QVERIFY(status.value(u"ok"_s).toBool());
        QVERIFY(status.value(u"processRunning"_s).toBool());
        QCOMPARE(status.value(u"instance"_s).toString(), instance);
        auto stop = message(id, u"stop"_s);
        stop.insert(u"instance"_s, instance);
        QVERIFY(exchange(replacement, stop).value(u"ok"_s).toBool());
        QTRY_COMPARE(guardian.state(), QProcess::NotRunning);
        QCOMPARE(guardian.exitCode(), 0);
    }
};
QTEST_GUILESS_MAIN(VirtualSessionGuardianTest)
#include "VirtualSessionGuardianTest.moc"
