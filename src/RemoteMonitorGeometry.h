// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QVector>

#include "SurfaceLayout.h"

namespace KRdp::RemoteMonitorGeometry
{
// KScreen positions are logical; capture and RDP surface sizes are pixels.
inline QSize logicalSize(const QSize &pixels, qreal scale)
{
    const qreal ratio = scale > 0.0 ? scale : 1.0;
    return QSize(int(std::ceil(pixels.width() / ratio)), int(std::ceil(pixels.height() / ratio)));
}

inline QRect logicalRect(const QPoint &position, const QSize &pixels, qreal scale)
{
    return QRect(position, logicalSize(pixels, scale));
}

struct Output {
    QPoint logicalPosition;
    QSize pixelSize;
    qreal scale = 1.0;
    bool primary = false;
};

// Pack native-sized RDP surfaces without treating KWin logical positions as
// pixel origins. Preserve ordering and logical gaps; any monitor to the right
// (or below) another starts after that monitor's complete pixel surface.
// Input and cursor mapping retain the independent logical origins.
inline QVector<VideoMonitor> projectToWire(const QVector<Output> &outputs)
{
    QVector<VideoMonitor> projected(outputs.size());
    if (outputs.isEmpty()) {
        return projected;
    }

    auto axis = [&outputs, &projected](bool horizontal) {
        QVector<qsizetype> order(outputs.size());
        std::iota(order.begin(), order.end(), 0);
        const auto start = [horizontal](const Output &output) {
            return horizontal ? output.logicalPosition.x() : output.logicalPosition.y();
        };
        const auto logicalExtent = [horizontal](const Output &output) {
            const QSize logical = logicalSize(output.pixelSize, output.scale);
            return horizontal ? logical.width() : logical.height();
        };
        const auto pixelExtent = [horizontal](const Output &output) {
            return horizontal ? output.pixelSize.width() : output.pixelSize.height();
        };
        std::stable_sort(order.begin(), order.end(), [&outputs, &start](qsizetype a, qsizetype b) {
            return start(outputs.at(a)) < start(outputs.at(b));
        });
        const int minimum = start(outputs.at(order.first()));
        for (qsizetype n = 0; n < order.size(); ++n) {
            const qsizetype i = order.at(n);
            int position = start(outputs.at(i)) - minimum;
            for (qsizetype p = 0; p < n; ++p) {
                const qsizetype j = order.at(p);
                const int end = start(outputs.at(j)) + logicalExtent(outputs.at(j));
                if (end <= start(outputs.at(i))) {
                    const int previous = horizontal ? projected.at(j).geometry.x() : projected.at(j).geometry.y();
                    position = std::max(position, previous + pixelExtent(outputs.at(j)) + start(outputs.at(i)) - end);
                }
            }
            if (horizontal) {
                projected[i].geometry.moveLeft(position);
            } else {
                projected[i].geometry.moveTop(position);
            }
        }
    };
    axis(true);
    axis(false);
    for (qsizetype i = 0; i < outputs.size(); ++i) {
        projected[i].geometry.setSize(outputs.at(i).pixelSize);
        projected[i].primary = outputs.at(i).primary;
    }
    return projected;
}

inline QPointF wireToLogical(const QPointF &pixel, const QVector<VideoMonitor> &wire, const QVector<Output> &outputs)
{
    if (wire.isEmpty() || wire.size() != outputs.size()) {
        return pixel;
    }
    qsizetype best = 0;
    qreal bestDistance = std::numeric_limits<qreal>::max();
    for (qsizetype i = 0; i < wire.size(); ++i) {
        const QRectF rect(wire.at(i).geometry);
        // RDP monitor edges are inclusive integers, but a floating pointer
        // at the next surface's origin belongs to that next surface.
        if (pixel.x() >= rect.left() && pixel.x() < rect.left() + rect.width()
            && pixel.y() >= rect.top() && pixel.y() < rect.top() + rect.height()) {
            best = i;
            break;
        }
        const qreal dx = std::max({rect.left() - pixel.x(), 0.0, pixel.x() - (rect.left() + rect.width())});
        const qreal dy = std::max({rect.top() - pixel.y(), 0.0, pixel.y() - (rect.top() + rect.height())});
        const qreal distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    const auto &surface = wire.at(best).geometry;
    const qreal scale = outputs.at(best).scale > 0.0 ? outputs.at(best).scale : 1.0;
    return QPointF(outputs.at(best).logicalPosition) + (pixel - QPointF(surface.topLeft())) / scale;
}
}
