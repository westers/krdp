// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ConsoleWorkerWire.h"
#include "RemoteMonitorGeometry.h"

#include <cmath>
#include <optional>

namespace KRdp::RetainedMultiInput
{
// Fake-input button/axis requests have no position. Send an absolute motion
// at the packet's mapped point just before those requests, even if this RDP
// client did not send a separate move packet.
inline bool positionBeforeDispatch(const ConsoleWorkerWire::Input &input)
{
    return input.type == ConsoleWorkerWire::Input::Type::Wheel
        || (input.type == ConsoleWorkerWire::Input::Type::Mouse && input.eventType != QEvent::MouseMove);
}

// Every pointer-bearing packet has the same inverse transform. Record the
// transformed event so a held-button release retains its compositor position.
inline std::optional<ConsoleWorkerWire::Input> toCompositor(ConsoleWorkerWire::Input input,
    const QVector<VideoMonitor> &wire, const QVector<RemoteMonitorGeometry::Output> &logical,
    const QPoint &workspaceOrigin)
{
    if (input.type == ConsoleWorkerWire::Input::Type::Key) return input;
    if (wire.isEmpty() || wire.size() != logical.size()
        || !std::isfinite(input.position.x()) || !std::isfinite(input.position.y())) return {};
    for (qsizetype i = 0; i < wire.size(); ++i) {
        if (!wire[i].geometry.isValid() || !std::isfinite(logical[i].scale) || logical[i].scale <= 0.0) return {};
    }
    input.position = RemoteMonitorGeometry::wireToLogical(input.position, wire, logical) + QPointF(workspaceOrigin);
    if (!std::isfinite(input.position.x()) || !std::isfinite(input.position.y())) return {};
    return input;
}
}
