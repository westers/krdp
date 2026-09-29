// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <limits>
#include <optional>

#include <PipeWireSourceStream>
#include <QHash>
#include <QImage>

#include "ConsoleWorkerWire.h"
#include "Cursor.h"

namespace KRdp
{
/**
 * FIX-CURSOR: the cursor shape a desktop shows, from KWin's screencast cursor metadata, and when
 * to tell an RDP client about it.
 *
 * KWin reports the cursor per stream: a stream whose output the pointer is on reports it visible
 * (with the bitmap whenever the shape changed, else only its position), every other stream reports
 * it absent, and all streams report it absent while it is hidden (a client set no cursor
 * surface: a fullscreen video, a game). So the cursor is hidden only when no source that reported
 * says it is visible, and its shape is the last bitmap any source sent. Positions are ignored: an
 * RDP client moves its own pointer.
 *
 * take() rate-limits what goes out (at most one shape per CursorShape::MinIntervalMs, the latest
 * wins) and holds a hide for HideDelayMs, so the pointer crossing from one captured output to the
 * next (the old one reports "absent" a moment before the new one reports "visible") does not
 * blink. Used by the capture worker (:3391/:3395, sources = its capture sessions) and by
 * krdpserver's SessionController (:3389, sources = its sessions).
 */
class CursorTracker
{
public:
    using Shape = ConsoleWorkerWire::CursorShape;
    static constexpr qint64 HideDelayMs = 100;

    /** A cursor report of capture source \a source at \a nowMs (a monotonic clock). */
    void report(quint64 source, const PipeWireCursor &cursor, qint64 nowMs)
    {
        m_visible.insert(source, cursor.visible);
        if (cursor.visible && !cursor.texture.isNull()) {
            m_image = fromImage(cursor.texture, cursor.hotspot);
        }
        settle(nowMs);
    }

    /** \a source stopped capturing (destroyed, or its stream went inactive): it no longer votes. */
    void forget(quint64 source, qint64 nowMs)
    {
        if (m_visible.remove(source)) {
            settle(nowMs);
        }
    }

    /** A new set of sources (a rebuilt capture): none of the old ones votes any more. */
    void resetSources(qint64 nowMs)
    {
        m_visible.clear();
        settle(nowMs);
    }

    /** What the client should show now. */
    Shape desired() const
    {
        if (hidden()) {
            Shape shape;
            shape.type = Shape::Type::Hidden;
            return shape;
        }
        return m_image.value_or(Shape{});
    }

    /** The shape to send at \a nowMs, if one is due; it counts as sent. */
    std::optional<Shape> take(qint64 nowMs)
    {
        if (dueIn(nowMs) != 0) {
            return std::nullopt;
        }
        m_sent = desired();
        m_lastSentMs = nowMs;
        return m_sent;
    }

    /** Milliseconds until take() returns a shape; 0 = now, -1 = nothing to send. */
    qint64 dueIn(qint64 nowMs) const
    {
        const Shape shape = desired();
        if (m_sent && *m_sent == shape) {
            return -1;
        }
        qint64 wait = 0;
        if (m_lastSentMs != std::numeric_limits<qint64>::min()) {
            wait = std::max<qint64>(wait, m_lastSentMs + Shape::MinIntervalMs - nowMs);
        }
        if (shape.type == Shape::Type::Hidden && m_hiddenSinceMs >= 0) {
            wait = std::max<qint64>(wait, m_hiddenSinceMs + HideDelayMs - nowMs);
        }
        return wait;
    }

    /** A new receiver: nothing has been sent to it yet. */
    void resend()
    {
        m_sent.reset();
        m_lastSentMs = std::numeric_limits<qint64>::min();
    }

    /**
     * A bitmap as the worker wire carries it: straight-alpha ARGB32, at most
     * CursorShape::MaxDimension a side (bigger = Default), the hotspot inside it.
     */
    static Shape fromImage(const QImage &texture, QPoint hotspot)
    {
        Shape shape;
        if (texture.isNull() || texture.width() > Shape::MaxDimension || texture.height() > Shape::MaxDimension) {
            return shape; // Default
        }
        const QImage image = texture.convertToFormat(QImage::Format_ARGB32);
        shape.type = Shape::Type::Image;
        shape.size = image.size();
        shape.hotspot = QPoint(std::clamp(hotspot.x(), 0, image.width() - 1), std::clamp(hotspot.y(), 0, image.height() - 1));
        shape.pixels.reserve(qsizetype(image.width()) * image.height() * 4);
        for (int y = 0; y < image.height(); ++y) {
            shape.pixels.append(reinterpret_cast<const char *>(image.constScanLine(y)), qsizetype(image.width()) * 4);
        }
        return shape;
    }

    /** Shows \a shape on an RDP connection's pointer (PointerSystem / PointerNew / PointerCached). */
    static void apply(Cursor &cursor, const Shape &shape)
    {
        switch (shape.type) {
        case Shape::Type::Hidden:
            cursor.hide();
            return;
        case Shape::Type::Default:
            cursor.showDefault();
            return;
        case Shape::Type::Image: {
            if (shape.pixels.size() != qsizetype(shape.size.width()) * shape.size.height() * 4) {
                cursor.showDefault();
                return;
            }
            Cursor::CursorUpdate update;
            update.hotspot = shape.hotspot;
            // A deep copy: the update is cached beyond the lifetime of the record's bytes.
            update.image = QImage(reinterpret_cast<const uchar *>(shape.pixels.constData()), shape.size.width(), shape.size.height(),
                                  shape.size.width() * 4, QImage::Format_ARGB32)
                               .copy();
            cursor.update(update);
            return;
        }
        }
    }

private:
    bool hidden() const
    {
        return !m_visible.isEmpty() && std::none_of(m_visible.cbegin(), m_visible.cend(), [](bool visible) {
            return visible;
        });
    }

    void settle(qint64 nowMs)
    {
        if (!hidden()) {
            m_hiddenSinceMs = -1;
        } else if (m_hiddenSinceMs < 0) {
            m_hiddenSinceMs = nowMs;
        }
    }

    QHash<quint64, bool> m_visible;
    std::optional<Shape> m_image;
    std::optional<Shape> m_sent;
    qint64 m_lastSentMs = std::numeric_limits<qint64>::min();
    qint64 m_hiddenSinceMs = -1;
};
}
