// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RemoteTopologyCatalog.h"
#include "RemoteMonitorGeometry.h"

#include <algorithm>
#include <cmath>

#include <QSet>

namespace KRdp::RemoteTopologyDraft
{
using Entry = RemoteTopologyCatalog::Entry;
using Snapshot = RemoteTopologyCatalog::Snapshot;

struct Capabilities {
    bool addVirtual = false;
    bool removeVirtual = false;
    bool moveVirtual = false;
    bool resizeVirtual = false;
    bool changePrimary = false;
    bool changePhysical = false;
    int maxOutputs = 0;
    int maxOutputDimension = 0;
    int maxAtlasDimension = 0;
};

struct Operation {
    enum class Kind { AddVirtual, Remove, Move, Resize, SetPrimary };
    Kind kind = Kind::Move;
    // An existing generation-scoped ID, or a unique "new:<name>" ID for Add.
    QString id;
    QPoint position;
    QSize pixels;
    qreal scale = 1.0;
};

struct Request {
    QString generation;
    quint64 expectedRevision = 0;
    QString owner;
    QVector<Operation> operations;
    bool allowRemoval = false;
    bool allowPhysicalChange = false;
};

struct Preview {
    QVector<Entry> before;
    QVector<Entry> after;
    QString error;
    bool valid() const { return error.isEmpty(); }
};

inline Preview preview(const Snapshot &snapshot, const Capabilities &caps, const Request &request)
{
    Preview result{snapshot.outputs, snapshot.outputs, {}};
    const auto fail = [&result](const QString &reason) {
        result.after.clear();
        result.error = reason;
        return result;
    };
    if (request.generation != snapshot.generation) return fail(QStringLiteral("stale-generation"));
    if (request.expectedRevision != snapshot.revision) return fail(QStringLiteral("stale-revision"));
    if (request.owner.isEmpty()) return fail(QStringLiteral("not-owner"));
    const auto modeAllowed = [&caps](const QSize &pixels, qreal scale) {
        return caps.maxOutputDimension > 0 && caps.maxAtlasDimension > 0
            && !pixels.isEmpty() && pixels.width() <= caps.maxOutputDimension && pixels.height() <= caps.maxOutputDimension
            && std::isfinite(scale) && scale > 0.0
            && std::isfinite(double(pixels.width()) / scale) && std::isfinite(double(pixels.height()) / scale)
            && double(pixels.width()) / scale <= caps.maxAtlasDimension
            && double(pixels.height()) / scale <= caps.maxAtlasDimension;
    };
    QSet<QString> ids;
    for (const auto &entry : result.after) {
        if (entry.id.isEmpty() || ids.contains(entry.id)) return fail(QStringLiteral("invalid"));
        ids.insert(entry.id);
    }
    QSet<QString> added;
    for (const auto &operation : request.operations) {
        if (operation.kind == Operation::Kind::AddVirtual) {
            if (!caps.addVirtual) return fail(QStringLiteral("unsupported"));
            if (!operation.id.startsWith(QStringLiteral("new:")) || operation.id.size() <= 4 || ids.contains(operation.id))
                return fail(QStringLiteral("invalid"));
            if (!modeAllowed(operation.pixels, operation.scale)) return fail(QStringLiteral("limit"));
            ids.insert(operation.id);
            added.insert(operation.id);
            result.after.append({operation.id, {
                .backendKey = {}, .name = operation.id, .nativePixels = operation.pixels,
                .logicalGeometry = RemoteMonitorGeometry::logicalRect(operation.position, operation.pixels, operation.scale),
                .scale = operation.scale, .enabled = true, .primary = result.after.isEmpty(),
                .physical = false, .owner = request.owner,
            }});
            continue;
        }
        auto it = std::find_if(result.after.begin(), result.after.end(), [&operation](const Entry &entry) { return entry.id == operation.id; });
        if (it == result.after.end()) return fail(QStringLiteral("stale-output"));
        if (it->output.physical) {
            if (!caps.changePhysical || !request.allowPhysicalChange) return fail(QStringLiteral("physical-change-requires-confirmation"));
        } else if (it->output.owner != request.owner) {
            return fail(QStringLiteral("not-owner"));
        }
        switch (operation.kind) {
        case Operation::Kind::Remove:
            if (!caps.removeVirtual || !request.allowRemoval || it->output.physical) return fail(QStringLiteral("removal-requires-confirmation"));
            added.remove(operation.id);
            result.after.erase(it);
            break;
        case Operation::Kind::Move:
            if (!it->output.physical && !caps.moveVirtual) return fail(QStringLiteral("unsupported"));
            it->output.logicalGeometry.moveTopLeft(operation.position);
            break;
        case Operation::Kind::Resize:
            if (!it->output.physical && !caps.resizeVirtual) return fail(QStringLiteral("unsupported"));
            if (!modeAllowed(operation.pixels, operation.scale)) return fail(QStringLiteral("limit"));
            it->output.nativePixels = operation.pixels;
            it->output.scale = operation.scale;
            it->output.logicalGeometry.setSize(RemoteMonitorGeometry::logicalSize(operation.pixels, operation.scale));
            break;
        case Operation::Kind::SetPrimary:
            if (!caps.changePrimary) return fail(QStringLiteral("unsupported"));
            for (auto &entry : result.after) entry.output.primary = entry.id == operation.id;
            break;
        case Operation::Kind::AddVirtual:
            Q_UNREACHABLE();
        }
    }
    if (result.after.isEmpty() || caps.maxOutputs <= 0 || result.after.size() > caps.maxOutputs
        || caps.maxOutputDimension <= 0 || caps.maxAtlasDimension <= 0) return fail(QStringLiteral("limit"));
    int primaryCount = 0;
    QVector<RemoteMonitorGeometry::Output> wireInputs;
    for (const auto &entry : result.after) {
        const auto &output = entry.output;
        if (!output.enabled) continue;
        if (output.primary) ++primaryCount;
        const QRect rect = output.logicalGeometry;
        if (!rect.isValid() || !modeAllowed(output.nativePixels, output.scale)
            || rect.left() < -caps.maxAtlasDimension || rect.top() < -caps.maxAtlasDimension
            || rect.right() > caps.maxAtlasDimension || rect.bottom() > caps.maxAtlasDimension)
            return fail(QStringLiteral("limit"));
        wireInputs.append({rect.topLeft(), output.nativePixels, output.scale, output.primary});
    }
    if (wireInputs.isEmpty() || primaryCount != 1) return fail(QStringLiteral("invalid-primary"));
    for (qsizetype i = 0; i < result.after.size(); ++i) {
        if (!result.after[i].output.enabled) continue;
        for (qsizetype j = i + 1; j < result.after.size(); ++j) {
            if (result.after[j].output.enabled && result.after[i].output.logicalGeometry.intersects(result.after[j].output.logicalGeometry))
                return fail(QStringLiteral("overlap"));
        }
    }
    const auto projected = RemoteMonitorGeometry::projectToWire(wireInputs);
    QRect atlas;
    for (const auto &surface : projected) atlas |= surface.geometry;
    if (atlas.width() > caps.maxAtlasDimension || atlas.height() > caps.maxAtlasDimension) return fail(QStringLiteral("limit"));
    // Each new monitor must connect by a positive-length seam to a surviving
    // pre-existing monitor (or to the first new one in an empty desktop).
    // Existing intentionally gapped/pinned outputs need not be repacked.
    QSet<QString> reachable;
    for (const auto &entry : result.after) {
        if (!added.contains(entry.id) && entry.output.enabled) reachable.insert(entry.id);
    }
    if (reachable.isEmpty() && !result.after.isEmpty()) reachable.insert(result.after.first().id);
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto &entry : result.after) {
            if (!added.contains(entry.id) || reachable.contains(entry.id)) continue;
            const auto rect = entry.output.logicalGeometry;
            for (const auto &other : result.after) {
                if (!reachable.contains(other.id)) continue;
                const auto peer = other.output.logicalGeometry;
                const bool verticalSeam = (rect.left() == peer.right() + 1 || peer.left() == rect.right() + 1)
                    && std::max(rect.top(), peer.top()) <= std::min(rect.bottom(), peer.bottom());
                const bool horizontalSeam = (rect.top() == peer.bottom() + 1 || peer.top() == rect.bottom() + 1)
                    && std::max(rect.left(), peer.left()) <= std::min(rect.right(), peer.right());
                if (verticalSeam || horizontalSeam) {
                    reachable.insert(entry.id);
                    grew = true;
                    break;
                }
            }
        }
    }
    if (!std::all_of(added.cbegin(), added.cend(), [&reachable](const QString &id) { return reachable.contains(id); }))
        return fail(QStringLiteral("unreachable"));
    return result;
}
}
