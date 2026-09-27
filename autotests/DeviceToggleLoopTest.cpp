// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-D1: toggle each KRdp PipeWire endpoint 100 times against a private
// PipeWire daemon and check that nothing is left behind: no krdp.* node, no
// extra fd or thread after 2 s, no PipeWireRuntime reference, and the
// default-sink metadata back to what it was.
//
// The daemon runs under a private PIPEWIRE_RUNTIME_DIR with the virtual-session
// graph config (a metadata object and one null sink, no devices, no policy),
// so nothing here can touch the desktop's graph. Exits 77 (skip) when that
// daemon can't be started.

#include "PipeWireAudioPlayback.h"
#include "PipeWireCamera.h"
#include "PipeWireMicrophone.h"
#include "PipeWireRuntime.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <unistd.h>

namespace
{
constexpr int Cycles = 100;
constexpr int SettleMs = 2000;
const QString BaselineSink = QStringLiteral("krdp.virtual-session.audio");
const QString BaselineSinkJson = QStringLiteral("{\"name\":\"krdp.virtual-session.audio\"}");
// Only the private config creates this driver; seeing it proves the tools
// below talk to our daemon and not to the desktop.
const QString PrivateGraphMarker = QStringLiteral("KRDP-Virtual-Driver");

// readdir() rather than QDir: QDir filters the dangling anon_inode:[...]
// links that eventfd/epoll/pidfd descriptors show up as.
int entryCount(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    int count = 0;
    while (const dirent *entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    closedir(dir);
    return count;
}
int fdCount()
{
    return entryCount("/proc/self/fd") - 1; // minus the directory stream itself
}
int threadCount()
{
    return entryCount("/proc/self/task");
}

struct Graph {
    std::unique_ptr<QTemporaryDir> runtime;
    QProcess daemon;
    QString skipReason;
    bool start()
    {
        const QString parent = QStringLiteral("/run/user/%1").arg(getuid());
        const QFileInfo parentInfo(parent);
        if (!parentInfo.isDir() || parentInfo.ownerId() != getuid()) {
            skipReason = QStringLiteral("no owned logind runtime directory %1").arg(parent);
            return false;
        }
        for (const char *tool : {"pw-dump", "pw-metadata"}) {
            if (QStandardPaths::findExecutable(QString::fromLatin1(tool)).isEmpty()) {
                skipReason = QStringLiteral("%1 is not installed").arg(QString::fromLatin1(tool));
                return false;
            }
        }
        runtime = std::make_unique<QTemporaryDir>(parent + QStringLiteral("/krdp-device-loop-XXXXXX"));
        if (!runtime->isValid()) {
            skipReason = QStringLiteral("cannot create a private runtime directory below %1").arg(parent);
            return false;
        }
        // Everything in this process and every child (pw-dump, pw-metadata
        // run by PipeWireAudioPlayback) resolves PipeWire here only.
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime->path().toUtf8());
        qputenv("XDG_RUNTIME_DIR", runtime->path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "pipewire-0");
        qputenv("PULSE_RUNTIME_PATH", (runtime->path() + QStringLiteral("/pulse")).toUtf8());
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PIPEWIRE_CONFIG_DIR"), QStringLiteral(KRDP_AUDIO_CONFIG_DIR));
        env.insert(QStringLiteral("PIPEWIRE_CONFIG_NAME"), QStringLiteral("virtual-session-pipewire.conf"));
        daemon.setProcessEnvironment(env);
        daemon.setStandardOutputFile(runtime->path() + QStringLiteral("/daemon.log"));
        daemon.setStandardErrorFile(runtime->path() + QStringLiteral("/daemon.log"), QIODevice::Append);
        daemon.start(QStringLiteral(KRDP_PIPEWIRE_EXECUTABLE), QStringList{});
        if (!daemon.waitForStarted(3000)) {
            skipReason = QStringLiteral("the private PipeWire daemon did not start");
            return false;
        }
        const QString socket = runtime->path() + QStringLiteral("/pipewire-0");
        QElapsedTimer timer;
        timer.start();
        while (!QFileInfo::exists(socket) && timer.elapsed() < 3000 && daemon.state() == QProcess::Running) {
            QThread::msleep(20);
        }
        if (!QFileInfo::exists(socket)) {
            skipReason = QStringLiteral("the private PipeWire daemon did not create its socket");
            return false;
        }
        return true;
    }
    void stop()
    {
        if (daemon.state() != QProcess::NotRunning) {
            daemon.terminate();
            if (!daemon.waitForFinished(3000)) {
                daemon.kill();
                daemon.waitForFinished(1000);
            }
        }
    }
};

QJsonArray pwDump()
{
    QProcess dump;
    dump.start(QStringLiteral("pw-dump"), QStringList{});
    if (!dump.waitForFinished(3000) || dump.exitStatus() != QProcess::NormalExit || dump.exitCode() != 0) {
        return {};
    }
    return QJsonDocument::fromJson(dump.readAllStandardOutput()).array();
}

struct NodeInfo {
    QSet<QString> names;
    QSet<QString> krdp;
    int outputStreams = -1;
};

NodeInfo nodes()
{
    NodeInfo info;
    const QJsonArray dump = pwDump();
    if (dump.isEmpty()) {
        return info;
    }
    info.outputStreams = 0;
    for (const QJsonValue &value : dump) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("type")).toString() != QLatin1String("PipeWire:Interface:Node")) {
            continue;
        }
        const QJsonObject props = object.value(QStringLiteral("info")).toObject().value(QStringLiteral("props")).toObject();
        const QString name = props.value(QStringLiteral("node.name")).toString();
        info.names.insert(name);
        if (name.startsWith(QLatin1String("krdp."))) {
            info.krdp.insert(name);
        }
        if (props.value(QStringLiteral("media.class")).toString() == QLatin1String("Stream/Output/Audio")) {
            ++info.outputStreams;
        }
    }
    return info;
}

