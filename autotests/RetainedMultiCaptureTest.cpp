// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QFile>
#include <QTest>

#include "RetainedMultiCapture.h"
#include "ConsoleTopologyReadback.h"

using KRdp::RetainedMultiCapture;
using KRdp::VideoFrame;

namespace
{
QByteArray fixture(const QString &size, const QString &extension = QStringLiteral("h264"))
{
    QFile file(QFINDTESTDATA(QStringLiteral("data/virtual-fit/%1.%2").arg(size, extension)));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

VideoFrame packet(QSize pixels, QSize logical, const QByteArray &data, bool keyframe = true,
                  std::optional<KRdp::VideoCodec> codec = std::nullopt)
{
    VideoFrame frame;
    frame.codec = codec;
    frame.size = pixels;
    frame.data = data;
    frame.isKeyFrame = keyframe;
    frame.monitors = {{QRect(QPoint(0, 0), logical), true}};
    return frame;
}

QVector<RetainedMultiCapture::Screen> screens()
{
    return {{QStringLiteral("Virtual-left"), QRect(-1280, 0, 1280, 720), false},
        {QStringLiteral("Virtual-right"), QRect(0, 100, 1280, 720), true}};
}
}

class RetainedMultiCaptureTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void waitsForEveryIndependentlyDecodedOutput()
    {
        RetainedMultiCapture set;
        QVERIFY(set.configure(screens()));
        const auto left = packet(QSize(1920, 1080), QSize(1280, 720), fixture(QStringLiteral("1920x1080")));
        const auto right = packet(QSize(1280, 720), QSize(1280, 720), fixture(QStringLiteral("1280x720")));
        QVERIFY(!left.data.isEmpty()); QVERIFY(!right.data.isEmpty());
        QVERIFY(set.submit(0, left).frames.isEmpty());
        QVERIFY(!set.ready());
        const auto ready = set.submit(1, right);
        QVERIFY(ready.becameReady); QVERIFY(set.ready());
        QCOMPARE(ready.outputs.monitors.size(), 2);
        QCOMPARE(ready.outputs.compositorOrigin, QPoint(-1280, 0));
        QCOMPARE(ready.outputs.monitors[0].geometry, QRect(0, 0, 1280, 720));
        QCOMPARE(ready.outputs.monitors[1].geometry, QRect(1280, 100, 1280, 720));
        QCOMPARE(ready.outputs.monitors[0].scale, 1.5);
        QCOMPARE(ready.frames.size(), 2);
        QCOMPARE(ready.frames[0].monitorIndex, 0);
        QCOMPARE(ready.frames[1].monitorIndex, 1);
        QCOMPARE(ready.atlas[0].geometry.size(), QSize(1920, 1080));
        QCOMPARE(ready.atlas[1].geometry.size(), QSize(1280, 720));
        QVERIFY(!ready.atlas[0].geometry.intersects(ready.atlas[1].geometry));
        const QVector<KRdp::RemoteMonitorGeometry::Output> logical{
            {QPoint(0, 0), left.size, 1.5, false}, {QPoint(1280, 100), right.size, 1.0, true}};
        const auto pointer = KRdp::RemoteMonitorGeometry::wireToLogical(QPointF(ready.atlas[1].geometry.topLeft()) + QPointF(100, 100), ready.atlas, logical);
        QCOMPARE(pointer, QPointF(1380, 200));
        // The wire atlas is normalized, but KWin fake input needs the
        // compositor-global point, including a negative workspace origin.
        QRect workspace;
        for (const auto &screen : screens()) workspace |= screen.logicalGeometry;
        QCOMPARE(pointer + QPointF(workspace.topLeft()), QPointF(100, 200));
        const auto next = set.submit(1, packet(QSize(1280, 720), QSize(1280, 720), QByteArrayLiteral("p-frame"), false));
        QCOMPARE(next.frames.size(), 1);
        QCOMPARE(next.frames.first().monitorIndex, 1);
    }

