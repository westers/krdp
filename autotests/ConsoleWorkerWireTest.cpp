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
    void positionRecordsAreBoundedAndCorrelated();
    void positionBatchRecordsAreBoundedAndCorrelated();
    void managedFitRecordsAreBoundedAndCorrelated();
    void primaryRecordsAreBoundedAndCorrelated();
    void mixedRecordsAreBoundedAndCorrelated();
    void mixedCreateRecordsAreBoundedAndCorrelated();
    void physicalLayoutRecordsRequireConsentAndCompleteBefore();
    void addVirtualRecordsAreBoundedAndCorrelated();
    void removeVirtualRecordsRequireOwnedName();
    void readOnlyTopologyRecordIsBounded();
};

void ConsoleWorkerWireTest::managedFitRecordsAreBoundedAndCorrelated()
{
    Deframer reader;
    const ManagedFit request{17, 9, QStringLiteral("Virtual-0"), QSize(1600, 900), 1.25,
        {{QStringLiteral("Virtual-0"), QStringLiteral("Virtual-1"), 1, 100},
         {QStringLiteral("Virtual-1"), QStringLiteral("Virtual-2"), 3, 0}}};
    const ManagedFitResult answer{17, 9, QStringLiteral("capture failed")};
    reader.feed(frame(request) + frame(answer));
    const auto first = reader.next();
    QVERIFY(first);
    QCOMPARE(managedFit(*first), std::optional<ManagedFit>(request));
    auto truncated = *first;
    truncated.payload.chop(1);
    QVERIFY(!managedFit(truncated));
    const auto second = reader.next();
    QVERIFY(second);
    QCOMPARE(managedFitResult(*second), std::optional<ManagedFitResult>(answer));
    for (const auto &invalid : {ManagedFit{0, 9, request.output, request.pixels, request.scale, request.relations},
                                ManagedFit{17, 9, request.output, QSize(1601, 900), request.scale, request.relations},
                                ManagedFit{17, 9, request.output, request.pixels, 0.5, request.relations},
                                ManagedFit{17, 9, request.output, request.pixels, request.scale,
                                    {{QStringLiteral("Virtual-0"), QStringLiteral("Virtual-1"), 4, 0}}},
                                ManagedFit{17, 9, request.output, request.pixels, request.scale,
                                    {{QStringLiteral("Virtual-0"), QStringLiteral("Virtual-1"), 1, 0},
                                     {QStringLiteral("Virtual-2"), QStringLiteral("Virtual-1"), 1, 0}}}}) {
        reader.feed(frame(invalid));
        const auto bad = reader.next();
        QVERIFY(bad);
        QVERIFY(!managedFit(*bad));
    }
}

void ConsoleWorkerWireTest::primaryRecordsAreBoundedAndCorrelated()
{
    Deframer reader;
    const Primary request{19, 9, QStringLiteral("Virtual-1")};
    const PrimaryResult answer{19, 9, QStringLiteral("readback failed")};
    reader.feed(frame(request) + frame(answer));
    const auto first = reader.next();
    QVERIFY(first);
    QCOMPARE(primary(*first), std::optional<Primary>(request));
    auto truncated = *first;
    truncated.payload.chop(1);
    QVERIFY(!primary(truncated));
    const auto second = reader.next();
    QVERIFY(second);
    QCOMPARE(primaryResult(*second), std::optional<PrimaryResult>(answer));
    for (const auto &bad : {Primary{0, 9, request.output}, Primary{19, 0, request.output},
                            Primary{19, 9, QStringLiteral("../Virtual-1")}}) {
        reader.feed(frame(bad));
        const auto record = reader.next();
        QVERIFY(record);
        QVERIFY(!primary(*record));
    }
}

void ConsoleWorkerWireTest::mixedRecordsAreBoundedAndCorrelated()
{
    Deframer reader;
    const Mixed request{21, 9, {
        {MixedOperation::Kind::Resize, QStringLiteral("Virtual-0"), {}, QSize(1600, 900), 1.25},
        {MixedOperation::Kind::Move, QStringLiteral("Virtual-1"), QPoint(1280, 100), {}, 1},
        {MixedOperation::Kind::Primary, QStringLiteral("Virtual-1"), {}, {}, 1}}};
    const MixedResult answer{21, 9, QStringLiteral("readback mismatch")};
    reader.feed(frame(request) + frame(answer));
    const auto first = reader.next();
    QVERIFY(first);
    QCOMPARE(mixed(*first), std::optional<Mixed>(request));
    auto truncated = *first;
    truncated.payload.chop(1);
    QVERIFY(!mixed(truncated));
    const auto second = reader.next();
    QVERIFY(second);
    QCOMPARE(mixedResult(*second), std::optional<MixedResult>(answer));
    for (const auto &bad : {Mixed{0, 9, request.operations}, Mixed{21, 0, request.operations},
                            Mixed{21, 9, {{MixedOperation::Kind::Primary, QStringLiteral("Virtual-1"), {}, {}, 1}}},
                            Mixed{21, 9, {{MixedOperation::Kind::Move, QStringLiteral("../bad"), QPoint(0, 0), {}, 1},
                                request.operations[1]}},
                            Mixed{21, 9, {{MixedOperation::Kind::Resize, QStringLiteral("Virtual-0"), {}, QSize(1601, 900), 1.25},
                                request.operations[1]}}}) {
        reader.feed(frame(bad));
        const auto record = reader.next();
        QVERIFY(record);
        QVERIFY(!mixed(*record));
    }
}

