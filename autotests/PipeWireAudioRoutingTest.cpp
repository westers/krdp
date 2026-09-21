// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "PipeWireAudioRouting.h"

using namespace KRdp::PipeWireAudioRouting;

class PipeWireAudioRoutingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void selectsOnlyMovablePlaybackStreams();
    void rejectsMalformedDump();
};

void PipeWireAudioRoutingTest::selectsOnlyMovablePlaybackStreams()
{
    const QByteArray dump = R"([{"id":10,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Stream/Output/Audio"}}},{"id":11,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Stream/Output/Audio","node.dont-move":"true"}}},{"id":12,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Stream/Input/Audio"}}},{"id":13,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Audio/Sink"}}},{"id":14,"type":"PipeWire:Interface:Device","info":{"props":{"media.class":"Stream/Output/Audio"}}},{"id":15,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Stream/Output/Audio"}}}])";
    QCOMPARE(movablePlaybackStreams(dump), (QVector<PlaybackStream>{{10}, {15}}));
}

void PipeWireAudioRoutingTest::rejectsMalformedDump()
{
    QVERIFY(movablePlaybackStreams("not JSON").isEmpty());
    QVERIFY(movablePlaybackStreams("{}").isEmpty());
}

QTEST_GUILESS_MAIN(PipeWireAudioRoutingTest)

#include "PipeWireAudioRoutingTest.moc"