    void provesLayoutWithHevcAndAv1Keyframes_data()
    {
        QTest::addColumn<int>("codec");
        QTest::addColumn<QString>("extension");
        QTest::newRow("hevc") << int(KRdp::VideoCodec::Hevc) << QStringLiteral("hevc");
        QTest::newRow("av1") << int(KRdp::VideoCodec::Av1) << QStringLiteral("av1");
    }

    // AUD-FIX9 R1: after a codec change at attach, the refreshed per-output captures deliver
    // HEVC/AV1 keyframes. 7a4a01b only accepted H.264 here, so the layout never became ready
    // again (no "Retained KScreen readback confirmed", no frames) on ace (HEVC) and cray (AV1).
    void provesLayoutWithHevcAndAv1Keyframes()
    {
        QFETCH(int, codec);
        QFETCH(QString, extension);
        const auto data = fixture(QStringLiteral("1280x720"), extension);
        QVERIFY(!data.isEmpty());
        const auto kind = KRdp::VideoCodec(codec);
        RetainedMultiCapture set;
        QVERIFY(set.configure({{QStringLiteral("Virtual-0"), QRect(0, 0, 1280, 720), true},
            {QStringLiteral("Virtual-1"), QRect(1280, 0, 1280, 720), false}}));
        QVERIFY(!set.submit(0, packet(QSize(1280, 720), QSize(1280, 720), data, true, kind)).becameReady);
        const auto ready = set.submit(1, packet(QSize(1280, 720), QSize(1280, 720), data, true, kind));
        QVERIFY(ready.becameReady);
        QCOMPARE(ready.frames.size(), 2);
        for (const auto &frame : ready.frames) QCOMPARE(frame.codec, std::optional(kind));
        // The same bytes labelled as H.264 (or unlabelled) are no proof.
        RetainedMultiCapture mislabelled;
        QVERIFY(mislabelled.configure({{QStringLiteral("Virtual-0"), QRect(0, 0, 1280, 720), true},
            {QStringLiteral("Virtual-1"), QRect(1280, 0, 1280, 720), false}}));
        QVERIFY(mislabelled.submit(0, packet(QSize(1280, 720), QSize(1280, 720), data)).frames.isEmpty());
        QVERIFY(mislabelled.submit(1, packet(QSize(1280, 720), QSize(1280, 720), data, true, KRdp::VideoCodec::Avc420)).frames.isEmpty());
        QVERIFY(!mislabelled.ready());
        // Headers alone are no proof either.
        QVERIFY(mislabelled.submit(0, packet(QSize(1280, 720), QSize(1280, 720), data.left(20), true, kind)).frames.isEmpty());
        QVERIFY(mislabelled.submit(1, packet(QSize(1280, 720), QSize(1280, 720), data.left(20), true, kind)).frames.isEmpty());
        QVERIFY(!mislabelled.ready());
    }

