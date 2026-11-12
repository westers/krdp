// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// S1b (OPT-057): the root settings helpers disable core dumps. The context is
// entered in a forked child (so the test process keeps its own limits) and the
// kernel state is read back with prctl(PR_GET_DUMPABLE) and getrlimit.
#include "PrivateExecutionContext.h"
#include <QFile>
#include <QTemporaryDir>
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

    // REVIEW-S2: the two root brokers become non-dumpable but keep RLIMIT_CORE, which their children (the console worker)
    // inherit. The flag is read back in a forked child (the test process keeps its own state).
    void brokerContextIsNonDumpableAndKeepsTheCoreLimit()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray missing = QFile::encodeName(directory.filePath(QStringLiteral("no-such-marker")));
        rlimit before{};
        QCOMPARE(::getrlimit(RLIMIT_CORE, &before), 0);
        const pid_t pid = ::fork();
        QVERIFY(pid >= 0);
        if (pid == 0) {
            if (KRdp::enterBrokerExecutionContext(missing.constData()) != KRdp::BrokerContext::NotDumpable) ::_exit(10);
            if (::prctl(PR_GET_DUMPABLE) != 0) ::_exit(11);
            rlimit after{};
            if (::getrlimit(RLIMIT_CORE, &after) || after.rlim_cur != before.rlim_cur || after.rlim_max != before.rlim_max) ::_exit(12);
            ::_exit(0);
        }
        int status = 0;
        QCOMPARE(::waitpid(pid, &status, 0), pid);
        QVERIFY(WIFEXITED(status));
        QCOMPARE(WEXITSTATUS(status), 0);
    }

    // The administrator marker keeps a broker dumpable only when it is a root-owned regular file nobody else can write.
    void markerNeedsRootOwnershipAndARegularFile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("marker"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(!KRdp::brokerCoreDumpsAllowed(QFile::encodeName(path).constData())); // owned by this (normally non-root) user, or group/other-writable
        QVERIFY(!KRdp::brokerCoreDumpsAllowed(QFile::encodeName(directory.filePath(QStringLiteral("missing"))).constData()));
        const QString link = directory.filePath(QStringLiteral("link"));
        QVERIFY(QFile::link(path, link));
        QVERIFY(!KRdp::brokerCoreDumpsAllowed(QFile::encodeName(link).constData())); // lstat: a symlink never counts
        QVERIFY(!KRdp::brokerCoreDumpsAllowed(QFile::encodeName(directory.path()).constData())); // a directory never counts
        if (::geteuid() == 0) {
            QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
            QVERIFY(KRdp::brokerCoreDumpsAllowed(QFile::encodeName(path).constData()));
            QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::WriteOther));
            QVERIFY(!KRdp::brokerCoreDumpsAllowed(QFile::encodeName(path).constData()));
        }
    }

    // Both brokers call it as the first statement of main, before the terminate handler and Qt start, and never touch RLIMIT_CORE.
    void brokersCallItFirst()
    {
        for (const char *name : {"consolehost.cpp", "virtualhost.cpp"}) {
            QFile file(QStringLiteral(SERVER_SOURCE_DIR "/") + QLatin1String(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto text = file.readAll();
            const auto call = text.indexOf("enterBrokerExecutionContext()");
            QVERIFY2(call > 0, name);
            QVERIFY2(call > text.indexOf("int main("), name);
            QVERIFY2(call < text.indexOf("installTerminateHandler("), name);
            QVERIFY2(call < text.indexOf("QCoreApplication application"), name);
            QVERIFY2(!text.contains("RLIMIT_CORE"), name);
            QVERIFY2(text.contains("BrokerContext::Failed"), name); // fail closed
        }
    }
};
QTEST_APPLESS_MAIN(PrivateExecutionContextTest)
#include "PrivateExecutionContextTest.moc"
