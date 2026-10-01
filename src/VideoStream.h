// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include <QImage>
#include <QObject>
#include <QVector>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QSize>
#include <QVector>

#include <freerdp/server/rdpgfx.h>

#include "SurfaceLayout.h"
#include "CodecPolicy.h"
#include "VideoCodecSupport.h"
#include "VideoFrame.h"
#include "krdp_export.h"

class VideoStreamCodecTest;

namespace KRdp
{

class RdpConnection;
class StatsReporter;
namespace Stats
{
struct Snapshot;
struct EventDetail;
}

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
     * RDP desktop space, anchored at the monitors' bounding union so that no
     * origin is negative. Entry \a i is the surface that frames with
     * `VideoFrame::monitorIndex == i` are sent to.
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
     * AUD-FIX10: the client asked for a repaint (RDP Refresh Rect, [MS-RDPBCGR] 2.2.11.2), e.g.
     * krdp-client 0.5.5 when a private-codec decoder has seen no frame on an idle desktop. Every
     * surface's session is asked for a keyframe (keyFrameRequested) at most once per
     * RefreshMinInterval per client; while output is suppressed nothing is asked (allowing it
     * asks anyway). Returns whether keyframes were requested. Any thread.
     */
    bool requestRefresh();
    static constexpr std::chrono::milliseconds RefreshMinInterval{1000};

    /**
     * A ResetGraphics (with its surfaces) has just gone out for \a monitors,
     * in RDP desktop space. Emitted from the frame submission thread;
     * connect with Qt::QueuedConnection. What a `KRDPCTL` `layout` record
     * waits for, so the client never reads a layout the wire has not
     * described yet (OPT-044).
     */
    Q_SIGNAL void graphicsReset(const QVector<KRdp::VideoMonitor> &monitors);

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

    /**
     * Set the standard AVC preference (main thread). Applies within the formats
     * allowed by the client's saved GFX caps and actual encoder availability;
     * an active private codec keeps running and uses this on AVC fallback.
     */
    void setCodecPreference(CodecPreference preference);
    CodecPreference codecPreference() const;
    /** Workers start unavailable until their own probe arrives. A running
     * encoder fallback can revoke availability. Does not expand client caps. */
    void setAvc444Available(bool available);
    /**
     * Select an own-client vendor codec after KRDPCTL capability exchange; nullopt = back to
     * the AVC codec the client's caps selected. Main thread only.
     */
    void setPrivateCodec(std::optional<VideoCodec> codec);
    /**
     * The encoders this host has and its `SoftwareEncoding` (AUD-FIX2). Set before the client's
     * `codec` request; main thread only. Without it only AVC is ever chosen.
     */
    void setEncoderPolicy(const CodecPolicy::Encoders &encoders, CodecPolicy::SoftwareEncoding mode);
    /// What setEncoderPolicy() set (less any encoder found unusable since). Main thread only.
    CodecPolicy::Encoders encoderPolicy() const;
    CodecPolicy::SoftwareEncoding softwareEncoding() const;
    /**
     * AV1-Q: the host's AV1 tile setting (CodecPolicy::parseAv1Tiles(): 0 = automatic) and the
     * client's decode path per codec (the `codec` request's `decode`; set before
     * setPrivateCodecPolicy()). Together they give encoderSettings()->av1Tiles
     * (CodecPolicy::resolveAv1Tiles()). Main thread only.
     */
    void setAv1TilesSetting(int tiles);
    int av1TilesSetting() const;
    void setClientDecode(const CodecPolicy::ClientDecode &decode);
    CodecPolicy::ClientDecode clientDecode() const;
    /// The AV1 tile count the encoders are told (EncoderSettings::av1Tiles).
    int av1Tiles() const;
    /**
     * The client's `codec` request: the private codecs it decodes (empty = AVC only) and whether
     * the server may switch codec mid-session (link and CPU, CodecPolicy). Chooses at once and
     * returns the choice, which the caller answers with. Main thread only.
     */
    CodecPolicy::Decision setPrivateCodecPolicy(const QVector<VideoCodec> &codecs, bool adaptive);
    /**
     * The running encoder could not produce \a codec (KPipeWire fell back to another encoder):
     * never choose it again on this connection, switch away at once and tell the client.
     */
    void privateCodecUnavailable(VideoCodec codec);
    /**
     * A session's encoder for \a codec opened on the hardware or software backend
     * (AbstractSession::encoderBackendReported). Logged; when it is not the backend the codec
     * policy announced for the current codec (KPipeWire's H.264 fallback from h264_vaapi to
     * libx264), the policy follows the real backend (the CPU guard applies to software) and an
     * own client gets a `codec` push with the real `backend`. Main thread only.
     */
    void encoderBackendReported(VideoCodec codec, bool hardware);
    /**
     * AUD-FIX7: the encoders the process that really encodes has (a console/virtual worker's own
     * probe) replace setEncoderPolicy()'s. When the running private codec has no encoder of the
     * chosen backend there, the policy moves off it at once (`codec` push "encoder unavailable").
     * Main thread only.
     */
    void updateEncoderPolicy(const CodecPolicy::Encoders &encoders);
    /// Whether the client asked for a private codec (setPrivateCodecPolicy() with a non-empty list).
    bool privateCodecPolicyActive() const;
    /**
     * AUD-FIX7: the CPU guard's input, the encoding process's CPU time in ns (monotonic; -1 =
     * unknown). Default: this process. A broker whose worker encodes passes the worker's report.
     */
    void setEncoderCpuTimeSource(std::function<qint64()> source);
    /**
     * What the encoders should run with besides the codec (backend, software preset, target
     * bitrate, frame-rate cap); nullopt until the client's `codec` request. Main thread only.
     */
    std::optional<CodecPolicy::EncoderSettings> encoderSettings() const;
    /**
     * encoderSettings() changed. Emitted before the matching negotiatedCodecChanged, so a
     * restarted encoder opens with its backend, preset and bitrate. Main thread.
     */
    Q_SIGNAL void encoderSettingsChanged(const KRdp::CodecPolicy::EncoderSettings &settings);
    /**
     * The codec chosen in onCapsAdvertise() from this preference and the
     * client's caps. nullopt until the client has advertised its caps.
     * May be read from any thread.
     */
    std::optional<VideoCodec> negotiatedCodec() const;
    /**
     * negotiatedCodec(), or the codec sessions are built for before caps are
     * known (VideoCodecSupport::expectedCodec(codecPreference())).
     */
    VideoCodec codecForSessions() const;
    /**
     * Emitted when the negotiated codec changes, from the FreeRDP peer
     * thread (onCapsAdvertise); connect with Qt::QueuedConnection.
     */
    Q_SIGNAL void negotiatedCodecChanged(KRdp::VideoCodec codec);

