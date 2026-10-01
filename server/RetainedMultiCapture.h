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
        /// Once ready: the frame to send now. On becameReady: each output's proof keyframe,
        /// output i at index i (the published evidence), to be sent before \a held.
        QVector<VideoFrame> frames;
        /// AUD-FIX11 R6: on becameReady, the packets each output encoded after its proof
        /// keyframe while the others proved themselves, in the order they were produced.
        QVector<VideoFrame> held;
        /// Outputs whose held packets had to be given up: they need a new keyframe to prove.
        QVector<qsizetype> keyFrameRequests;
    };

    /// AUD-FIX11 R6: what one output may hold while the others prove themselves: ~2 s at 60 fps.
    static constexpr qsizetype MaxHeldPackets = 120;
    static constexpr qsizetype MaxHeldBytes = 64 * 1024 * 1024;

    bool configure(const QVector<Screen> &screens, int minimumCount = 2)
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
        if (minimumCount < 1 || minimumCount > 2 || screens.size() < minimumCount || screens.size() > 16 || primary != 1) return reject();
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
        // AUD-FIX9 R1: the proof is a keyframe of the codec that produced it (HEVC/AV1 after a
        // codec change at attach), not only H.264: an H.264-only check never became ready again.
        const auto codec = frame.codec.value_or(VideoCodec::Avc420);
        // AUD-FIX10 R5: the keyframe shows this size (AMD AV1 codes 1920x1080 as 1920x1082).
        if (!frame.isKeyFrame || !encodedKeyframeShows(codec, frame.data, frame.size)) {
            // AUD-FIX11 R6: an output that has proven itself keeps encoding while the others
            // prove themselves, and its next packets reference those it encoded in between.
            // Dropping them broke every decoder's reference chain: libdav1d rejects the first
            // AV1 frame after the gap ("Invalid data"), H.264/HEVC show corruption until the
            // next keyframe. So they are held, in order, and published after the keyframe.
            hold(index, frame, codec, result);
            return result;
        }
        if (m_keyframeCodec && *m_keyframeCodec != codec) {
            // The encoders changed codec while the layout was being proven: one published layout
            // never mixes codecs, so every output proves itself again in the new one.
            m_keyframes = QVector<std::optional<VideoFrame>>(m_screens.size());
            m_held = QVector<QVector<Held>>(m_screens.size());
        }
        m_keyframeCodec = codec;
        m_keyframes[index] = frame;
        m_held[index].clear(); // a new keyframe starts the chain again
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
        // Every output's held packets, in the order the encoders produced them, each after its
        // own output's keyframe.
        QVector<Held> held;
        for (const auto &chain : std::as_const(m_held)) held += chain;
        std::sort(held.begin(), held.end(), [](const Held &a, const Held &b) {
            return a.sequence < b.sequence;
        });
        for (auto &packet : held) {
            packet.frame.monitors = m_atlas;
            result.held.append(std::move(packet.frame));
        }
        m_keyframes.clear();
        m_held.clear();
        m_keyframeCodec.reset();
        return result;
    }

    /// Packets held for \a index behind its proof keyframe (tests).
    qsizetype heldPackets(qsizetype index) const
    {
        return index >= 0 && index < m_held.size() ? m_held[index].size() : 0;
    }

private:
    struct Held {
        quint64 sequence = 0;
        VideoFrame frame;
    };

    /// A packet of an output that is not a proof: kept behind that output's proof keyframe (if
    /// it has one, of the same codec and size), else dropped - nothing before a proof is sent.
    void hold(qsizetype index, const VideoFrame &frame, VideoCodec codec, Result &result)
    {
        auto &chain = m_held[index];
        if (!m_keyframes[index]) return; // before this output's proof keyframe: never sent
        if (codec != m_keyframeCodec) return; // the replaced encoder's last packets: not this chain
        if (frame.isKeyFrame || frame.size != m_keyframes[index]->size) {
            // A keyframe that proves nothing (another size) restarts the reference chain
            // without a proof: this output must prove itself again.
            m_keyframes[index].reset();
            chain.clear();
            return;
        }
        qsizetype bytes = frame.data.size() + frame.aux.size();
        for (const auto &packet : std::as_const(chain)) bytes += packet.frame.data.size() + packet.frame.aux.size();
        if (chain.size() >= MaxHeldPackets || bytes > MaxHeldBytes) {
            // The other outputs take too long: give this proof up and ask for a new keyframe.
            m_keyframes[index].reset();
            chain.clear();
            result.keyFrameRequests.append(index);
            return;
        }
        auto stamped = frame;
        stamped.monitorIndex = int(index);
        chain.append({m_sequence++, std::move(stamped)});
    }

    void clearFrames()
    {
        m_ready = false;
        m_outputs = {};
        m_atlas.clear();
        m_keyframes = QVector<std::optional<VideoFrame>>(m_screens.size());
        m_held = QVector<QVector<Held>>(m_screens.size());
        m_keyframeCodec.reset();
        m_sizes = QVector<QSize>(m_screens.size());
        m_scales = QVector<qreal>(m_screens.size());
    }

    QVector<Screen> m_screens;
    QVector<std::optional<VideoFrame>> m_keyframes;
    QVector<QVector<Held>> m_held; ///< per output, the packets after its proof keyframe (AUD-FIX11)
    quint64 m_sequence = 0;
    std::optional<VideoCodec> m_keyframeCodec; ///< the codec of the keyframes held in m_keyframes
    QVector<QSize> m_sizes;
    QVector<qreal> m_scales;
    ConsoleWorkerWire::Outputs m_outputs;
    QVector<VideoMonitor> m_atlas;
    bool m_ready = false;
};
}
