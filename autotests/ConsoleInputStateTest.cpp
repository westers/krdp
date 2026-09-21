// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "ConsoleInputState.h"

class ConsoleInputStateTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void deduplicatesRepeatsAndClearsAfterRelease()
    {
        KRdp::ConsoleInputState state;
        KRdp::ConsoleWorkerWire::Input key;
        key.type = decltype(key.type)::Key;
        key.eventType = QEvent::KeyPress;
        key.nativeScanCode = 38;
        key.nativeVirtualKey = 'A';
        key.text = QStringLiteral("A");
        state.record(key);
        state.record(key);
        const auto releases = state.releaseAll();
        QCOMPARE(releases.size(), 1);
        QCOMPARE(releases.first().eventType, QEvent::KeyRelease);
        QCOMPARE(releases.first().nativeScanCode, quint32(38));
        QVERIFY(releases.first().text.isEmpty());
        QVERIFY(state.releaseAll().isEmpty());

        state.record(key);
        key.eventType = QEvent::KeyRelease;
        key.nativeVirtualKey = 'a';
        state.record(key);
        QVERIFY(state.releaseAll().isEmpty());
    }

    void releasesDragAtLatestPosition()
    {
        KRdp::ConsoleInputState state;
        KRdp::ConsoleWorkerWire::Input mouse;
        mouse.type = decltype(mouse.type)::Mouse;
        mouse.eventType = QEvent::MouseButtonPress;
        mouse.button = Qt::LeftButton;
        state.record(mouse);
        mouse.button = Qt::RightButton;
        state.record(mouse);
        mouse.eventType = QEvent::MouseMove;
        mouse.button = Qt::NoButton;
        mouse.position = QPointF(120, 240);
        state.record(mouse);
        const auto releases = state.releaseAll();
        QCOMPARE(releases.size(), 2);
        QCOMPARE(releases[0].eventType, QEvent::MouseButtonRelease);
        QCOMPARE(releases[0].position, mouse.position);
        QCOMPARE(releases[0].button, Qt::LeftButton);
        QCOMPARE(releases[0].buttons, Qt::MouseButtons(Qt::RightButton));
        QCOMPARE(releases[1].button, Qt::RightButton);
        QCOMPARE(releases[1].buttons, Qt::MouseButtons());
        QVERIFY(state.releaseAll().isEmpty());
    }
};

QTEST_GUILESS_MAIN(ConsoleInputStateTest)
#include "ConsoleInputStateTest.moc"
