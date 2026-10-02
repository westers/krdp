// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "InputHandler.h"
#include "PeerContext_p.h"
#include "RelativePointerEvent.h"
#include "ConsoleInputState.h"
#include "RetainedMultiInput.h"
#include "PhysicalInputWatcher.h"
#include <limits>

class GameInputTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void standardRdpHeldKeysAndSignedRelativeEvents() {
        KRdp::InputHandler handler(nullptr);
        KRdp::PeerContext context{};
        context.inputHandler = &handler;
        rdpInput input{}; input.context = &context._p;
        handler.initialize(&input);
        QVector<std::shared_ptr<QInputEvent>> events;
        connect(&handler, &KRdp::InputHandler::inputEvent, this, [&](auto event) { events.append(event); });
        QVERIFY(input.RelMouseEvent);
        QVERIFY(input.KeyboardEvent(&input, KBD_FLAGS_DOWN, 0x11)); // W down.
        QCOMPARE(std::static_pointer_cast<QKeyEvent>(events.last())->nativeScanCode(), quint32(KEY_W));
        QCOMPARE(events.last()->type(), QEvent::KeyPress);
        QVERIFY(input.RelMouseEvent(&input, PTR_FLAGS_MOVE, -32000, 1234));
        auto relative = std::dynamic_pointer_cast<KRdp::RelativePointerEvent>(events.last());
        QVERIFY(relative); QCOMPARE(relative->delta, QPointF(-32000, 1234));
        QVERIFY(input.KeyboardEvent(&input, KBD_FLAGS_RELEASE, 0x11));
        QCOMPARE(events.last()->type(), QEvent::KeyRelease);
        QCOMPARE(std::static_pointer_cast<QKeyEvent>(events.last())->nativeScanCode(), quint32(KEY_W));
        QVERIFY(input.RelMouseEvent(&input, PTR_FLAGS_BUTTON1 | PTR_FLAGS_DOWN, 0, 0));
        relative = std::dynamic_pointer_cast<KRdp::RelativePointerEvent>(events.last());
        QCOMPARE(relative->action, QEvent::MouseButtonPress); QCOMPARE(relative->button, Qt::LeftButton);
        QVERIFY(input.RelMouseEvent(&input, PTR_FLAGS_WHEEL | 120, 0, 0));
        relative = std::dynamic_pointer_cast<KRdp::RelativePointerEvent>(events.last());
        QCOMPARE(relative->action, QEvent::Wheel); QCOMPARE(relative->angleDelta, QPoint(0, -120));
    }
    void relativeWireBypassesAbsoluteMappingAndCleanupNeverWarps() {
        using namespace KRdp; using namespace ConsoleWorkerWire;
        Input delta; delta.type = Input::Type::RelativePointer; delta.eventType = QEvent::MouseMove;
        delta.position = {-32000, 240};
        Deframer reader; reader.feed(frame(delta));
        auto decoded = input(*reader.next()); QVERIFY(decoded); QCOMPARE(*decoded, delta);
        // Even without resolved monitor geometry, relative input remains a delta.
        QCOMPARE(RetainedMultiInput::toCompositor(*decoded, {}, {}, {1000, 2000}), decoded);
        QVERIFY(!RetainedMultiInput::positionBeforeDispatch(delta));
        delta.position.setX(std::numeric_limits<double>::quiet_NaN());
        reader.feed(frame(delta)); QVERIFY(!input(*reader.next()));
        ConsoleInputState held;
        Input key; key.type = Input::Type::Key; key.eventType = QEvent::KeyPress; key.nativeScanCode = KEY_W;
        held.record(key);
        Input button; button.type = Input::Type::RelativePointer; button.eventType = QEvent::MouseButtonPress; button.button = Qt::LeftButton;
        held.record(button);
        auto releases = held.releaseAll(); QCOMPARE(releases.size(), 2);
        QCOMPARE(releases[0].eventType, QEvent::KeyRelease);
        QCOMPARE(releases[1].type, Input::Type::RelativePointer);
        QCOMPARE(releases[1].position, QPointF());
        QVERIFY(!RetainedMultiInput::positionBeforeDispatch(releases[1]));
        QVERIFY(held.releaseAll().isEmpty());
    }
    void onlyRealDeviceActivityTriggersReclaim() {
        input_event event{};
        event.type = EV_SYN; event.value = 1; QVERIFY(!KRdp::PhysicalInputWatcher::isActivity(event));
        event.type = EV_KEY; event.code = KEY_W; event.value = 0; QVERIFY(!KRdp::PhysicalInputWatcher::isActivity(event));
        event.value = 1; QVERIFY(KRdp::PhysicalInputWatcher::isActivity(event));
        event.type = EV_REL; event.value = 0; QVERIFY(!KRdp::PhysicalInputWatcher::isActivity(event));
        event.value = -200; QVERIFY(KRdp::PhysicalInputWatcher::isActivity(event));
        // App cursor warps have no evdev event. Cursor metadata is no longer a reclaim trigger.
    }
};
QTEST_GUILESS_MAIN(GameInputTest)
#include "GameInputTest.moc"
