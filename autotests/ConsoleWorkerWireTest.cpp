// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "ConsoleWorkerWire.h"
#include "ConsoleMicrophoneWire.h"

using namespace KRdp;
using namespace KRdp::ConsoleWorkerWire;

class ConsoleWorkerWireTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void deframesSplitAndCoalescedRecords();
    void roundTripsEncodedFrame();
    void roundTripsNormalizedInput();
    void roundTripsMediaAndPcm();
    void roundTripsOutputs();
    void rejectsInvalidOutputs();
    void roundTripsControlGeneration();
    void roundTripsResizeAndRejectsMalformedRequests();
    void rejectsOversizedRecord();
    void videoQualityIsBoundedAndGenerationScoped();
    void microphoneRecordsAreBoundedAndCorrelated();
};

void ConsoleWorkerWireTest::microphoneRecordsAreBoundedAndCorrelated()
{
    Deframer reader;
    const MicrophonePolicy policy{5, 17, true};
    const MicrophoneAudio audio{5, 17, QByteArray(3840, 'x')};
    const MicrophoneResult result{5, 17, QStringLiteral("source unavailable")};
    reader.feed(frame(policy) + frame(audio) + frame(result));
    auto record = reader.next();
    QVERIFY(record);
    QCOMPARE(microphonePolicy(*record), std::optional<MicrophonePolicy>(policy));
    auto invalid = *record;
    invalid.payload[16] = 2;
    QVERIFY(!microphonePolicy(invalid));
    invalid.payload.chop(1);
    QVERIFY(!microphonePolicy(invalid));
    record = reader.next();
    QVERIFY(record);
    QCOMPARE(microphoneAudio(*record), std::optional<MicrophoneAudio>(audio));
    invalid = *record;
    invalid.payload.append('x');
    QVERIFY(!microphoneAudio(invalid));
    record = reader.next();
    QVERIFY(record);
    QCOMPARE(microphoneResult(*record), std::optional<MicrophoneResult>(result));
    for (const auto &bad : {MicrophoneAudio{0, 17, "1234"}, MicrophoneAudio{5, 0, "1234"},
                            MicrophoneAudio{5, 17, {}}, MicrophoneAudio{5, 17, "123"},
                            MicrophoneAudio{5, 17, QByteArray(3844, 'x')}}) {
        reader.feed(frame(bad));
        record = reader.next();
        QVERIFY(record);
        QVERIFY(!microphoneAudio(*record));
    }
}

void ConsoleWorkerWireTest::videoQualityIsBoundedAndGenerationScoped()
{
    Deframer reader;
    const VideoQuality request{42, 60};
    reader.feed(frame(request));
    const auto record = reader.next();
    QVERIFY(record);
    QCOMPARE(videoQuality(*record), std::optional<VideoQuality>(request));
    QVERIFY(mayApplyQuality(request, {42, true}));
    QVERIFY(!mayApplyQuality(request, {41, true}));
    QVERIFY(!mayApplyQuality(request, {42, false}));
    QVERIFY(!mayApplyQuality({0, 60}, {0, true}));
    auto invalid = *record;
    invalid.payload.chop(1);
    QVERIFY(!videoQuality(invalid));
    invalid = *record;
    invalid.payload.append('x');
    QVERIFY(!videoQuality(invalid));
    for (const auto bad : {VideoQuality{0, 60}, VideoQuality{42, 9}, VideoQuality{42, 101}}) {
        reader.feed(frame(bad));
        const auto malformed = reader.next();
        QVERIFY(malformed);
        QVERIFY(!videoQuality(*malformed));
        QVERIFY(!mayApplyQuality(bad, {42, true}));
    }
}

void ConsoleWorkerWireTest::roundTripsResizeAndRejectsMalformedRequests()
{
    const Resize request{17, 4, QStringLiteral("DP-3"), QSize(1280, 720), 1.25};
    Deframer reader;
    reader.feed(frame(request) + frame(ResizeResult{17, 4, QStringLiteral("unsupported mode")}));
    const auto record = reader.next();
    QVERIFY(record);
    QCOMPARE(resize(*record), std::optional<Resize>(request));
    const auto result = reader.next();
    QVERIFY(result);
    QCOMPARE(resizeResult(*result), std::optional<ResizeResult>({17, 4, QStringLiteral("unsupported mode")}));
    auto truncated = *record;
    truncated.payload.chop(1);
    QVERIFY(!resize(truncated));
    QVERIFY(!resize(*result));
    for (const auto &invalid : {Resize{0, 4, QStringLiteral("DP-3"), QSize(1280, 720), 1},
                               Resize{17, 0, QStringLiteral("DP-3"), QSize(1280, 720), 1},
                               Resize{17, 4, QStringLiteral("DP-3"), QSize(8192, 4320), 1},
                               Resize{17, 4, QStringLiteral("DP-3"), QSize(1280, 720), qQNaN()}}) {
        reader.feed(frame(invalid));
        const auto bad = reader.next();
        QVERIFY(bad);
        QVERIFY(!resize(*bad));
    }
}