    /**
     * AUD-FIX4 D1: the in-flight window (FrameQueuePolicy::windowLimits()) and what it did.
     * Safe to read from any thread; the counters only grow.
     */
    struct FlowStats {
        int inFlight = 0; ///< frames sent and not yet acknowledged (while suspended: sent recently)
        int maxInFlight = 0; ///< the most ever in flight right after a send
        int windowFrames = 0; ///< the current frame limit
        qint64 windowBytes = 0; ///< the current byte budget
        quint64 sent = 0;
        quint64 acknowledged = 0;
        quint64 dropped = 0; ///< frames coalesced away while the window was full
        quint64 keyFrameRequests = 0; ///< keyframes asked for after coalescing
        quint64 keyFramesDeferred = 0; ///< AUD-FIX6 F2: looks at a starved monitor that found its keyframe still unacknowledged
        bool acksSuspended = false;
        qint64 ackLatencyUs = 0; ///< AUD-FIX7 F2: the recent frame-ack latency (p75) the window follows
        int frameRate = 0; ///< the frame rate asked of the source (throttled or not)
        bool linkClear = false; ///< the socket had sent everything at the last window check
    };
    FlowStats flowStats() const;

    /**
     * KRDPCTL `stats` (STATS-PANEL-DESIGN.md §5): \a rateHz > 0 subscribes this connection to
     * `stats-sample` records at that rate (clamped to 1-4 Hz, 1 Hz while the link is slow) and to
     * `stats-event` records; 0 unsubscribes. Returns the rate samples go out at (0 =
     * unsubscribed). Main thread only. The subscription also ends when the control channel is gone.
     */
    int setStatsSubscription(int rateHz);
    /// The connection's stats reporter (never null).
    StatsReporter *statsReporter() const;
    /// Whether a KRDPCTL client is subscribed to stats (a worker then sends EncoderStats).
    bool statsSubscribed() const;
    Q_SIGNAL void statsSubscriptionChanged(bool subscribed);
    /**
     * Stage 6 (EncoderStats): what the encoding worker reported for its last interval: frames its
     * encoders produced, frames it did not forward, and a measured encode time per frame (nullopt =
     * unknown). Main thread only.
     */
    void addWorkerEncoderStats(quint32 framesEncoded, quint32 framesSkipped, std::optional<double> encodeMs);
    /// A measured per-frame encode time (e.g. the AVC444 encoder's timing report). Main thread only.
    void setMeasuredEncodeTime(double ms);
    /// What the next `stats-sample` would be built from (tests). Main thread only.
    Stats::Snapshot statsSnapshot() const;
    /**
     * Stage 7: the last RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU the client sent (MS-RDPEGFX 2.2.2.21):
     * timeDiffSE = ms from the start to the end of the frame's decode, timeDiffEDR = ms from the
     * end of decoding to its render. nullopt before the client sent one. Any thread. `stats-sample`
     * carries their averages over the interval (flow.clientDecodeMs / clientRenderMs).
     */
    struct ClientQoe {
        quint32 frameId = 0;
        quint16 timeDiffSE = 0;
        quint16 timeDiffEDR = 0;
    };
    std::optional<ClientQoe> clientQoe() const;

