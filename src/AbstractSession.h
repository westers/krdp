// SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleix.pol_gonzalez@mercedes-benz.com>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "krdp_export.h"

#include <PipeWireEncodedStream>
#include <PipeWireSourceStream>
#include <QRect>
#include <QString>

class QMimeData;

namespace KRdp
{
struct VideoFrame;
class Server;

struct VirtualMonitor {
    QString name;
    QSize size;
    qreal dpr;
};

class KRDP_EXPORT AbstractSession : public QObject
{
    Q_OBJECT
public:
    AbstractSession();
    ~AbstractSession() override;

    /**
     * Properties have been initialised and we can start the session
     */
    virtual void start() = 0;

    bool streamingEnabled() const;
    void setStreamingEnabled(bool enable);
    void setVideoFrameRate(quint32 framerate);
    void setActiveStream(int stream);
    void setVirtualMonitor(const VirtualMonitor &vm);
    void setVideoQuality(quint8 quality);

    /**
     * Re-create the capture stream after the display topology changed.
     *
     * The default implementation does nothing; sessions that can retarget an
     * output override it.
     */
    virtual void refreshDisplayConfiguration();

    /**
     * Ask the encoder for a fresh keyframe (the RDPGFX surface was just
     * re-created and has no reference picture).
     *
     * Sessions ask the encoder for a keyframe through
     * `PipeWireBaseEncodedStream::requestKeyFrame()` when the linked KPipeWire
     * has it, and otherwise restart their encoded stream (which always opens
     * with an IDR). The default implementation only logs.
     */
    virtual void requestKeyFrame();

    void requestStreamingEnable(QObject *requester);
    void requestStreamingDisable(QObject *requester);

    /**
     * Set the system's clipboard data.
     *
     * The data is provided by the remote RDP client.
     */
    virtual void setClipboardData(std::unique_ptr<QMimeData> data) = 0;

    /**
     * Send a new event to the portal.
     *
     * \param event The new event to send.
     */
    virtual void sendEvent(const std::shared_ptr<QEvent> &event) = 0;

    /**
     * Send an event whose pointer position is already in KWin-global
     * coordinates, i.e. relative to the origin of the whole compositor
     * workspace rather than to this session's own captured output.
     *
     * Used by the multi-monitor path, where the client sends one pointer
     * position for the whole workspace while each session only captures a
     * single output. The default implementation just forwards to sendEvent().
     *
     * \param event The new event to send.
     */
    virtual void sendGlobalEvent(const std::shared_ptr<QEvent> &event);

    /**
     * The index of the RDPGFX surface this session feeds.
     *
     * Stamped into every emitted VideoFrame so the video stream knows which
     * surface a frame belongs to. Always 0 unless MonitorMode is `multi`.
     *
     * Not to be confused with SessionController::setMonitorIndex(), which
     * picks which monitor to capture in `specific` mode.
     */
    void setMonitorIndex(int index);
    int monitorIndex() const;

    /**
     * The geometry of the captured output in KWin-global (logical) coordinates.
     *
     * The default implementation reports the session's own logical size at the
     * origin; sessions that capture a single output of a larger workspace
     * override it with that output's position.
     *
     * Meaningful only after started(); it may be empty while KWin re-adds its
     * outputs after a DPMS wake, so callers must check isEmpty(). Note also
     * that this is logical, not pixels: a surface's size is the capture size,
     * which is this scaled by the output's device pixel ratio.
     */
    virtual QRect outputGeometry() const;

Q_SIGNALS:
    void started();
    void error();

    /**
     * Emitted whenever a new frame has been received.
     *
     * Received in this case means that the portal has sent the data and it has
     * been encoded by libav.
     */
    void frameReceived(const VideoFrame &frame);

    /**
     * Emitted whenever a new cursor update was received.
     *
     * These are separate from frames as RDP has a separate protocol for mouse
     * movement that is more performant than embedding things into the video
     * stream.
     */
    void cursorUpdate(const PipeWireCursor &cursor);

    /**
     * Emitted whenever the system's clipboard data changes.
     */
    void clipboardDataChanged(const QMimeData *data);

protected:
    QSize size() const;
    QSize logicalSize() const;
    bool streamingRequested() const;
    std::optional<VirtualMonitor> virtualMonitor() const;
    int activeStream() const;

    void setStarted(bool started);
    void setSize(QSize size);
    void setLogicalSize(QSize size);
    PipeWireEncodedStream *stream();

private:
    class Private;
    const std::unique_ptr<Private> d;
};

}
