// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-055 K6.5 (T-K6d): a failed pthread_create must come back as `false`, not as an exception
// that reaches std::terminate. pthread_create is interposed by this executable (libstdc++ calls
// it through the dynamic symbol, so --wrap on our own objects would not reach it).

#include "SafeThread.h"

#include <QTest>

#include <atomic>
#include <cerrno>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

static std::atomic<bool> g_failCreate{false};

extern "C" int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    if (g_failCreate.load()) {
        return EAGAIN;
    }
    using Fn = int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "pthread_create"));
    return real(thread, attr, start, arg);
}

class SafeThreadTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void startsAndRunsTheBody()
    {
        std::atomic<bool> ran{false};
        std::jthread thread;
        QVERIFY(KRdp::startJThread(thread, [&](std::stop_token) { ran = true; }));
        QVERIFY(thread.joinable());
        thread.join();
        QVERIFY(ran);
    }

    void bodyReceivesTheStopToken()
    {
        std::atomic<bool> stopped{false};
        std::jthread thread;
        QVERIFY(KRdp::startJThread(thread, [&](std::stop_token token) {
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            stopped = true;
        }));
        thread.request_stop();
        thread.join();
        QVERIFY(stopped);
    }

    void threadStartFailureIsReportedNotThrown()
    {
        const pid_t child = ::fork();
        if (child == 0) {
            g_failCreate = true;
            std::jthread thread;
            const bool started = KRdp::startJThread(thread, [](std::stop_token) {});
            g_failCreate = false;
            // Alive, reporting failure, and the object is left empty.
            ::_exit(!started && !thread.joinable() ? 0 : 1);
        }
        int status = 0;
        QCOMPARE(::waitpid(child, &status, 0), child);
        QVERIFY2(WIFEXITED(status), qPrintable(QStringLiteral("child was killed by signal %1").arg(WIFSIGNALED(status) ? WTERMSIG(status) : 0)));
        QCOMPARE(WEXITSTATUS(status), 0);
    }
};

QTEST_MAIN(SafeThreadTest)
#include "SafeThreadTest.moc"