void ConsoleWorkerWireTest::deframesSplitAndCoalescedRecords()
{
    const QByteArray first = frame(Kind::Hello, "greeter");
    const QByteArray second = frame(Kind::Ready, "ok");
    Deframer deframer;
    deframer.feed(first.left(3));
    QVERIFY(!deframer.next());
    deframer.feed(first.mid(3) + second);
    QCOMPARE(deframer.next(), std::optional<Record>(Record{Kind::Hello, "greeter"}));
    QCOMPARE(deframer.next(), std::optional<Record>(Record{Kind::Ready, "ok"}));
}

void ConsoleWorkerWireTest::roundTripsEncodedFrame()
{
    VideoFrame sent;
    sent.size = QSize(1920, 1080);
    sent.data = "main";
    sent.aux = "aux";
    sent.isKeyFrame = true;
    sent.auxIsKeyFrame = true;
    sent.monitorIndex = 1;
    sent.monitors = {{QRect(0, 0, 1920, 1080), true}};
    Deframer deframer;
    deframer.feed(frame(sent));
    const auto record = deframer.next();
    QVERIFY(record);
    const auto received = videoFrame(*record);
    QVERIFY(received);
    QCOMPARE(received->size, sent.size);
    QCOMPARE(received->data, sent.data);
    QCOMPARE(received->aux, sent.aux);
    QCOMPARE(received->isKeyFrame, sent.isKeyFrame);
    QCOMPARE(received->auxIsKeyFrame, sent.auxIsKeyFrame);
    QCOMPARE(received->monitorIndex, sent.monitorIndex);
    QCOMPARE(received->monitors, sent.monitors);
}

void ConsoleWorkerWireTest::roundTripsNormalizedInput()
{
    const Input sent{Input::Type::Key, QEvent::KeyPress, QPointF(10, 20), Qt::LeftButton, Qt::LeftButton, QPoint(0, 120), 38, 0, QStringLiteral("a")};
    Deframer deframer;
    deframer.feed(frame(sent));
    const auto record = deframer.next();
    QVERIFY(record);
    QCOMPARE(input(*record), std::optional<Input>(sent));
}

void ConsoleWorkerWireTest::roundTripsMediaAndPcm()
{
    Deframer deframer;
    const Media policy{true, true};
    const Audio samples{QByteArray(3528, '\x01')}; // 20 ms of 44.1 kHz stereo S16
    deframer.feed(frame(policy) + frame(samples));
    const auto mediaRecord = deframer.next();
    QVERIFY(mediaRecord);
    QCOMPARE(media(*mediaRecord), std::optional<Media>(policy));
    const auto audioRecord = deframer.next();
    QVERIFY(audioRecord);
    QCOMPARE(audio(*audioRecord), std::optional<Audio>(samples));
}

void ConsoleWorkerWireTest::rejectsOversizedRecord()
{
    QByteArray oversized(4, Qt::Uninitialized);
    QDataStream stream(&oversized, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(MaxRecordBytes + 1);
    Deframer deframer;
    deframer.feed(oversized);
    QVERIFY(!deframer.next());
    QVERIFY(deframer.overflowed());
}

void ConsoleWorkerWireTest::roundTripsOutputs()
{
    const Outputs sent{{{QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), 1, true},
                        {QStringLiteral("HDMI-A-1"), QRect(1920, 0, 1280, 720), 1.5, false}}};
    Deframer deframer;
    deframer.feed(frame(sent));
    const auto record = deframer.next();
    QVERIFY(record);
    QCOMPARE(outputs(*record), std::optional<Outputs>(sent));
    Record truncated = *record;
    truncated.payload.chop(1);
    QVERIFY(!outputs(truncated));
}

void ConsoleWorkerWireTest::rejectsInvalidOutputs()
{
    const auto rejected = [](const Outputs &value) {
        Deframer deframer;
        deframer.feed(frame(value));
        const auto record = deframer.next();
        return record && !outputs(*record);
    };
    QVERIFY(rejected({}));
    Outputs value{{{QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), 1, true}}};
    value.monitors.append(value.monitors.first());
    QVERIFY(rejected(value));
    value.monitors.removeLast();
    value.monitors[0].scale = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(rejected(value));
    value.monitors[0].scale = 1;
    value.monitors[0].geometry.setWidth(0);
    QVERIFY(rejected(value));
}

void ConsoleWorkerWireTest::roundTripsControlGeneration()
{
    const ControlState sent{42, true};
    Deframer deframer;
    deframer.feed(frame(sent) + frame(sent, Kind::LocalTakeover));
    const auto control = deframer.next();
    QVERIFY(control);
    QCOMPARE(controlState(*control), std::optional<ControlState>(sent));
    QVERIFY(!controlState(*control, Kind::LocalTakeover));
    const auto takeover = deframer.next();
    QVERIFY(takeover);
    QCOMPARE(controlState(*takeover, Kind::LocalTakeover), std::optional<ControlState>(sent));
    Record truncated = *takeover;
    truncated.payload.chop(1);
    QVERIFY(!controlState(truncated, Kind::LocalTakeover));
}

QTEST_GUILESS_MAIN(ConsoleWorkerWireTest)

#include "ConsoleWorkerWireTest.moc"
