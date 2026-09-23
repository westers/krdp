// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QFile>
#include <QTest>

#include "RetainedMultiCapture.h"

using KRdp::RetainedMultiCapture;
using KRdp::VideoFrame;

namespace
{
QByteArray fixture(const QString &size)
{
    QFile file(QFINDTESTDATA(QStringLiteral("data/virtual-fit/%1.h264").arg(size)));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

VideoFrame packet(QSize pixels, QSize logical, const QByteArray &data, bool keyframe = true)
{
    VideoFrame frame;
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
};

QTEST_GUILESS_MAIN(RetainedMultiCaptureTest)
#include "RetainedMultiCaptureTest.moc"