struct Metadata {
    QString value;
    bool present = false;
    bool operator==(const Metadata &) const = default;
};

Metadata metadata(quint32 subject, const QString &key)
{
    QProcess process;
    process.start(QStringLiteral("pw-metadata"), {QStringLiteral("-n"), QStringLiteral("default"), QString::number(subject), key});
    if (!process.waitForFinished(3000) || process.exitCode() != 0) {
        return {QStringLiteral("<pw-metadata failed>"), true};
    }
    const QRegularExpression match(QStringLiteral("key:'%1' value:'([^']*)'").arg(QRegularExpression::escape(key)));
    const auto found = match.match(QString::fromUtf8(process.readAllStandardOutput()));
    return found.hasMatch() ? Metadata{found.captured(1), true} : Metadata{};
}

bool setMetadata(quint32 subject, const QString &key, const QString &value, const QString &type)
{
    QProcess process;
    process.start(QStringLiteral("pw-metadata"), {QStringLiteral("-n"), QStringLiteral("default"), QString::number(subject), key, value, type});
    return process.waitForFinished(3000) && process.exitCode() == 0;
}

Graph *g_graph = nullptr;
}

class DeviceToggleLoopTest : public QObject
{
    Q_OBJECT

    QSet<QString> m_baselineNodes;

    // Poll until the node shows up; PipeWire registers it asynchronously.
    bool waitForNode(const QString &name, int timeoutMs = 3000)
    {
        QElapsedTimer timer;
        timer.start();
        do {
            if (nodes().krdp.contains(name)) {
                return true;
            }
            QThread::msleep(10);
        } while (timer.elapsed() < timeoutMs);
        return false;
    }

    struct Settle {
        bool ok = false;
        qint64 ms = 0;
        QString why;
    };
    // After a stop: no extra krdp.* node, fds and threads back to baseline.
    Settle settle(int baseFds, int baseThreads)
    {
        QElapsedTimer timer;
        timer.start();
        QString why;
        do {
            const NodeInfo info = nodes();
            const int fds = fdCount();
            const int threads = threadCount();
            if (info.outputStreams >= 0 && info.krdp == m_baselineNodes && fds <= baseFds && threads <= baseThreads) {
                return {true, timer.elapsed(), {}};
            }
            why = QStringLiteral("krdp nodes %1, fds %2/%3, threads %4/%5")
                      .arg(QStringList(info.krdp.values()).join(QLatin1Char(',')))
                      .arg(fds)
                      .arg(baseFds)
                      .arg(threads)
                      .arg(baseThreads);
            QThread::msleep(10);
        } while (timer.elapsed() < SettleMs);
        return {false, timer.elapsed(), why};
    }

