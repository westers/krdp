// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// S1b (OPT-057): the root settings helpers disable core dumps. The context is
// entered in a forked child (so the test process keeps its own limits) and the
// kernel state is read back with prctl(PR_GET_DUMPABLE) and getrlimit.
#include "PrivateExecutionContext.h"
#include <QFile>
#include <QTest>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

class PrivateExecutionContextTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void childHasNoCoreAndIsNotDumpable()
    {
        QCOMPARE(::prctl(PR_GET_DUMPABLE), 1); // precondition: the test process itself is dumpable
        const pid_t pid = ::fork();
        QVERIFY(pid >= 0);
        if (pid == 0) {
            if (!KRdp::enterPrivateExecutionContext()) ::_exit(10);
            rlimit limit{1, 1};
            if (::getrlimit(RLIMIT_CORE, &limit) || limit.rlim_cur != 0 || limit.rlim_max != 0) ::_exit(11);
            if (::prctl(PR_GET_DUMPABLE) != 0) ::_exit(12);
            ::_exit(0);
        }
        int status = 0;
        QCOMPARE(::waitpid(pid, &status, 0), pid);
        QVERIFY(WIFEXITED(status));
        QCOMPARE(WEXITSTATUS(status), 0);
    }
    // Both root helpers must call it before reading any input.
    void helpersCallItFirst()
    {
        for (const char *name : {"authenticationhelper.cpp", "hostsettingshelper.cpp"}) {
            QFile file(QStringLiteral(SERVER_SOURCE_DIR "/") + QLatin1String(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto text = file.readAll();
            const auto call = text.indexOf("enterPrivateExecutionContext()");
            QVERIFY2(call > 0, name);
            QVERIFY2(call < text.indexOf("QCoreApplication app"), name);
            QVERIFY2(call < text.indexOf("::read(STDIN_FILENO"), name);
        }
    }
};
QTEST_APPLESS_MAIN(PrivateExecutionContextTest)
#include "PrivateExecutionContextTest.moc"
