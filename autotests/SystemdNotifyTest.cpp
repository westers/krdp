// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-055 K6.1 (T-K6a): the watchdog heartbeat is driven by the main thread's event loop, so a
// blocked main thread stops pinging. A fake NOTIFY_SOCKET datagram socket records every message
// with its arrival time on a reader thread (it keeps reading while the main thread is blocked).

#include "SystemdNotify.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace
{
struct Message {
    std::string text;
    Clock::time_point at;
};

class FakeNotifySocket
{
public:
    explicit FakeNotifySocket(const QString &path)
    {
        m_fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto bytes = path.toUtf8();
        ::strncpy(address.sun_path, bytes.constData(), sizeof(address.sun_path) - 1);
        m_ok = m_fd >= 0 && ::bind(m_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
        timeval timeout{0, 100000};
        ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        m_reader = std::thread([this] {
            char buffer[256];
            while (!m_stop.load()) {
                const auto got = ::recv(m_fd, buffer, sizeof(buffer) - 1, 0);
                if (got > 0) {
                    std::lock_guard lock(m_mutex);
                    m_messages.push_back({std::string(buffer, size_t(got)), Clock::now()});
                }
            }
        });
    }
    ~FakeNotifySocket()
    {
        m_stop = true;
        m_reader.join();
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }
    bool ok() const { return m_ok; }
    std::vector<Message> messages() const
    {
        std::lock_guard lock(m_mutex);
        return m_messages;
    }
    int pingsBetween(Clock::time_point from, Clock::time_point to) const
    {
        int count = 0;
        for (const auto &message : messages()) {
            if (message.text == "WATCHDOG=1" && message.at >= from && message.at < to) {
                ++count;
            }
        }
        return count;
    }

private:
    int m_fd = -1;
    bool m_ok = false;
    std::atomic<bool> m_stop{false};
    mutable std::mutex m_mutex;
    std::vector<Message> m_messages;
    std::thread m_reader;
};
}

class SystemdNotifyTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init()
    {
        qunsetenv("NOTIFY_SOCKET");
        qunsetenv("WATCHDOG_USEC");
        qunsetenv("WATCHDOG_PID");
    }

    void pingIntervalIsAThirdCappedAtTenSeconds()
    {
        using namespace std::chrono_literals;
        QCOMPARE(KRdp::SystemdNotify::pingInterval(90s), 10000ms);
        QCOMPARE(KRdp::SystemdNotify::pingInterval(30s), 10000ms);
        QCOMPARE(KRdp::SystemdNotify::pingInterval(15s), 5000ms);
        QCOMPARE(KRdp::SystemdNotify::pingInterval(3s), 1000ms);
    }

    void inertWithoutAnArmedWatchdog()
    {
        QTemporaryDir dir;
        FakeNotifySocket socket(dir.filePath(QStringLiteral("notify")));
        QVERIFY(socket.ok());
        qputenv("NOTIFY_SOCKET", dir.filePath(QStringLiteral("notify")).toUtf8());
        QCOMPARE(KRdp::SystemdNotify::watchdogPeriod(), std::chrono::microseconds::zero());
        QVERIFY(!KRdp::SystemdNotify::ping());
        KRdp::SystemdNotify notify;
        QVERIFY(!notify.start());
        QTest::qWait(300);
        QCOMPARE(socket.messages().size(), size_t(0));
    }

    void watchdogForAnotherProcessIsIgnored()
    {
        QTemporaryDir dir;
        FakeNotifySocket socket(dir.filePath(QStringLiteral("notify")));
        QVERIFY(socket.ok());
        qputenv("NOTIFY_SOCKET", dir.filePath(QStringLiteral("notify")).toUtf8());
        qputenv("WATCHDOG_USEC", "3000000");
        qputenv("WATCHDOG_PID", QByteArray::number(qint64(::getpid()) + 1));
        KRdp::SystemdNotify notify;
        QVERIFY(!notify.start());
        QTest::qWait(300);
        QCOMPARE(socket.messages().size(), size_t(0));
    }

    void pingsWhileTheMainThreadRunsAndStopsWhileItIsBlocked()
    {
        using namespace std::chrono_literals;
        QTemporaryDir dir;
        FakeNotifySocket socket(dir.filePath(QStringLiteral("notify")));
        QVERIFY(socket.ok());
        qputenv("NOTIFY_SOCKET", dir.filePath(QStringLiteral("notify")).toUtf8());
        qputenv("WATCHDOG_USEC", "3000000"); // ping every 1 s
        qputenv("WATCHDOG_PID", QByteArray::number(qint64(::getpid())));

        KRdp::SystemdNotify notify;
        const auto start = Clock::now();
        QVERIFY(notify.start());
        QTest::qWait(4200);
        const auto running = Clock::now();
        QVERIFY2(socket.pingsBetween(start, running) >= 2, qPrintable(QString::number(socket.pingsBetween(start, running))));

        // Block the main thread: the reader thread is still listening, and must hear nothing.
        const auto blockStart = Clock::now();
        QThread::sleep(4);
        const auto blockEnd = Clock::now();
        // A ping already on its way when the block began may land just after blockStart.
        QCOMPARE(socket.pingsBetween(blockStart + 300ms, blockEnd), 0);

        // Back in the event loop, the heartbeat resumes.
        QTest::qWait(1500);
        QVERIFY(socket.pingsBetween(blockEnd, Clock::now()) >= 1);
    }

    void explicitPingReachesTheSocketBeforeAnEventLoopExists()
    {
        QTemporaryDir dir;
        FakeNotifySocket socket(dir.filePath(QStringLiteral("notify")));
        QVERIFY(socket.ok());
        qputenv("NOTIFY_SOCKET", dir.filePath(QStringLiteral("notify")).toUtf8());
        qputenv("WATCHDOG_USEC", "90000000");
        qputenv("WATCHDOG_PID", QByteArray::number(qint64(::getpid())));
        QVERIFY(KRdp::SystemdNotify::ping());
        QTRY_VERIFY(socket.messages().size() >= 1);
        QCOMPARE(QString::fromStdString(socket.messages().front().text), QStringLiteral("WATCHDOG=1"));
    }
};

QTEST_MAIN(SystemdNotifyTest)
#include "SystemdNotifyTest.moc"
