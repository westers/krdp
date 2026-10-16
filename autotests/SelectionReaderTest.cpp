// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#include <QTest>

#include "SelectionReaderFd.h"

using namespace KRdp::SelectionReader;

namespace
{
struct Pipe {
    int fds[2];
    Pipe()
    {
        if (::pipe2(fds, O_CLOEXEC) != 0) qFatal("pipe2 failed");
    }
    ~Pipe()
    {
        if (fds[0] >= 0) ::close(fds[0]);
        if (fds[1] >= 0) ::close(fds[1]);
    }
    void closeWrite()
    {
        ::close(fds[1]);
        fds[1] = -1;
    }
};

void writeAll(int fd, const QByteArray &data)
{
    qsizetype done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.constData() + done, size_t(data.size() - done));
        if (n <= 0) return; // the reader gave up: EPIPE
        done += n;
    }
}
}

class SelectionReaderTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { signal(SIGPIPE, SIG_IGN); }

    void readsEverythingUpToTheCap()
    {
        Pipe pipe;
        QByteArray payload(300 * 1000, 'x');
        for (int i = 0; i < payload.size(); i += 997) payload[i] = char(i & 0x7f);
        std::thread writer([&] {
            writeAll(pipe.fds[1], payload);
            pipe.closeWrite();
        });
        const FdRead read = readCapped(pipe.fds[0], payload.size(), 1000, 5000);
        writer.join();
        QCOMPARE(read.status, Status::Ok);
        QCOMPARE(read.data, payload);
        QCOMPARE(read.bytes, payload.size());
    }

    void anEmptySourceIsOkAndEmpty()
    {
        Pipe pipe;
        pipe.closeWrite();
        const FdRead read = readCapped(pipe.fds[0], 1024, 1000, 5000);
        QCOMPARE(read.status, Status::Ok);
        QVERIFY(read.data.isEmpty());
    }

    void exactlyTheCapIsAccepted()
    {
        Pipe pipe;
        const QByteArray payload(4096, 'a');
        std::thread writer([&] {
            writeAll(pipe.fds[1], payload);
            pipe.closeWrite();
        });
        const FdRead read = readCapped(pipe.fds[0], 4096, 1000, 5000);
        writer.join();
        QCOMPARE(read.status, Status::Ok);
        QCOMPARE(read.data.size(), 4096);
    }

    void oneByteOverTheCapIsRefusedAndKeepsNothing()
    {
        Pipe pipe;
        const QByteArray payload(4097, 'a');
        std::thread writer([&] {
            writeAll(pipe.fds[1], payload);
            pipe.closeWrite();
        });
        const FdRead read = readCapped(pipe.fds[0], 4096, 1000, 5000);
        writer.join();
        QCOMPARE(read.status, Status::TooLarge);
        QVERIFY(read.data.isEmpty());
        QVERIFY(read.bytes > 4096);
    }

    void anEndlessSourceIsAbandonedNearTheCapNotAfterItsEnd()
    {
        // A source that never stops: the read must end after cap + one chunk, and the writer must see the closed pipe.
        Pipe pipe;
        std::atomic<qint64> written{0};
        std::atomic<bool> sawClosedPipe{false};
        std::thread writer([&] {
            const QByteArray chunk(32 * 1024, 'e');
            for (int i = 0; i < 100000; ++i) {
                const ssize_t n = ::write(pipe.fds[1], chunk.constData(), size_t(chunk.size()));
                if (n < 0) {
                    sawClosedPipe = errno == EPIPE;
                    return;
                }
                written += n;
            }
        });
        const int cap = 1024 * 1024;
        const FdRead read = readCapped(pipe.fds[0], cap, 1000, 10000);
        QCOMPARE(read.status, Status::TooLarge);
        QVERIFY(read.bytes > cap);
        QVERIFY2(read.bytes <= cap + 64 * 1024, "stopped within one read chunk of the cap");
        ::close(pipe.fds[0]); // what readSelection does when it abandons
        pipe.fds[0] = -1;
        writer.join();
        QVERIFY(sawClosedPipe);
        QVERIFY(written < 100000LL * 32 * 1024);
    }

    void aStalledSourceTimesOutOnIdle()
    {
        Pipe pipe; // nothing is ever written, the write end stays open
        QElapsedTimer clock;
        clock.start();
        const FdRead read = readCapped(pipe.fds[0], 1024, 150, 5000);
        QCOMPARE(read.status, Status::Timeout);
        QVERIFY(clock.elapsed() >= 140);
        QVERIFY(clock.elapsed() < 2000);
    }

    void aSlowTrickleHitsTheTotalLimit()
    {
        Pipe pipe;
        std::atomic<bool> stop{false};
        std::thread writer([&] {
            while (!stop) {
                if (::write(pipe.fds[1], "x", 1) < 0) return;
                usleep(40 * 1000);
            }
        });
        QElapsedTimer clock;
        clock.start();
        const FdRead read = readCapped(pipe.fds[0], 1024 * 1024, 500, 400); // each chunk is in time; the whole is not
        stop = true;
        ::close(pipe.fds[0]);
        pipe.fds[0] = -1;
        writer.join();
        QCOMPARE(read.status, Status::Timeout);
        QVERIFY(clock.elapsed() < 1500);
    }

    void statusNamesMatchTheLogVocabulary()
    {
        QCOMPARE(QString::fromLatin1(statusName(Status::TooLarge)), QStringLiteral("oversize"));
        QCOMPARE(QString::fromLatin1(statusName(Status::Unavailable)), QStringLiteral("unavailable"));
    }
};

QTEST_GUILESS_MAIN(SelectionReaderTest)
#include "SelectionReaderTest.moc"
