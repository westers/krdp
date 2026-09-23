// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>
#include "RetainedMultiInput.h"
#include "ConsoleInputState.h"

class RetainedMultiInputTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void mapsPressDragReleaseAndWheelAcrossMixedScaleAndNegativeOrigin()
    {
        using namespace KRdp;
        using Input = ConsoleWorkerWire::Input;
        const QVector<RemoteMonitorGeometry::Output> logical{
            {{0, 0}, {1600, 900}, 1.25, true},
            {{1280, 100}, {1280, 720}, 1.0, false},
        };
        const auto wire = RemoteMonitorGeometry::projectToWire(logical);
        QCOMPARE(wire[1].geometry.topLeft(), QPoint(1600, 100));
        const QPoint workspaceOrigin(-1280, -50);
        Input press;
        press.type = Input::Type::Mouse;
        press.eventType = QEvent::MouseButtonPress;
        press.position = {400, 250};
        press.button = Qt::LeftButton;
        press.buttons = Qt::LeftButton;
        const auto mappedPress = RetainedMultiInput::toCompositor(press, wire, logical, workspaceOrigin);
        QVERIFY(mappedPress);
        QCOMPARE(mappedPress->position, QPointF(-960, 150));

        Input motion = press;
        motion.eventType = QEvent::MouseMove;
        motion.button = Qt::NoButton;
        motion.position = {1800, 250};
        const auto mappedMotion = RetainedMultiInput::toCompositor(motion, wire, logical, workspaceOrigin);
        QVERIFY(mappedMotion);
        QCOMPARE(mappedMotion->position, QPointF(200, 200));

        ConsoleInputState held;
        held.record(*mappedPress);
        held.record(*mappedMotion);
        const auto releases = held.releaseAll();
        QCOMPARE(releases.size(), 1);
        QCOMPARE(releases.first().position, mappedMotion->position);
        QCOMPARE(releases.first().eventType, QEvent::MouseButtonRelease);

        Input click = press;
        click.position = motion.position;
        QCOMPARE(RetainedMultiInput::toCompositor(click, wire, logical, workspaceOrigin)->position, mappedMotion->position);
        Input wheel;
        wheel.type = Input::Type::Wheel;
        wheel.eventType = QEvent::Wheel;
        wheel.position = motion.position;
        QCOMPARE(RetainedMultiInput::toCompositor(wheel, wire, logical, workspaceOrigin)->position, mappedMotion->position);
    }

    void refusesMissingOrNonFiniteLayoutButKeepsKeyboardPositionless()
    {
        using namespace KRdp;
        using Input = ConsoleWorkerWire::Input;
        Input pointer;
        pointer.position = {50, 50};
        QVERIFY(!RetainedMultiInput::toCompositor(pointer, {}, {}, {}));
        pointer.position.setX(std::numeric_limits<qreal>::quiet_NaN());
        QVERIFY(!RetainedMultiInput::toCompositor(pointer, {{QRect(0, 0, 100, 100), true}}, {{{0, 0}, {100, 100}, 1, true}}, {}));
        Input key;
        key.type = Input::Type::Key;
        key.eventType = QEvent::KeyPress;
        key.nativeScanCode = 38;
        QCOMPARE(RetainedMultiInput::toCompositor(key, {}, {}, {}).value(), key);
    }
};

QTEST_GUILESS_MAIN(RetainedMultiInputTest)
#include "RetainedMultiInputTest.moc"
