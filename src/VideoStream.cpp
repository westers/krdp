// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// This file is roughly based on grd-rdp-graphics-pipeline.c from Gnome Remote
// Desktop which is:
//
// SPDX-FileCopyrightText: 2021 Pascal Nowack
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoStream.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <time.h>
#include <vector>

#include <QDateTime>
#include <QQueue>
#include <QRect>
#include <QStringList>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

#include "AdaptiveQuality.h"
#include "FrameQueuePolicy.h"
#include "GfxSurfaceCommand.h"
#include "LayoutControl.h"
#include "NetworkDetection.h"
#include "PeerContext_p.h"
#include "RdpConnection.h"

#include "krdp_logging.h"

namespace KRdp
{

namespace clk = std::chrono;

constexpr uint16_t MaxRdpCoordinate = std::numeric_limits<uint16_t>::max();
constexpr int MaxMonitorLayoutCount = 16;
constexpr auto KeyFrameRequestMinInterval = clk::seconds(2);
constexpr auto QualityUpdateInterval = clk::milliseconds(1500);
// Don't take adaptive-quality decisions until NetworkDetection has had a few
// RTT probes (every 70 ms) to establish a minimum-RTT baseline.
constexpr auto WarmupAfterStreamStart = clk::seconds(3);
// AUD-FIX4 D1: while the in-flight window is full the submission thread looks
// again this often (an ack also wakes it at once).
constexpr auto WindowPollInterval = clk::milliseconds(10);
// A decision interval in which frames waited this long in total for the window,
// or any frame was coalesced away, counts as congestion for adaptive quality and
// the codec policy.
constexpr auto WindowPressureHeld = clk::milliseconds(200);
// Between two info-level "window full" lines.
constexpr auto WindowLogInterval = clk::seconds(5);

// Atomically lowers `value` to `candidate` if it's lower, otherwise leaves it
// alone. Used to track how close the client got to caught up since the
// adaptive-quality decision last looked - see minPendingAfterAckSinceDecision.
void bumpAtomicMin(std::atomic<int> &value, int candidate)
{
    int current = value.load(std::memory_order_relaxed);
    while (candidate < current && !value.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

RECTANGLE_16 toRdpRect(const QRect &rect)
{
    auto left = std::clamp(rect.x(), 0, int(MaxRdpCoordinate));
    auto top = std::clamp(rect.y(), 0, int(MaxRdpCoordinate));
    auto right = std::clamp(rect.x() + rect.width(), 0, int(MaxRdpCoordinate));
    auto bottom = std::clamp(rect.y() + rect.height(), 0, int(MaxRdpCoordinate));

    if (right <= left) {
        right = std::min(left + 1, int(MaxRdpCoordinate));
    }
    if (bottom <= top) {
        bottom = std::min(top + 1, int(MaxRdpCoordinate));
    }

    RECTANGLE_16 region;
    region.left = static_cast<UINT16>(left);
    region.top = static_cast<UINT16>(top);
    region.right = static_cast<UINT16>(right);
    region.bottom = static_cast<UINT16>(bottom);
    return region;
}

QVector<VideoMonitor> monitorLayoutForReset(const VideoFrame &frame)
{
    QVector<VideoMonitor> monitors;
    if (frame.size.isEmpty()) {
        return monitors;
    }

    const QRect frameBounds(QPoint(0, 0), frame.size);
    monitors.reserve(std::min<qsizetype>(frame.monitors.size(), MaxMonitorLayoutCount));
    for (const auto &candidate : frame.monitors) {
        if (monitors.size() >= MaxMonitorLayoutCount) {
            break;
        }

        auto clippedGeometry = candidate.geometry.intersected(frameBounds);
        if (clippedGeometry.isEmpty()) {
            continue;
        }
        monitors.push_back(VideoMonitor{
            .geometry = clippedGeometry,
            .primary = candidate.primary,
        });
    }

    if (monitors.isEmpty()) {
        monitors.push_back(VideoMonitor{
            .geometry = frameBounds,
            .primary = true,
        });
    }

    bool foundPrimary = false;
    for (auto &monitor : monitors) {
        if (monitor.primary && !foundPrimary) {
            foundPrimary = true;
        } else {
            monitor.primary = false;
        }
    }
    if (!foundPrimary) {
        monitors.first().primary = true;
    }

    return monitors;
}

QString monitorLayoutSummary(const QVector<VideoMonitor> &monitors)
{
    QStringList parts;
    parts.reserve(monitors.size());
    for (const auto &monitor : monitors) {
        parts.push_back(QStringLiteral("%1,%2 %3x%4%5")
                            .arg(monitor.geometry.x())
                            .arg(monitor.geometry.y())
                            .arg(monitor.geometry.width())
                            .arg(monitor.geometry.height())
                            .arg(monitor.primary ? QStringLiteral(" primary") : QString()));
    }
    return parts.join(QStringLiteral("; "));
}

// Everything a reset needs: what ResetGraphics advertises and which surfaces
// carry it. Kept in one place so the single-surface and per-monitor paths
// cannot drift apart.
struct ResetPlan {
    QSize desktopSize;
    QVector<VideoMonitor> monitors;
    QVector<SurfaceLayout::Entry> surfaces;
};

ResetPlan planReset(const VideoFrame &frame, const QVector<VideoMonitor> &configuredLayout)
{
    ResetPlan plan;

    if (configuredLayout.isEmpty()) {
        // No explicit layout: one surface covering the whole frame, with the
        // frame's own monitor list advertised inside it. This is what every
        // mode but MonitorMode=multi does, and it is unchanged.
        plan.desktopSize = frame.size;
        plan.monitors = monitorLayoutForReset(frame);
        plan.surfaces = {SurfaceLayout::Entry{
            .size = frame.size,
            .origin = QPoint(0, 0),
            .primary = true,
        }};
        return plan;
    }

    // One surface per monitor, each mapped at its own RDP-space origin. The
    // entries are consumed exactly as the helper produced them: it is the only
    // place the layout is translated, and it already anchored the union at
    // (0, 0), so the bounds below are the desktop extent.
    plan.surfaces = SurfaceLayout::fromMonitors(configuredLayout);
    plan.monitors.reserve(plan.surfaces.size());
    QRect bounds;
    for (const auto &entry : plan.surfaces) {
        const QRect geometry(entry.origin, entry.size);
        plan.monitors.push_back(VideoMonitor{
            .geometry = geometry,
            .primary = entry.primary,
        });
        bounds = bounds.united(geometry);
    }
    plan.desktopSize = bounds.size();

    return plan;
}

struct RdpCapsInformation {
    uint32_t version;
    RDPGFX_CAPSET capSet;
    bool avcSupported : 1 = false;
    bool yuv420Supported : 1 = false;
};

const char *capVersionToString(uint32_t version)
{
    switch (version) {
    case RDPGFX_CAPVERSION_107:
        return "RDPGFX_CAPVERSION_107";
    case RDPGFX_CAPVERSION_106:
        return "RDPGFX_CAPVERSION_106";
    case RDPGFX_CAPVERSION_105:
        return "RDPGFX_CAPVERSION_105";
    case RDPGFX_CAPVERSION_104:
        return "RDPGFX_CAPVERSION_104";
    case RDPGFX_CAPVERSION_103:
        return "RDPGFX_CAPVERSION_103";
    case RDPGFX_CAPVERSION_102:
        return "RDPGFX_CAPVERSION_102";
    case RDPGFX_CAPVERSION_101:
        return "RDPGFX_CAPVERSION_101";
    case RDPGFX_CAPVERSION_10:
        return "RDPGFX_CAPVERSION_10";
    case RDPGFX_CAPVERSION_81:
        return "RDPGFX_CAPVERSION_81";
    case RDPGFX_CAPVERSION_8:
        return "RDPGFX_CAPVERSION_8";
    default:
        return "UNKNOWN_VERSION";
    }
}

BOOL gfxChannelIdAssigned(RdpgfxServerContext *context, uint32_t channelId)
{
    auto stream = reinterpret_cast<VideoStream *>(context->custom);
    if (stream->onChannelIdAssigned(channelId)) {
        return TRUE;
    }
    return FALSE;
}

uint32_t gfxCapsAdvertise(RdpgfxServerContext *context, const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise)
{
    auto stream = reinterpret_cast<VideoStream *>(context->custom);
    return stream->onCapsAdvertise(capsAdvertise);
}

uint32_t gfxFrameAcknowledge(RdpgfxServerContext *context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge)
{
    auto stream = reinterpret_cast<VideoStream *>(context->custom);
    return stream->onFrameAcknowledge(frameAcknowledge);
}

uint32_t gfxQoEFrameAcknowledge(RdpgfxServerContext *, const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU *)
{
    return CHANNEL_RC_OK;
}

struct Surface {
    uint16_t id = 0;
    QSize size;
    // Top-left in RDP desktop space, as passed to MapSurfaceToOutput.
    QPoint origin;
    // AVC444: whether a keyframe has ever been sent on this surface. An aux-only refresh (LC = 2)
    // needs the decoder the main IDR set up, so it is dropped until this is true; a re-created
    // surface (performReset()) starts a fresh Surface and so starts over.
    bool keyFrameSent = false;
};

class KRDP_NO_EXPORT VideoStream::Private
{
public:
    using RdpGfxContextPtr = std::unique_ptr<RdpgfxServerContext, decltype(&rdpgfx_server_context_free)>;

    RdpConnection *session;

    RdpGfxContextPtr gfxContext = RdpGfxContextPtr(nullptr, rdpgfx_server_context_free);

    uint32_t frameId = 0;
    uint32_t channelId = 0;

    uint16_t nextSurfaceId = 1;
    // surfaces/monitorLayout/configuredLayout are touched by three threads -
    // the frame submission thread (performReset, sendFrame), the FreeRDP peer
    // thread (onCapsAdvertise) and the main thread (setMonitorLayout) - so
    // they need a lock. The single POD Surface they replace did not: clearing
    // a QVector frees storage another thread may still be reading.
    std::mutex layoutMutex;
    // One surface per monitor; index == VideoFrame::monitorIndex. Exactly one
    // entry unless a layout was configured.
    QVector<Surface> surfaces;

    // Set by reset() on the main thread, consumed on the submission thread.
    std::atomic<bool> pendingReset = true;
    // AUD-P7: set on the session thread (RdpConnection::run), read on the main
    // thread (queueFrame) and the submission thread.
    std::atomic<bool> enabled = false;
    // Written on the FreeRDP peer thread (onCapsAdvertise), read by the submission thread.
    std::atomic<bool> capsConfirmed = false;

    std::jthread frameSubmissionThread;
    std::mutex frameQueueMutex;
    std::condition_variable frameQueueCondition;

    QQueue<VideoFrame> frameQueue;
    // pendingFrames is inserted on the submission thread (sendFrame) and erased on
    // the FreeRDP peer thread (onFrameAcknowledge).
    // AUD-P6: also knows when the client suspended acknowledgements.
    FrameQueuePolicy::FrameAckTracker pendingFrames;
    std::mutex pendingFramesMutex;

    // Fixed at the client-configured rate; read by the submission thread for its
    // idle sleep interval. The RTT-derived source-rate heuristic was removed
    // (upstream 978f1cb): it starved high-latency links without measuring real
    // send-side pressure.
    std::atomic_int requestedFrameRate = 60;
    // The monitor list as last advertised in ResetGraphics, in RDP desktop
    // space; a difference against the freshly planned one triggers a reset.
    QVector<VideoMonitor> monitorLayout;
    // The layout set by setMonitorLayout(); empty means "derive it from the
    // frames", i.e. the single-surface behaviour.
    QVector<VideoMonitor> configuredLayout;
    // Submission-thread only: rate-limits keyFrameRequested so a reset storm
    // (e.g. repeated caps re-advertisement) does not restart the encoder more
    // than once per KeyFrameRequestMinInterval. One timestamp per surface.
    QVector<clk::steady_clock::time_point> lastKeyFrameRequest;
    // Latches so a frame for a surface that does not exist, a frame whose size
    // does not match its surface, a failed CreateSurface or a session reporting
    // a broken layout is reported once and not once per frame. The first three
    // are cleared by a successful reset, so a later, different misconfiguration
    // is still reported.
    bool loggedUnknownSurface = false;
    bool loggedSizeMismatch = false;
    bool loggedCreateSurfaceFailure = false;
    bool warnedInvalidLayout = false;

    // setQualityCap()/setAdaptiveQuality() and updateAdaptiveQuality() (the
    // adaptiveTimer slot) all run on the main thread - VideoStream is
    // constructed in RdpConnection's constructor and never moved to another
    // thread. These stay atomic as belt-and-braces in case that ever changes,
    // not because it's true today.
    std::atomic<quint8> quality = 100; // current adaptive value
    std::atomic<quint8> qualityCap = 100; // configured Quality
    std::atomic<bool> graphicsDelivered = false; // see VideoStream::graphicsDelivered()
    std::atomic<bool> adaptiveQuality = true;
    // setCodecPreference()/codecPreference() are main-thread only (set before
    // caps are advertised); onCapsAdvertise() (peer thread) only reads it.
    CodecPreference codecPreference = CodecPreference::Auto;
    // -1 = standard RDPGFX negotiation. Set by the main-thread KRDPCTL preflight before
    // sessions exist; read by the peer and submission threads beside negotiatedCodec.
    std::atomic<int> privateCodec = -1;
    // The AVC codec the client's caps selected (onCapsAdvertise, peer thread); -1 = none yet.
    // What setPrivateCodec(nullopt) goes back to.
    std::atomic<int> capsCodec = -1;
    // AUD-FIX2 codec policy (CodecPolicy.h); main thread only. Active once the client asked for
    // at least one private codec.
    CodecPolicy::Encoders encoders; // default: none, so only AVC until setEncoderPolicy()
    CodecPolicy::SoftwareEncoding softwareEncoding = CodecPolicy::SoftwareEncoding::Auto;
    bool codecPolicyActive = false;
    QList<CodecPolicy::Family> clientFamilies;
    bool codecPolicyAdaptive = true;
    CodecPolicy::State codecPolicy;
    CodecPolicy::LoadWindow encodeLoad;
    // What the sessions' encoders were last told (encoderSettingsChanged); main thread only.
    std::optional<CodecPolicy::EncoderSettings> encoderSettings;
    // The backend an encoder of the current codec reported, and whether the client heard it.
    std::optional<bool> reportedHardware;
    // Encode-cost sampling for the CPU guard (CodecPolicy::encodeLoadSample()): process CPU time
    // per encoded frame over the encoder's parallelism, against the frame budget, and the
    // delivered frame rate. framesEncoded counts queueFrame() calls.
    std::atomic<int> framesEncoded = 0;
    int framesAtLastSample = 0;
    qint64 cpuNsAtLastSample = -1;
    clk::steady_clock::time_point loadSampledAt{};
    // -1 = not negotiated yet (no CapsAdvertise received). Written on the
    // FreeRDP peer thread (onCapsAdvertise), read from any thread via
    // negotiatedCodec()/codecForSessions().
    std::atomic<int> negotiatedCodec = -1;
    // The adaptive-quality chroma rung's state (AdaptiveQuality::Input::chromaEnabled/
    // Result::chromaEnabled), and whether the running encoder actually reports the aux stream
    // (setChromaCapable(), from AbstractSession::chromaCapabilityChanged via SessionWrapper).
    // Read/written on the main thread (updateAdaptiveQuality(), setAdaptiveQuality(),
    // onCapsAdvertise()) and setChromaCapable() (whichever thread the session signal arrives on);
    // atomic as belt-and-braces like the other quality state above.
    std::atomic<bool> chromaEnabled = true;
    std::atomic<bool> chromaCapable = false;
    // The total pixel count over every surface, as a single atomic value, so
    // updateAdaptiveQuality() (main thread) never sees a torn width/height
    // while performReset() (submission thread) or onCapsAdvertise() (peer
    // thread) rebuilds the surfaces. Summed because one quality steers every
    // session feeding this connection.
    std::atomic<qint64> surfacePixels = 0;
    // AUD-FIX4 D1: the in-flight window. Its inputs are refreshed on the main thread
    // (updateAdaptiveQuality()) and read by the submission thread.
    std::atomic<qint64> windowMinRttUs = 0;
    std::atomic<quint32> windowRateKbps = 0;
    std::atomic<int> surfaceCount = 1;
    // Monitors whose queued frames were coalesced away: their P-frames are dropped until a
    // keyframe arrives (queueFrame()). Guarded by frameQueueMutex, like the queue.
    std::vector<char> starvedMonitors;
    std::vector<clk::steady_clock::time_point> starvedKeyFrameRequestAt;
    // Time frames waited for the window and frames coalesced since the last decision
    // (updateAdaptiveQuality() reads and resets them).
    std::atomic<qint64> heldNsSinceDecision = 0;
    std::atomic<int> droppedSinceDecision = 0;
    // Submission thread only. heldSince: the last look at a full window (epoch = not held).
    clk::steady_clock::time_point heldSince{};
    clk::steady_clock::time_point lastWindowLog{};
    bool loggedAckTimeout = false;
    // flowStats().
    std::atomic<int> statInFlight = 0;
    std::atomic<int> statMaxInFlight = 0;
    std::atomic<int> statWindowFrames = FrameQueuePolicy::MinInFlightFrames;
    std::atomic<qint64> statWindowBytes = FrameQueuePolicy::MinInFlightBytes;
    std::atomic<quint64> statSent = 0;
    std::atomic<quint64> statAcknowledged = 0;
    std::atomic<quint64> statDropped = 0;
    std::atomic<quint64> statKeyFrameRequests = 0;
    std::atomic<bool> statSuspended = false;

    FrameQueuePolicy::WindowLimits windowLimits() const
    {
        return FrameQueuePolicy::windowLimits(requestedFrameRate.load(), surfaceCount.load(), clk::microseconds(windowMinRttUs.load()), windowRateKbps.load());
    }
    // Call with frameQueueMutex held.
    bool starved(int monitorIndex) const
    {
        return monitorIndex >= 0 && size_t(monitorIndex) < starvedMonitors.size() && starvedMonitors[size_t(monitorIndex)];
    }
    // Call with frameQueueMutex held.
    void clearStarved()
    {
        std::fill(starvedMonitors.begin(), starvedMonitors.end(), 0);
    }
    // Backlog evidence for the adaptive-quality decision: the smallest number
    // of frames still unacknowledged right after any ack since the last
    // decision (INT_MAX = no ack since). Lowered on the connection thread in
    // onFrameAcknowledge() under pendingFramesMutex (and to 0 wherever
    // pendingFrames is cleared - a reset counts as caught up); read-and-reset
    // by updateAdaptiveQuality() on the main thread.
    std::atomic<int> minPendingAfterAckSinceDecision = std::numeric_limits<int>::max();
    // Main-thread timer that drives updateAdaptiveQuality() every
    // QualityUpdateInterval while streaming; started/stopped via queued
    // invokeMethod so initialize()/close() may run on any thread.
    QTimer adaptiveTimer;
    clk::steady_clock::time_point streamingSince;
    clk::steady_clock::time_point lastStepDown; // default = epoch = "never"

    // Call with layoutMutex held. Null when no surface carries that index,
    // which is every index but 0 while no layout is configured.
    Surface *surfaceFor(int index)
    {
        if (index < 0 || index >= surfaces.size()) {
            return nullptr;
        }
        return &surfaces[index];
    }
};


VideoStream::VideoStream(RdpConnection *session)
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
    d->session = session;

    // Needed for the queued negotiatedCodecChanged connection (emitted from the
    // FreeRDP peer thread) and, once S2+ send them, chromaTimingReported.
    qRegisterMetaType<KRdp::VideoCodec>();
    qRegisterMetaType<KRdp::ChromaTimingReport>();
    qRegisterMetaType<KRdp::CodecPolicy::EncoderSettings>();

    d->adaptiveTimer.setInterval(QualityUpdateInterval);
    d->adaptiveTimer.setTimerType(Qt::CoarseTimer);
    connect(&d->adaptiveTimer, &QTimer::timeout, this, &VideoStream::updateAdaptiveQuality);
}

VideoStream::~VideoStream()
{
    close();
}

bool VideoStream::initialize()
{
    if (d->gfxContext) {
        return true;
    }

    auto peerContext = reinterpret_cast<PeerContext *>(d->session->rdpPeerContext());

    d->gfxContext = Private::RdpGfxContextPtr{rdpgfx_server_context_new(peerContext->virtualChannelManager), rdpgfx_server_context_free};
    if (!d->gfxContext) {
        qCWarning(KRDP) << "Failed creating RDPGFX context";
        return false;
    }

    d->gfxContext->ChannelIdAssigned = gfxChannelIdAssigned;
    d->gfxContext->CapsAdvertise = gfxCapsAdvertise;
    d->gfxContext->FrameAcknowledge = gfxFrameAcknowledge;
    d->gfxContext->QoeFrameAcknowledge = gfxQoEFrameAcknowledge;

    d->gfxContext->custom = this;
    d->gfxContext->rdpcontext = d->session->rdpPeerContext();

    if (!d->gfxContext->Open(d->gfxContext.get())) {
        qCWarning(KRDP) << "Could not open GFX context";
        return false;
    }

    d->frameSubmissionThread = std::jthread([this](std::stop_token token) {
        while (!token.stop_requested()) {
            // Don't dequeue frames until the GFX channel is ready. This keeps the
            // initial keyframe in the queue until CapsAdvertise has completed and
            // we can actually send it; otherwise the client shows a black screen
            // until the next keyframe.
            if (!d->gfxContext || !d->capsConfirmed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            requestStarvedKeyFrames(clk::steady_clock::now());

            VideoFrame nextFrame;
            {
                std::unique_lock lock(d->frameQueueMutex);
                auto frameInterval = std::chrono::milliseconds(1000 / std::max(d->requestedFrameRate.load(), 1));
                d->frameQueueCondition.wait_for(lock, frameInterval, [this, token]() {
                    return token.stop_requested() || !d->frameQueue.isEmpty();
                });
                if (token.stop_requested()) {
                    break;
                }
                if (d->frameQueue.isEmpty()) {
                    continue;
                }
            }

            // AUD-FIX4 D1: never more than the window in flight. While it is full the
            // frames wait here (coalesced beyond MaxHeldFramesPerMonitor), and the peer
            // thread, whose channel queue now holds at most a window of video, gets
            // back to reading the client's acks and input.
            const auto now = clk::steady_clock::now();
            if (!windowOpen(now)) {
                // Counted as it goes, so a hold that lasts past a decision still shows in it.
                if (d->heldSince != clk::steady_clock::time_point{}) {
                    d->heldNsSinceDecision.fetch_add(clk::duration_cast<clk::nanoseconds>(now - d->heldSince).count(), std::memory_order_relaxed);
                }
                d->heldSince = now;
                holdFrames(now);
                std::unique_lock lock(d->frameQueueMutex);
                d->frameQueueCondition.wait_for(lock, WindowPollInterval); // an ack notifies
                continue;
            }
            if (d->heldSince != clk::steady_clock::time_point{}) {
                d->heldNsSinceDecision.fetch_add(clk::duration_cast<clk::nanoseconds>(now - d->heldSince).count(), std::memory_order_relaxed);
                d->heldSince = {};
            }
            {
                std::lock_guard lock(d->frameQueueMutex);
                if (d->frameQueue.isEmpty()) {
                    continue; // cleared meanwhile (layout change, disable)
                }
                // Always send in order: every encoded P-frame references the one
                // before it, so skipping a queued frame would corrupt the client's
                // decode until the next keyframe. The queue is bounded by
                // queueFrame() clearing it whenever a new keyframe arrives, and
                // while the window is full by holdFrames().
                nextFrame = d->frameQueue.takeFirst();
            }
            if (!sendFrame(nextFrame)) {
                // Caps were reset between dequeue and send (e.g. a client
                // re-advertisement); hold the frame rather than lose it. If it
                // is the initial IDR, losing it means a blank client until the
                // next keyframe.
                std::lock_guard lock(d->frameQueueMutex);
                d->frameQueue.prepend(nextFrame);
            }
        }
    });

    qCDebug(KRDP) << "Video stream initialized";

    d->streamingSince = clk::steady_clock::now();
    d->minPendingAfterAckSinceDecision = std::numeric_limits<int>::max();
    QMetaObject::invokeMethod(&d->adaptiveTimer, qOverload<>(&QTimer::start), Qt::QueuedConnection);

    return true;
}

void VideoStream::close()
{
    QMetaObject::invokeMethod(&d->adaptiveTimer, &QTimer::stop, Qt::QueuedConnection);

    if (!d->gfxContext) {
        return;
    }

    // Stop the frame submission thread first to prevent use-after-free on
    // gfxContext during close.
    if (d->frameSubmissionThread.joinable()) {
        d->frameSubmissionThread.request_stop();
        d->frameQueueCondition.notify_all();
        d->frameSubmissionThread.join();
    }

    {
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.clear();
        d->statInFlight = 0;
        bumpAtomicMin(d->minPendingAfterAckSinceDecision, 0);
    }
    {
        std::lock_guard lock(d->frameQueueMutex);
        d->frameQueue.clear();
        d->clearStarved();
    }

    d->gfxContext->Close(d->gfxContext.get());
    d->gfxContext.reset();

    Q_EMIT closed();
}

void VideoStream::queueFrame(const KRdp::VideoFrame &frame)
{
    if (d->session->state() != RdpConnection::State::Streaming || !d->enabled) {
        return;
    }
    d->framesEncoded.fetch_add(1, std::memory_order_relaxed); // the codec policy's CPU guard

    {
        std::lock_guard lock(d->frameQueueMutex);
        // AUD-FIX4 D1: this monitor's queued frames were coalesced away while the
        // window was full, so a P-frame has no reference on the client: drop it
        // until the keyframe that was asked for (or the next organic one) arrives.
        if (d->starved(frame.monitorIndex)) {
            if (!frame.isKeyFrame || frame.data.isEmpty()) {
                d->statDropped.fetch_add(1, std::memory_order_relaxed);
                d->droppedSinceDecision.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            d->starvedMonitors[size_t(frame.monitorIndex)] = 0;
        }
        // A keyframe supersedes everything of its OWN monitor still waiting to
        // be sent, so that monitor's queue can never grow beyond one keyframe
        // interval. Never drop anything else: encoded P-frames must be sent in
        // order, and another monitor's queued frames come from another encoder
        // whose reference chain this keyframe says nothing about. With a single
        // surface every frame carries index 0, so this still clears the queue.
        if (frame.isKeyFrame) {
            FrameQueuePolicy::dropSupersededFrames(d->frameQueue, frame.monitorIndex);
        }
        d->frameQueue.append(frame);
    }
    d->frameQueueCondition.notify_one();
}

void VideoStream::reset()
{
    d->pendingReset = true;
}

bool VideoStream::enabled() const
{
    return d->enabled;
}

void VideoStream::setEnabled(bool enabled)
{
    if (d->enabled.exchange(enabled) == enabled) {
        return;
    }

    if (!enabled) {
        std::lock_guard lock(d->frameQueueMutex);
        d->frameQueue.clear();
        d->clearStarved();
    }
    Q_EMIT enabledChanged();
}

uint32_t VideoStream::requestedFrameRate() const
{
    return d->requestedFrameRate;
}

void VideoStream::setQualityCap(quint8 cap)
{
    d->qualityCap = cap;
    const quint8 current = d->quality.load();
    const quint8 next = (d->adaptiveQuality.load() || d->session->audioPriorityActive()) ? std::min(current, cap) : cap;
    d->quality = next;
    // Always emit, even when next == current: this is the only path that
    // tells a brand-new session its quality (SessionController no longer
    // calls session->setVideoQuality() directly while adaptive quality is on,
    // to avoid desyncing the encoder from d->quality - see SessionController::
    // setQuality()/onNewConnection()), and a fresh session needs that push
    // even when the computed value happens to match VideoStream's default.
    // The session/KPipeWire chain already no-ops on an unchanged value, so
    // this costs nothing when it really is a no-op.
    Q_EMIT requestedQualityChanged(next);
}

void VideoStream::setMonitorLayout(const QVector<VideoMonitor> &layout)
{
    if (!layout.isEmpty()) {
        const auto primaryCount = std::count_if(layout.cbegin(), layout.cend(), [](const VideoMonitor &monitor) {
            return monitor.primary;
        });
        const bool hasEmptyGeometry = std::any_of(layout.cbegin(), layout.cend(), [](const VideoMonitor &monitor) {
            return monitor.geometry.isEmpty();
        });

        if (hasEmptyGeometry || primaryCount != 1 || layout.size() > MaxMonitorLayoutCount) {
            // A session reports a null output geometry while KWin removes and
            // re-adds its outputs on a DPMS wake; tearing every surface down
            // over that is worse than streaming the previous layout until the
            // session settles. Warn once per run of bad layouts, not per call.
            if (!d->warnedInvalidLayout) {
                d->warnedInvalidLayout = true;
                qCWarning(KRDP) << "Ignoring invalid monitor layout:" << monitorLayoutSummary(layout) << "- keeping the previous one";
            }
            return;
        }
        d->warnedInvalidLayout = false;
    }

    std::lock_guard lock(d->layoutMutex);
    if (d->configuredLayout == layout) {
        return;
    }

    d->configuredLayout = layout;
    qCDebug(KRDP) << "Monitor layout configured:" << (layout.isEmpty() ? QStringLiteral("(derived from frames)") : monitorLayoutSummary(layout));
    // The surfaces are rebuilt from the new layout on the next frame.
    d->pendingReset = true;

    // A layout change is intentional, unlike the caps re-advertisement storm
    // the per-index limiter (sendFrame()) exists to bound: reset every entry
    // so a kept surface's fresh surface after a second layout change within
    // 2 s of the first still gets its own IDR request.
    std::fill(d->lastKeyFrameRequest.begin(), d->lastKeyFrameRequest.end(), clk::steady_clock::time_point{});

    // Every queued frame is stamped with an index into the layout that is being
    // replaced. Sending one after the surfaces are rebuilt would paint a
    // monitor's picture onto whatever surface now holds that index, or have it
    // silently dropped by the size guard. The sessions re-open each surface
    // with a keyframe, so nothing of value is lost by starting from empty.
    //
    // Lock order is layoutMutex -> frameQueueMutex, and only ever that way: the
    // submission thread releases frameQueueMutex before sendFrame() takes
    // layoutMutex, and queueFrame() takes frameQueueMutex alone.
    std::lock_guard queueLock(d->frameQueueMutex);
    d->frameQueue.clear();
    d->clearStarved(); // the new surfaces start from keyframes (sendFrame() asks for them)
}

void VideoStream::setAdaptiveQuality(bool enabled)
{
    if (d->adaptiveQuality.exchange(enabled) == enabled) {
        return;
    }
    if (!enabled && !d->session->audioPriorityActive()) {
        const quint8 cap = d->qualityCap.load();
        const quint8 previous = d->quality.exchange(cap);
        if (previous != cap) {
            Q_EMIT requestedQualityChanged(cap);
        }
        if (!d->chromaEnabled.exchange(true)) {
            Q_EMIT requestedChromaChanged(true);
        }
    }
}

void VideoStream::setCodecPreference(CodecPreference preference)
{
    d->codecPreference = preference;
}

CodecPreference VideoStream::codecPreference() const
{
    return d->codecPreference;
}

void VideoStream::setPrivateCodec(std::optional<VideoCodec> codec)
{
    const int value = codec ? int(*codec) : -1;
    d->privateCodec.store(value);
    // Back to AVC: the codec the caps selected, not whatever private codec ran before, so the
    // RDPGFX codec id always matches the encoder the sessions restart with.
    // Without caps yet: undecided again (onCapsAdvertise() decides), and sessions go back to
    // the codec they are built for before caps.
    const int next = codec ? value : d->capsCodec.load();
    const int previous = d->negotiatedCodec.exchange(next);
    if (previous != next) {
        Q_EMIT negotiatedCodecChanged(next >= 0 ? VideoCodec(next) : VideoCodecSupport::expectedCodec(d->codecPreference));
    }
}

namespace
{
CodecPolicy::Family familyOf(VideoCodec codec)
{
    return codec == VideoCodec::Hevc ? CodecPolicy::Family::Hevc : codec == VideoCodec::Av1 ? CodecPolicy::Family::Av1 : CodecPolicy::Family::Avc;
}
std::optional<VideoCodec> privateCodecOf(CodecPolicy::Family family)
{
    switch (family) {
    case CodecPolicy::Family::Hevc:
        return VideoCodec::Hevc;
    case CodecPolicy::Family::Av1:
        return VideoCodec::Av1;
    case CodecPolicy::Family::Avc:
        break;
    }
    return std::nullopt;
}
qint64 processCpuNs()
{
    timespec ts{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
        return -1;
    }
    return qint64(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
}

void VideoStream::setEncoderPolicy(const CodecPolicy::Encoders &encoders, CodecPolicy::SoftwareEncoding mode)
{
    d->encoders = encoders;
    d->softwareEncoding = mode;
}

CodecPolicy::Encoders VideoStream::encoderPolicy() const
{
    return d->encoders;
}

CodecPolicy::SoftwareEncoding VideoStream::softwareEncoding() const
{
    return d->softwareEncoding;
}

CodecPolicy::Decision VideoStream::setPrivateCodecPolicy(const QVector<VideoCodec> &codecs, bool adaptive)
{
    d->clientFamilies.clear();
    for (const VideoCodec codec : codecs) {
        const auto family = familyOf(codec);
        if (family != CodecPolicy::Family::Avc && !d->clientFamilies.contains(family)) {
            d->clientFamilies.append(family);
        }
    }
    d->codecPolicyAdaptive = adaptive;
    d->codecPolicyActive = !d->clientFamilies.isEmpty();
    d->codecPolicy = {};
    d->encodeLoad.clear();
    d->reportedHardware.reset();

    CodecPolicy::Input in;
    in.mode = d->softwareEncoding;
    in.encoders = d->encoders;
    in.client = d->clientFamilies;
    in.adaptive = false; // the first choice never waits for a link measurement
    in.quality = d->quality.load(); // software HEVC/AV1 start at this quality's bitrate
    if (d->surfacePixels.load() > 0) {
        in.pixels = d->surfacePixels.load(); // else 1080p until the surfaces exist
    }
    const auto decision = CodecPolicy::step(d->codecPolicy, in, clk::steady_clock::now());
    applyEncoderSettings(decision.settings);
    setPrivateCodec(privateCodecOf(decision.choice.family));
    qCInfo(KRDP).nospace() << "Codec policy (" << CodecPolicy::softwareEncodingName(d->softwareEncoding) << "): "
                           << CodecPolicy::familyName(decision.choice.family) << (decision.choice.hardware ? " in hardware" : " in software")
                           << " for a client decoding avc" << (d->clientFamilies.contains(CodecPolicy::Family::Hevc) ? "+hevc" : "")
                           << (d->clientFamilies.contains(CodecPolicy::Family::Av1) ? "+av1" : "") << (adaptive ? "" : ", fixed");
    return decision;
}

void VideoStream::privateCodecUnavailable(VideoCodec codec)
{
    const auto family = familyOf(codec);
    if (family == CodecPolicy::Family::Avc || !d->codecPolicyActive) {
        return;
    }
    d->encoders.of(family) = {};
    CodecPolicy::Input in;
    in.mode = d->softwareEncoding;
    in.encoders = d->encoders;
    in.client = d->clientFamilies;
    in.adaptive = d->codecPolicyAdaptive;
    in.quality = d->quality.load();
    if (d->surfacePixels.load() > 0) {
        in.pixels = d->surfacePixels.load();
    }
    d->codecPolicy.lastSwitch = {}; // a wrong stream cannot wait for the switch interval
    auto decision = CodecPolicy::step(d->codecPolicy, in, clk::steady_clock::now());
    decision.reason = QStringLiteral("encoder unavailable");
    applyCodecDecision(decision);
}

void VideoStream::applyCodecDecision(const CodecPolicy::Decision &decision)
{
    if (decision.settingsChanged && !decision.changed && decision.bitrateOnly && !decision.restartsEncoder) {
        // A live bitrate change (adaptive quality on libx265) keeps the encoder and its budget:
        // the load window stays, and it is logged at debug level (it can happen every interval).
        qCDebug(KRDP).noquote() << QStringLiteral("Codec policy: %1 target bitrate %2 kbit/s, in place (%3 encoder restarts so far)")
                                       .arg(QLatin1String(CodecPolicy::familyName(decision.choice.family)))
                                       .arg(decision.settings.targetKbps)
                                       .arg(d->codecPolicy.encoderRestarts);
        applyEncoderSettings(decision.settings);
    } else if (decision.settingsChanged) {
        // A preset, bitrate or frame-rate change reopens or re-budgets the encoder: the load
        // samples from before it (and the one with the reopen stall) no longer apply.
        d->encodeLoad.clear();
        d->cpuNsAtLastSample = -1;
        if (!decision.changed) {
            const auto &s = decision.settings;
            qCInfo(KRDP).noquote() << QStringLiteral("Codec policy: %1 in %2, preset %3, %4, %5: %6")
                                          .arg(QLatin1String(CodecPolicy::familyName(decision.choice.family)),
                                               s.hardware ? QStringLiteral("hardware") : QStringLiteral("software"),
                                               QLatin1String(CodecPolicy::presetName(s.preset)),
                                               s.targetKbps ? QStringLiteral("%1 kbit/s").arg(s.targetKbps) : QStringLiteral("quality mode"),
                                               s.maxFrameRate ? QStringLiteral("max %1 fps").arg(s.maxFrameRate) : QStringLiteral("no frame-rate cap"),
                                               decision.settingsReason
                                                   + (decision.restartsEncoder ? QStringLiteral(" (encoder restart %1)").arg(d->codecPolicy.encoderRestarts)
                                                                               : QString()));
        }
        applyEncoderSettings(decision.settings);
    }
    if (!decision.changed) {
        return;
    }
    d->encodeLoad.clear();
    d->cpuNsAtLastSample = -1;
    d->reportedHardware.reset();
    setPrivateCodec(privateCodecOf(decision.choice.family));
    qCInfo(KRDP).noquote() << QStringLiteral("Codec policy: switching to %1 in %2: %3")
                                  .arg(QLatin1String(CodecPolicy::familyName(decision.choice.family)),
                                       decision.choice.hardware ? QStringLiteral("hardware") : QStringLiteral("software"),
                                       decision.reason);
    // Unsolicited: no requestId. Only our own client ever gets here (it sent `codec`).
    d->session->sendControlRecord(LayoutControl::codecRecord(QString::fromLatin1(CodecPolicy::familyName(decision.choice.family)),
                                                             decision.choice.hardware,
                                                             decision.reason));
}

void VideoStream::applyEncoderSettings(const CodecPolicy::EncoderSettings &settings)
{
    const bool changed = d->encoderSettings != settings;
    d->encoderSettings = settings;
    // The frame-rate cap (software HEVC/AV1 at 30 fps, the CPU guard's last step) is what the
    // sessions ask the capture for and the budget the CPU guard measures against.
    const int rate = settings.maxFrameRate > 0 ? std::min(settings.maxFrameRate, CodecPolicy::DefaultFrameRate) : CodecPolicy::DefaultFrameRate;
    if (d->requestedFrameRate.exchange(rate) != rate) {
        qCInfo(KRDP) << "Video frame rate:" << rate << "fps";
        Q_EMIT requestedFrameRateChanged();
    }
    if (changed) {
        Q_EMIT encoderSettingsChanged(settings);
    }
}

std::optional<CodecPolicy::EncoderSettings> VideoStream::encoderSettings() const
{
    return d->encoderSettings;
}

void VideoStream::encoderBackendReported(VideoCodec codec, bool hardware)
{
    qCInfo(KRDP).noquote() << QStringLiteral("Encoder backend: %1 in %2").arg(QLatin1String(VideoCodecSupport::codecName(codec)), hardware ? QStringLiteral("hardware") : QStringLiteral("software"));
    if (!d->codecPolicyActive || !d->codecPolicy.current || d->codecPolicy.current->family != familyOf(codec)) {
        return; // a stock client (nothing announced), or a report from before a codec switch
    }
    const bool announced = d->codecPolicy.current->hardware;
    if (d->reportedHardware == hardware) {
        return;
    }
    d->reportedHardware = hardware;
    if (announced == hardware) {
        return;
    }
    // KPipeWire fell back (h264_vaapi -> libx264): follow the encoder that really runs.
    qCWarning(KRDP).noquote() << QStringLiteral("Codec policy: %1 runs in %2, not %3 as chosen; following the encoder")
                                     .arg(QLatin1String(CodecPolicy::familyName(d->codecPolicy.current->family)),
                                          hardware ? QStringLiteral("hardware") : QStringLiteral("software"),
                                          announced ? QStringLiteral("hardware") : QStringLiteral("software"));
    d->codecPolicy.current->hardware = hardware;
    d->codecPolicy.applied.hardware = hardware;
    if (d->encoderSettings) {
        auto settings = *d->encoderSettings;
        settings.hardware = hardware; // the sessions do not restart for what already runs
        applyEncoderSettings(settings);
    }
    d->encodeLoad.clear();
    d->cpuNsAtLastSample = -1;
    d->session->sendControlRecord(LayoutControl::codecRecord(QString::fromLatin1(CodecPolicy::familyName(d->codecPolicy.current->family)),
                                                             hardware,
                                                             QStringLiteral("encoder backend: %1").arg(hardware ? QStringLiteral("hardware") : QStringLiteral("software"))));
}

void VideoStream::stepCodecPolicy(bool congested)
{
    if (!d->codecPolicyActive || !d->codecPolicy.current) {
        return;
    }
    // CPU guard input: only a software encoder has one.
    const qint64 cpuNs = processCpuNs();
    const int frames = d->framesEncoded.load();
    const auto sampledAt = clk::steady_clock::now();
    if (!d->codecPolicy.current->hardware && cpuNs >= 0 && d->cpuNsAtLastSample >= 0) {
        // AUD-FIX4 D3: the estimated encode time per frame against the frame budget, over the
        // encoder's own threads (not every core), plus delivered frames falling short of the cap.
        const auto family = d->codecPolicy.current->family;
        bool forcedOk = false;
        const int forced = qEnvironmentVariableIntValue("KPIPEWIRE_SW_ENCODER_THREADS", &forcedOk);
        const int threads = CodecPolicy::softwareEncoderThreads(family, QThread::idealThreadCount(), forcedOk ? std::optional<int>(forced) : std::nullopt);
        const double seconds = clk::duration<double>(sampledAt - d->loadSampledAt).count();
        const auto sample = CodecPolicy::encodeLoadSample(double(cpuNs - d->cpuNsAtLastSample) / 1e6,
                                                          frames - d->framesAtLastSample,
                                                          seconds,
                                                          std::max(1, d->requestedFrameRate.load()),
                                                          family,
                                                          threads,
                                                          d->surfaceCount.load());
        if (sample) {
            d->encodeLoad.add(*sample);
        }
    }
    d->cpuNsAtLastSample = cpuNs;
    d->framesAtLastSample = frames;
    d->loadSampledAt = sampledAt;

    auto *network = d->session->networkDetection();
    CodecPolicy::Input in;
    in.mode = d->softwareEncoding;
    in.encoders = d->encoders;
    in.client = d->clientFamilies;
    in.adaptive = d->codecPolicyAdaptive;
    if (network && network->validBandwidthSamples() >= 2) {
        in.bandwidthKbps = network->bandwidth();
    }
    in.congested = congested;
    in.pixels = std::max<qint64>(d->surfacePixels.load(), 1);
    // Software HEVC/AV1 follow adaptive quality through their target bitrate (a CRF change would
    // reopen the encoder; libx265 changes its bitrate in place): CodecPolicy::qualityKbps().
    in.quality = d->quality.load();
    if (!d->codecPolicy.current->hardware) {
        in.encodeLoadP95 = d->encodeLoad.p95();
    }
    const auto decision = CodecPolicy::step(d->codecPolicy, in, clk::steady_clock::now());
    if (!decision.changed && !decision.reason.isEmpty()) {
        qCDebug(KRDP).noquote() << "Codec policy:" << decision.reason;
    }
    applyCodecDecision(decision);
}

std::optional<VideoCodec> VideoStream::negotiatedCodec() const
{
    const int v = d->negotiatedCodec.load();
    return v < 0 ? std::nullopt : std::optional<VideoCodec>(VideoCodec(v));
}

VideoCodec VideoStream::codecForSessions() const
{
    return negotiatedCodec().value_or(VideoCodecSupport::expectedCodec(d->codecPreference));
}

void VideoStream::setChromaCapable(bool capable)
{
    if (d->chromaCapable.exchange(capable) == capable) {
        return;
    }
    qCDebug(KRDP) << "Encoder chroma stream" << (capable ? "available" : "unavailable") << "for adaptive quality";
    if (!capable) {
        if (!d->chromaEnabled.exchange(true)) {
            // Nothing to shed any more (the rung is inert while chromaAvailable is false); the
            // next step-down lowers QP instead. Emitted (not just stored) so the session's own
            // chromaEnabled flag - which gates setAuxStreamEnabled() - resyncs to this reset
            // instead of staying stuck off if capability later recovers (see the branch below).
            Q_EMIT requestedChromaChanged(true);
        }
    } else {
        // Capability recovered (e.g. a stream restart gave the encoder's SPS precondition another
        // chance): re-emit the current desired state even though updateAdaptiveQuality() sees no
        // change of its own (chromaAvailable flips independently of chromaEnabled), so a session
        // whose flag was force-reset by the branch above while incapable resyncs to it now that
        // applying it is meaningful again.
        Q_EMIT requestedChromaChanged(d->chromaEnabled.load());
    }
}

void VideoStream::updateAdaptiveQuality()
{
    // AUD-FIX4 D1: the in-flight window follows the path (minimum RTT) and the rate the link
    // carries; and a window that held frames back, or had to coalesce them, is congestion.
    auto *networkForWindow = d->session->networkDetection();
    if (networkForWindow) {
        d->windowMinRttUs = clk::duration_cast<clk::microseconds>(networkForWindow->minimumRTT()).count();
        d->windowRateKbps = networkForWindow->validBandwidthSamples() >= 2 ? quint32(networkForWindow->bandwidth()) : 0u;
    }
    const qint64 heldNs = d->heldNsSinceDecision.exchange(0);
    const int coalesced = d->droppedSinceDecision.exchange(0);
    const bool windowPressure = clk::nanoseconds(heldNs) >= WindowPressureHeld || coalesced > 0;

    std::optional<bool> policyCongested;
    // The codec policy steps after adaptive quality, on every path out of here, so a software
    // HEVC/AV1 target bitrate follows this interval's quality at once.
    const auto policyStep = qScopeGuard([this, &policyCongested] {
        if (policyCongested) {
            stepCodecPolicy(*policyCongested);
        }
    });
    if (d->codecPolicyActive && d->surfacePixels.load() > 0 && clk::steady_clock::now() - d->streamingSince >= WarmupAfterStreamStart) {
        // The codec policy runs whether or not adaptive quality does. Congestion as
        // AdaptiveQuality sees it: RTT inflation, or the client several frames behind.
        auto *network = d->session->networkDetection();
        int pending = 0;
        {
            std::lock_guard lock(d->pendingFramesMutex);
            pending = d->pendingFrames.suspended() ? 0 : d->pendingFrames.pending();
        }
        const bool congested = AdaptiveQuality::rttCongested(clk::duration_cast<clk::microseconds>(network->averageRTT()),
                                                             clk::duration_cast<clk::microseconds>(network->minimumRTT()))
            || pending >= AdaptiveQuality::BacklogFrames || windowPressure;
        policyCongested = congested;
    }
    const bool audioPriority = d->session->audioPriorityActive();
    if (!d->adaptiveQuality.load() && !audioPriority) {
        // A live priority override can temporarily enable steering even when
        // ordinary video adaptation is disabled. Restore its fixed cap after
        // that override (or the final audio direction) is switched off.
        const quint8 cap = d->qualityCap.load();
        if (d->quality.exchange(cap) != cap) {
            Q_EMIT requestedQualityChanged(cap);
        }
        if (!d->chromaEnabled.exchange(true)) {
            Q_EMIT requestedChromaChanged(true);
        }
        return;
    }
    const auto now = clk::steady_clock::now();
    if (now - d->streamingSince < WarmupAfterStreamStart) {
        return;
    }
    if (d->surfacePixels.load() == 0) {
        return; // no surface yet, nothing is being sent
    }

    int pendingNow = 0;
    bool acksSuspended = false;
    {
        std::lock_guard lock(d->pendingFramesMutex);
        pendingNow = d->pendingFrames.pending();
        acksSuspended = d->pendingFrames.suspended();
    }
    const int minAfterAck = d->minPendingAfterAckSinceDecision.exchange(std::numeric_limits<int>::max());
    // Backlogged = the client never got within BacklogFrames of caught up:
    // neither after any ack this interval nor right now. Idle (pendingNow 0)
    // is never a backlog; a stall with no acks at all is (min(INT_MAX, now)).
    // Suppress only this startup-burst signal; RTT congestion remains active.
    const bool backlogged = AdaptiveQuality::backlogIsPressure(now - d->streamingSince, minAfterAck, pendingNow, acksSuspended) || windowPressure;

    auto *network = d->session->networkDetection();
    const auto averageRtt = clk::duration_cast<clk::microseconds>(network->averageRTT());
    const auto minimumRtt = clk::duration_cast<clk::microseconds>(network->minimumRTT());
    const quint8 current = d->quality.load();
    // The rung exists only for a 4:4:4 connection whose encoder really produces the aux stream (a
    // KPipeWire without AVC444, no h264_vaapi, or the rewriter's fallback all report false).
    const bool chromaAvailable = negotiatedCodec().has_value() && VideoCodecSupport::isAvc444(*negotiatedCodec()) && d->chromaCapable.load();
    const bool chromaNow = d->chromaEnabled.load();
    const auto result = AdaptiveQuality::step({
        .current = current,
        .cap = d->qualityCap.load(),
        .averageRtt = averageRtt,
        .minimumRtt = minimumRtt,
        .backlogged = backlogged,
        .climbAllowed = (now - d->lastStepDown) >= AdaptiveQuality::ClimbHoldAfterStepDown,
        .chromaAvailable = chromaAvailable,
        .chromaEnabled = chromaNow,
        .preferAudioQuality = audioPriority,
    });

    // The cap or the adaptive-quality flag may have changed while step() ran;
    // re-check both before committing so a stale result never overshoots a
    // just-lowered cap or gets applied after adaptive quality was turned off.
    if (!d->adaptiveQuality.load() && !d->session->audioPriorityActive()) {
        return;
    }
    const quint8 bounded = quint8(std::min<int>(result.next, d->qualityCap.load()));
    const bool chromaChanged = chromaAvailable && result.chromaEnabled != chromaNow;
    if (bounded < current || (chromaChanged && !result.chromaEnabled)) {
        d->lastStepDown = now; // shedding chroma is a step down: the climb hold applies to it too
    }
    if (bounded == current && !chromaChanged) {
        return;
    }

    d->quality = bounded;
    if (chromaChanged) {
        d->chromaEnabled = result.chromaEnabled;
    }
    qCDebug(KRDP) << "Adaptive quality ->" << bounded << "chroma=" << (chromaAvailable ? (result.chromaEnabled ? "on" : "off") : "n/a") << "("
                  << (result.congested ? "congested" : (backlogged ? "backlogged" : "clear")) << "rtt avg" << averageRtt.count() << "min" << minimumRtt.count()
                  << "us, pending min-after-ack" << (minAfterAck == std::numeric_limits<int>::max() ? -1 : minAfterAck) << "now" << pendingNow
                  << ", window held" << clk::duration_cast<clk::milliseconds>(clk::nanoseconds(heldNs)).count() << "ms, coalesced" << coalesced << ", goodput" << network->bandwidth() << "kbit/s, cap"
                  << d->qualityCap.load() << ")";
    if (chromaChanged) {
        Q_EMIT requestedChromaChanged(result.chromaEnabled);
    }
    if (bounded != current) {
        Q_EMIT requestedQualityChanged(bounded);
    }
}

bool VideoStream::onChannelIdAssigned(uint32_t channelId)
{
    d->channelId = channelId;

    return true;
}

uint32_t VideoStream::onCapsAdvertise(const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise)
{
    // Windows clients (mstsc) send CapsAdvertise twice: once during
    // initial setup and again after confirming. If we already confirmed
    // caps, this is a GFX channel reset — clear surface state so
    // surfaces get re-created on the next frame.
    if (d->capsConfirmed) {
        qCDebug(KRDP) << "GFX channel reset (re-advertisement), resetting surface state";
        d->capsConfirmed = false;
        {
            // pendingReset belongs with the surfaces it re-arms: this runs on
            // the peer thread while the submission thread may be inside
            // sendFrame(), which reads the flag and the surfaces together
            // under this same lock.
            // Not configuredLayout: that is configuration, not surface state,
            // and the next reset rebuilds the surfaces from it.
            std::lock_guard lock(d->layoutMutex);
            d->pendingReset = true;
            d->surfaces.clear();
            d->monitorLayout.clear();
        }
        d->surfacePixels = 0;
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.clear();
        d->statInFlight = 0;
        bumpAtomicMin(d->minPendingAfterAckSinceDecision, 0);
    }

    auto capsSets = capsAdvertise->capsSets;
    auto count = capsAdvertise->capsSetCount;

    std::vector<RdpCapsInformation> capsInformation;
    capsInformation.reserve(count);

    qCDebug(KRDP) << "Received caps:";
    for (int i = 0; i < count; ++i) {
        auto set = capsSets[i];

        RdpCapsInformation caps;
        caps.version = set.version;
        caps.capSet = set;

        switch (set.version) {
        case RDPGFX_CAPVERSION_107:
        case RDPGFX_CAPVERSION_106:
        case RDPGFX_CAPVERSION_105:
        case RDPGFX_CAPVERSION_104:
            caps.yuv420Supported = true;
            Q_FALLTHROUGH();
        case RDPGFX_CAPVERSION_103:
        case RDPGFX_CAPVERSION_102:
        case RDPGFX_CAPVERSION_101:
        case RDPGFX_CAPVERSION_10:
            if (!(set.flags & RDPGFX_CAPS_FLAG_AVC_DISABLED)) {
                caps.avcSupported = true;
            }
            break;
        case RDPGFX_CAPVERSION_81:
            if (set.flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) {
                caps.avcSupported = true;
                caps.yuv420Supported = true;
            }
            break;
        case RDPGFX_CAPVERSION_8:
            break;
        }

        qCDebug(KRDP) << " " << capVersionToString(caps.version) << "AVC:" << caps.avcSupported << "YUV420:" << caps.yuv420Supported;

        capsInformation.push_back(caps);
    }

    const bool supportsH264 = std::any_of(capsInformation.begin(), capsInformation.end(), [](const RdpCapsInformation &caps) {
        return caps.avcSupported && caps.yuv420Supported;
    });
    if (!supportsH264) {
        qCWarning(KRDP) << "Client does not support H.264 in YUV420 mode!";
        d->session->close(RdpConnection::CloseReason::VideoInitFailed);
        return CHANNEL_RC_INITIALIZATION_ERROR;
    }

    auto selectedCaps = std::max_element(capsInformation.begin(), capsInformation.end(), [](const auto &first, const auto &second) {
        return first.version < second.version;
    });

    qCDebug(KRDP) << "Selected caps:" << capVersionToString(selectedCaps->version);

    // Not reset to -1 on a re-advertisement (see the capsConfirmed branch above):
    // the previous codec stays the best guess until the caps parsed just above
    // settle on a new one a few lines later.
    const int privateCodec = d->privateCodec.load();
    const VideoCodec capsCodec = VideoCodecSupport::codecFor(selectedCaps->version, selectedCaps->capSet.flags, d->codecPreference);
    d->capsCodec.store(int(capsCodec));
    const VideoCodec codec = privateCodec >= 0 ? VideoCodec(privateCodec) : capsCodec;
    const int previous = d->negotiatedCodec.exchange(int(codec));
    qCInfo(KRDP).noquote() << QStringLiteral("GFX caps confirmed: %1 codec=%2").arg(QLatin1String(capVersionToString(selectedCaps->version)), QLatin1String(VideoCodecSupport::codecName(codec)));
    if (previous != int(codec)) {
        // Gated on the codec actually (re)settling on avc420, not on every advertisement: a client
        // that re-sends CapsAdvertise with the same unsupported result (e.g. mstsc) would otherwise
        // log this every time instead of once per negotiation.
        if (d->codecPreference == CodecPreference::Avc444 && codec == VideoCodec::Avc420) {
            qCWarning(KRDP) << "Codec=avc444 requested but this client advertises no AVC444 support (caps" << capVersionToString(selectedCaps->version) << "); using avc420";
        }
        if (!d->chromaEnabled.exchange(true)) { // a fresh negotiation starts with chroma on
            Q_EMIT requestedChromaChanged(true);
        }
        Q_EMIT negotiatedCodecChanged(codec); // peer thread; the session side connects queued
    }

    RDPGFX_CAPS_CONFIRM_PDU capsConfirmPdu;
    capsConfirmPdu.capsSet = &(selectedCaps->capSet);
    d->gfxContext->CapsConfirm(d->gfxContext.get(), &capsConfirmPdu);

    d->capsConfirmed = true;

    return CHANNEL_RC_OK;
}

uint32_t VideoStream::onFrameAcknowledge(const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge)
{
    static_assert(FrameQueuePolicy::FrameAckTracker::SuspendFrameAcknowledgement == SUSPEND_FRAME_ACKNOWLEDGEMENT);
    auto id = frameAcknowledge->frameId;

    // AUD-FIX4 D1: an ack may open the in-flight window; wake the submission thread
    // (after the lock below is released).
    const auto wake = qScopeGuard([this] {
        d->frameQueueCondition.notify_one();
    });
    std::lock_guard lock(d->pendingFramesMutex);

    const auto ack = d->pendingFrames.acknowledge(id, frameAcknowledge->queueDepth);
    if ((ack == FrameQueuePolicy::FrameAckTracker::Ack::Acknowledged || ack == FrameQueuePolicy::FrameAckTracker::Ack::Suspended)
        && !d->graphicsDelivered.exchange(true)) {
        qCDebug(KRDP) << "The client acknowledged its first frame";
        Q_EMIT graphicsDelivered();
    }
    d->statInFlight = d->pendingFrames.inFlightFrames();
    d->statSuspended = d->pendingFrames.suspended();
    switch (ack) {
    case FrameQueuePolicy::FrameAckTracker::Ack::Unknown:
        qCDebug(KRDP) << "Got frame acknowledge for an unknown frame" << id;
        return CHANNEL_RC_OK;
    case FrameQueuePolicy::FrameAckTracker::Ack::Suspended:
        qCInfo(KRDP) << "Client suspended frame acknowledgements; pending frames no longer count toward adaptive quality, and the in-flight window becomes a time and socket budget";
        // Nothing is outstanding any more: a suspension counts as caught up.
        bumpAtomicMin(d->minPendingAfterAckSinceDecision, 0);
        return CHANNEL_RC_OK;
    case FrameQueuePolicy::FrameAckTracker::Ack::Resumed:
        qCInfo(KRDP) << "Client resumed frame acknowledgements";
        break;
    case FrameQueuePolicy::FrameAckTracker::Ack::Acknowledged:
        d->statAcknowledged.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    // How far behind the client still is now that this ack landed - see
    // minPendingAfterAckSinceDecision.
    bumpAtomicMin(d->minPendingAfterAckSinceDecision, d->pendingFrames.pending());

    return CHANNEL_RC_OK;
}

bool VideoStream::performReset(const QSize &desktopSize, const QVector<VideoMonitor> &monitors, const QVector<SurfaceLayout::Entry> &surfaces)
{
    RDPGFX_RESET_GRAPHICS_PDU resetGraphicsPdu;
    resetGraphicsPdu.width = desktopSize.width();
    resetGraphicsPdu.height = desktopSize.height();
    resetGraphicsPdu.monitorCount = monitors.size();

    auto monitorDefs = std::make_unique<MONITOR_DEF[]>(monitors.size());
    for (int i = 0; i < monitors.size(); ++i) {
        const auto &monitor = monitors.at(i);
        // Inclusive edges: see SurfaceLayout::edgesOf().
        const auto edges = SurfaceLayout::edgesOf(monitor.geometry);
        monitorDefs[i].left = edges.left;
        monitorDefs[i].top = edges.top;
        monitorDefs[i].right = edges.right;
        monitorDefs[i].bottom = edges.bottom;
        monitorDefs[i].flags = monitor.primary ? MONITOR_PRIMARY : 0;
    }
    resetGraphicsPdu.monitorDefArray = monitorDefs.get();

    qCDebug(KRDP) << "Reset graphics desktop" << desktopSize << "with" << monitors.size() << "monitor(s):" << monitorLayoutSummary(monitors);
    d->gfxContext->ResetGraphics(d->gfxContext.get(), &resetGraphicsPdu);

    d->surfaces.clear();
    d->surfaces.reserve(surfaces.size());
    qint64 totalPixels = 0;

    for (const auto &entry : surfaces) {
        RDPGFX_CREATE_SURFACE_PDU createSurfacePdu;
        createSurfacePdu.width = entry.size.width();
        createSurfacePdu.height = entry.size.height();
        uint16_t surfaceId = d->nextSurfaceId++;
        createSurfacePdu.surfaceId = surfaceId;
        createSurfacePdu.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;
        if (d->gfxContext->CreateSurface(d->gfxContext.get(), &createSurfacePdu) != CHANNEL_RC_OK) {
            // Sending frames to surfaces the client never created is worse
            // than sending nothing: roll the whole reset back and let the next
            // frame retry it.
            if (!d->loggedCreateSurfaceFailure) {
                d->loggedCreateSurfaceFailure = true;
                qCWarning(KRDP) << "CreateSurface failed for a" << entry.size << "surface at" << entry.origin << "- aborting this reset";
            }
            d->surfaces.clear();
            d->surfacePixels = 0;
            d->pendingReset = true;
            return false;
        }

        d->surfaces.push_back(Surface{
            .id = surfaceId,
            .size = entry.size,
            .origin = entry.origin,
        });
        totalPixels += qint64(entry.size.width()) * entry.size.height();

        RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU mapSurfaceToOutputPdu;
        mapSurfaceToOutputPdu.outputOriginX = entry.origin.x();
        mapSurfaceToOutputPdu.outputOriginY = entry.origin.y();
        mapSurfaceToOutputPdu.surfaceId = surfaceId;
        d->gfxContext->MapSurfaceToOutput(d->gfxContext.get(), &mapSurfaceToOutputPdu);
    }

    d->surfacePixels = totalPixels;
    d->surfaceCount = std::max<int>(1, surfaces.size());
    // resize() keeps the timestamps of the surfaces that survive this reset,
    // so a reset storm still cannot ask the encoder for more than one keyframe
    // per KeyFrameRequestMinInterval per surface.
    d->lastKeyFrameRequest.resize(surfaces.size());
    d->loggedCreateSurfaceFailure = false;

    return true;
}

VideoStream::FlowStats VideoStream::flowStats() const
{
    FlowStats stats;
    stats.inFlight = d->statInFlight.load();
    stats.maxInFlight = d->statMaxInFlight.load();
    stats.windowFrames = d->statWindowFrames.load();
    stats.windowBytes = d->statWindowBytes.load();
    stats.sent = d->statSent.load();
    stats.acknowledged = d->statAcknowledged.load();
    stats.dropped = d->statDropped.load();
    stats.keyFrameRequests = d->statKeyFrameRequests.load();
    stats.acksSuspended = d->statSuspended.load();
    return stats;
}

bool VideoStream::windowOpen(clk::steady_clock::time_point now)
{
    const auto limits = d->windowLimits();
    d->statWindowFrames = limits.frames;
    d->statWindowBytes = limits.bytes;
    std::lock_guard lock(d->pendingFramesMutex);
    if (const int timedOut = d->pendingFrames.expire(now, limits); timedOut > 0 && !d->loggedAckTimeout) {
        d->loggedAckTimeout = true;
        qCWarning(KRDP) << "The client left" << timedOut << "frame(s) unacknowledged for"
                        << clk::duration_cast<clk::seconds>(FrameQueuePolicy::AckTimeout).count() << "s; no longer counting them as in flight";
    }
    d->statInFlight = d->pendingFrames.inFlightFrames();
    d->statSuspended = d->pendingFrames.suspended();
    const qint64 socketQueued = d->pendingFrames.suspended() ? d->session->socketQueuedBytes() : -1;
    return d->pendingFrames.canSend(limits, socketQueued);
}

void VideoStream::holdFrames(clk::steady_clock::time_point now)
{
    std::vector<int> starved;
    int dropped = 0;
    {
        std::lock_guard lock(d->frameQueueMutex);
        dropped = FrameQueuePolicy::coalesceHeldFrames(d->frameQueue, FrameQueuePolicy::MaxHeldFramesPerMonitor, starved);
        for (const int monitor : starved) {
            if (monitor < 0) {
                continue;
            }
            if (size_t(monitor) >= d->starvedMonitors.size()) {
                d->starvedMonitors.resize(size_t(monitor) + 1, 0);
                d->starvedKeyFrameRequestAt.resize(size_t(monitor) + 1);
            }
            d->starvedMonitors[size_t(monitor)] = 1;
        }
    }
    if (dropped > 0) {
        d->statDropped.fetch_add(quint64(dropped), std::memory_order_relaxed);
        d->droppedSinceDecision.fetch_add(dropped, std::memory_order_relaxed);
        if (d->lastWindowLog == clk::steady_clock::time_point{} || now - d->lastWindowLog >= WindowLogInterval) {
            d->lastWindowLog = now;
            const auto limits = d->windowLimits();
            int inFlight = 0;
            qint64 bytes = 0;
            {
                std::lock_guard lock(d->pendingFramesMutex);
                inFlight = d->pendingFrames.inFlightFrames();
                bytes = d->pendingFrames.inFlightBytes();
            }
            QStringList monitors;
            for (const int monitor : starved) {
                monitors << QString::number(monitor);
            }
            qCInfo(KRDP).noquote() << QStringLiteral("Video: the client is behind (%1 frames, %2 KiB in flight; window %3 frames, %4 KiB); dropped %5 queued frame(s) of monitor %6, keyframe requested")
                                          .arg(inFlight)
                                          .arg(bytes / 1024)
                                          .arg(limits.frames)
                                          .arg(limits.bytes / 1024)
                                          .arg(dropped)
                                          .arg(monitors.join(QLatin1Char(',')));
        }
    }
    requestStarvedKeyFrames(now);
}

void VideoStream::requestStarvedKeyFrames(clk::steady_clock::time_point now)
{
    std::vector<int> request;
    {
        std::lock_guard lock(d->frameQueueMutex);
        for (size_t monitor = 0; monitor < d->starvedMonitors.size(); ++monitor) {
            if (!d->starvedMonitors[monitor]) {
                continue;
            }
            auto &last = d->starvedKeyFrameRequestAt[monitor];
            if (last == clk::steady_clock::time_point{} || now - last >= FrameQueuePolicy::CoalesceKeyFrameMinInterval) {
                last = now;
                request.push_back(int(monitor));
            }
        }
    }
    for (const int monitor : request) {
        // Outside the lock, like sendFrame()'s request. The encoder answers with an IDR of
        // the newest picture (KPipeWire re-feeds the last captured frame for a request), so
        // what the client sees next is current, even on a desktop that stopped changing.
        d->statKeyFrameRequests.fetch_add(1, std::memory_order_relaxed);
        qCDebug(KRDP) << "Frames of monitor" << monitor << "were coalesced away; requesting a keyframe";
        Q_EMIT keyFrameRequested(monitor);
    }
}

bool VideoStream::sendFrame(const VideoFrame &frame)
{
    if (!d->gfxContext || !d->capsConfirmed) {
        // Not a drop: the caller keeps the frame queued until the channel is ready.
        return false;
    }

    const VideoCodec codec = negotiatedCodec().value_or(VideoCodec::Avc420); // caps are confirmed here, so it is set
    const bool auxOnly = frame.data.isEmpty() && !frame.aux.isEmpty();
    if (frame.data.isEmpty() && (!auxOnly || !VideoCodecSupport::isAvc444(codec))) {
        qCDebug(KRDP) << "Skipping empty encoded frame"; // nothing to send; an aux-only frame is meaningless to an AVC420 client
        return true;
    }

    Surface surface;
    bool requestKeyFrame = false;
    {
        std::lock_guard lock(d->layoutMutex);

        const auto plan = planReset(frame, d->configuredLayout);
        if (frame.monitorIndex < 0 || frame.monitorIndex >= plan.surfaces.size()) {
            // Nothing sensible to send this to, and resetting would not create
            // it either. Drop it; the session that produced it is misconfigured.
            if (!d->loggedUnknownSurface) {
                d->loggedUnknownSurface = true;
                qCWarning(KRDP) << "Dropping frames for monitor index" << frame.monitorIndex << "- the layout has" << plan.surfaces.size() << "surface(s)";
            }
            return true;
        }

        Surface *target = d->surfaceFor(frame.monitorIndex);
        // With a configured layout the surface sizes come from that layout, so
        // only a new layout resizes them. Without one the single surface still
        // follows the frame, exactly as it did before per-monitor surfaces.
        const bool frameSizeChanged = target && d->configuredLayout.isEmpty() && target->size != frame.size;
        if (d->pendingReset || d->monitorLayout != plan.monitors || !target || frameSizeChanged) {
            d->pendingReset = false;
            d->monitorLayout = plan.monitors;
            // performReset() sends ResetGraphics, CreateSurface and
            // MapSurfaceToOutput with layoutMutex held, deliberately. The ack
            // path (onFrameAcknowledge) never takes this lock, so the only
            // thread a reset can hold up is the peer thread inside
            // onCapsAdvertise, which is about to tear these surfaces down
            // anyway. Dropping the lock around the sends would instead let a
            // second reset interleave with this one on the wire.
            if (!performReset(plan.desktopSize, plan.monitors, plan.surfaces)) {
                // The surfaces were rolled back and pendingReset re-armed.
                // Drop this frame so the next one retries; holding it would
                // busy-spin the submission thread, which re-dequeues a
                // prepended frame with no wait.
                return true;
            }
            Q_EMIT graphicsReset(plan.monitors);
            target = d->surfaceFor(frame.monitorIndex);
            // A successful reset is a clean slate: let a later, different
            // misconfiguration be reported rather than staying silent for the
            // rest of the stream.
            d->loggedUnknownSurface = false;
            d->loggedSizeMismatch = false;

            // A freshly created surface has no reference picture. If the frame we
            // are about to send cannot serve as this surface's keyframe (e.g. after a caps
            // re-advertisement the queue holds P-frames), ask the session for one
            // now instead of waiting for the next organic IDR, which on a static
            // desktop can be seconds away (gop 100, frames only on damage).
            // AVC444: every aux picture is intra, but an aux-only refresh can never be the main
            // IDR a fresh surface needs (the keyFrameSent gate below drops it outright), so it
            // must always join the request too - excluding it here would silently consume this
            // reset's one request slot on a frame that was going to be dropped anyway, leaving no
            // other path to ever ask for a keyframe on this surface.
            if (!frame.isKeyFrame || auxOnly) {
                const auto now = clk::steady_clock::now();
                auto &lastRequest = d->lastKeyFrameRequest[frame.monitorIndex];
                if (lastRequest == clk::steady_clock::time_point{} || (now - lastRequest) >= KeyFrameRequestMinInterval) {
                    lastRequest = now;
                    requestKeyFrame = true;
                }
            }
        }

        if (!target) {
            // Unreachable: the index is inside the plan, so either a surface
            // was already there or performReset just made one. Belt and braces
            // so a future edit cannot turn this into a null dereference.
            return true;
        }

        // With a configured layout the surface is the size that layout asked
        // for, and a frame of any other size cannot be sent: the surface
        // command and the AVC420 region rect describe the surface, and
        // FreeRDP's client-side avc420 decode rejects a region rect larger
        // than the decoded picture without telling the server anything.
        if (!d->configuredLayout.isEmpty() && target->size != frame.size) {
            if (!d->loggedSizeMismatch) {
                d->loggedSizeMismatch = true;
                qCWarning(KRDP) << "Dropping frames for monitor" << frame.monitorIndex << ": the frame is" << frame.size << "but its surface is" << target->size
                                << "- the configured monitor layout must be in pixels, not logical coordinates";
            }
            return true;
        }

        // An aux-only refresh (LC = 2) needs the decoder the main IDR set up: never the first
        // frame on a fresh surface. It is dropped, and the next main picture is what the reset
        // rule above already asked a keyframe for.
        if (auxOnly && !target->keyFrameSent) {
            return true;
        }
        if (frame.isKeyFrame) {
            target->keyFrameSent = true;
        }
        surface = *target;
    }

    if (requestKeyFrame) {
        // Emitted outside the lock: the slot runs on the session's thread, but
        // signalling under a lock another thread takes is a trap not worth
        // leaving lying around.
        qCDebug(KRDP) << "Surface (re)created on a non-keyframe, requesting a keyframe from the encoder for monitor" << frame.monitorIndex;
        Q_EMIT keyFrameRequested(frame.monitorIndex);
    }

    auto frameId = d->frameId++;

    // Debug-gated (off unless org.kde.krdp.debug is on), but the only place
    // that shows which monitor a frame actually reached the wire on - the one
    // thing a multi-monitor stream has to be checked against.
    qCDebug(KRDP) << "Sending frame" << frameId << "monitorIndex" << frame.monitorIndex << "surface" << surface.id << "at" << surface.origin << surface.size
                  << (frame.isKeyFrame ? "keyframe" : "delta") << frame.data.size() << "bytes"
                  << "aux" << frame.aux.size() << "bytes" << VideoCodecSupport::codecName(codec) << (auxOnly ? "chroma-only" : "");

    {
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.frameSent(frameId, frame.data.size() + frame.aux.size());
        const int inFlight = d->pendingFrames.inFlightFrames();
        d->statInFlight = inFlight;
        int seen = d->statMaxInFlight.load(std::memory_order_relaxed);
        while (inFlight > seen && !d->statMaxInFlight.compare_exchange_weak(seen, inFlight, std::memory_order_relaxed)) {
        }
    }
    d->statSent.fetch_add(1, std::memory_order_relaxed);

    RDPGFX_START_FRAME_PDU startFramePdu;
    RDPGFX_END_FRAME_PDU endFramePdu;

    auto now = QDateTime::currentDateTimeUtc().time();
    startFramePdu.timestamp = now.hour() << 22 | now.minute() << 16 | now.second() << 10 | now.msec();

    startFramePdu.frameId = frameId;
    endFramePdu.frameId = frameId;

    GfxSurfaceCommand::Storage command;
    GfxSurfaceCommand::build(command, codec, surface.id, surface.size, frame.data, frame.aux, d->quality.load());

    d->gfxContext->StartFrame(d->gfxContext.get(), &startFramePdu);
    d->gfxContext->SurfaceCommand(d->gfxContext.get(), &command.command);
    d->gfxContext->EndFrame(d->gfxContext.get(), &endFramePdu);

    return true;
}
}

#include "moc_VideoStream.cpp"
