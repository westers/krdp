// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX8: the worker -> broker message order. 23b328a's worker reported its encoder backend
// as soon as the encoder opened, before Ready, and the broker rejected it ("worker did not
// confirm active capture"): Sol :3391 and :3395 and the ace/cray virtual desktops never got a
// picture. The earlier tests' fake worker sent Hello + Ready only, so they passed. These tests
// drive ConsoleWorkerOutbox - the class the real worker sends through - in the real worker's
// event order, and replay 23b328a's recorded record order against the real endpoint.

#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "ConsoleWorkerEndpoint.h"
#include "ConsoleWorkerOutbox.h"

using namespace KRdp;
using Wire = ConsoleWorkerWire::Kind;
using Report = ConsoleWorkerWire::EncoderReport;

namespace
{
QVector<ConsoleWorkerWire::Record> records(const QByteArray &bytes)
{
    ConsoleWorkerWire::Deframer deframer;
    deframer.feed(bytes);
    QVector<ConsoleWorkerWire::Record> result;
    while (const auto record = deframer.next()) {
        result.append(*record);
    }
    return result;
}

ConsoleWorkerWire::EncoderCaps probeCaps()
{
    ConsoleWorkerWire::EncoderCaps caps;
    caps.encoders.avc = {true, true};
    caps.encoders.hevc = {true, true};
    caps.encoders.av1 = {false, true};
    caps.renderNode = QStringLiteral("/dev/dri/renderD128");
    return caps;
}

VideoFrame keyFrame(QSize size)
{
    VideoFrame frame;
    frame.size = size;
    frame.data = QByteArray("\x00\x00\x00\x01\x65keyframe", 13);
    frame.isKeyFrame = true;
    frame.monitors = {{QRect(QPoint(0, 0), size), true}};
    return frame;
}

/// A broker endpoint plus a connected worker socket; `events` is the order the broker saw.
struct Harness {
    QTemporaryDir directory;
    ConsoleWorkerEndpoint endpoint;
    QLocalSocket worker;
    QByteArray token = QByteArray(24, 'o');
    ConsoleHandoff::Target target;
    QStringList events;
    QStringList errors;

    explicit Harness(ConsoleSeat::Adapter adapter, const QString &session)
        : target{adapter, session, quint32(getuid())}
    {
    }

    bool start()
    {
        if (!directory.isValid() || !endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), target, token)) {
            return false;
        }
        QObject::connect(&endpoint, &ConsoleWorkerEndpoint::encoderCapsReceived, &endpoint, [this](const auto &) {
            events << QStringLiteral("caps");
        });
        QObject::connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, &endpoint, [this](const auto &) {
            events << QStringLiteral("ready");
        });
        QObject::connect(&endpoint, &ConsoleWorkerEndpoint::encoderReported, &endpoint, [this](const Report &report) {
            events << QStringLiteral("report:%1:%2:%3")
                          .arg(report.event == Report::Event::Backend ? QStringLiteral("backend") : QStringLiteral("unavailable"))
                          .arg(int(report.codec))
                          .arg(report.hardware ? QStringLiteral("hw") : QStringLiteral("sw"));
        });
        QObject::connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, &endpoint, [this](const auto &) {
            events << QStringLiteral("outputs");
        });
        QObject::connect(&endpoint, &ConsoleWorkerEndpoint::frameReceived, &endpoint, [this](const VideoFrame &frame) {
            events << (frame.isKeyFrame ? QStringLiteral("keyframe") : QStringLiteral("frame"));
        });
        QObject::connect(&endpoint, &ConsoleWorkerEndpoint::protocolError, &endpoint, [this](const QString &message) {
            errors << message;
        });
        worker.connectToServer(endpoint.socketName());
        return worker.waitForConnected(1000);
    }

    ConsoleWorkerOutbox outbox()
    {
        return ConsoleWorkerOutbox([this](const QByteArray &record) {
            worker.write(record);
        });
    }

    void flush()
    {
        worker.waitForBytesWritten(1000);
        QTest::qWait(30);
    }
};
}

class ConsoleWorkerOrderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void outboxHoldsEncoderReportsUntilReady();
    void outboxBoundsHeldReports();
    void consoleWorkerOrderReachesReady();
    void virtualBootstrapOrderReachesReady();
    void recordedFix7OrderIsAccepted();
    void earlyWorkerErrorIsReported();
    void tooManyEarlyReportsFail();
    void anythingElseBeforeReadyStillFails();
    void drainedWorkerDropsEarlyReports();
    void encoderStatsNeverPrecedeReady();
    void chromaCostsRequireReadyAndStayBounded();
    void encoderStatsBeforeReadyFailTheWorker();
};

void ConsoleWorkerOrderTest::outboxHoldsEncoderReportsUntilReady()
{
    QByteArray sent;
    ConsoleWorkerOutbox outbox([&sent](const QByteArray &record) {
        sent += record;
    });
    // Nothing leaves before Hello: a report from an encoder that opened before the socket did.
    outbox.report({Report::Event::Backend, VideoCodec::Avc420, true});
    outbox.ready();
    QVERIFY(sent.isEmpty());

    outbox.hello({QStringLiteral("3"), 1000, QByteArray(24, 't')});
    outbox.caps(probeCaps());
    // The real worker's order: the encoder opens (backend report), the codec policy's HEVC
    // encoder is unavailable, the H.264 encoder reopens in software, a CPU sample - then capture
    // is confirmed.
    outbox.report({Report::Event::Backend, VideoCodec::Avc420, true});
    outbox.report({Report::Event::Unavailable, VideoCodec::Hevc, false});
    outbox.report({Report::Event::Backend, VideoCodec::Avc420, false});
    outbox.load({123});
    QCOMPARE(outbox.held().size(), 2);
    const auto beforeReady = records(sent);
    QCOMPARE(beforeReady.size(), 2);
    QCOMPARE(beforeReady[0].kind, Wire::Hello);
    QCOMPARE(beforeReady[1].kind, Wire::EncoderCaps);

    outbox.ready();
    outbox.ready(); // once
    outbox.caps(probeCaps()); // caps belong before Ready only
    outbox.report({Report::Event::Backend, VideoCodec::Av1, true});
    outbox.load({456});
    const auto all = records(sent);
    QCOMPARE(all.size(), 7);
    QCOMPARE(all[2].kind, Wire::Ready);
    QCOMPARE(ConsoleWorkerWire::encoderReport(all[3]), std::optional<Report>(Report{Report::Event::Unavailable, VideoCodec::Hevc, false}));
    QCOMPARE(ConsoleWorkerWire::encoderReport(all[4]), std::optional<Report>(Report{Report::Event::Backend, VideoCodec::Avc420, false}));
    QCOMPARE(ConsoleWorkerWire::encoderReport(all[5]), std::optional<Report>(Report{Report::Event::Backend, VideoCodec::Av1, true}));
    QCOMPARE(ConsoleWorkerWire::encoderLoad(all[6]), std::optional<ConsoleWorkerWire::EncoderLoad>(ConsoleWorkerWire::EncoderLoad{456}));

    // A new connection starts over.
    sent.clear();
    outbox.hello({QStringLiteral("3"), 1000, QByteArray(24, 't')});
    QVERIFY(!outbox.readySent());
    outbox.report({Report::Event::Backend, VideoCodec::Avc420, true});
    QCOMPARE(records(sent).size(), 1);
}

void ConsoleWorkerOrderTest::outboxBoundsHeldReports()
{
    int written = 0;
    ConsoleWorkerOutbox outbox([&written](const QByteArray &) {
        ++written;
    });
    outbox.hello({QStringLiteral("3"), 1000, QByteArray(24, 't')});
    // Distinct (event, codec) pairs cannot exceed 6, but the bound must hold even so.
    for (int i = 0; i < 100; ++i) {
        outbox.report({i % 2 ? Report::Event::Backend : Report::Event::Unavailable, VideoCodec(i % 5), bool(i % 3)});
    }
    const auto held = outbox.held().size();
    QCOMPARE(held, qsizetype(10)); // the latest of each (event, codec)
    QVERIFY(held <= ConsoleWorkerOutbox::MaxHeldReports);
    QVERIFY(ConsoleWorkerOutbox::MaxHeldReports <= ConsoleWorkerEndpoint::MaxEarlyReports);
    QCOMPARE(written, 1); // Hello only
    outbox.ready();
    QCOMPARE(written, 2 + int(held));
    QVERIFY(outbox.held().isEmpty());
}

