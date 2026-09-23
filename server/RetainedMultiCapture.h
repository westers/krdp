// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ConsoleWorkerWire.h"
#include "H264KeyframeSize.h"
#include "RemoteMonitorGeometry.h"
#include "VirtualResize.h"

#include <algorithm>
#include <optional>

#include <QSet>

namespace KRdp
{
// Coordinates and encoded packets for independent captures of one retained
// compositor. No compositor mutation or socket IO here. A new layout is not
// publishable until every output has a self-contained keyframe whose decoded
// payload dimensions agree with its capture metadata and logical geometry.
class RetainedMultiCapture
{
public:
    struct Screen {
        QString name;
        QRect logicalGeometry;
        bool primary = false;
        bool operator==(const Screen &) const = default;
    };

    struct Result {
        bool reset = false;
        bool becameReady = false;
        ConsoleWorkerWire::Outputs outputs;
        QVector<VideoMonitor> atlas;
        QVector<VideoFrame> frames;
    };

    bool configure(const QVector<Screen> &screens)
    {
        const auto reject = [this] {
            m_screens.clear();
            clearFrames();
            return false;
        };
        QSet<QString> names;
        int primary = 0;
        for (const auto &screen : screens) {
            if (screen.name.isEmpty() || names.contains(screen.name) || !screen.logicalGeometry.isValid()
                || screen.logicalGeometry.width() > 8192 || screen.logicalGeometry.height() > 8192
                || screen.logicalGeometry.left() < -32768 || screen.logicalGeometry.top() < -32768
                || screen.logicalGeometry.right() > 32768 || screen.logicalGeometry.bottom() > 32768) return reject();
            names.insert(screen.name);
            if (screen.primary) ++primary;
        }
        if (screens.size() < 2 || screens.size() > 16 || primary != 1) return reject();
        for (qsizetype i = 0; i < screens.size(); ++i) {
            for (qsizetype j = i + 1; j < screens.size(); ++j) {
                if (screens[i].logicalGeometry.intersects(screens[j].logicalGeometry)) return reject();
            }
        }
        if (m_screens == screens) return true;
        m_screens = screens;
        clearFrames();
        return true;
    }

    void invalidate() { clearFrames(); }
    bool ready() const { return m_ready; }
    const ConsoleWorkerWire::Outputs &outputs() const { return m_outputs; }
    const QVector<VideoMonitor> &atlas() const { return m_atlas; }
    const QVector<Screen> &screens() const { return m_screens; }

    Result submit(qsizetype index, const VideoFrame &frame)
    {
        Result result;
        if (index < 0 || index >= m_screens.size() || frame.size.isEmpty() || frame.data.isEmpty()
            || frame.size.width() > 4096 || frame.size.height() > 4096 || frame.monitors.size() != 1
            || frame.monitors.first().geometry != QRect(QPoint(0, 0), m_screens[index].logicalGeometry.size())) {
            if (m_ready && index >= 0 && index < m_screens.size()) {
                clearFrames();
                result.reset = true;
            }
            return result;
        }
        const auto scale = VirtualResize::frameScale(frame.size, m_screens[index].logicalGeometry.size());
        if (!scale) {
            if (m_ready) {
                clearFrames();
                result.reset = true;
            }
            return result;
        }
        if (m_ready && (frame.size != m_sizes[index] || !VirtualResize::sameScale(*scale, m_scales[index]))) {
            clearFrames();
            result.reset = true;
        }
        if (m_ready) {
            auto stamped = frame;
            stamped.monitorIndex = int(index);
            stamped.monitors = m_atlas;
            result.frames.append(std::move(stamped));
            return result;
        }
        if (!frame.isKeyFrame || h264KeyframeSize(frame.data) != frame.size) return result;
        m_keyframes[index] = frame;
        m_sizes[index] = frame.size;
        m_scales[index] = *scale;
        if (std::any_of(m_keyframes.cbegin(), m_keyframes.cend(), [](const auto &packet) { return !packet; })) return result;

        QRect workspace;
        for (const auto &screen : m_screens) workspace |= screen.logicalGeometry;
        QVector<RemoteMonitorGeometry::Output> projected;
        projected.reserve(m_screens.size());
        ConsoleWorkerWire::Outputs outputs;
        outputs.compositorOrigin = workspace.topLeft();
        for (qsizetype i = 0; i < m_screens.size(); ++i) {
            const auto logical = m_screens[i].logicalGeometry.translated(-workspace.topLeft());
            projected.append({logical.topLeft(), m_sizes[i], m_scales[i], m_screens[i].primary});
            outputs.monitors.append({m_screens[i].name, logical, m_scales[i], m_screens[i].primary});
        }
        const auto atlas = RemoteMonitorGeometry::projectToWire(projected);
        QRect bounds;
        for (qsizetype i = 0; i < atlas.size(); ++i) {
            if (atlas[i].geometry.left() < 0 || atlas[i].geometry.top() < 0) return result;
            for (qsizetype j = i + 1; j < atlas.size(); ++j) {
                if (atlas[i].geometry.intersects(atlas[j].geometry)) return result;
            }
            bounds |= atlas[i].geometry;
        }
        if (bounds.width() > 8192 || bounds.height() > 8192) return result;
        m_outputs = outputs;
        m_atlas = atlas;
        m_ready = true;
        result.becameReady = true;
        result.outputs = m_outputs;
        result.atlas = m_atlas;
        for (qsizetype i = 0; i < m_keyframes.size(); ++i) {
            auto stamped = *m_keyframes[i];
            stamped.monitorIndex = int(i);
            stamped.monitors = m_atlas;
            result.frames.append(std::move(stamped));
        }
        m_keyframes.clear();
        return result;
    }

private:
    void clearFrames()
    {
        m_ready = false;
        m_outputs = {};
        m_atlas.clear();
        m_keyframes = QVector<std::optional<VideoFrame>>(m_screens.size());
        m_sizes = QVector<QSize>(m_screens.size());
        m_scales = QVector<qreal>(m_screens.size());
    }

    QVector<Screen> m_screens;
    QVector<std::optional<VideoFrame>> m_keyframes;
    QVector<QSize> m_sizes;
    QVector<qreal> m_scales;
    ConsoleWorkerWire::Outputs m_outputs;
    QVector<VideoMonitor> m_atlas;
    bool m_ready = false;
};
}