void ConsoleWorkerWireTest::physicalLayoutRecordsRequireConsentAndCompleteBefore()
{
    Deframer reader;
    const PhysicalLayout request{31, 9, QStringLiteral("console-generation-1"), 4, true,
        {{QStringLiteral("DP-1"), QSize(1600, 900), QRect(0, 0, 1280, 720), 1.25, true, 1},
         {QStringLiteral("HDMI-A-1"), QSize(1280, 720), QRect(1280, 100, 1280, 720), 1, false, 2}},
        {{MixedOperation::Kind::Move, QStringLiteral("HDMI-A-1"), QPoint(-1280, 100), {}, 1},
         {MixedOperation::Kind::Resize, QStringLiteral("DP-1"), {}, QSize(1920, 1080), 1}}};
    const PhysicalLayoutResult answer{31, 9, QStringLiteral("capture failed")};
    reader.feed(frame(request) + frame(answer));
    const auto first = reader.next();
    QVERIFY(first);
    QCOMPARE(physicalLayout(*first), std::optional<PhysicalLayout>(request));
    auto truncated = *first;
    truncated.payload.chop(1);
    QVERIFY(!physicalLayout(truncated));
    const auto second = reader.next();
    QVERIFY(second);
    QCOMPARE(physicalLayoutResult(*second), std::optional<PhysicalLayoutResult>(answer));
    const PhysicalLeaseReleased released{9, true};
    reader.feed(frame(released));
    const auto releaseRecord = reader.next();
    QVERIFY(releaseRecord);
    QCOMPARE(physicalLeaseReleased(*releaseRecord), std::optional<PhysicalLeaseReleased>(released));
    auto invalidRelease = *releaseRecord;
    invalidRelease.payload.chop(1);
    QVERIFY(!physicalLeaseReleased(invalidRelease));
    reader.feed(frame(PhysicalLeaseReleased{0, false}));
    QVERIFY(!physicalLeaseReleased(*reader.next()));

    auto bad = request;
    bad.allowPhysicalChange = false;
    reader.feed(frame(bad));
    QVERIFY(!physicalLayout(*reader.next()));
    bad = request;
    bad.before[1].priority = 1;
    reader.feed(frame(bad));
    QVERIFY(!physicalLayout(*reader.next()));
    bad = request;
    bad.before[1].name = QStringLiteral("Virtual-1");
    reader.feed(frame(bad));
    QVERIFY(!physicalLayout(*reader.next()));
    bad = request;
    bad.before[0].logical.setWidth(1600); // Native pixels cannot masquerade as scaled logical size.
    reader.feed(frame(bad));
    QVERIFY(!physicalLayout(*reader.next()));
    bad = request;
    bad.operations[0].output = QStringLiteral("unknown");
    reader.feed(frame(bad));
    QVERIFY(!physicalLayout(*reader.next()));
    bad = request;
    bad.operations.append(bad.operations.first());
    reader.feed(frame(bad));
    QVERIFY(!physicalLayout(*reader.next()));
    bad = request;
    bad.operations[0].globalLogical.setX(-32769);
    reader.feed(frame(bad));
    QVERIFY(!physicalLayout(*reader.next()));
}