    /**
     * AUD-FIX5: emitted once, when the client acknowledged its first frame (or suspended
     * acknowledgements): it has the graphics pipeline up and a picture. From the FreeRDP peer
     * thread; connect with Qt::QueuedConnection. Clipboard waits for it before asking the client
     * for anything (a stock client that blocks on the request must still get its picture).
     */
    Q_SIGNAL void graphicsDelivered();

    /**
     * Emitted when the adaptive-quality chroma rung requests the chroma stream be shed or restored
     * (S3). Stubbed out here (unemitted) so SessionController's connect compiles before S3 lands.
     */
    Q_SIGNAL void requestedChromaChanged(bool enabled);
    /** Current adaptive chroma demand, readable from any thread. */
    bool requestedChroma() const;
    /**
     * Whether the running encoder actually produces the chroma stream, as reported by the session(s)
     * (AbstractSession::chromaCapabilityChanged). Stub: stores nothing until S3 uses it to AND the
     * chroma rung out of the adaptive ladder when the encoder cannot deliver it.
     */
    void setChromaCapable(bool capable);

private:
    friend class ::VideoStreamCodecTest; // drives applyCodecDecision() with the CPU guard's decisions
    friend BOOL gfxChannelIdAssigned(RdpgfxServerContext *, uint32_t);
    friend uint32_t gfxCapsAdvertise(RdpgfxServerContext *, const RDPGFX_CAPS_ADVERTISE_PDU *);
    friend uint32_t gfxFrameAcknowledge(RdpgfxServerContext *, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *);
    friend uint32_t gfxQoEFrameAcknowledge(RdpgfxServerContext *, const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU *);

    bool onChannelIdAssigned(uint32_t channelId);
    uint32_t onCapsAdvertise(const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise);
    uint32_t onFrameAcknowledge(const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge);
    uint32_t onQoeFrameAcknowledge(const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU *qoe);
    /// The backend of the encoder that runs for stats (the policy's, or the last report). Main thread.
    std::optional<bool> statsHardware() const;
    /// Refreshes the backend stats events made on other threads carry. Main thread.
    void updateStatsBackend();
    /// A stats event's detail: \a reason plus the codec and backend of the moment. Any thread.
    Stats::EventDetail statsDetail(const QString &reason) const;

    // Driven by the main-thread adaptive timer, which fires every
    // QualityUpdateInterval while streaming (started in initialize(), stopped
    // in close()); this slot therefore runs on VideoStream's own (main) thread.
    Q_SLOT void updateAdaptiveQuality();
    /// One CodecPolicy step (link, CPU guard, switch interval); from updateAdaptiveQuality().
    void stepCodecPolicy(bool congested);
    /// Applies a changed decision: the codec id, the encoder restart, the `codec` push.
    void applyCodecDecision(const CodecPolicy::Decision &decision);
    /// A policy step outside the interval (the switch interval does not apply), with \a reason.
    void restepCodecPolicyNow(const QString &reason);
    /// AUD-FIX7 F2: the frame rate asked of the source: the policy's, or the delivery throttle's.
    void refreshFrameRate();
    /// AUD-FIX7 F2: one delivery-throttle step per adaptive interval; also the backlog threshold.
    void updateDeliveryThrottle(bool windowPressure);
    /// AUD-FIX13: one LinkEvidence step per adaptive interval (before the codec policy's): the
    /// socket's view of \a congested, the client-limited state and the stats' `policy.limit`.
    void judgeLink(bool congested);
    void applyEncoderSettings(const CodecPolicy::EncoderSettings &settings);

    /**
     * Send ResetGraphics for \a monitors (already in RDP desktop space, with
     * the desktop itself \a desktopSize) and re-create one surface per entry
     * of \a surfaces.
     *
     * Call with the private layout mutex held; it publishes the new surface
     * vector. Returns false when the client refused to create a surface, in
     * which case no surfaces are left behind and a reset is re-armed for the
     * next frame.
     */
    bool performReset(const QSize &desktopSize, const QVector<VideoMonitor> &monitors, const QVector<SurfaceLayout::Entry> &surfaces);
    /**
     * Returns false only when the frame could not be sent because the GFX
     * channel is not ready (context gone or caps reset mid-flight); the caller
     * then keeps the frame at the head of the queue instead of dropping it.
     */
    bool sendFrame(const VideoFrame &frame);
    /// AUD-FIX4 D1 (submission thread): whether the in-flight window has room for a frame now.
    bool windowOpen(std::chrono::steady_clock::time_point now);
    /// AUD-FIX4 D1 (submission thread): coalesce what waits while the window is full.
    void holdFrames(std::chrono::steady_clock::time_point now);
    /// AUD-FIX4 D1 (submission thread): (re)request keyframes for monitors whose P-frames are dropped.
    void requestStarvedKeyFrames(std::chrono::steady_clock::time_point now);

    class Private;
    const std::unique_ptr<Private> d;
};

}
