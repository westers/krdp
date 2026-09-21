// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QMouseEvent>
#include <QTest>

#include "ConsoleWorkerSession.h"

using namespace KRdp;

class ConsoleWorkerSessionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void gatesFramesAndInputUntilWorkerReady();
};

void ConsoleWorkerSessionTest::gatesFramesAndInputUntilWorkerReady()
{
    QVector<ConsoleWorkerWire::Input> inputs;
    ConsoleWorkerSession session([&inputs](const auto &input) { inputs.append(input); });
    int frames = 0;
    connect(&session, &AbstractSession::frameReceived, this, [&frames](const VideoFrame &) { ++frames; });

    VideoFrame frame;
    frame.size = QSize(1280, 720);
    frame.data = "frame";
    session.submitFrame(frame);
    QCOMPARE(frames, 0);

    auto mouse = std::make_shared<QMouseEvent>(QEvent::MouseMove, QPointF(11, 12), QPointF{}, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    session.sendEvent(mouse);
    QVERIFY(inputs.isEmpty());

    session.setWorkerActive(true);
    QVERIFY(session.streamActive());
    session.submitFrame(frame);
    QCOMPARE(frames, 1);
    session.sendEvent(mouse);
    QCOMPARE(inputs.size(), 1);
    QCOMPARE(inputs.front().type, ConsoleWorkerWire::Input::Type::Mouse);
    QCOMPARE(inputs.front().position, QPointF(11, 12));

    session.setWorkerActive(false);
    QVERIFY(!session.streamActive());
    session.submitFrame(frame);
    QCOMPARE(frames, 1);
}

QTEST_GUILESS_MAIN(ConsoleWorkerSessionTest)

#include "ConsoleWorkerSessionTest.moc"