void ConsoleWorkerWireTest::mixedCreateRecordsAreBoundedAndCorrelated()
{
    Deframer reader;
    const MixedCreate request{25, 9, QStringLiteral("Virtual-krdp-added-test"), QSize(960, 540), 1,
        QPoint(2560, 100), {{MixedOperation::Kind::Resize, QStringLiteral("Virtual-0"), {}, QSize(1600, 900), 1.25},
            {MixedOperation::Kind::Primary, QStringLiteral("Virtual-krdp-added-test"), {}, {}, 1}}};
    const MixedCreateResult answer{25, 9, QStringLiteral("capture mismatch")};
    reader.feed(frame(request) + frame(answer));
    const auto first = reader.next();
    QVERIFY(first);
    QCOMPARE(mixedCreate(*first), std::optional<MixedCreate>(request));
    auto truncated = *first;
    truncated.payload.chop(1);
    QVERIFY(!mixedCreate(truncated));
    const auto second = reader.next();
    QVERIFY(second);
    QCOMPARE(mixedCreateResult(*second), std::optional<MixedCreateResult>(answer));
    auto bad = request;
    bad.newOutput = QStringLiteral("../Virtual-krdp-added-test");
    reader.feed(frame(bad));
    QVERIFY(!mixedCreate(*reader.next()));
    bad = request;
    bad.changes[0].pixels = QSize(1601, 900);
    reader.feed(frame(bad));
    QVERIFY(!mixedCreate(*reader.next()));
    bad = request;
    bad.changes.clear();
    reader.feed(frame(bad));
    QVERIFY(!mixedCreate(*reader.next()));
}

void ConsoleWorkerWireTest::readOnlyTopologyRecordIsBounded()
{
    const Topology expected{{{QStringLiteral("DP-1"), QSize(2560, 1440), QRect(0, 0, 2560, 1440), 1.0, true, 1},
        {QStringLiteral("HDMI-A-1"), QSize(1600, 900), QRect(2560, 100, 1280, 720), 1.25, false, 3}}};
    Deframer reader;
    reader.feed(frame(expected) + frame(Topology{}));
    auto record = reader.next();
    QVERIFY(record);
    QCOMPARE(topology(*record), std::optional<Topology>(expected));
    auto truncated = *record;
    truncated.payload.chop(1);
    QVERIFY(!topology(truncated));
    auto duplicate = expected;
    duplicate.outputs[1].name = duplicate.outputs[0].name;
    reader.feed(frame(duplicate));
    record = reader.next();
    QVERIFY(record);
    QVERIFY(topology(*record));
    QCOMPARE(topology(*record)->outputs.size(), 0); // Explicit invalidation.
    record = reader.next();
    QVERIFY(record);
    QVERIFY(!topology(*record));
    auto invalidPriority = expected;
    invalidPriority.outputs[1].priority = 1;
    reader.feed(frame(invalidPriority));
    QVERIFY(!topology(*reader.next()));
    invalidPriority.outputs[1].priority = 0;
    reader.feed(frame(invalidPriority));
    QVERIFY(!topology(*reader.next()));
    invalidPriority.outputs[1].priority = 2;
    invalidPriority.outputs[1].primary = true;
    reader.feed(frame(invalidPriority));
    QVERIFY(!topology(*reader.next()));
    reader.feed(frame(Kind::TopologyQuery));
    record = reader.next();
    QVERIFY(record);
    QCOMPARE(record->kind, Kind::TopologyQuery);
    QVERIFY(record->payload.isEmpty());
    auto owned = expected;
    owned.outputs[1].physical = false;
    reader.feed(frame(owned));
    record = reader.next();
    QVERIFY(record);
    QCOMPARE(topology(*record), std::optional<Topology>(owned));
}

void ConsoleWorkerWireTest::removeVirtualRecordsRequireOwnedName()
{
    Deframer reader;
    const RemoveVirtual request{11, 9, QStringLiteral("Virtual-krdp-added-abcdef")};
    const RemoveVirtualResult answer{11, 9, QStringLiteral("capture failed")};
    reader.feed(frame(request) + frame(answer));
    const auto first = reader.next();
    QVERIFY(first);
    QCOMPARE(removeVirtual(*first), std::optional<RemoveVirtual>(request));
    auto truncated = *first;
    truncated.payload.chop(1);
    QVERIFY(!removeVirtual(truncated));
    const auto second = reader.next();
    QVERIFY(second);
    QCOMPARE(removeVirtualResult(*second), std::optional<RemoveVirtualResult>(answer));
    for (const auto &bad : {RemoveVirtual{0, 9, request.output}, RemoveVirtual{11, 0, request.output},
                            RemoveVirtual{11, 9, QStringLiteral("Virtual-0")},
                            RemoveVirtual{11, 9, QStringLiteral("Virtual-krdp-added-../bad")}}) {
        reader.feed(frame(bad));
        const auto record = reader.next();
        QVERIFY(record);
        QVERIFY(!removeVirtual(*record));
    }
}

