// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ConsoleWorkerWire.h"
#include "RemoteMonitorGeometry.h"

#include <cmath>
#include <optional>

namespace KRdp::RetainedMultiInput
{
// Every pointer-bearing event has the same inverse transform. A press at an
// RDP surface pixel and a motion to that pixel must address the same KWin
// global logical point; otherwise a drag moves but its button is pressed on
// another output. Record the transformed event for held-button release too.
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
