// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "RemoteTopologyDraft.h"

#include <cmath>
#include <limits>
#include <optional>

#include <QJsonArray>
#include <QJsonObject>
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

inline std::optional<QVector<Screen>> parseScreens(const QJsonArray &records)
{
    if (records.isEmpty() || records.size() > 16) return {};
    QVector<Screen> screens;
    screens.reserve(records.size());
    const auto integer = [](const QJsonValue &value, int minimum, int maximum) -> std::optional<int> {
        if (!value.isDouble()) return {};
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::floor(number) != number || number < minimum || number > maximum) return {};
        return int(number);
    };
    for (const auto &value : records) {
        if (!value.isObject()) return {};
        const auto object = value.toObject();
        const auto logical = object.value(QStringLiteral("logical")).toObject();
        const auto pixels = object.value(QStringLiteral("pixels")).toObject();
        const auto id = object.value(QStringLiteral("id")).toString();
        const auto x = integer(logical.value(QStringLiteral("x")), -32768, 32768);
        const auto y = integer(logical.value(QStringLiteral("y")), -32768, 32768);
        const auto width = integer(pixels.value(QStringLiteral("width")), 320, 4096);
        const auto height = integer(pixels.value(QStringLiteral("height")), 200, 4096);
        const auto scale = object.value(QStringLiteral("scale"));
        const auto primary = object.value(QStringLiteral("primary"));
        if (object.size() != 5 || logical.size() != 2 || pixels.size() != 2
            || id.isEmpty() || id.size() > 64 || !x || !y || !width || !height
            || !scale.isDouble() || !std::isfinite(scale.toDouble())
            || !primary.isBool()) return {};
        // A client-local selection key, never a compositor name. Keep it
        // printable and bounded so logs/replies cannot carry control text.
        for (const QChar ch : id) {
            const auto code = ch.unicode();
            if (!((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')
                || (code >= '0' && code <= '9') || code == '-' || code == '_')) return {};
        }
        screens.append({id, QPoint(*x, *y), QSize(*width, *height), scale.toDouble(), primary.toBool()});
    }
    return screens;
}

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