    struct Summary {
        int baseFds = 0;
        int baseThreads = 0;
        qint64 maxSettleMs = 0;
        qint64 totalMs = 0;
    };

    // One warm-up cycle, then the baseline, then Cycles cycles checked each time.
    void loop(const char *label, const std::function<bool(int, QString *)> &start, const std::function<void()> &stop,
              const std::function<void(int)> &afterStop = {})
    {
        QString node;
        QVERIFY2(start(-1, &node), label);
        QVERIFY2(waitForNode(node), qPrintable(QStringLiteral("%1 warm-up node %2 never appeared").arg(QString::fromLatin1(label), node)));
        stop();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(nodes().krdp, m_baselineNodes, SettleMs);
        QTest::qWait(300); // let any worker thread that is on its way out exit
        Summary summary;
        summary.baseFds = fdCount();
        summary.baseThreads = threadCount();
        QElapsedTimer total;
        total.start();
        for (int cycle = 0; cycle < Cycles; ++cycle) {
            // pw_deinit() frees whatever PipeWire loops are still alive, so a
            // leaked pw_thread_loop is invisible when the last reference goes.
            // Even cycles hold an extra reference (as KPipeWire does in the
            // real server) so leaks show up in the fd count; odd cycles let
            // the count reach zero and exercise pw_deinit()/pw_init() again.
            KRdp::PipeWireRuntime::Reference hold;
            if (cycle % 2 == 0) {
                hold.acquire();
            }
            QVERIFY2(start(cycle, &node), qPrintable(QStringLiteral("%1 cycle %2 did not start").arg(QString::fromLatin1(label)).arg(cycle)));
            QVERIFY2(waitForNode(node), qPrintable(QStringLiteral("%1 cycle %2: node %3 never appeared").arg(QString::fromLatin1(label)).arg(cycle).arg(node)));
            stop();
            if (QTest::currentTestFailed()) {
                return;
            }
            const Settle result = settle(summary.baseFds, summary.baseThreads);
            QVERIFY2(result.ok, qPrintable(QStringLiteral("%1 cycle %2 did not settle within %3 ms: %4").arg(QString::fromLatin1(label)).arg(cycle).arg(SettleMs).arg(result.why)));
            summary.maxSettleMs = qMax(summary.maxSettleMs, result.ms);
            QCOMPARE(KRdp::PipeWireRuntime::references(), hold.held() ? 1 : 0);
            if (afterStop) {
                afterStop(cycle);
                if (QTest::currentTestFailed()) {
                    return;
                }
            }
        }
        summary.totalMs = total.elapsed();
        QCOMPARE(KRdp::PipeWireRuntime::references(), 0);
        const int finalFds = fdCount();
        const int finalThreads = threadCount();
        std::printf("DEVICE-LOOP %s: %d cycles in %lld ms; fds %d -> %d, threads %d -> %d; slowest settle %lld ms\n", label, Cycles,
                    static_cast<long long>(summary.totalMs), summary.baseFds, finalFds, summary.baseThreads, finalThreads,
                    static_cast<long long>(summary.maxSettleMs));
        std::fflush(stdout);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(g_graph);
        const NodeInfo info = nodes();
        QVERIFY2(info.names.contains(PrivateGraphMarker), "pw-dump does not see the private graph; refusing to touch PipeWire metadata");
        m_baselineNodes = info.krdp; // the config's own null sink
        QVERIFY(m_baselineNodes.contains(BaselineSink));
        // A "user choice" to be preserved: the private null sink as both the
        // effective and the configured default.
        QVERIFY(setMetadata(0, QStringLiteral("default.audio.sink"), BaselineSinkJson, QStringLiteral("Spa:String:JSON")));
        QVERIFY(setMetadata(0, QStringLiteral("default.configured.audio.sink"), BaselineSinkJson, QStringLiteral("Spa:String:JSON")));
        QCOMPARE(KRdp::PipeWireRuntime::references(), 0);
    }

