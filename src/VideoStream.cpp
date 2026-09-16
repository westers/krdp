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
#include <vector>

#include <QDateTime>
#include <QQueue>
#include <QRect>
#include <QStringList>
#include <QTimer>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

#include "AdaptiveQuality.h"
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

    bool pendingReset = true;
    bool enabled = false;
    // Written on the FreeRDP peer thread (onCapsAdvertise), read by the submission thread.
    std::atomic<bool> capsConfirmed = false;

    std::jthread frameSubmissionThread;
    std::mutex frameQueueMutex;
    std::condition_variable frameQueueCondition;

    QQueue<VideoFrame> frameQueue;
    // pendingFrames is inserted on the submission thread (sendFrame) and erased on
    // the FreeRDP peer thread (onFrameAcknowledge).
    QSet<uint32_t> pendingFrames;
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
    std::atomic<bool> adaptiveQuality = true;
    // The total pixel count over every surface, as a single atomic value, so
    // updateAdaptiveQuality() (main thread) never sees a torn width/height
    // while performReset() (submission thread) or onCapsAdvertise() (peer
    // thread) rebuilds the surfaces. Summed because one quality steers every
    // session feeding this connection.
    std::atomic<qint64> surfacePixels = 0;
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
                // Always send in order: every encoded P-frame references the one
                // before it, so skipping a queued frame would corrupt the client's
                // decode until the next keyframe. The queue is bounded by
                // queueFrame() clearing it whenever a new keyframe arrives.
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
        bumpAtomicMin(d->minPendingAfterAckSinceDecision, 0);
    }
    {
        std::lock_guard lock(d->frameQueueMutex);
        d->frameQueue.clear();
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

    {
        std::lock_guard lock(d->frameQueueMutex);
        // A keyframe supersedes everything still waiting to be sent, so the
        // pending-send queue can never grow beyond one keyframe interval.
        // Never drop anything else: encoded P-frames must be sent in order.
        if (frame.isKeyFrame) {
            d->frameQueue.clear();
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
    if (d->enabled == enabled) {
        return;
    }

    d->enabled = enabled;
    if (!enabled) {
        std::lock_guard lock(d->frameQueueMutex);
        d->frameQueue.clear();
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
    const quint8 next = d->adaptiveQuality.load() ? std::min(current, cap) : cap;
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
}

void VideoStream::setAdaptiveQuality(bool enabled)
{
    if (d->adaptiveQuality.exchange(enabled) == enabled) {
        return;
    }
    if (!enabled) {
        const quint8 cap = d->qualityCap.load();
        const quint8 previous = d->quality.exchange(cap);
        if (previous != cap) {
            Q_EMIT requestedQualityChanged(cap);
        }
    }
}

void VideoStream::updateAdaptiveQuality()
{
    if (!d->adaptiveQuality.load()) {
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
    {
        std::lock_guard lock(d->pendingFramesMutex);
        pendingNow = int(d->pendingFrames.size());
    }
    const int minAfterAck = d->minPendingAfterAckSinceDecision.exchange(std::numeric_limits<int>::max());
    // Backlogged = the client never got within BacklogFrames of caught up:
    // neither after any ack this interval nor right now. Idle (pendingNow 0)
    // is never a backlog; a stall with no acks at all is (min(INT_MAX, now)).
    const bool backlogged = std::min(minAfterAck, pendingNow) >= AdaptiveQuality::BacklogFrames;

    auto *network = d->session->networkDetection();
    const auto averageRtt = clk::duration_cast<clk::microseconds>(network->averageRTT());
    const auto minimumRtt = clk::duration_cast<clk::microseconds>(network->minimumRTT());
    const quint8 current = d->quality.load();
    const auto result = AdaptiveQuality::step({
        .current = current,
        .cap = d->qualityCap.load(),
        .averageRtt = averageRtt,
        .minimumRtt = minimumRtt,
        .backlogged = backlogged,
        .climbAllowed = (now - d->lastStepDown) >= AdaptiveQuality::ClimbHoldAfterStepDown,
    });

    // The cap or the adaptive-quality flag may have changed while step() ran;
    // re-check both before committing so a stale result never overshoots a
    // just-lowered cap or gets applied after adaptive quality was turned off.
    if (!d->adaptiveQuality.load()) {
        return;
    }
    const quint8 bounded = quint8(std::min<int>(result.next, d->qualityCap.load()));
    if (bounded < current) {
        d->lastStepDown = now;
    }
    if (bounded == current) {
        return;
    }

    d->quality = bounded;
    qCDebug(KRDP) << "Adaptive quality ->" << bounded << "(" << (result.congested ? "congested" : (backlogged ? "backlogged" : "clear")) << "rtt avg" << averageRtt.count() << "min" << minimumRtt.count()
                  << "us, pending min-after-ack" << (minAfterAck == std::numeric_limits<int>::max() ? -1 : minAfterAck) << "now" << pendingNow << ", goodput" << network->bandwidth() << "kbit/s, cap"
                  << d->qualityCap.load() << ")";
    Q_EMIT requestedQualityChanged(bounded);
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
        d->pendingReset = true;
        {
            // Not configuredLayout: that is configuration, not surface state,
            // and the next reset rebuilds the surfaces from it.
            std::lock_guard lock(d->layoutMutex);
            d->surfaces.clear();
            d->monitorLayout.clear();
        }
        d->surfacePixels = 0;
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.clear();
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

    RDPGFX_CAPS_CONFIRM_PDU capsConfirmPdu;
    capsConfirmPdu.capsSet = &(selectedCaps->capSet);
    d->gfxContext->CapsConfirm(d->gfxContext.get(), &capsConfirmPdu);

    d->capsConfirmed = true;

    return CHANNEL_RC_OK;
}

uint32_t VideoStream::onFrameAcknowledge(const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge)
{
    auto id = frameAcknowledge->frameId;

    std::lock_guard lock(d->pendingFramesMutex);

    auto itr = d->pendingFrames.constFind(id);
    if (itr == d->pendingFrames.cend()) {
        qCWarning(KRDP) << "Got frame acknowledge for an unknown frame";
        return CHANNEL_RC_OK;
    }

    d->pendingFrames.erase(itr);

    // How far behind the client still is now that this ack landed - see
    // minPendingAfterAckSinceDecision.
    bumpAtomicMin(d->minPendingAfterAckSinceDecision, int(d->pendingFrames.size()));

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
        monitorDefs[i].left = monitor.geometry.x();
        monitorDefs[i].top = monitor.geometry.y();
        monitorDefs[i].right = monitor.geometry.x() + monitor.geometry.width();
        monitorDefs[i].bottom = monitor.geometry.y() + monitor.geometry.height();
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
    // resize() keeps the timestamps of the surfaces that survive this reset,
    // so a reset storm still cannot ask the encoder for more than one keyframe
    // per KeyFrameRequestMinInterval per surface.
    d->lastKeyFrameRequest.resize(surfaces.size());
    d->loggedCreateSurfaceFailure = false;

    return true;
}

bool VideoStream::sendFrame(const VideoFrame &frame)
{
    if (!d->gfxContext || !d->capsConfirmed) {
        // Not a drop: the caller keeps the frame queued until the channel is ready.
        return false;
    }

    if (frame.data.size() == 0) {
        qCDebug(KRDP) << "Skipping empty encoded frame";
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

        const Surface *target = d->surfaceFor(frame.monitorIndex);
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
            target = d->surfaceFor(frame.monitorIndex);
            // A successful reset is a clean slate: let a later, different
            // misconfiguration be reported rather than staying silent for the
            // rest of the stream.
            d->loggedUnknownSurface = false;
            d->loggedSizeMismatch = false;

            // A freshly created surface has no reference picture. If the frame we
            // are about to send is not a keyframe (e.g. after a caps
            // re-advertisement the queue holds P-frames), ask the session for one
            // now instead of waiting for the next organic IDR, which on a static
            // desktop can be seconds away (gop 100, frames only on damage).
            if (!frame.isKeyFrame) {
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
                  << (frame.isKeyFrame ? "keyframe" : "delta") << frame.data.size() << "bytes";

    {
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.insert(frameId);
    }

    RDPGFX_START_FRAME_PDU startFramePdu;
    RDPGFX_END_FRAME_PDU endFramePdu;

    auto now = QDateTime::currentDateTimeUtc().time();
    startFramePdu.timestamp = now.hour() << 22 | now.minute() << 16 | now.second() << 10 | now.msec();

    startFramePdu.frameId = frameId;
    endFramePdu.frameId = frameId;

    // The encoder produces a full-frame H.264 picture (KPipeWire does not crop
    // to damage), so send a single region rect covering the whole surface, as
    // upstream and gnome-remote-desktop do for AVC420.
    RDPGFX_SURFACE_COMMAND surfaceCommand = {};
    surfaceCommand.surfaceId = surface.id;
    surfaceCommand.codecId = RDPGFX_CODECID_AVC420;
    surfaceCommand.format = PIXEL_FORMAT_BGRX32;
    // Each surface is a whole monitor, so the command covers all of it.
    surfaceCommand.left = 0;
    surfaceCommand.top = 0;
    surfaceCommand.right = surface.size.width();
    surfaceCommand.bottom = surface.size.height();
    surfaceCommand.length = 0;
    surfaceCommand.data = nullptr;

    RDPGFX_AVC420_BITMAP_STREAM avcStream = {};
    surfaceCommand.extra = &avcStream;
    avcStream.data = (BYTE *)frame.data.data();
    avcStream.length = frame.data.length();

    avcStream.meta.numRegionRects = 1;
    RECTANGLE_16 rect = {0, 0, static_cast<UINT16>(surface.size.width()), static_cast<UINT16>(surface.size.height())};
    avcStream.meta.regionRects = &rect;
    // Informational for the client (MS-RDPEGFX 2.2.4.4.1), but keep it honest:
    // the same map the private KPipeWire uses (quality 100 -> QP 12, 0 -> QP 40).
    // RDPGFX_H264_QUANT_QUALITY's member order is {qpVal, qualityVal, qp, r, p}
    // (freerdp/channels/rdpgfx.h) - name the members explicitly rather than
    // relying on positional aggregate init, which previously put the QP in the
    // unused qpVal slot (qp itself landed on quality, and qualityVal stayed 0).
    const quint8 currentQuality = d->quality.load();
    const quint8 qp = quint8(std::lround(40.0 - 0.28 * currentQuality));
    RDPGFX_H264_QUANT_QUALITY quality{};
    quality.qp = qp;
    quality.qualityVal = currentQuality;
    avcStream.meta.quantQualityVals = &quality;

    d->gfxContext->StartFrame(d->gfxContext.get(), &startFramePdu);
    d->gfxContext->SurfaceCommand(d->gfxContext.get(), &surfaceCommand);
    d->gfxContext->EndFrame(d->gfxContext.get(), &endFramePdu);

    return true;
}
}

#include "moc_VideoStream.cpp"
