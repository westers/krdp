// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX F6: the console worker is only started once its logind session is
// ready. On Sol the worker started right after SDDM handed over, before the
// new session's Wayland socket existed, and aborted (no Qt platform plugin);
// the broker only recovered on its 30 s failure backoff.
//
// The launcher's session lookup is a real ConsoleSeatWatcher fed by
// FakeLogindBus, the Wayland socket a real AF_UNIX socket in a temporary
// directory, and the process start a recorder (the real starter drops
// privileges, which a test cannot).

#include <QLocalServer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ConsoleSessionReadiness.h"
#include "ConsoleSeatWatcher.h"
#include "ConsoleWorkerLauncher.h"
#include "FakeLogindBus.h"

namespace KRdp
{
namespace
{
const QString SessionPath = QStringLiteral("/org/freedesktop/login1/session/_32");
constexpr quint32 Uid = 1000;

struct Harness {
    std::shared_ptr<FakeLogindBus::State> logind = std::make_shared<FakeLogindBus::State>();
    std::unique_ptr<ConsoleSeatWatcher> watcher;
    QTemporaryDir runtime;
    ConsoleWorkerLauncher launcher{QStringLiteral("/nonexistent/krdp-console-worker")};
    QStringList started;
    bool environmentPublished = true;

    explicit Harness(const QString &state)
    {
        auto session = logindSession(QStringLiteral("2"), SessionPath, Uid, QStringLiteral("user"));
        session.state = state;
        session.active = state == QLatin1String("active");
        logind->sessions = {session};
        watcher = std::make_unique<ConsoleSeatWatcher>(std::make_unique<FakeLogindBus>(logind));
        launcher.setSessionLookup([this](const QString &id) { return watcher->session(id); });
        launcher.setEnvironmentProbe([this](const ConsoleSeat::Session &, QString *error) {
            if (!environmentPublished) {
                *error = QStringLiteral("no process in the logind session has a usable Wayland environment");
                return QProcessEnvironment();
            }
            QProcessEnvironment environment;
            environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime.path());
            environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-0"));
            environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("unix:path=/dev/null"));
            return environment;
        });
        launcher.setStarter([this](const ConsoleHandoff::Target &target, const ConsoleSeat::Session &session, const QString &socketName,
                                   const QByteArray &, QProcessEnvironment environment, QString *) {
            // What the worker would get: the ready session and its display.
            if (session.state != QLatin1String("active") || environment.value(QStringLiteral("WAYLAND_DISPLAY")).isEmpty()) return false;
            started.append(target.sessionId + QLatin1Char('@') + socketName);
            return true;
        });
    }
    bool start()
    {
        QSignalSpy published(watcher.get(), &ConsoleSeatWatcher::sessionsChanged);
        watcher->start();
        return published.wait(2000) || watcher->session(QStringLiteral("2")).has_value();
    }
    void setState(const QString &state)
    {
        logind->find(QStringLiteral("2"))->state = state;
        logind->find(QStringLiteral("2"))->active = state == QLatin1String("active");
        logind->propertiesChanged(SessionPath);
    }
    QString socketPath() const { return runtime.filePath(QStringLiteral("wayland-0")); }
    static ConsoleHandoff::Target target()
    {
        return {ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("2"), Uid};
    }
};
}

class ConsoleWorkerLaunchReadinessTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readinessDecisions()
    {
        using namespace ConsoleSessionReadiness;
        ConsoleSeat::Session session = logindSession(QStringLiteral("2"), SessionPath, Uid, QStringLiteral("user"));
        QProcessEnvironment environment;
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), QStringLiteral("/run/user/1000"));
        environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-0"));
        QCOMPARE(waylandSocketPath(environment), QStringLiteral("/run/user/1000/wayland-0"));
        auto absolute = environment;
        absolute.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("/tmp/w"));
        QCOMPARE(waylandSocketPath(absolute), QStringLiteral("/tmp/w"));

        Observation ready{session, Uid, environment, {}, true};
        QCOMPARE(evaluate(ready).verdict, Verdict::Ready);
        auto opening = ready;
        opening.session->state = QStringLiteral("opening");
        opening.session->active = false;
        QCOMPARE(evaluate(opening).verdict, Verdict::Wait);
        auto online = ready;
        online.session->state = QStringLiteral("online");
        QCOMPARE(evaluate(online).verdict, Verdict::Wait);
        auto closing = ready;
        closing.session->state = QStringLiteral("closing");
        QCOMPARE(evaluate(closing).verdict, Verdict::Gone);
        auto gone = ready;
        gone.session.reset();
        QCOMPARE(evaluate(gone).verdict, Verdict::Gone);
        auto otherOwner = ready;
        otherOwner.expectedUid = 1001;
        QCOMPARE(evaluate(otherOwner).verdict, Verdict::Gone);
        auto noEnvironment = ready;
        noEnvironment.environment = {};
        QCOMPARE(evaluate(noEnvironment).verdict, Verdict::Wait);
        // The Sol case: the session is active and names its display, but the
        // compositor has not created the socket yet.
        auto noSocket = ready;
        noSocket.socketConnectable = false;
        QCOMPARE(evaluate(noSocket).verdict, Verdict::Wait);
        QVERIFY(evaluate(noSocket).reason.contains(QStringLiteral("wayland-0")));

        // Backoff: 100 ms doubling to a 2 s cap, inside a bounded budget.
        QCOMPARE(retryDelayMs(0), 100);
        QCOMPARE(retryDelayMs(1), 200);
        QCOMPARE(retryDelayMs(4), 1600);
        QCOMPARE(retryDelayMs(5), 2000);
        QCOMPARE(retryDelayMs(40), 2000);
        int total = 0, attempts = 0;
        while (total < WaitBudgetMs) total += retryDelayMs(attempts++);
        QVERIFY(attempts < 25);
    }

    void startsImmediatelyWhenReady()
    {
        Harness h(QStringLiteral("active"));
        QVERIFY(h.start());
        QLocalServer compositor;
        QVERIFY(compositor.listen(h.socketPath()));
        QSignalSpy exited(&h.launcher, &ConsoleWorkerLauncher::workerExited);
        QString error;
        QVERIFY2(h.launcher.launch(Harness::target(), QStringLiteral("w1"), QByteArray(32, 't'), &error), qPrintable(error));
        QCOMPARE(h.started, QStringList{QStringLiteral("2@w1")});
        QCOMPARE(h.launcher.waitingCount(), 0);
        QTest::qWait(50);
        QCOMPARE(exited.size(), 0);
    }

    // The Sol sequence: logind reports the new session before its compositor
    // has a socket; the worker must wait for it instead of crashing.
    void waitsForOpeningSessionAndItsSocket()
    {
        Harness h(QStringLiteral("opening"));
        h.environmentPublished = false;
        QVERIFY(h.start());
        QSignalSpy exited(&h.launcher, &ConsoleWorkerLauncher::workerExited);
        QVERIFY(h.launcher.launch(Harness::target(), QStringLiteral("w1"), QByteArray(32, 't')));
        QVERIFY(h.started.isEmpty());
        QCOMPARE(h.launcher.waitingCount(), 1);
        QTest::qWait(250);
        QVERIFY(h.started.isEmpty());

        // Active, with an environment, but no socket yet.
        h.setState(QStringLiteral("active"));
        h.environmentPublished = true;
        QTest::qWait(400);
        QVERIFY(h.started.isEmpty());

        // A stale socket file (nothing listening) is not ready either.
        const QByteArray path = QFile::encodeName(h.socketPath());
        const int stale = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(stale >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        qstrncpy(address.sun_path, path.constData(), sizeof(address.sun_path));
        QCOMPARE(::bind(stale, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
        QTest::qWait(500);
        QVERIFY(h.started.isEmpty());
        ::close(stale);
        QVERIFY(QFile::remove(h.socketPath()));

        // The compositor comes up: the worker starts, with no failure reported.
        QLocalServer compositor;
        QVERIFY(compositor.listen(h.socketPath()));
        QTRY_COMPARE_WITH_TIMEOUT(h.started, QStringList{QStringLiteral("2@w1")}, 5000);
        QCOMPARE(h.launcher.waitingCount(), 0);
        QCOMPARE(exited.size(), 0);
    }

    void boundedWaitReportsTheLaunchAsEnded()
    {
        Harness h(QStringLiteral("active"));
        QVERIFY(h.start());
        h.launcher.setWaitBudgetMs(600);
        QSignalSpy exited(&h.launcher, &ConsoleWorkerLauncher::workerExited);
        QVERIFY(h.launcher.launch(Harness::target(), QStringLiteral("w1"), QByteArray(32, 't')));
        QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
        QCOMPARE(exited.first().first().toString(), QStringLiteral("w1"));
        QVERIFY(h.started.isEmpty());
        QCOMPARE(h.launcher.runningCount(), 0);
    }

    void sessionThatGoesAwayEndsTheWait()
    {
        Harness h(QStringLiteral("opening"));
        QVERIFY(h.start());
        QSignalSpy exited(&h.launcher, &ConsoleWorkerLauncher::workerExited);
        QVERIFY(h.launcher.launch(Harness::target(), QStringLiteral("w1"), QByteArray(32, 't')));
        h.setState(QStringLiteral("closing"));
        QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
        QVERIFY(h.started.isEmpty());
    }

    void stopCancelsAWaitingLaunch()
    {
        Harness h(QStringLiteral("opening"));
        QVERIFY(h.start());
        QSignalSpy exited(&h.launcher, &ConsoleWorkerLauncher::workerExited);
        QVERIFY(h.launcher.launch(Harness::target(), QStringLiteral("w1"), QByteArray(32, 't')));
        // A second launch under the same name is refused while it waits.
        QVERIFY(!h.launcher.launch(Harness::target(), QStringLiteral("w1"), QByteArray(32, 't')));
        h.launcher.signalWorker(QStringLiteral("w1"), SIGTERM);
        QCOMPARE(exited.size(), 0); // Never re-entrant: queued.
        QTRY_COMPARE(exited.size(), 1);
        h.setState(QStringLiteral("active"));
        QLocalServer compositor;
        QVERIFY(compositor.listen(h.socketPath()));
        QTest::qWait(400);
        QVERIFY(h.started.isEmpty());
    }

    void goneSessionFailsSynchronously()
    {
        Harness h(QStringLiteral("active"));
        QVERIFY(h.start());
        QString error;
        QVERIFY(!h.launcher.launch({ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("9"), Uid}, QStringLiteral("w1"), QByteArray(32, 't'), &error));
        QVERIFY(error.contains(QStringLiteral("disappeared")));
        QCOMPARE(h.launcher.waitingCount(), 0);
    }
};
}

QTEST_GUILESS_MAIN(KRdp::ConsoleWorkerLaunchReadinessTest)

#include "ConsoleWorkerLaunchReadinessTest.moc"