void ConsoleWorkerOrderTest::consoleWorkerOrderReachesReady()
{
    // The console worker (consoleworker.cpp, m_initialOutputs empty): connected -> Hello, caps;
    // the encoder opens (Backend) before PipeWire reports the stream active -> Ready; then the
    // first frame carries Outputs and the frame itself.
    Harness h(ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"));
    QVERIFY(h.start());
    auto outbox = h.outbox();
    outbox.hello({h.target.sessionId, h.target.uid, h.token});
    outbox.caps(probeCaps());
    outbox.report({Report::Event::Backend, VideoCodec::Avc420, true});
    h.flush();
    QVERIFY(h.errors.isEmpty());
    QVERIFY(!h.endpoint.ready());
    outbox.ready();
    h.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Outputs{{{QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true}}}));
    h.worker.write(ConsoleWorkerWire::frame(keyFrame({1280, 720})));
    h.flush();
    QTRY_COMPARE(h.events.size(), 5);
    QVERIFY2(h.errors.isEmpty(), qPrintable(h.errors.join(QLatin1Char('\n'))));
    QCOMPARE(h.events, (QStringList{QStringLiteral("caps"), QStringLiteral("ready"), QStringLiteral("report:backend:0:hw"),
                                    QStringLiteral("outputs"), QStringLiteral("keyframe")}));
    QVERIFY(h.endpoint.ready());
    QCOMPARE(h.endpoint.encoderCaps(), std::optional(probeCaps()));
}

void ConsoleWorkerOrderTest::virtualBootstrapOrderReachesReady()
{
    // A virtual desktop with a committed initial layout: the worker's encoder opens and its
    // policy may already fall back (HEVC unavailable -> AVC) while it waits for the keyframe that
    // proves the bootstrap layout; Ready follows that keyframe, then Outputs and the frame.
    Harness h(ConsoleSeat::Adapter::VirtualUser, QStringLiteral("0a1b2c3d-0000-4000-8000-000000000001"));
    QVERIFY(h.start());
    auto outbox = h.outbox();
    outbox.hello({h.target.sessionId, h.target.uid, h.token});
    outbox.caps(probeCaps());
    outbox.report({Report::Event::Backend, VideoCodec::Hevc, true});
    outbox.report({Report::Event::Unavailable, VideoCodec::Av1, false});
    outbox.load({1000}); // not controlled yet, and not before Ready
    h.flush();
    QVERIFY(h.errors.isEmpty());
    outbox.ready();
    h.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Outputs{{{QStringLiteral("Virtual-krdp-initial-1"), QRect(0, 0, 1920, 1080), 1, true}}}));
    h.worker.write(ConsoleWorkerWire::frame(keyFrame({1920, 1080})));
    outbox.report({Report::Event::Backend, VideoCodec::Avc420, true});
    h.flush();
    QTRY_COMPARE(h.events.size(), 7);
    QVERIFY2(h.errors.isEmpty(), qPrintable(h.errors.join(QLatin1Char('\n'))));
    QCOMPARE(h.events, (QStringList{QStringLiteral("caps"), QStringLiteral("ready"), QStringLiteral("report:backend:3:hw"),
                                    QStringLiteral("report:unavailable:4:sw"), QStringLiteral("outputs"), QStringLiteral("keyframe"),
                                    QStringLiteral("report:backend:0:hw")}));
}

void ConsoleWorkerOrderTest::recordedFix7OrderIsAccepted()
{
    // The record order 23b328a's worker produced on Sol, ace and cray (one write each, coalesced
    // by the socket): Hello, EncoderCaps, EncoderReport{Backend}, Ready, Outputs, Frame. A worker
    // that old must still get its picture from a fixed broker.
    Harness h(ConsoleSeat::Adapter::VirtualUser, QStringLiteral("0a1b2c3d-0000-4000-8000-000000000002"));
    QVERIFY(h.start());
    QByteArray recorded = ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{h.target.sessionId, h.target.uid, h.token});
    recorded += ConsoleWorkerWire::frame(probeCaps());
    recorded += ConsoleWorkerWire::frame(Report{Report::Event::Backend, VideoCodec::Avc420, true});
    recorded += ConsoleWorkerWire::frame(ConsoleWorkerWire::EncoderLoad{42});
    recorded += ConsoleWorkerWire::frame(Wire::Ready);
    recorded += ConsoleWorkerWire::frame(ConsoleWorkerWire::Outputs{{{QStringLiteral("Virtual-0"), QRect(0, 0, 1280, 720), 1, true}}});
    recorded += ConsoleWorkerWire::frame(keyFrame({1280, 720}));
    h.worker.write(recorded);
    h.flush();
    QTRY_COMPARE(h.events.size(), 5);
    QVERIFY2(h.errors.isEmpty(), qPrintable(h.errors.join(QLatin1Char('\n'))));
    QCOMPARE(h.events, (QStringList{QStringLiteral("caps"), QStringLiteral("ready"), QStringLiteral("report:backend:0:hw"),
                                    QStringLiteral("outputs"), QStringLiteral("keyframe")}));
    QCOMPARE(h.endpoint.workerCpuNs(), qint64(42));
}

