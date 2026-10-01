// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <optional>
#include <QString>
#include <QVector>

namespace KRdp
{
// Console capture selection is independent of the connection type. Retained
// Virtual desktops always capture their own committed output layout.
struct MonitorCapturePolicy {
    enum class Mode : quint8 { Multi, Workspace, Primary, Specific };
    Mode mode = Mode::Multi;
    int index = 0;
    bool isValid() const { return quint8(mode) <= quint8(Mode::Specific) && index >= 0 && index <= 65535; }
    bool operator==(const MonitorCapturePolicy &) const = default;

    static std::optional<MonitorCapturePolicy> parse(const QString &mode, int index = 0)
    {
        MonitorCapturePolicy result;
        result.index = index;
        if (mode == QStringLiteral("multi")) result.mode = Mode::Multi;
        else if (mode == QStringLiteral("workspace")) result.mode = Mode::Workspace;
        else if (mode == QStringLiteral("primary")) result.mode = Mode::Primary;
        else if (mode == QStringLiteral("specific")) result.mode = Mode::Specific;
        else return {}; // Client-created virtual outputs need their separate lifecycle policy.
        return result.isValid() ? std::optional(result) : std::nullopt;
    }

    struct Selection {
        bool workspace = false;
        QVector<int> indices;
        bool operator==(const Selection &) const = default;
    };
    // The caller supplies the actual QScreen indices and authoritative primary.
    // Missing specific outputs wait/fail rather than exposing the whole desktop.
    std::optional<Selection> select(int count, int primary) const
    {
        if (!isValid() || count <= 0 || count > 16 || primary < 0 || primary >= count) return {};
        Selection result;
        switch (mode) {
        case Mode::Workspace: result.workspace = true; break;
        case Mode::Primary: result.indices.append(primary); break;
        case Mode::Specific:
            if (index >= count) return {};
            result.indices.append(index); break;
        case Mode::Multi:
            for (int i = 0; i < count; ++i) result.indices.append(i);
            break;
        }
        return result;
    }
};
}