    // AUD-FIX10 R5: two 1920x1080 outputs in AMD hardware AV1 (coded 1920x1082, no render size,
    // Hal's 780M and cray's Strix Halo) or HEVC (1088 with a conformance window) prove the layout.
    // 299cd25 compared the AV1 frame size with the output and never became ready (0 frames on cray).
    void provesA1080pLayoutWithAmdPaddedKeyframes_data()
    {
        QTest::addColumn<int>("codec");
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("extension");
        QTest::newRow("av1 hal") << int(KRdp::VideoCodec::Av1) << QStringLiteral("1920x1080-hal") << QStringLiteral("av1");
        QTest::newRow("av1 cray") << int(KRdp::VideoCodec::Av1) << QStringLiteral("1920x1080-cray") << QStringLiteral("av1");
        QTest::newRow("hevc hal") << int(KRdp::VideoCodec::Hevc) << QStringLiteral("1920x1080-hal") << QStringLiteral("hevc");
        QTest::newRow("hevc cray") << int(KRdp::VideoCodec::Hevc) << QStringLiteral("1920x1080-cray") << QStringLiteral("hevc");
    }
    void provesA1080pLayoutWithAmdPaddedKeyframes()
    {
        QFETCH(int, codec);
        QFETCH(QString, name);
        QFETCH(QString, extension);
        const auto data = fixture(name, extension);
        QVERIFY(!data.isEmpty());
        const auto kind = KRdp::VideoCodec(codec);
        const QSize size(1920, 1080);
        RetainedMultiCapture set;
        QVERIFY(set.configure({{QStringLiteral("Virtual-0"), QRect(QPoint(0, 0), size), true},
            {QStringLiteral("Virtual-1"), QRect(QPoint(1920, 0), size), false}}));
        QVERIFY(!set.submit(0, packet(size, size, data, true, kind)).becameReady);
        const auto ready = set.submit(1, packet(size, size, data, true, kind));
        QVERIFY(ready.becameReady);
        QCOMPARE(ready.frames.size(), 2);
        QCOMPARE(ready.atlas[0].geometry.size(), size);
        // The same keyframe is no proof of an output it cannot show.
        RetainedMultiCapture other;
        QVERIFY(other.configure({{QStringLiteral("Virtual-0"), QRect(0, 0, 1920, 1200), true},
            {QStringLiteral("Virtual-1"), QRect(1920, 0, 1920, 1200), false}}));
        QVERIFY(other.submit(0, packet(QSize(1920, 1200), QSize(1920, 1200), data, true, kind)).frames.isEmpty());
        QVERIFY(other.submit(1, packet(QSize(1920, 1200), QSize(1920, 1200), data, true, kind)).frames.isEmpty());
        QVERIFY(!other.ready());
    }

    // AUD-FIX9 R1, the attach sequence as the capture sees it: a published AVC layout, the new
    // grant's refresh (invalidate), one output's AVC keyframe, then the codec change reaches the
    // encoders. The layout is proven again entirely in the new codec, never with mixed codecs.
    void codecChangeWhileProvingNeedsEveryOutputInTheNewCodec()
    {
        const auto avc = fixture(QStringLiteral("1280x720"));
        const auto hevc = fixture(QStringLiteral("1280x720"), QStringLiteral("hevc"));
        QVERIFY(!avc.isEmpty()); QVERIFY(!hevc.isEmpty());
        const QSize size(1280, 720);
        RetainedMultiCapture set;
        QVERIFY(set.configure({{QStringLiteral("Virtual-0"), QRect(0, 0, 1280, 720), true},
            {QStringLiteral("Virtual-1"), QRect(1280, 0, 1280, 720), false}}));
        QVERIFY(!set.submit(0, packet(size, size, avc, true, KRdp::VideoCodec::Avc420)).becameReady);
        QVERIFY(set.submit(1, packet(size, size, avc, true, KRdp::VideoCodec::Avc420)).becameReady);

        set.invalidate(); // Refreshing per-output captures for a new client
        QVERIFY(!set.ready());
        QVERIFY(set.submit(0, packet(size, size, avc, true, KRdp::VideoCodec::Avc420)).frames.isEmpty());
        // The encoders switch to HEVC: output 1 proves itself in HEVC; output 0's AVC proof is void.
        QVERIFY(set.submit(1, packet(size, size, hevc, true, KRdp::VideoCodec::Hevc)).frames.isEmpty());
        QVERIFY(!set.ready());
        const auto ready = set.submit(0, packet(size, size, hevc, true, KRdp::VideoCodec::Hevc));
        QVERIFY(ready.becameReady);
        QCOMPARE(ready.frames.size(), 2);
        for (const auto &frame : ready.frames) QCOMPARE(frame.codec, std::optional(KRdp::VideoCodec::Hevc));

        // A later codec change on the published layout (the encoder swap on the same capture
        // streams) keeps it published: the new codec's frames flow without a new layout.
        const auto av1 = fixture(QStringLiteral("1280x720"), QStringLiteral("av1"));
        QVERIFY(!av1.isEmpty());
        const auto next = set.submit(0, packet(size, size, av1, true, KRdp::VideoCodec::Av1));
        QVERIFY(!next.reset); QVERIFY(!next.becameReady);
        QCOMPARE(next.frames.size(), 1);
        QCOMPARE(next.frames.first().codec, std::optional(KRdp::VideoCodec::Av1));
        QVERIFY(set.ready());
    }

