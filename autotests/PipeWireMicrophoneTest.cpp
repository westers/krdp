// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PipeWireMicrophone.h"
#include "ConsoleMicrophoneSession.h"
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <cmath>
#include <unistd.h>

class PipeWireMicrophoneTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sourcePcmDelivery()
    {
        QTemporaryDir runtime(QStringLiteral("/run/user/%1/krdp-mic-pcm-XXXXXX").arg(getuid()));
        QVERIFY(runtime.isValid());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "pipewire-0");
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime.path());
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), runtime.path() + QStringLiteral("/config"));
        env.insert(QStringLiteral("XDG_STATE_HOME"), runtime.path() + QStringLiteral("/state"));
        env.insert(QStringLiteral("XDG_CACHE_HOME"), runtime.path() + QStringLiteral("/cache"));
        env.insert(QStringLiteral("PULSE_RUNTIME_PATH"), runtime.path() + QStringLiteral("/pulse"));
        env.insert(QStringLiteral("PIPEWIRE_CONFIG_DIR"), QStringLiteral(KRDP_AUDIO_CONFIG_DIR));
        env.insert(QStringLiteral("PIPEWIRE_CONFIG_NAME"), QStringLiteral("virtual-session-pipewire.conf"));
        QProcess daemon;
        daemon.setProcessEnvironment(env);
        daemon.start(QStringLiteral(KRDP_PIPEWIRE_EXECUTABLE), QStringList{});
        QVERIFY(daemon.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(runtime.path() + QStringLiteral("/pipewire-0")), 3000);
        // CTest wraps this case in a private D-Bus session; policy only means
        // no ALSA/Bluetooth devices. Nothing attaches to the desktop graph.
        env.remove(QStringLiteral("PIPEWIRE_CONFIG_DIR"));
        env.remove(QStringLiteral("PIPEWIRE_CONFIG_NAME"));
        QProcess policy;
        env.insert(QStringLiteral("WIREPLUMBER_CONFIG_DIR"), QStringLiteral("/usr/share/wireplumber"));
        policy.setProcessEnvironment(env);
        policy.start(QStringLiteral("wireplumber"), {QStringLiteral("--profile"), QStringLiteral("policy")});
        QVERIFY(policy.waitForStarted());
        KRdp::PipeWireMicrophone mic;
        QVERIFY(mic.start(QStringLiteral("pcm-test")));
        QTRY_COMPARE_WITH_TIMEOUT(mic.state(), KRdp::PipeWireMicrophone::State::Ready, 3000);
        QProcess recorder;
        recorder.setProcessEnvironment(env);
        recorder.start(QStringLiteral("pw-cat"), {QStringLiteral("--record"), QStringLiteral("--raw"),
            QStringLiteral("--rate"), QStringLiteral("48000"), QStringLiteral("--channels"), QStringLiteral("2"),
            QStringLiteral("--format"), QStringLiteral("s16"), QStringLiteral("--target"),
            QStringLiteral("krdp.remote-microphone.pcm-test"), QStringLiteral("-")});
        QVERIFY(recorder.waitForStarted());
        QTest::qWait(500);
        constexpr double tau = 6.283185307179586;
        int sample = 0;
        for (int packet = 0; packet < 100; ++packet) {
            QByteArray pcm(3840, '\0');
            for (int frame = 0; frame < 960; ++frame, ++sample) {
                const qint16 value = qint16(4000 * std::sin(tau * 997 * sample / 48000));
                qToLittleEndian(value, pcm.data() + frame * 4);
                qToLittleEndian(value, pcm.data() + frame * 4 + 2);
            }
            mic.write(pcm);
            QTest::qWait(20);
        }
        QTest::qWait(200);
        QVERIFY2(recorder.state() == QProcess::Running, recorder.readAllStandardError().constData());
        recorder.terminate();
        QVERIFY(recorder.waitForFinished(3000));
        const QByteArray received = recorder.readAllStandardOutput();
        QVERIFY2(received.size() > 48000, recorder.readAllStandardError().constData());
        double energy = 0, real = 0, imaginary = 0;
        const int frames = received.size() / 4;
        for (int frame = 0; frame < frames; ++frame) {
            const double value = qFromLittleEndian<qint16>(received.constData() + frame * 4);
            energy += value * value;
            real += value * std::cos(tau * 997 * frame / 48000);
            imaginary += value * std::sin(tau * 997 * frame / 48000);
        }
        const double toneFraction = energy ? 2 * (real * real + imaginary * imaginary) / (frames * energy) : 0;
        qInfo() << "Microphone PCM frames" << frames << "RMS" << std::sqrt(energy / frames) << "997Hz energy fraction" << toneFraction;
        QVERIFY(energy / frames > 10000);
        QVERIFY(toneFraction > 0.25);
        mic.stop();
        policy.terminate();
        QVERIFY(policy.waitForFinished(3000));
        daemon.terminate();
        QVERIFY(daemon.waitForFinished(3000));
    }
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
