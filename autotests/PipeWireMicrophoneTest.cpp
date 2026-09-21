// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PipeWireMicrophone.h"
#include "ConsoleMicrophoneSession.h"
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

        KRdp::ConsoleMicrophoneSession worker(true);
        QList<KRdp::ConsoleWorkerWire::MicrophoneResult> results;
        connect(&worker, &KRdp::ConsoleMicrophoneSession::result, this, [&results](const auto &result) { results.append(result); });
        worker.setControl({4, true});
        worker.request({4, 1, true});
        const QByteArray pcm(3840, '\0');
        QVERIFY(!worker.audio({4, 1, pcm})); // Source not acknowledged yet.
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 3000);
        QVERIFY(results.last().error.isEmpty());
        QVERIFY(worker.audio({4, 1, pcm}));
        QVERIFY(!worker.audio({3, 1, pcm}));
        QVERIFY(!worker.audio({4, 2, pcm}));
        worker.request({4, 2, false});
        QVERIFY(!worker.audio({4, 1, pcm}));
        worker.request({4, 3, true});
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 3, 3000);
        QVERIFY(results.last().error.isEmpty());
        QVERIFY(!worker.audio({4, 1, pcm}));
        QVERIFY(worker.audio({4, 3, pcm}));
        worker.setControl({4, false}); // Immediate local takeover.
        QVERIFY(!worker.audio({4, 3, pcm}));
        worker.setControl({5, true});
        QVERIFY(!worker.audio({4, 3, pcm}));
        worker.request({4, 4, true});
        QVERIFY(!results.last().error.isEmpty());
        worker.request({5, 1, true});
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 5, 3000);
        QVERIFY(results.last().error.isEmpty());
        QVERIFY(worker.audio({5, 1, pcm}));
        worker.stop();
        QVERIFY(!worker.audio({5, 1, pcm}));

        KRdp::ConsoleMicrophoneSession greeter(false);
        bool refused = false;
        connect(&greeter, &KRdp::ConsoleMicrophoneSession::result, this, [&refused](const auto &result) { refused = !result.error.isEmpty(); });
        greeter.setControl({7, true});
        greeter.request({7, 1, true});
        QVERIFY(refused);
        QVERIFY(!greeter.audio({7, 1, pcm}));
        worker.setControl({5, false});
        worker.request({5, 2, true});
        QVERIFY(!results.last().error.isEmpty());
        worker.setControl({6, true});
        worker.request({6, 1, true});
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 7, 3000);
        QVERIFY(results.last().error.isEmpty());
        daemon.terminate();
        QVERIFY(daemon.waitForFinished(3000));
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 8, 3000);
        QVERIFY(!results.last().error.isEmpty());
        QVERIFY(!worker.audio({6, 1, pcm}));
        worker.request({6, 2, true});
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 9, 3000);
        QVERIFY(!results.last().error.isEmpty());
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