void ConsoleWorkerOrderTest::earlyWorkerErrorIsReported()
{
    Harness h(ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"));
    QVERIFY(h.start());
    h.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{h.target.sessionId, h.target.uid, h.token})
                   + ConsoleWorkerWire::frame(Wire::Error, QByteArrayLiteral("screencast failed")));
    h.flush();
    QTRY_COMPARE(h.errors.size(), 1);
    QVERIFY2(h.errors.first().contains(QStringLiteral("screencast failed")), qPrintable(h.errors.first()));
    QVERIFY(!h.endpoint.ready());
}

void ConsoleWorkerOrderTest::tooManyEarlyReportsFail()
{
    Harness h(ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"));
    QVERIFY(h.start());
    QByteArray flood = ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{h.target.sessionId, h.target.uid, h.token});
    for (int i = 0; i <= ConsoleWorkerEndpoint::MaxEarlyReports; ++i) {
        flood += ConsoleWorkerWire::frame(Report{Report::Event::Backend, VideoCodec::Avc420, true});
    }
    h.worker.write(flood);
    h.flush();
    QTRY_COMPARE(h.errors.size(), 1);
    QVERIFY(!h.endpoint.ready());
    QVERIFY(!h.events.contains(QStringLiteral("report:backend:0:hw")));
}

void ConsoleWorkerOrderTest::anythingElseBeforeReadyStillFails()
{
    Harness h(ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"));
    QVERIFY(h.start());
    h.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{h.target.sessionId, h.target.uid, h.token})
                   + ConsoleWorkerWire::frame(Report{Report::Event::Backend, VideoCodec::Avc420, true})
                   + ConsoleWorkerWire::frame(ConsoleWorkerWire::Outputs{{{QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true}}}));
    h.flush();
    QTRY_COMPARE(h.errors.size(), 1);
    QVERIFY(h.errors.first().startsWith(QStringLiteral("worker did not confirm active capture")));
    QVERIFY(!h.endpoint.ready());
    QVERIFY(h.events.isEmpty()); // the held report never reaches a broker that did not get Ready
}