    void refusesMetadataOnlyProofAndResetsOnSizeChange()
    {
        RetainedMultiCapture set;
        QVERIFY(set.configure(screens()));
        const auto h264left = fixture(QStringLiteral("1920x1080"));
        const auto h264right = fixture(QStringLiteral("1280x720"));
        QVERIFY(set.submit(0, packet(QSize(1280, 720), QSize(1280, 720), h264left)).frames.isEmpty());
        QVERIFY(set.submit(1, packet(QSize(1280, 720), QSize(1280, 720), h264right)).frames.isEmpty());
        QVERIFY(!set.ready()); // Wrong left payload dimensions cannot complete readiness.
        QVERIFY(set.submit(0, packet(QSize(1920, 1080), QSize(1280, 720), h264left)).becameReady);
        QVERIFY(set.ready());
        const auto changed = set.submit(0, packet(QSize(1280, 720), QSize(1280, 720), h264right));
        QVERIFY(changed.reset);
        QVERIFY(!set.ready());
        QVERIFY(changed.frames.isEmpty());
        QVERIFY(set.submit(1, packet(QSize(1280, 720), QSize(1280, 720), h264right)).becameReady);
        QCOMPARE(set.atlas()[0].geometry.size(), QSize(1280, 720));
    }

    void scaleOnlyRestartRequiresFreshKeyframesFromEveryOutput()
    {
        RetainedMultiCapture set;
        const QVector<RetainedMultiCapture::Screen> inventory{
            {QStringLiteral("Virtual-0"), QRect(0, 0, 1280, 720), true},
            {QStringLiteral("Virtual-1"), QRect(1280, 0, 1280, 720), false},
        };
        QVERIFY(set.configure(inventory));
        const auto standard = fixture(QStringLiteral("1280x720"));
        const auto scaled = fixture(QStringLiteral("1920x1080"));
        QVERIFY(!standard.isEmpty());
        QVERIFY(!scaled.isEmpty());
        QVERIFY(set.submit(0, packet(QSize(1280, 720), QSize(1280, 720), standard)).frames.isEmpty());
        QVERIFY(set.submit(1, packet(QSize(1280, 720), QSize(1280, 720), standard)).becameReady);
        QCOMPARE(set.outputs().monitors[1].scale, 1.0);
        QCOMPARE(set.atlas()[1].geometry, QRect(1280, 0, 1280, 720));

        // KScreen has changed only the native mode and scale of Virtual-1;
        // both QScreen logical rectangles still compare equal. The worker's
        // forced restart must invalidate the old verified pair explicitly.
        set.invalidate();
        QVERIFY(!set.ready());
        QCOMPARE(set.screens(), inventory);
        QVERIFY(set.submit(1, packet(QSize(1920, 1080), QSize(1280, 720), scaled)).frames.isEmpty());
        QVERIFY(!set.ready());
        QVERIFY(set.submit(0, packet(QSize(1280, 720), QSize(1280, 720), QByteArrayLiteral("old p-frame"), false)).frames.isEmpty());
        QVERIFY(!set.ready());
        const auto recovered = set.submit(0, packet(QSize(1280, 720), QSize(1280, 720), standard));
        QVERIFY(recovered.becameReady);
        QCOMPARE(recovered.frames.size(), 2);
        QCOMPARE(recovered.frames[0].size, QSize(1280, 720));
        QCOMPARE(recovered.frames[1].size, QSize(1920, 1080));
        QCOMPARE(recovered.outputs.monitors[0].geometry, QRect(0, 0, 1280, 720));
        QCOMPARE(recovered.outputs.monitors[1].geometry, QRect(1280, 0, 1280, 720));
        QCOMPARE(recovered.outputs.monitors[1].scale, 1.5);
        QCOMPARE(recovered.atlas[1].geometry, QRect(1280, 0, 1920, 1080));
    }

