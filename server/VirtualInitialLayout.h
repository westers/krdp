// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "RemoteTopologyDraft.h"

#include <cmath>
#include <limits>

#include <QSet>

namespace KRdp::VirtualInitialLayout
{
// An explicitly selected client screen. Its position is in client logical
// coordinates; pixels and scale describe the proposed compositor output.
struct Screen {
    QString id;
    QPoint logicalPosition;
    QSize pixels;
    qreal scale = 1;
    bool primary = false;
};

struct Plan {
    QVector<RemoteTopologyDraft::Operation> operations;
    QVector<RemoteTopologyDraft::Entry> outputs;
    QString error;
    bool valid() const { return error.isEmpty(); }
};

inline Plan plan(const QVector<Screen> &screens, const QString &owner,
    const RemoteTopologyDraft::Capabilities &caps)
{
    Plan result;
    const auto fail = [&result](const QString &reason) {
        result.operations.clear();
        result.outputs.clear();
        result.error = reason;
        return result;
    };
    if (owner.isEmpty() || screens.isEmpty() || screens.size() > 16) return fail(QStringLiteral("invalid"));
    QSet<QString> ids;
    int primary = -1;
    qint64 minimumX = std::numeric_limits<int>::max();
    qint64 minimumY = std::numeric_limits<int>::max();
    for (qsizetype i = 0; i < screens.size(); ++i) {
        const auto &screen = screens[i];
        if (screen.id.isEmpty() || ids.contains(screen.id)) return fail(QStringLiteral("invalid"));
        if (screen.pixels.width() < 320 || screen.pixels.height() < 200
            || screen.pixels.width() % 2 || screen.pixels.height() % 2
            || !std::isfinite(screen.scale) || screen.scale < 1 || screen.scale > 4)
            return fail(QStringLiteral("limit"));
        ids.insert(screen.id);
        if (screen.primary) {
            if (primary >= 0) return fail(QStringLiteral("invalid-primary"));
            primary = int(i);
        }
        minimumX = std::min(minimumX, qint64(screen.logicalPosition.x()));
        minimumY = std::min(minimumY, qint64(screen.logicalPosition.y()));
    }
    if (primary < 0) return fail(QStringLiteral("invalid-primary"));

    // Draft::preview makes its first newly added output primary. Put the
    // selected primary first, regardless of client screen enumeration order.
    QVector<qsizetype> order;
    order.reserve(screens.size());
    order.append(primary);
    for (qsizetype i = 0; i < screens.size(); ++i) if (i != primary) order.append(i);
    for (qsizetype index : order) {
        const auto &screen = screens[index];
        const qint64 x = qint64(screen.logicalPosition.x()) - minimumX;
        const qint64 y = qint64(screen.logicalPosition.y()) - minimumY;
        if (x > std::numeric_limits<int>::max() || y > std::numeric_limits<int>::max())
            return fail(QStringLiteral("limit"));
        result.operations.append({RemoteTopologyDraft::Operation::Kind::AddVirtual,
            QStringLiteral("new:initial-%1").arg(index), QPoint(int(x), int(y)), screen.pixels, screen.scale});
    }
    const RemoteTopologyDraft::Snapshot empty{QStringLiteral("initial"), 1, {}};
    const RemoteTopologyDraft::Request request{empty.generation, empty.revision, owner, result.operations};
    const auto preview = RemoteTopologyDraft::preview(empty, caps, request);
    if (!preview.valid()) return fail(preview.error);
    result.outputs = preview.after;
    return result;
}
}