void ConsoleWorkerOrderTest::drainedWorkerDropsEarlyReports()
{
    Harness h(ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"));
    QVERIFY(h.start());
    h.endpoint.stopWorker(); // before authentication: delivered on Hello
    auto outbox = h.outbox();
    outbox.hello({h.target.sessionId, h.target.uid, h.token});
    h.worker.write(ConsoleWorkerWire::frame(Report{Report::Event::Backend, VideoCodec::Avc420, true})
                   + ConsoleWorkerWire::frame(Wire::Ready));
    h.flush();
    QVERIFY(h.errors.isEmpty());
    QVERIFY(!h.endpoint.ready());
    QVERIFY(h.events.isEmpty());
}

void ConsoleWorkerOrderTest::encoderStatsNeverPrecedeReady()
{
    // STATS-S6: the worker's EncoderStats go out after Ready only (the Ready-order rule AUD-FIX8
    // made for EncoderReport): one made before it - the stats timer can fire while capture is
    // still being confirmed - is dropped, not held (the next one follows in a second).
    QByteArray sent;
    ConsoleWorkerOutbox outbox([&sent](const QByteArray &record) {
        sent += record;
    });
    const ConsoleWorkerWire::EncoderStats stats{1000, 30, 1, -1};
    outbox.stats(stats); // before Hello
    outbox.hello({QStringLiteral("3"), 1000, QByteArray(24, 't')});
    outbox.caps(probeCaps());
    outbox.stats(stats); // before Ready
    auto before = records(sent);
    QCOMPARE(before.size(), 2);
    QCOMPARE(before[0].kind, Wire::Hello);
    QCOMPARE(before[1].kind, Wire::EncoderCaps);
    outbox.ready();
    QCOMPARE(records(sent).size(), 3); // Ready, and no stats held for it
    outbox.stats(stats);
    const auto all = records(sent);
    QCOMPARE(all.size(), 4);
    QCOMPARE(all[2].kind, Wire::Ready);
    QCOMPARE(ConsoleWorkerWire::encoderStats(all[3]), std::optional(stats));
    // A new connection starts over: nothing before its Ready again.
    sent.clear();
    outbox.hello({QStringLiteral("3"), 1000, QByteArray(24, 't')});
    outbox.stats(stats);
    QCOMPARE(records(sent).size(), 1);

    // The broker delivers them once the worker is ready.
    Harness h(ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"));
    QVERIFY(h.start());
    QList<ConsoleWorkerWire::EncoderStats> received;
    QObject::connect(&h.endpoint, &ConsoleWorkerEndpoint::encoderStatsReceived, &h.endpoint, [&received](const auto &s) {
        received << s;
    });
    auto worker = h.outbox();
    worker.hello({h.target.sessionId, h.target.uid, h.token});
    worker.stats(stats);
    worker.ready();
    worker.stats(stats);
    h.flush();
    QTRY_COMPARE(received.size(), 1);
    QVERIFY2(h.errors.isEmpty(), qPrintable(h.errors.join(QLatin1Char('\n'))));
    QCOMPARE(received.first(), stats);
}

void ConsoleWorkerOrderTest::chromaCostsRequireReadyAndStayBounded()
{
    ConsoleWorkerWire::ChromaTiming cost{7, VideoCodec::Avc444v2, 0, {}};
    cost.timing.splitVariant = QStringLiteral("avx2");
    cost.timing.frames = 30; cost.timing.auxMaxGap = 2;
    Harness h(ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"));
    QVERIFY(h.start());
    int received = 0;
    QObject::connect(&h.endpoint, &ConsoleWorkerEndpoint::chromaTimingReceived, &h.endpoint, [&](const auto &report) {
        QCOMPARE(report, cost); ++received;
    });
    auto outbox = h.outbox();
    outbox.chromaTiming(cost);
    outbox.hello({h.target.sessionId, h.target.uid, h.token});
    outbox.chromaTiming(cost);
    outbox.ready(); h.flush();
    QVERIFY(h.errors.isEmpty()); QCOMPARE(received, 0);
    outbox.chromaTiming(cost); h.flush(); QTRY_COMPARE(received, 1);
    // Invalid worker input is refused rather than forwarded as current costs.
    auto invalid = cost; invalid.timing.encodeMainAvg = -1;
    h.worker.write(ConsoleWorkerWire::frame(invalid)); h.flush();
    QTRY_COMPARE(h.errors.size(), 1); QCOMPARE(received, 1);

    Harness early(ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"));
    QVERIFY(early.start());
    early.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{early.target.sessionId, early.target.uid, early.token})
                       + ConsoleWorkerWire::frame(cost) + ConsoleWorkerWire::frame(Wire::Ready));
    early.flush(); QTRY_COMPARE(early.errors.size(), 1);
    QVERIFY(!early.endpoint.ready());
}

void ConsoleWorkerOrderTest::encoderStatsBeforeReadyFailTheWorker()
{
    // A worker that ignores the order (EncoderStats before Ready) is failed like any other
    // record before Ready: the broker never takes stats from a worker that did not confirm capture.
    Harness h(ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"));
    QVERIFY(h.start());
    int received = 0;
    QObject::connect(&h.endpoint, &ConsoleWorkerEndpoint::encoderStatsReceived, &h.endpoint, [&received](const auto &) {
        ++received;
    });
    h.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{h.target.sessionId, h.target.uid, h.token})
                   + ConsoleWorkerWire::frame(ConsoleWorkerWire::EncoderStats{1000, 30, 0, -1}) + ConsoleWorkerWire::frame(Wire::Ready));
    h.flush();
    QTRY_COMPARE(h.errors.size(), 1);
    QVERIFY(h.errors.first().startsWith(QStringLiteral("worker did not confirm active capture")));
    QVERIFY(!h.endpoint.ready());
    QCOMPARE(received, 0);
}

QTEST_GUILESS_MAIN(ConsoleWorkerOrderTest)

#include "ConsoleWorkerOrderTest.moc"