    void rejectsAmbiguousScreenInventory()
    {
        RetainedMultiCapture set;
        auto list = screens();
        list[1].name = list[0].name;
        QVERIFY(!set.configure(list));
        list = screens(); list[1].primary = false;
        QVERIFY(!set.configure(list));
        list = screens(); list[1].logicalGeometry = {};
        QVERIFY(!set.configure(list));
    }

    void invalidLiveMetadataRevokesReadyLayout()
    {
        RetainedMultiCapture set;
        QVERIFY(set.configure(screens()));
        QVERIFY(set.submit(0, packet(QSize(1920, 1080), QSize(1280, 720), fixture(QStringLiteral("1920x1080")))).frames.isEmpty());
        QVERIFY(set.submit(1, packet(QSize(1280, 720), QSize(1280, 720), fixture(QStringLiteral("1280x720")))).becameReady);
        auto wrong = packet(QSize(1280, 720), QSize(1920, 1080), QByteArrayLiteral("p-frame"), false);
        QVERIFY(set.submit(1, wrong).reset);
        QVERIFY(!set.ready());
        QVERIFY(set.submit(1, packet(QSize(1280, 720), QSize(1280, 720), fixture(QStringLiteral("1280x720")))).frames.isEmpty());
    }

    void physicalNamesAndMixedScalesReachConsoleProof()
    {
        RetainedMultiCapture set;
        QVERIFY(set.configure({
            {QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), true},
            {QStringLiteral("HDMI-A-1"), QRect(1280, 100, 1280, 720), false},
        }));
        const auto standard = fixture(QStringLiteral("1280x720"));
        const auto scaled = fixture(QStringLiteral("1920x1080"));
        QVERIFY(!set.submit(0, packet(QSize(1920, 1080), QSize(1280, 720), scaled)).becameReady);
        const auto ready = set.submit(1, packet(QSize(1280, 720), QSize(1280, 720), standard));
        QVERIFY(ready.becameReady);
        KRdp::RetainedKScreenReadback::Snapshot fresh;
        fresh.outputs = {
            {.backendKey = QStringLiteral("DP-1"), .name = QStringLiteral("DP-1"), .nativePixels = QSize(1920, 1080),
                .logicalGeometry = QRect(0, 0, 1280, 720), .scale = 1.5, .enabled = true, .primary = true,
                .physical = true, .owner = {}},
            {.backendKey = QStringLiteral("HDMI-A-1"), .name = QStringLiteral("HDMI-A-1"), .nativePixels = QSize(1280, 720),
                .logicalGeometry = QRect(1280, 100, 1280, 720), .scale = 1, .enabled = true, .primary = false,
                .physical = true, .owner = {}},
        };
        const auto confirmed = KRdp::ConsoleTopologyReadback::confirmedMulti(fresh, ready.outputs, ready.frames);
        QVERIFY(confirmed);
        QCOMPARE(confirmed->outputs[0].pixels, QSize(1920, 1080));
        QCOMPARE(confirmed->outputs[1].logical.topLeft(), QPoint(1280, 100));
        QCOMPARE(ready.atlas[0].geometry.size(), QSize(1920, 1080));
        QVERIFY(!ready.atlas[0].geometry.intersects(ready.atlas[1].geometry));
    }
};

QTEST_GUILESS_MAIN(RetainedMultiCaptureTest)
#include "RetainedMultiCaptureTest.moc"
