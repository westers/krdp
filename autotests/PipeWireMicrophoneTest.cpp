// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PipeWireMicrophone.h"
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <unistd.h>

class PipeWireMicrophoneTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sourceReadinessAndTeardown()
    {
        // Never connect to the desktop's graph, even when startup fails.
        QTemporaryDir runtime(QStringLiteral("/run/user/%1/krdp-mic-test-XXXXXX").arg(getuid()));
        QVERIFY(runtime.isValid());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "pipewire-0");
        QProcess daemon;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime.path());
        env.insert(QStringLiteral("PULSE_RUNTIME_PATH"), runtime.path() + QStringLiteral("/pulse"));
        env.insert(QStringLiteral("PIPEWIRE_CONFIG_DIR"), QStringLiteral(KRDP_AUDIO_CONFIG_DIR));
        env.insert(QStringLiteral("PIPEWIRE_CONFIG_NAME"), QStringLiteral("virtual-session-pipewire.conf"));
        daemon.setProcessEnvironment(env);
        daemon.start(QStringLiteral(KRDP_PIPEWIRE_EXECUTABLE), QStringList{});
        QVERIFY(daemon.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(runtime.path() + QStringLiteral("/pipewire-0")), 3000);
        KRdp::PipeWireMicrophone mic;
        using State = KRdp::PipeWireMicrophone::State;
        QCOMPARE(mic.state(), State::Stopped);
        QVERIFY(mic.start(QStringLiteral("readiness-test")));
        QTRY_COMPARE_WITH_TIMEOUT(mic.state(), State::Ready, 3000);
        mic.stop();
        QCOMPARE(mic.state(), State::Stopped);
        QVERIFY(mic.start(QStringLiteral("readiness-test-reopen")));
        QTRY_COMPARE_WITH_TIMEOUT(mic.state(), State::Ready, 3000);
        daemon.terminate();
        QVERIFY(daemon.waitForFinished(3000));
        QTRY_COMPARE_WITH_TIMEOUT(mic.state(), State::Failed, 3000);
        mic.stop();
        QCOMPARE(mic.state(), State::Stopped);
        // Missing private graph must never look like a usable source.
        mic.start(QStringLiteral("missing-graph"));
        QTRY_COMPARE_WITH_TIMEOUT(mic.state(), State::Failed, 3000);
        mic.stop();
        QCOMPARE(mic.state(), State::Stopped);
    }
};

QTEST_GUILESS_MAIN(PipeWireMicrophoneTest)
#include "PipeWireMicrophoneTest.moc"
