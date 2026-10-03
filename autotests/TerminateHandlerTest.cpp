// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-055 K6.4 (T-K6c): an escaped exception reaches std::terminate; the installed handler names
// it on stderr and aborts (SIGABRT, so the core survives). Each case runs in a forked child.
// This file is built with exceptions so it can throw; the library under test is not.

#include "TerminateHandler.h"

#include <QTest>

#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
struct Outcome {
    int status = 0;
    QByteArray err;
};

template<typename Body>
Outcome runInChild(Body body)
{
    int fds[2];
    if (::pipe(fds) != 0) {
        return {-1, {}};
    }
    const pid_t child = ::fork();
    if (child == 0) {
        struct rlimit noCore{0, 0};
        ::setrlimit(RLIMIT_CORE, &noCore); // keep the deliberate abort out of the host's coredump store
        ::dup2(fds[1], 2);
        ::close(fds[0]);
        ::close(fds[1]);
        body();
        ::_exit(0);
    }
    ::close(fds[1]);
    Outcome outcome;
    char buffer[1024];
    for (;;) {
        const auto got = ::read(fds[0], buffer, sizeof(buffer));
        if (got <= 0) {
            break;
        }
        outcome.err.append(buffer, qsizetype(got));
    }
    ::close(fds[0]);
    ::waitpid(child, &outcome.status, 0);
    return outcome;
}

void throwThroughNoexcept() noexcept
{
    throw std::runtime_error("boom-marker");
}
}

class TerminateHandlerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void escapedExceptionIsNamedThenAborts()
    {
        const auto outcome = runInChild([] {
            KRdp::installTerminateHandler("testprog");
            throwThroughNoexcept();
        });
        QVERIFY(WIFSIGNALED(outcome.status));
        QCOMPARE(WTERMSIG(outcome.status), SIGABRT);
        QVERIFY2(outcome.err.contains("farside: testprog: std::terminate: "), outcome.err.constData());
        QVERIFY2(outcome.err.contains("boom-marker"), outcome.err.constData());
        QVERIFY2(outcome.err.contains("runtime_error"), outcome.err.constData());
    }

    void terminateWithoutAnExceptionStillAborts()
    {
        const auto outcome = runInChild([] {
            KRdp::installTerminateHandler("testprog");
            std::terminate();
        });
        QVERIFY(WIFSIGNALED(outcome.status));
        QCOMPARE(WTERMSIG(outcome.status), SIGABRT);
        QVERIFY2(outcome.err.contains("farside: testprog: std::terminate: "), outcome.err.constData());
        QVERIFY2(outcome.err.contains("no active exception"), outcome.err.constData());
    }

    void nonStdExceptionIsReported()
    {
        const auto outcome = runInChild([] {
            KRdp::installTerminateHandler("testprog");
            []() noexcept { throw 42; }();
        });
        QVERIFY(WIFSIGNALED(outcome.status));
        QCOMPARE(WTERMSIG(outcome.status), SIGABRT);
        QVERIFY2(outcome.err.contains("unknown exception"), outcome.err.constData());
    }
};

QTEST_MAIN(TerminateHandlerTest)
#include "TerminateHandlerTest.moc"
