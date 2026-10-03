// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-055 K3 / T-R5: the listener recovers from accept errors (EMFILE), logs
// once per episode, and Server teardown with live sessions is bounded.

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "RdpConnection.h"
#include "Server.h"

using namespace KRdp;

namespace
{
std::atomic<int> s_acceptLines = 0;
std::atomic<int> s_pressureLines = 0;
QtMessageHandler s_previousHandler = nullptr;

void countingHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    if (type == QtWarningMsg) {
        if (msg.contains(QLatin1String("accept"), Qt::CaseInsensitive)) {
            ++s_acceptLines;
        }
        if (msg.contains(QLatin1String("descriptor pressure"), Qt::CaseInsensitive)) {
            ++s_pressureLines;
        }
    }
    if (s_previousHandler) {
        s_previousHandler(type, context, msg);
    }
}

int openFdCount()
{
    int n = 0;
    if (DIR *dir = ::opendir("/proc/self/fd")) {
        while (::readdir(dir)) {
            ++n;
        }
        ::closedir(dir);
    }
    return n;
}

int rawConnect(quint16 port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

class CountingServer : public Server
{
public:
    std::atomic<int> accepted = 0;
    std::vector<int> held; // accepted handles stay open, as live sessions would

    ~CountingServer() override
    {
        releaseHeld();
    }
    void releaseHeld()
    {
        for (int fd : held) {
            ::close(fd);
        }
        held.clear();
    }

protected:
    void incomingConnection(qintptr handle) override
    {
        ++accepted;
        held.push_back(int(handle));
    }
};

struct Cleanup {
    rlimit original{};
    std::vector<int> fds;
    ~Cleanup()
    {
        for (int fd : fds) {
            ::close(fd);
        }
        ::setrlimit(RLIMIT_NOFILE, &original);
    }
};
}

class ServerAcceptRecoveryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        s_previousHandler = qInstallMessageHandler(countingHandler);
    }

    void cleanupTestCase()
    {
        qInstallMessageHandler(s_previousHandler);
    }

    void emfileRecovers()
    {
        Cleanup cleanup;
        QCOMPARE(::getrlimit(RLIMIT_NOFILE, &cleanup.original), 0);
        rlimit lowered = cleanup.original;
        lowered.rlim_cur = std::min<rlim_t>(cleanup.original.rlim_cur, rlim_t(openFdCount() + 64));
        QCOMPARE(::setrlimit(RLIMIT_NOFILE, &lowered), 0);

        std::vector<int> hogs;
        CountingServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();
        QSignalSpy errors(&server, &QTcpServer::acceptError);

        // The kernel completes handshakes into the backlog, no accept needed.
        for (int i = 0; i < 3; ++i) {
            const int c = rawConnect(port);
            QVERIFY(c >= 0);
            cleanup.fds.push_back(c);
        }

        int fd;
        while ((fd = ::dup(0)) >= 0) {
            hogs.push_back(fd);
        }
        QCOMPARE(errno, EMFILE);

        s_acceptLines = 0;
        s_pressureLines = 0;

        // Episode: held for 2 s, which spans the first back-off retry (1 s).
        QElapsedTimer held;
        held.start();
        while (held.elapsed() < 2000) {
            QTest::qWait(50);
        }
        qInfo() << "during the episode: accepted" << server.accepted.load() << "acceptError signals" << errors.count();
        QVERIFY2(server.accepted.load() < 3, "all three cannot be accepted without a free fd");

        // Free fds (the accepted handles and a few hogs); stay above 80 % of the limit.
        server.releaseHeld();
        for (int i = 0; i < 12; ++i) {
            ::close(hogs.back());
            hogs.pop_back();
        }
        cleanup.fds.insert(cleanup.fds.end(), hogs.begin(), hogs.end());
        QElapsedTimer sinceRelease;
        sinceRelease.start();
        QTRY_VERIFY_WITH_TIMEOUT(server.accepted.load() == 3, 3000);
        for (const auto &e : errors) {
            qInfo() << "acceptError" << e.at(0);
        }
        qInfo() << "all three accepted" << sinceRelease.elapsed() << "ms after the fds were freed; acceptError signals" << errors.count();

        // A new connection is accepted afterwards.
        const int c = rawConnect(port);
        QVERIFY(c >= 0);
        cleanup.fds.push_back(c);
        QTRY_VERIFY_WITH_TIMEOUT(server.accepted.load() == 4, 3000);

        // Entry line plus the "accepting again" line, and no more.
        QTRY_COMPARE_WITH_TIMEOUT(s_acceptLines.load(), 2, 1000);
        QTest::qWait(1200);
        QVERIFY2(s_acceptLines.load() <= 2, qPrintable(QStringLiteral("%1 accept log lines in one episode").arg(s_acceptLines.load())));
        QCOMPARE(s_pressureLines.load(), 1);
    }

    // Destroying a Server that owns live sessions returns within a bounded time.
    void destructionWithSessionsIsBounded()
    {
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QSignalSpy created(&server, &Server::newConnectionCreated);
        QTcpSocket a;
        QTcpSocket b;
        a.connectToHost(QHostAddress::LocalHost, server.serverPort());
        b.connectToHost(QHostAddress::LocalHost, server.serverPort());
        QTRY_COMPARE_WITH_TIMEOUT(created.count(), 2, 5000);

        QElapsedTimer timer;
        timer.start();
        server.stop();
        // ~Server runs at scope exit; measure it explicitly with a heap object.
        auto *heap = new Server;
        QVERIFY(heap->listen(QHostAddress::LocalHost, 0));
        QSignalSpy created2(heap, &Server::newConnectionCreated);
        QTcpSocket c;
        c.connectToHost(QHostAddress::LocalHost, heap->serverPort());
        QTRY_COMPARE_WITH_TIMEOUT(created2.count(), 1, 5000);
        timer.restart();
        delete heap;
        QVERIFY2(timer.elapsed() < 10000, qPrintable(QStringLiteral("destructor took %1 ms").arg(timer.elapsed())));
    }
};

QTEST_MAIN(ServerAcceptRecoveryTest)
#include "ServerAcceptRecoveryTest.moc"
