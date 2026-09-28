// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX5: a stock client's clipboard must never cost it its picture. On 2026-09-28 wlfreerdp3
// 3.22 (Buzz, desktop unlocked) connected to Sol twice, announced its clipboard with three or four
// format lists within 6 ms, got a text data request for each at once, and never sent its RDPGFX
// caps: a white window that ignored SIGTERM. wlfreerdp3 reads the local Wayland clipboard for a
// request synchronously, holding its clipboard lock, and its event loop needs that lock for the
// next clipboard offer; when the read does not come back, the whole client stops. The same client
// worked with the desktop locked (no clipboard offer: every read failed at once).
//
// A real KRdp::Server on 127.0.0.1 and krdpctl-probe as the stock client (--no-krdpctl, RDPGFX
// with --gfx-count, --clipboard MODE announcing text like wlfreerdp3). The test feeds numbered
// frames into the connection's VideoStream and checks:
// - `stall` (the whole client stops at the first data request): it still gets a picture, because
//   the server asks for nothing before the client acknowledged a frame;
// - `never` (the request is ignored, the client carries on): frames keep coming;
// - `answer`: the client's text still arrives, with one request for the three lists;
// - no clipboard consumer (the console host): no request at all.

#include <QDeadlineTimer>
#include <QHostAddress>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "Clipboard.h"
#include "RdpConnection.h"
#include "Server.h"
#include "VideoStream.h"

using namespace KRdp;
using namespace std::chrono_literals;

namespace
{
const QString TestUser = QStringLiteral("alice");
const QString Password = QStringLiteral("correct horse");

/// The last 16 bytes of every fed frame: "KRDPSEQ:" + the sequence number (little endian).
QByteArray frameData(quint64 seq, int size)
{
    QByteArray data(size, '\x5a');
    QByteArray tail("KRDPSEQ:");
    for (int i = 0; i < 8; ++i) {
        tail.append(char((seq >> (8 * i)) & 0xff));
    }
    data.replace(size - tail.size(), tail.size(), tail);
    return data;
}

/// A running server, a frame feeder and a probe; the probe's stderr is collected.
struct Run {
    Server server;
    QPointer<RdpConnection> connection;
    bool closed = false;
    quint64 seq = 0;
    QTimer feeder;
    QProcess probe;
    QByteArray log;
    quint64 lastSeenSeq = 0;
    QString clientText;
    int clientTextChanges = 0;

    void readProbe()
    {
        const QByteArray more = probe.readAllStandardError();
        log += more;
        for (const QByteArray &line : more.split('\n')) {
            const int at = line.indexOf("frame seq ");
            if (at >= 0) {
                lastSeenSeq = std::max(lastSeenSeq, line.mid(at + 10).split(' ').value(0).toULongLong());
            }
        }
    }
    void wait(std::chrono::milliseconds duration)
    {
        QDeadlineTimer deadline(duration);
        while (!deadline.hasExpired()) {
            QTest::qWait(20);
            readProbe();
        }
    }
    /// Waits up to \a limit for \a done; returns it.
    template<typename F>
    bool waitFor(std::chrono::milliseconds limit, F done)
    {
        QDeadlineTimer deadline(limit);
        while (!done() && !deadline.hasExpired()) {
            QTest::qWait(20);
            readProbe();
        }
        return done();
    }
    int count(const char *needle) const
    {
        return int(log.count(needle));
    }
    ~Run()
    {
        if (probe.state() != QProcess::NotRunning) {
            probe.terminate();
            if (!probe.waitForFinished(5000)) {
                probe.kill();
                probe.waitForFinished(5000);
            }
        }
    }
};
}

class ClipboardStallLoopbackTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_certificate;
    QString m_key;

    /// Starts \a run with the probe in clipboard \a mode; \a consumer: something takes the client's clipboard.
    bool start(Run &run, const char *mode, bool consumer)
    {
        run.server.setAddress(QHostAddress::LocalHost);
        run.server.setPort(0);
        run.server.setTlsCertificate(m_certificate.toStdString());
        run.server.setTlsCertificateKey(m_key.toStdString());
        run.server.setUsers({{TestUser, Password}});
        connect(&run.server, &Server::newConnectionCreated, this, [&run, consumer](RdpConnection *c) {
            run.connection = c;
            c->videoStream()->setCodecPreference(CodecPreference::Avc420);
            connect(c, &RdpConnection::stateChanged, c, [&run, c] {
                if (c->state() == RdpConnection::State::Closed) {
                    run.closed = true;
                }
            });
            if (consumer) {
                // What SessionController does for a Plasma session.
                connect(c->clipboard(), &Clipboard::clientDataChanged, c, [&run, c] {
                    const auto data = c->clipboard()->getClipboard();
                    run.clientText = data ? data->text() : QString();
                    ++run.clientTextChanges;
                });
            }
        });
        if (!run.server.start()) {
            return false;
        }
        run.feeder.setInterval(33);
        connect(&run.feeder, &QTimer::timeout, this, [&run] {
            const auto &connection = run.connection;
            if (!connection || connection->state() != RdpConnection::State::Streaming || !connection->videoStream()->enabled()) {
                return;
            }
            const bool keyFrame = run.seq % 60 == 0;
            VideoFrame frame;
            frame.size = QSize(640, 480);
            frame.isKeyFrame = keyFrame;
            frame.data = frameData(++run.seq, keyFrame ? 16000 : 4000);
            frame.presentationTimeStamp = std::chrono::system_clock::now();
            connection->videoStream()->queueFrame(frame);
        });
        run.feeder.start();

        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), m_dir.path());
        environment.insert(QStringLiteral("HOME"), m_dir.path());
        run.probe.setProcessEnvironment(environment);
        // The password goes over stdin ("-"), never on the command line.
        run.probe.start(QStringLiteral(KRDPCTL_PROBE),
                        {QStringLiteral("127.0.0.1"), QString::number(run.server.serverPort()), TestUser, QStringLiteral("-"), QStringLiteral("--silent"),
                         QStringLiteral("--no-krdpctl"), QStringLiteral("--gfx"), QStringLiteral("--gfx-count"), QStringLiteral("--clipboard"),
                         QString::fromLatin1(mode), QStringLiteral("--timeout"), QStringLiteral("60")});
        if (!run.probe.waitForStarted(5000)) {
            return false;
        }
        run.probe.write(Password.toUtf8() + '\n');
        run.probe.closeWriteChannel();
        return true;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        const QString openssl = QStandardPaths::findExecutable(QStringLiteral("openssl"));
        if (openssl.isEmpty()) {
            QSKIP("openssl is needed to make the test certificate");
        }
        m_certificate = m_dir.filePath(QStringLiteral("server.crt"));
        m_key = m_dir.filePath(QStringLiteral("server.key"));
        QProcess process;
        process.start(openssl,
                      {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"), QStringLiteral("rsa:2048"), QStringLiteral("-nodes"),
                       QStringLiteral("-keyout"), m_key, QStringLiteral("-out"), m_certificate, QStringLiteral("-days"), QStringLiteral("1"),
                       QStringLiteral("-subj"), QStringLiteral("/CN=krdp-test")});
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(process.exitCode(), 0);
    }

    // The client that stops dead at the first clipboard request still gets its picture first.
    void stalledClientStillGetsAPicture()
    {
        Run run;
        QVERIFY(start(run, "stall", true));
        QVERIFY2(run.waitFor(30s, [&run] {
            return run.count("clipboard data request") > 0 || run.probe.state() != QProcess::Running;
        }),
                 run.log.constData());
        if (run.lastSeenSeq == 0 && run.log.contains("does not support H.264")) {
            QSKIP("this libfreerdp cannot negotiate AVC420 (built WITH_GFX_H264=OFF)");
        }
        run.wait(2s);
        qInfo().noquote() << "probe:\n" << run.log.right(3000);
        // A picture, before the request that stopped the client.
        QVERIFY2(run.lastSeenSeq >= 1, run.log.constData());
        QVERIFY2(run.log.indexOf("frame seq ") < run.log.indexOf("clipboard data request"), run.log.constData());
        // One request for the three format lists.
        QCOMPARE(run.count("clipboard: announced"), 3);
        QCOMPARE(run.count("clipboard data request"), 1);
        // The server is still there; the stalled client is the client's business.
        QVERIFY(!run.closed);
        QCOMPARE(run.probe.state(), QProcess::Running);
    }

    // A client that ignores the request but carries on keeps streaming, and so does the server.
    void unansweredRequestDoesNotStopTheStream()
    {
        Run run;
        QVERIFY(start(run, "never", true));
        QVERIFY2(run.waitFor(30s, [&run] {
            return run.count("clipboard data request") > 0 || run.probe.state() != QProcess::Running;
        }),
                 run.log.constData());
        if (run.lastSeenSeq == 0 && run.log.contains("does not support H.264")) {
            QSKIP("this libfreerdp cannot negotiate AVC420 (built WITH_GFX_H264=OFF)");
        }
        const quint64 atRequest = run.lastSeenSeq;
        // Past the server's ResponseTimeoutMs, frames keep arriving.
        run.wait(std::chrono::milliseconds(Clipboard::ResponseTimeoutMs + 1000));
        qInfo() << "frames seen at the request:" << atRequest << ", 6 s later:" << run.lastSeenSeq;
        QVERIFY2(run.lastSeenSeq >= atRequest + 100, run.log.right(3000).constData());
        QCOMPARE(run.count("clipboard data request"), 1);
        QVERIFY(!run.closed);
        QCOMPARE(run.probe.state(), QProcess::Running);
    }

    // The client's text still arrives: one request once the picture is up, the answer decoded.
    void answeredRequestDeliversTheText()
    {
        Run run;
        QVERIFY(start(run, "answer", true));
        QVERIFY2(run.waitFor(30s, [&run] {
            return run.clientTextChanges > 0 || run.probe.state() != QProcess::Running;
        }),
                 run.log.constData());
        if (run.lastSeenSeq == 0 && run.log.contains("does not support H.264")) {
            QSKIP("this libfreerdp cannot negotiate AVC420 (built WITH_GFX_H264=OFF)");
        }
        QCOMPARE(run.clientText, QStringLiteral("probe clipboard"));
        QCOMPARE(run.count("clipboard data request"), 1);
        QVERIFY(run.log.indexOf("frame seq ") < run.log.indexOf("clipboard data request"));
    }

    // Nothing takes the client's clipboard (the console host): the client is never asked.
    void noConsumerNoRequest()
    {
        Run run;
        QVERIFY(start(run, "answer", false));
        QVERIFY2(run.waitFor(30s, [&run] {
            return run.lastSeenSeq >= 30 || run.probe.state() != QProcess::Running;
        }),
                 run.log.constData());
        if (run.lastSeenSeq == 0 && run.log.contains("does not support H.264")) {
            QSKIP("this libfreerdp cannot negotiate AVC420 (built WITH_GFX_H264=OFF)");
        }
        run.wait(std::chrono::milliseconds(Clipboard::FormatListSettleMs + 1500));
        QCOMPARE(run.count("clipboard: announced"), 3);
        QCOMPARE(run.count("clipboard data request"), 0);
    }
};

QTEST_GUILESS_MAIN(ClipboardStallLoopbackTest)

#include "ClipboardStallLoopbackTest.moc"
