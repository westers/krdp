// SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleix.pol_gonzalez@mercedes-benz.com>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <PipeWireEncodedStream>
#include <PipeWireSourceStream>
#include <QPointF>
#include <QRect>
#include <QString>

// Qt's QMetaType (needed for VideoCodecSupport.h's Q_DECLARE_METATYPE) comes in transitively via
// the Qt/PipeWire includes above; VideoCodecSupport.h must come after them, not before.
#include "VideoCodecSupport.h"
#include "krdp_export.h"

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
    /**
     * Whether this session's encoded stream is actually running.
     *
     * False while there is no stream at all, and while a screencast that was
     * closed (a DPMS wake re-adds every wl_output) is being recovered. A
     * session in that state silently drops every event handed to it, so the
     * multi-monitor input path picks a session for which this is true.
     */
    bool streamActive() const;
    void setStreamingEnabled(bool enable);
    void setVideoFrameRate(quint32 framerate);
    void setActiveStream(int stream);
    void setVirtualMonitor(const VirtualMonitor &vm);
    void setVideoQuality(quint8 quality);

    /**
     * The codec the connection negotiated (or expects): picks the encoder's chroma mode. Before
     * start() it is applied when the stream is created; on a running stream a change restarts the
     * encoded stream (restartStreamForCodecChange()), which opens with an IDR.
     */
    void setVideoCodec(VideoCodec codec);
    VideoCodec videoCodec() const;
    /**
     * AVC444: whether frames carry the chroma picture (the adaptive rung); a no-op for AVC420.
     */
    void setChromaEnabled(bool enabled);
    Q_SIGNAL void chromaTimingReported(const KRdp::ChromaTimingReport &report);
    /**
     * Whether the running encoder really produces the chroma stream: false for a 4:2:0 codec, for a
     * KPipeWire without AVC444, when h264_vaapi is unavailable, and after the encoder's mid-session
     * fallback (activeChromaModeChanged). Emitted on every encoder start and on a fallback.
     */
    Q_SIGNAL void chromaCapabilityChanged(bool capable);

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

    /**
     * Whether outputGeometry() is the captured output's real place in the
     * workspace yet.
     *
     * False only for a virtual-monitor session whose QScreen has not been
     * found yet: KWin places the requested output itself, so until the
     * matching screen shows up the geometry is a provisional (0,0) rect and
     * pointer input mapped through it would land on a physical monitor.
     */
    virtual bool outputGeometryResolved() const
    {
        return true;
    }

    /**
     * Map a position in this session's captured output, in capture PIXELS
     * (an RDP pointer position, or the screencast's cursor metadata), to
     * KWin-global logical coordinates: the space fake input's absolute
     * pointer motion takes and outputGeometry() lives in.
     *
     * This is exactly the arithmetic sendEvent() injects pointer motion
     * with - normalised over (pixelSize()-1), spanned over (logical size-1)
     * and offset by outputGeometry().topLeft() - so a caller that needs to
     * know where an injected move landed, or where a cursor sample sits, in
     * that space gets the same answer the compositor did.
     *
     * Meaningful only once pixelSize() and the logical size are known
     * (started()); before that the input is returned offset by the origin.
     */
    QPointF mapToGlobal(const QPointF &local) const;

    /**
     * The size of the captured stream in PIXELS, as the compositor reports it.
     *
     * This, not the logical geometry scaled by a device pixel ratio, is the
     * size every frame of this session actually has, and so the size its
     * RDPGFX surface must be created at: a fractional scale makes the derived
     * value disagree with this one by a pixel, and every frame is then dropped
     * for not matching its surface.
     *
     * Valid from started() onwards.
     */
    QSize pixelSize() const;

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

    /**
     * The KWin-global logical rect of this session's captured output changed.
     *
     * For a virtual output this is first known some time after start(), once
     * KWin has created and placed the output; see outputGeometryResolved().
     */
    void outputGeometryChanged(const QRect &geometry);

    /**
     * The virtual output requested from KWin never showed up as a QScreen
     * (5 s). Pointer input stays gated; the stream itself may still run.
     */
    void virtualOutputUnresolved();

    /**
     * streamActive() changed. True arrives later than started(): the encoded
     * stream reports itself active only once its produce thread runs, which is
     * when frames can actually flow.
     */
    void streamActiveChanged(bool active);

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

    /**
     * Restart the encoded stream so a codec change (setVideoCodec()) takes effect on a stream that
     * is already running. The default implementation only logs; PlasmaScreencastV1Session restarts
     * through its existing deferred re-attach.
     */
    virtual void restartStreamForCodecChange();

private:
    // Replaces the signal-to-signal connect at stream()'s activeChanged: re-emits
    // streamActiveChanged, logs the encoder line, and reports the chroma capability.
    Q_SLOT void onStreamActiveChanged(bool active);

    class Private;
    const std::unique_ptr<Private> d;
};

}