void ConsoleWorkerWireTest::addVirtualRecordsAreBoundedAndCorrelated()
{
    Deframer reader;
    const AddVirtual request{7, 9, QStringLiteral("Virtual-krdp-new"), QSize(960, 540), 1.25, QPoint(2560, 100)};
    const AddVirtualResult answer{7, 9, QStringLiteral("capture failed")};
    reader.feed(frame(request) + frame(answer));
    const auto first = reader.next();
    QVERIFY(first);
    QCOMPARE(addVirtual(*first), std::optional<AddVirtual>(request));
    auto truncated = *first;
    truncated.payload.chop(1);
    QVERIFY(!addVirtual(truncated));
    const auto second = reader.next();
    QVERIFY(second);
    QCOMPARE(addVirtualResult(*second), std::optional<AddVirtualResult>(answer));
    for (const auto &invalid : {AddVirtual{0, 9, request.output, request.pixels, 1, request.globalLogical},
                                AddVirtual{7, 9, QStringLiteral("DP-1"), request.pixels, 1, request.globalLogical},
                                AddVirtual{7, 9, QStringLiteral("Virtual-../bad"), request.pixels, 1, request.globalLogical},
                                AddVirtual{7, 9, request.output, QSize(8192, 540), 1, request.globalLogical},
                                AddVirtual{7, 9, request.output, request.pixels, 0.5, request.globalLogical},
                                AddVirtual{7, 9, request.output, request.pixels, 1, QPoint(-32769, 0)}}) {
        reader.feed(frame(invalid));
        const auto bad = reader.next();
        QVERIFY(bad);
        QVERIFY(!addVirtual(*bad));
    }
}

void ConsoleWorkerWireTest::positionRecordsAreBoundedAndCorrelated()
{
    Deframer reader;
    const Position request{7, 9, QStringLiteral("Virtual-1"), QPoint(1280, 100)};
    const PositionResult answer{7, 9, QStringLiteral("capture failed")};
    reader.feed(frame(request) + frame(answer));
    const auto first = reader.next();
    QVERIFY(first);
    QCOMPARE(position(*first), std::optional<Position>(request));
    auto truncated = *first;
    truncated.payload.chop(1);
    QVERIFY(!position(truncated));
    const auto second = reader.next();
    QVERIFY(second);
    QCOMPARE(positionResult(*second), std::optional<PositionResult>(answer));
    Position invalid = request;
    invalid.globalLogical.setX(-32769);
    reader.feed(frame(invalid));
    const auto bad = reader.next();
    QVERIFY(bad);
    QVERIFY(!position(*bad));
}

void ConsoleWorkerWireTest::positionBatchRecordsAreBoundedAndCorrelated()
{
    Deframer reader;
    const PositionBatch request{7, 9, {{QStringLiteral("Virtual-0"), QPoint(1280, 0)},
        {QStringLiteral("Virtual-1"), QPoint(0, 0)}}};
    const PositionBatchResult answer{7, 9, QStringLiteral("capture failed")};
    reader.feed(frame(request) + frame(answer));
    const auto first = reader.next();
    QVERIFY(first);
    QCOMPARE(positionBatch(*first), std::optional<PositionBatch>(request));
    auto truncated = *first;
    truncated.payload.chop(1);
    QVERIFY(!positionBatch(truncated));
    const auto second = reader.next();
    QVERIFY(second);
    QCOMPARE(positionBatchResult(*second), std::optional<PositionBatchResult>(answer));
    auto duplicate = request;
    duplicate.targets[1].output = duplicate.targets[0].output;
    reader.feed(frame(duplicate));
    const auto bad = reader.next();
    QVERIFY(bad);
    QVERIFY(!positionBatch(*bad));
    auto invalid = request;
    invalid.targets[0].globalLogical.setX(-32769);
    reader.feed(frame(invalid));
    const auto outOfBounds = reader.next();
    QVERIFY(outOfBounds);
    QVERIFY(!positionBatch(*outOfBounds));
    invalid = request;
    invalid.targets.removeLast();
    reader.feed(frame(invalid));
    const auto tooFew = reader.next();
    QVERIFY(tooFew);
    QVERIFY(!positionBatch(*tooFew));
}

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
                        {QStringLiteral("HDMI-A-1"), QRect(1920, 0, 1280, 720), 1.5, false}}, QPoint(-1920, -100)};
    Deframer deframer;
    deframer.feed(frame(sent));
    const auto record = deframer.next();
    QVERIFY(record);
    QCOMPARE(outputs(*record), std::optional<Outputs>(sent));
    Record truncated = *record;
    truncated.payload.chop(1);
    QVERIFY(!outputs(truncated));
    auto invalidOrigin = sent;
    invalidOrigin.compositorOrigin = QPoint(-32769, 0);
    deframer.feed(frame(invalidOrigin));
    const auto bad = deframer.next();
    QVERIFY(bad);
    QVERIFY(!outputs(*bad));
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
