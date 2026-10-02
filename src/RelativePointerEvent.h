// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QInputEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <memory>

namespace KRdp {
/** Relative motion is deliberately distinct from an absolute Qt mouse position. */
class RelativePointerEvent final : public QInputEvent {
public:
    static constexpr QEvent::Type EventType = QEvent::User;
    RelativePointerEvent(QEvent::Type action, QPointF delta = {}, Qt::MouseButton button = Qt::NoButton, QPoint angleDelta = {})
        : QInputEvent(EventType, nullptr, Qt::NoModifier), action(action), delta(delta), button(button), angleDelta(angleDelta) {}
    const QEvent::Type action;
    const QPointF delta;
    const Qt::MouseButton button;
    const QPoint angleDelta;
    std::shared_ptr<QEvent> nonMotionEvent() const {
        if (action == QEvent::Wheel)
            return std::make_shared<QWheelEvent>(QPointF{}, QPointF{}, QPoint{}, angleDelta, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        return std::make_shared<QMouseEvent>(action, QPointF{}, QPointF{}, button, button, Qt::NoModifier);
    }
};
}
