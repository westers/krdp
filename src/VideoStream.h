// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>
#include <optional>

#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QSize>
#include <QVector>

#include <freerdp/server/rdpgfx.h>

#include "SurfaceLayout.h"
#include "VideoFrame.h"
#include "krdp_export.h"

namespace KRdp
{

class RdpConnection;

/**
 * A class that encapsulates an RdpGfx video stream.
 *
 * Video streaming is done using the "RDP Graphics Pipeline" protocol
 * extension which allows using h264 as the codec for the video stream.
 * However, this protocol extension is fairly complex to setup and use.
 *
 * VideoStream makes sure to handle most of the complexity of the RdpGfx
 * protocol like ensuring the client knows the right resolution and a
 * surface at the right size. It also takes care of sending the frames,
 * using a separate thread for a submission queue.
 *
 * VideoStream is managed by Session. Each session will have one instance
 * of this class.
 */
class KRDP_EXPORT VideoStream : public QObject
{
    Q_OBJECT

public:
    explicit VideoStream(RdpConnection *session);
    ~VideoStream() override;

    bool initialize();
    void close();
    Q_SIGNAL void closed();

    /**
     * Queue a frame to be sent to the client.
     *
     * This will add the provided frame to the queue of frames that should
     * be sent to the client.
     *
     * \param frame The frame to send.
     */
    void queueFrame(const VideoFrame &frame);

    /**
     * Indicate that the video state should be reset.
     *
     * This means the screen resolution and other information of the client
     * will be updated based on the current state of the VideoStream.
     */
    void reset();

    /**
     */
    bool enabled() const;
    void setEnabled(bool enabled);
    Q_SIGNAL void enabledChanged();

    uint32_t requestedFrameRate() const;
    Q_SIGNAL void requestedFrameRateChanged();

    /**
     * Give the stream an explicit monitor layout, one RDPGFX surface per
     * monitor.
     *
     * \a layout is in any coordinate space whose monitors are laid out
     * relative to each other; SurfaceLayout::fromMonitors() translates it into
     * RDP desktop space (primary at (0, 0)). Entry \a i is the surface that
     * frames with `VideoFrame::monitorIndex == i` are sent to.
     *
     * A layout with an empty geometry, with more than 16 monitors, or without
     * exactly one primary is rejected and the previous layout kept: a session
     * can briefly report a null output geometry while KWin re-adds its outputs
     * after a DPMS wake, and tearing the surfaces down over that is worse than
     * streaming a stale layout for a frame or two.
     *
     * An empty layout (the default) means "derive it from the frames", which
     * is the single-surface behaviour every mode but `MonitorMode=multi` uses.
     */
    void setMonitorLayout(const QVector<VideoMonitor> &layout);

    /**
     * Emitted (from the frame submission thread) when the RDPGFX surface for
     * \a monitorIndex was just (re)created but the frame being sent is not a
     * keyframe, so the client has no reference picture until the next IDR. The
     * session feeding that surface should obtain a fresh keyframe from the
     * encoder.
     */
    Q_SIGNAL void keyFrameRequested(int monitorIndex);

    /**
     * Set the upper bound for the video quality.
     *
     * With adaptive quality enabled this is a cap on the value the stream
     * steers towards; with it disabled this is the quality used outright.
     */
    void setQualityCap(quint8 cap);
    /**
     * Enable or disable steering quality from measured goodput and RTT.
     *
     * When disabled the stream uses the configured quality cap outright.
     */
    void setAdaptiveQuality(bool enabled);
    /**
     * Emitted when the stream wants the session to use a new video quality,
     * either because adaptive quality moved it or because the cap/adaptive
     * setting changed. May be emitted from a thread other than the session's.
     */
    Q_SIGNAL void requestedQualityChanged(quint8 quality);

private:
    friend BOOL gfxChannelIdAssigned(RdpgfxServerContext *, uint32_t);
    friend uint32_t gfxCapsAdvertise(RdpgfxServerContext *, const RDPGFX_CAPS_ADVERTISE_PDU *);
    friend uint32_t gfxFrameAcknowledge(RdpgfxServerContext *, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *);

    bool onChannelIdAssigned(uint32_t channelId);
    uint32_t onCapsAdvertise(const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise);
    uint32_t onFrameAcknowledge(const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge);

    // Driven by the main-thread adaptive timer, which fires every
    // QualityUpdateInterval while streaming (started in initialize(), stopped
    // in close()); this slot therefore runs on VideoStream's own (main) thread.
    Q_SLOT void updateAdaptiveQuality();

    /**
     * Send ResetGraphics for \a monitors (already in RDP desktop space, with
     * the desktop itself \a desktopSize) and re-create one surface per entry
     * of \a surfaces.
     */
    void performReset(const QSize &desktopSize, const QVector<VideoMonitor> &monitors, const QVector<SurfaceLayout::Entry> &surfaces);
    /**
     * Returns false only when the frame could not be sent because the GFX
     * channel is not ready (context gone or caps reset mid-flight); the caller
     * then keeps the frame at the head of the queue instead of dropping it.
     */
    bool sendFrame(const VideoFrame &frame);

    class Private;
    const std::unique_ptr<Private> d;
};

}