    void microphoneToggleLoop()
    {
        KRdp::PipeWireMicrophone mic;
        loop(
            "microphone",
            [&mic](int cycle, QString *node) {
                const QString id = QStringLiteral("loop-%1").arg(cycle + 1);
                *node = QStringLiteral("krdp.remote-microphone.") + id;
                if (!mic.start(id)) {
                    return false;
                }
                // Wait for the stream to reach its steady state, as a session would.
                QElapsedTimer timer;
                timer.start();
                while (mic.state() != KRdp::PipeWireMicrophone::State::Ready && timer.elapsed() < 3000) {
                    QThread::msleep(5);
                }
                return mic.state() == KRdp::PipeWireMicrophone::State::Ready;
            },
            [&mic] {
                mic.stop();
            });
    }

    void cameraToggleLoop()
    {
        loop(
            "camera",
            [this](int cycle, QString *node) {
                m_camera = std::make_unique<KRdp::PipeWireCamera>();
                const QString id = QStringLiteral("loop-%1").arg(cycle + 1);
                *node = QStringLiteral("krdp.remote-camera.") + id;
                return m_camera->start(id, 640, 480, 30);
            },
            [this] {
                m_camera.reset();
            });
    }

    void cameraLoopbackToggleLoop()
    {
        // A real v4l2loopback device needs the kernel module and may be in use
        // by the host's live service, so a regular file stands in: open()
        // succeeds and VIDIOC_S_FMT fails, which exercises the endpoint's
        // loopback fd handling (open, then close on the failed configure).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString fakeLoopback = dir.filePath(QStringLiteral("video-loopback"));
        {
            QFile file(fakeLoopback);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        loop(
            "camera+loopback",
            [this, &fakeLoopback](int cycle, QString *node) {
                m_camera = std::make_unique<KRdp::PipeWireCamera>();
                const QString id = QStringLiteral("loopback-%1").arg(cycle + 1);
                *node = QStringLiteral("krdp.remote-camera.") + id;
                // Proves the fd was opened and then closed on the failed configure.
                QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("could not configure V4L2 loopback")));
                return m_camera->start(id, 640, 480, 30, fakeLoopback);
            },
            [this] {
                m_camera->stop();
                m_camera.reset();
            });
    }

    void isolatedPlaybackToggleLoop()
    {
        // An application already playing when the session silences the host:
        // startIsolated() retargets it and stop() must clear that again. No
        // policy daemon runs, so it never links; its node is enough.
        QProcess player;
        const bool havePlayer = !QStandardPaths::findExecutable(QStringLiteral("pw-cat")).isEmpty();
        quint32 playerId = 0;
        if (havePlayer) {
            player.start(QStringLiteral("pw-cat"), {QStringLiteral("--playback"), QStringLiteral("--raw"), QStringLiteral("--rate"), QStringLiteral("48000"),
                                                    QStringLiteral("--channels"), QStringLiteral("2"), QStringLiteral("--format"), QStringLiteral("s16"),
                                                    QStringLiteral("-")});
            QVERIFY(player.waitForStarted());
            QTRY_VERIFY_WITH_TIMEOUT(nodes().outputStreams == 1, 3000);
            for (const QJsonValue &value : pwDump()) {
                const QJsonObject object = value.toObject();
                if (object.value(QStringLiteral("info")).toObject().value(QStringLiteral("props")).toObject().value(QStringLiteral("media.class")).toString()
                    == QLatin1String("Stream/Output/Audio")) {
                    playerId = quint32(object.value(QStringLiteral("id")).toInt());
                }
            }
            QVERIFY(playerId != 0);
        } else {
            qWarning("pw-cat not installed: moved-stream restore not exercised");
        }
        const Metadata baseDefault = metadata(0, QStringLiteral("default.audio.sink"));
        const Metadata baseConfigured = metadata(0, QStringLiteral("default.configured.audio.sink"));
        QCOMPARE(baseDefault.value, BaselineSinkJson);
        QCOMPARE(baseConfigured.value, BaselineSinkJson);
        qint64 slowestStopJob = 0;
        qint64 slowestStopCall = 0;
        loop(
            "playback-isolated",
            [this, havePlayer](int cycle, QString *node) {
                m_playback = std::make_unique<KRdp::PipeWireAudioPlayback>();
                const QString id = QStringLiteral("loop-%1").arg(cycle + 1);
                *node = QStringLiteral("krdp.remote-audio.") + id;
                if (havePlayer) {
                    QTest::ignoreMessage(QtInfoMsg, QRegularExpression(QStringLiteral("^Moved 1 existing PipeWire playback stream")));
                }
                if (!m_playback->startIsolated(id)) {
                    return false;
                }
                const QString selected = QStringLiteral("{\"name\":\"%1\"}").arg(*node);
                return metadata(0, QStringLiteral("default.audio.sink")).value == selected
                    && metadata(0, QStringLiteral("default.configured.audio.sink")).value == selected;
            },
            [&slowestStopJob, &slowestStopCall, this] {
                // The path RdpConnection::onClose() takes: hand over and return.
                QElapsedTimer timer;
                timer.start();
                KRdp::PipeWireAudioPlayback::stopAsync(std::move(m_playback));
                slowestStopCall = qMax(slowestStopCall, timer.elapsed());
                QVERIFY(KRdp::PipeWireAudioPlayback::waitForPendingStops(10000));
                slowestStopJob = qMax(slowestStopJob, timer.elapsed());
            },
            [&](int cycle) {
                QCOMPARE(metadata(0, QStringLiteral("default.audio.sink")), baseDefault);
                QCOMPARE(metadata(0, QStringLiteral("default.configured.audio.sink")), baseConfigured);
                if (havePlayer) {
                    QVERIFY2(!metadata(playerId, QStringLiteral("target.object")).present, qPrintable(QStringLiteral("cycle %1 left the player retargeted").arg(cycle)));
                }
            });
        std::printf("DEVICE-LOOP playback-isolated: stopAsync() returned within %lld ms; slowest stop job %lld ms\n",
                    static_cast<long long>(slowestStopCall), static_cast<long long>(slowestStopJob));
        std::fflush(stdout);
        QVERIFY(slowestStopCall < 100);
        if (havePlayer) {
            player.closeWriteChannel();
            player.terminate();
            player.waitForFinished(3000);
        }
    }

    void playbackStopAsyncOutlivesCaller()
    {
        // The job owns the endpoint: the "connection" that queued it is gone
        // at once, and a new isolated start waits for the old restore first.
        {
            auto first = std::make_unique<KRdp::PipeWireAudioPlayback>();
            QVERIFY(first->startIsolated(QStringLiteral("handover-a")));
            KRdp::PipeWireAudioPlayback::stopAsync(std::move(first));
        }
        KRdp::PipeWireAudioPlayback second;
        QVERIFY(second.startIsolated(QStringLiteral("handover-b")));
        second.stop();
        QVERIFY(KRdp::PipeWireAudioPlayback::waitForPendingStops(10000));
        QCOMPARE(metadata(0, QStringLiteral("default.audio.sink")).value, BaselineSinkJson);
        QCOMPARE(metadata(0, QStringLiteral("default.configured.audio.sink")).value, BaselineSinkJson);
        QTRY_COMPARE_WITH_TIMEOUT(nodes().krdp, m_baselineNodes, SettleMs);
        QCOMPARE(KRdp::PipeWireRuntime::references(), 0);
    }

private:
    std::unique_ptr<KRdp::PipeWireCamera> m_camera;
    std::unique_ptr<KRdp::PipeWireAudioPlayback> m_playback;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Graph graph;
    if (!graph.start()) {
        graph.stop();
        std::fprintf(stderr, "SKIP DeviceToggleLoopTest: %s\n", qPrintable(graph.skipReason));
        return 77;
    }
    g_graph = &graph;
    DeviceToggleLoopTest test;
    const int result = QTest::qExec(&test, argc, argv);
    g_graph = nullptr;
    graph.stop();
    return result;
}

#include "DeviceToggleLoopTest.moc"
