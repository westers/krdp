// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// This file is roughly based on grd-rdp-graphics-pipeline.c from Gnome Remote
// Desktop which is:
//
// SPDX-FileCopyrightText: 2021 Pascal Nowack
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoStream.h"
#include "SafeThread.h"
#include "AvcCodecSelection.h"

#include <algorithm>
#include <array>
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
#include "LinkEvidence.h"
#include "NetworkDetection.h"
#include "PeerContext_p.h"
#include "RdpConnection.h"
#include "StatsReporter.h"
#include "SurfaceChain.h"

#include "LogThrottle.h"
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

uint32_t gfxQoEFrameAcknowledge(RdpgfxServerContext *context, const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU *qoe)
{
    auto stream = reinterpret_cast<VideoStream *>(context->custom);
    return stream->onQoeFrameAcknowledge(qoe);
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
    // AUD-FIX11 R6: which codec's reference chain this surface's decoder on the client is in.
    SurfaceChain chain;
    // The private codec family (HEVC/AV1) first sent on this surface: krdp-client opens one
    // decoder per surface for the first private codec it sees there and keeps it, so another
    // private codec needs new surfaces (a graphics reset) and with them new decoders.
    std::optional<int> privateFamily;
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
    // AUD-FIX10: the last Refresh Rect that asked for keyframes (steady_clock ticks; 0 = never).
    std::atomic<clk::steady_clock::rep> lastRefresh = 0;
    // Latches so a frame for a surface that does not exist, a frame whose size
    // does not match its surface, a failed CreateSurface or a session reporting
    // a broken layout is reported once and not once per frame. The first three
    // are cleared by a successful reset, so a later, different misconfiguration
    // is still reported.
    bool loggedUnknownSurface = false;
    bool loggedSizeMismatch = false;
    // AUD-FIX11: log each kind of chain drop once per reset, not per frame.
    bool loggedChainWait = false;
    bool loggedHeaderless = false;
    std::atomic<int> droppedAtSendFamily = -1;
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
    AvcCodecSelection codecSelection;
    // AUD-FIX2 codec policy (CodecPolicy.h); main thread only. Active once the client asked for
    // at least one private codec.
    CodecPolicy::Encoders encoders; // default: none, so only AVC until setEncoderPolicy()
    CodecPolicy::SoftwareEncoding softwareEncoding = CodecPolicy::SoftwareEncoding::Auto;
    // AV1-Q: the host's AV1 tile setting and the client's decode paths (main thread only).
    int av1TilesSetting = CodecPolicy::Av1TilesAutomatic;
    CodecPolicy::ClientDecode clientDecode;
    bool codecPolicyActive = false;
    QList<CodecPolicy::Family> clientFamilies;
    bool codecPolicyAdaptive = true;
    CodecPolicy::State codecPolicy;
    // OPT-062 S1: the ordered request an old client's record stands for, and the host's software
    // allowance, for every CodecPolicy::Input of this connection. S2 replaces the request by the
    // client's own order/encode/decode modes (explicitRequest).
    std::optional<CodecPolicy::Request> explicitRequest;
    LogThrottle codecChoiceLog{std::chrono::seconds(30), 3};
    void fillSelection(CodecPolicy::Input &in) const
    {
        in.request = explicitRequest ? *explicitRequest : CodecPolicy::requestFromRecord(clientFamilies, clientDecode, codecPolicyAdaptive);
        in.host.allowance = CodecPolicy::allowanceFor(softwareEncoding);
    }
    // OPT-055: the "waiting for the switch interval" reason last logged; the line repeats every ~1.5 s otherwise.
    QString lastWaitReason;
    CodecPolicy::LoadWindow encodeLoad;
    // What the sessions' encoders were last told (encoderSettingsChanged); main thread only.
    std::optional<CodecPolicy::EncoderSettings> encoderSettings;
    // The backend an encoder of the current codec reported, and whether the client heard it.
    std::optional<bool> reportedHardware;
    // Encode-cost sampling for the CPU guard (CodecPolicy::encodeLoadSample()): process CPU time
    // per encoded frame over the encoder's parallelism, against the frame budget, and the
    // delivered frame rate. framesEncoded counts queueFrame() calls.
    std::atomic<int> framesEncoded = 0;
    // AUD-FIX7: where the CPU guard reads the encoder's CPU time (ns, monotonic; -1 = unknown).
    // Empty: this process (krdpserver encodes in-process). A broker sets the worker's report.
    std::function<qint64()> cpuTimeSource;
    // AUD-FIX7: the codec family whose frames queueFrame() last dropped as not matching the
    // connection's codec (-1 = none), so the drop is logged once per switch, not per frame.
    std::atomic<int> droppedCodecFamily = -1;
    int framesAtLastSample = 0;
    qint64 cpuNsAtLastSample = -1;
    clk::steady_clock::time_point loadSampledAt{};
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
    // AUD-FIX6 F2: per monitor, when a starved monitor may ask for a keyframe (guarded by
    // frameQueueMutex); the largest recent keyframe (frameQueueMutex) and its size for the
    // byte budget (any thread); the monitors whose keyframe was acknowledged since the
    // submission thread last looked (bit per monitor, set on the peer thread).
    std::vector<FrameQueuePolicy::KeyFrameRequestBackoff> keyFrameBackoff;
    // AUD-FIX12: per monitor, the run of frames dropped for want of a keyframe (frameQueueMutex).
    std::vector<FrameQueuePolicy::SurfaceSilence> silence;
    // AUD-FIX12: TCP's network-limited delivery rate (stats samples and, AUD-FIX13, every
    // adaptive interval; the main thread).
    Stats::CapacityEstimate capacity;
    // AUD-FIX13: the socket's view of each interval's congestion (main thread): whether the
    // network or the client holds the stream back (LinkEvidence), and the stats' `policy.limit`.
    LinkEvidence::State linkEvidence;
    LinkEvidence::Verdict linkVerdict;
    std::optional<quint32> linkCapacityKbps;
    LinkEvidence::Limit limit = LinkEvidence::Limit::None;
    // Monitors whose frames piled up behind an unacknowledged keyframe: they may keep
    // keyFrameHoldFrames() queued until that backlog has drained to MaxHeldFramesPerMonitor,
    // even after the keyframe was acknowledged (frameQueueMutex).
    FrameQueuePolicy::KeyFramePeak keyFramePeak;
    std::atomic<qint64> keyFramePeakBytes = 0;
    // AUD-FIX7 F2: the frame-ack latency the window follows (FrameAckTracker::ackLatency(), set
    // on each ack), the average delta frame the byte budget covers it with (submission thread),
    // and whether the socket had sent what it was given at the last window check.
    std::atomic<qint64> windowAckLatencyUs = 0;
    std::atomic<qint64> averageFrameBytes = 0;
    std::atomic<bool> windowLinkClear = false;
    clk::steady_clock::time_point linkBusyAt{}; // submission thread: the last check that saw a full socket
    // The frame rate the window is sized for: the policy's, not the throttled one (a window that
    // shrank with the throttle would deliver less, and the throttle would follow it down).
    std::atomic<int> windowFrameRate = CodecPolicy::DefaultFrameRate;
    // AUD-FIX7 F2: how many unacknowledged frames still count as keeping up (backlogFrames()),
    // refreshed each interval from the client's ack-latency floor.
    std::atomic<int> backlogFrames = AdaptiveQuality::BacklogFrames;
    // AUD-FIX7 F2: the capture/encode frame rate under a full window (main thread): the policy's
    // rate (policyFrameRate) or less, never frames dropped after encoding.
    FrameQueuePolicy::DeliveryThrottle deliveryThrottle;
    int policyFrameRate = CodecPolicy::DefaultFrameRate;
    quint64 acknowledgedAtDecision = 0;
    quint64 sentAtDecision = 0;
    int offeredAtDecision = 0;
    clk::steady_clock::time_point decisionAt{};
    // Submission thread only: a deferred request was logged (once per stream).
    bool loggedKeyFrameDeferred = false;
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
    std::atomic<quint64> statKeyFramesDeferred = 0;
    std::atomic<quint64> statKeyFramesForced = 0; ///< AUD-FIX12 (SurfaceSilence)
    std::atomic<bool> statSuspended = false;
    // KRDPCTL `stats` (StatsReporter): what samples read besides flowStats(). The counters only
    // grow and cost one relaxed add per frame whether or not anyone is subscribed.
    std::unique_ptr<StatsReporter> stats;
    std::atomic<quint64> statBytesSent = 0;
    std::atomic<quint64> statKeyFramesSent = 0;
    std::array<std::atomic<quint64>, MaxMonitorLayoutCount> statSurfaceSent{};
    std::array<std::atomic<quint64>, MaxMonitorLayoutCount> statSurfaceCoalesced{};
    std::array<std::atomic<quint64>, MaxMonitorLayoutCount> statSurfaceKeyFrames{};
    std::atomic<qint64> desktopSize = 0; ///< width << 32 | height of the last ResetGraphics
    // Stage 7: RDPGFX QoE frame acknowledgements (peer thread).
    std::atomic<quint64> qoeFrames = 0;
    std::atomic<quint64> qoeDecodeMs = 0;
    std::atomic<quint64> qoeRenderMs = 0;
    std::atomic<quint32> qoeLastFrame = 0;
    std::atomic<quint32> qoeLastTimes = 0; ///< timeDiffSE << 16 | timeDiffEDR
    // Main thread: the last congestion verdict, the backend an encoder last reported, and what a
    // worker's EncoderStats and a measured encode time said.
    bool statCongested = false;
    std::optional<std::pair<int, bool>> reportedBackend; ///< codec family, hardware
    bool workerStats = false;
    quint64 workerFramesEncoded = 0;
    quint64 workerFramesSkipped = 0;
    std::optional<double> measuredEncodeMs;
    clk::steady_clock::time_point measuredEncodeAt{};
    // Any thread: the backend stats events carry (-1 = unknown), refreshed on the main thread.
    std::atomic<int> statsBackend = -1;

    void countSurfaceCoalesced(int monitorIndex, quint64 count = 1)
    {
        if (monitorIndex >= 0 && monitorIndex < MaxMonitorLayoutCount) {
            statSurfaceCoalesced[size_t(monitorIndex)].fetch_add(count, std::memory_order_relaxed);
        }
    }

    FrameQueuePolicy::WindowLimits windowLimits() const
    {
        return FrameQueuePolicy::windowLimits(windowFrameRate.load(),
                                              surfaceCount.load(),
                                              clk::microseconds(windowMinRttUs.load()),
                                              windowRateKbps.load(),
                                              keyFramePeakBytes.load(),
                                              clk::microseconds(windowAckLatencyUs.load()),
                                              averageFrameBytes.load(),
                                              windowLinkClear.load());
    }
    // Call with frameQueueMutex held.
    void ensureMonitor(int monitorIndex)
    {
        if (monitorIndex >= 0 && size_t(monitorIndex) >= starvedMonitors.size()) {
            starvedMonitors.resize(size_t(monitorIndex) + 1, 0);
            keyFrameBackoff.resize(size_t(monitorIndex) + 1);
        }
        if (monitorIndex >= 0 && size_t(monitorIndex) >= silence.size()) {
            silence.resize(size_t(monitorIndex) + 1);
        }
    }
    // Call with frameQueueMutex held (AUD-FIX12).
    void droppedForKeyFrame(int monitorIndex, clk::steady_clock::time_point now)
    {
        if (monitorIndex >= 0 && monitorIndex < MaxMonitorLayoutCount) {
            ensureMonitor(monitorIndex);
            silence[size_t(monitorIndex)].dropped(now);
        }
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
        for (auto &run : silence) {
            run.delivered(); // new surfaces (or none): nothing is owed to the old ones
        }
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

    d->stats = std::make_unique<StatsReporter>(
        [this] {
            return statsSnapshot();
        },
        [this](const QJsonObject &record) {
            // Only a KRDPCTL client ever subscribes; the subscription ends with its channel.
            if (!d->session->hasControlChannel()) {
                return false;
            }
            d->session->sendControlRecord(record);
            return true;
        });
    connect(d->stats.get(), &StatsReporter::subscriptionChanged, this, &VideoStream::statsSubscriptionChanged);
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

    // OPT-055 K6.5: a failed thread start returns false (the caller closes the connection)
    // instead of throwing into a -fno-exceptions tree.
    if (!startJThread(d->frameSubmissionThread, [this](std::stop_token token) {
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
    })) {
        qCWarning(KRDP) << "Could not start the frame submission thread";
        d->gfxContext->Close(d->gfxContext.get());
        d->gfxContext.reset();
        return false;
    }

    qCInfo(KRDP) << "Video stream initialized";

    d->streamingSince = clk::steady_clock::now();
    d->linkBusyAt = d->streamingSince; // clear only after LinkClearHold of evidence
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
    // AUD-FIX7: bytes of one codec never go out under another codec's id. Around a codec switch
    // the old encoder's last frames (a worker process's still in flight on its socket) are
    // dropped; the new encoder opens with a keyframe.
    if (frame.codec) {
        const int produced = int(*frame.codec);
        if (*frame.codec != codecForSessions()) {
            if (d->droppedCodecFamily.exchange(produced) != produced) {
                qCInfo(KRDP) << "Dropping" << VideoCodecSupport::codecName(*frame.codec) << "frames: the connection now sends"
                             << VideoCodecSupport::codecName(codecForSessions());
            }
            return;
        }
        d->droppedCodecFamily = -1;
    }
    d->framesEncoded.fetch_add(1, std::memory_order_relaxed); // the codec policy's CPU guard

    {
        std::lock_guard lock(d->frameQueueMutex);
        // AUD-FIX6 F2: the byte budget covers twice the largest recent keyframe, this one
        // included, so the P-frames behind a big keyframe can follow it.
        const auto now = clk::steady_clock::now();
        if (frame.isKeyFrame && !frame.data.isEmpty()) {
            d->keyFramePeak.record(frame.data.size() + frame.aux.size(), now);
        }
        d->keyFramePeakBytes = d->keyFramePeak.largest(now);
        // AUD-FIX4 D1: this monitor's queued frames were coalesced away while the
        // window was full, so a P-frame has no reference on the client: drop it
        // until the keyframe that was asked for (or the next organic one) arrives.
        if (d->starved(frame.monitorIndex)) {
            if (!frame.isKeyFrame || frame.data.isEmpty()) {
                d->statDropped.fetch_add(1, std::memory_order_relaxed);
                d->countSurfaceCoalesced(frame.monitorIndex);
                d->droppedSinceDecision.fetch_add(1, std::memory_order_relaxed);
                d->droppedForKeyFrame(frame.monitorIndex, now);
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

bool VideoStream::requestRefresh()
{
    if (!d->enabled) {
        return false;
    }
    const auto now = clk::steady_clock::now().time_since_epoch().count();
    auto last = d->lastRefresh.load();
    do {
        if (last != 0 && clk::steady_clock::duration(now - last) < RefreshMinInterval) {
            qCDebug(KRDP) << "Refresh Rect within" << RefreshMinInterval.count() << "ms of the last one; ignored";
            return false;
        }
    } while (!d->lastRefresh.compare_exchange_weak(last, now));
    const int surfaces = std::max(1, d->surfaceCount.load());
    qCInfo(KRDP) << "Client asked for a refresh; requesting a keyframe for" << surfaces << "surface(s)";
    d->statKeyFrameRequests.fetch_add(1, std::memory_order_relaxed);
    d->stats->event(Stats::EventKind::KeyFrame, [this, surfaces] {
        return statsDetail(QStringLiteral("keyframe requested for %1 surface(s): the client asked for a refresh").arg(surfaces));
    });
    for (int monitor = 0; monitor < surfaces; ++monitor) {
        Q_EMIT keyFrameRequested(monitor);
    }
    return true;
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
    qCInfo(KRDP) << "Monitor layout configured:" << (layout.isEmpty() ? QStringLiteral("(derived from frames)") : monitorLayoutSummary(layout));
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
    if (const auto changed = d->codecSelection.setPreference(preference)) Q_EMIT negotiatedCodecChanged(*changed);
}

CodecPreference VideoStream::codecPreference() const
{
    return d->codecSelection.preference();
}

void VideoStream::setAvc444Available(bool available)
{
    if (const auto changed = d->codecSelection.setAvc444Available(available)) Q_EMIT negotiatedCodecChanged(*changed);
}

void VideoStream::setPrivateCodec(std::optional<VideoCodec> codec)
{
    if (const auto changed = d->codecSelection.setPrivateCodec(codec)) Q_EMIT negotiatedCodecChanged(*changed);
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
/// A codec-policy reason that is one of the CPU guard's own steps (not a retry after a hold).
bool isGuardStep(const QString &reason)
{
    return reason.startsWith(QLatin1String("CPU guard")) && !reason.startsWith(QLatin1String("CPU guard: retrying"));
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

void VideoStream::setAv1TilesSetting(int tiles)
{
    d->av1TilesSetting = tiles;
    if (d->encoderSettings) {
        applyEncoderSettings(*d->encoderSettings); // a settings reload between connections: re-resolve
    }
}

int VideoStream::av1TilesSetting() const
{
    return d->av1TilesSetting;
}

void VideoStream::setClientDecode(const CodecPolicy::ClientDecode &decode)
{
    d->clientDecode = decode;
    if (d->encoderSettings) {
        applyEncoderSettings(*d->encoderSettings);
    }
}

CodecPolicy::ClientDecode VideoStream::clientDecode() const
{
    return d->clientDecode;
}

int VideoStream::av1Tiles() const
{
    return CodecPolicy::resolveAv1Tiles(d->av1TilesSetting, d->clientDecode.av1);
}

CodecPolicy::SoftwareEncoding VideoStream::softwareEncoding() const
{
    return d->softwareEncoding;
}

namespace
{
/// OPT-062: one line per decision: the request, what was skipped and why, what was selected.
QString codecDecisionText(const CodecPolicy::Decision &decision, const CodecPolicy::Input &in, bool adaptive)
{
    const auto &request = *in.request;
    QString text = QStringLiteral("Codec policy (%1): order [%2], encode %3, decode %4")
                       .arg(QLatin1String(CodecPolicy::softwareEncodingName(in.mode)),
                            CodecPolicy::orderText(CodecPolicy::normalizedOrder(request.order)),
                            QLatin1String(CodecPolicy::modeName(request.encode)),
                            QLatin1String(CodecPolicy::modeName(request.decode)));
    if (!decision.skipped.isEmpty()) {
        text += QStringLiteral("; skipped %1").arg(CodecPolicy::skippedText(decision.skipped));
    }
    text += QStringLiteral("; selected %1 (%2)%3%4")
                .arg(QLatin1String(CodecPolicy::familyName(decision.choice.family)),
                     decision.choice.hardware ? QStringLiteral("hardware") : QStringLiteral("software"),
                     decision.baseline ? QStringLiteral(", baseline: nothing in the list can be used") : QString(),
                     adaptive ? QString() : QStringLiteral(", fixed"));
    return text;
}
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
    d->fillSelection(in);
    in.adaptive = false; // the first choice never waits for a link measurement
    in.quality = d->quality.load(); // software HEVC/AV1 start at this quality's bitrate
    if (d->surfacePixels.load() > 0) {
        in.pixels = d->surfacePixels.load(); // else 1080p until the surfaces exist
    }
    const auto decision = CodecPolicy::step(d->codecPolicy, in, clk::steady_clock::now());
    applyEncoderSettings(decision.settings);
    setPrivateCodec(privateCodecOf(decision.choice.family));
    d->stats->setSlowLink(d->codecPolicy.slowLink);
    updateStatsBackend();
    d->stats->event(Stats::EventKind::Codec, [this, &decision] {
        return statsDetail(decision.reason.isEmpty() ? QStringLiteral("the client's codec request") : decision.reason);
    });
    qCInfo(KRDP).noquote() << codecDecisionText(decision, in, adaptive);
    return decision;
}

void VideoStream::privateCodecUnavailable(VideoCodec codec)
{
    const auto family = familyOf(codec);
    if (family == CodecPolicy::Family::Avc || !d->codecPolicyActive) {
        return;
    }
    d->encoders.of(family) = {};
    restepCodecPolicyNow(QStringLiteral("encoder unavailable"));
}

void VideoStream::restepCodecPolicyNow(const QString &reason)
{
    CodecPolicy::Input in;
    in.mode = d->softwareEncoding;
    in.encoders = d->encoders;
    in.client = d->clientFamilies;
    d->fillSelection(in);
    in.adaptive = d->codecPolicyAdaptive;
    in.quality = d->quality.load();
    if (d->surfacePixels.load() > 0) {
        in.pixels = d->surfacePixels.load();
    }
    d->codecPolicy.lastSwitch = {}; // a wrong stream cannot wait for the switch interval
    auto decision = CodecPolicy::step(d->codecPolicy, in, clk::steady_clock::now());
    decision.reason = reason;
    applyCodecDecision(decision);
}

void VideoStream::updateEncoderPolicy(const CodecPolicy::Encoders &encoders)
{
    d->encoders = encoders;
    if (!d->codecPolicyActive || !d->codecPolicy.current) {
        return;
    }
    // H.264 always has KPipeWire's own fallback (the backend report follows it); a private codec
    // whose chosen backend the encoding process lacks must be left at once.
    const auto current = *d->codecPolicy.current;
    if (current.family == CodecPolicy::Family::Avc) {
        return;
    }
    const auto &backends = encoders.of(current.family);
    if (current.hardware ? backends.hardware : backends.software) {
        return;
    }
    qCInfo(KRDP).noquote() << QStringLiteral("Codec policy: the encoding process has no %1 %2 encoder")
                                  .arg(current.hardware ? QStringLiteral("hardware") : QStringLiteral("software"),
                                       QLatin1String(CodecPolicy::familyName(current.family)));
    restepCodecPolicyNow(QStringLiteral("encoder unavailable"));
}

bool VideoStream::privateCodecPolicyActive() const
{
    return d->codecPolicyActive;
}

void VideoStream::setEncoderCpuTimeSource(std::function<qint64()> source)
{
    d->cpuTimeSource = std::move(source);
    d->cpuNsAtLastSample = -1;
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
        if (!decision.changed) {
            // A preset step, a bitrate reopen, a frame-rate cap: the CPU guard's own steps are
            // `cpu-guard`, the rest `settings`.
            d->stats->event(isGuardStep(decision.settingsReason) ? Stats::EventKind::CpuGuard : Stats::EventKind::Settings, [this, &decision] {
                return statsDetail(decision.settingsReason);
            });
        }
    }
    if (!decision.changed) {
        return;
    }
    d->encodeLoad.clear();
    d->cpuNsAtLastSample = -1;
    d->reportedHardware.reset();
    setPrivateCodec(privateCodecOf(decision.choice.family));
    if (quint64 suppressed = 0; d->codecChoiceLog.allow(&suppressed)) {
        qCInfo(KRDP).noquote() << QStringLiteral("Codec policy: switching to %1 in %2: %3%4%5")
                                      .arg(QLatin1String(CodecPolicy::familyName(decision.choice.family)),
                                           decision.choice.hardware ? QStringLiteral("hardware") : QStringLiteral("software"),
                                           decision.reason,
                                           decision.skipped.isEmpty() ? QString() : QStringLiteral("; skipped %1").arg(CodecPolicy::skippedText(decision.skipped)),
                                           suppressed ? QStringLiteral(" (%1 similar lines suppressed)").arg(suppressed) : QString());
    }
    updateStatsBackend();
    d->stats->event(Stats::EventKind::Codec, [this, &decision] {
        return statsDetail(decision.reason);
    });
    if (isGuardStep(decision.reason)) {
        d->stats->event(Stats::EventKind::CpuGuard, [this, &decision] {
            return statsDetail(decision.reason);
        });
    }
    // Unsolicited: no requestId. Only our own client ever gets here (it sent `codec`).
    d->session->sendControlRecord(LayoutControl::codecRecord(QString::fromLatin1(CodecPolicy::familyName(decision.choice.family)),
                                                             decision.choice.hardware,
                                                             decision.reason));
}

void VideoStream::applyEncoderSettings(const CodecPolicy::EncoderSettings &policySettings)
{
    // AV1-Q: the tile count is the host's and the client's, not the policy's.
    auto settings = policySettings;
    settings.av1Tiles = av1Tiles();
    const bool changed = d->encoderSettings != settings;
    d->encoderSettings = settings;
    // The frame-rate cap (software HEVC/AV1 at 30 fps, the CPU guard's last step) is what the
    // sessions ask the capture for and the budget the CPU guard measures against.
    d->policyFrameRate = settings.maxFrameRate > 0 ? std::min(settings.maxFrameRate, CodecPolicy::DefaultFrameRate) : CodecPolicy::DefaultFrameRate;
    d->windowFrameRate = d->policyFrameRate;
    refreshFrameRate();
    if (changed) {
        Q_EMIT encoderSettingsChanged(settings);
    }
}

void VideoStream::refreshFrameRate()
{
    // The policy's rate (software HEVC/AV1 cap, CPU guard), or less while the delivery throttle
    // holds the source back (AUD-FIX7 F2).
    const int rate = d->deliveryThrottle.rate(d->policyFrameRate);
    if (d->requestedFrameRate.exchange(rate) != rate) {
        qCInfo(KRDP) << "Video frame rate:" << rate << "fps" << (d->deliveryThrottle.active() ? "(throttled to what the client takes)" : "");
        d->stats->event(Stats::EventKind::Throttle, [this, rate] {
            return statsDetail(d->deliveryThrottle.active() ? QStringLiteral("frame rate %1 fps: throttled to what the client takes").arg(rate)
                                                            : QStringLiteral("frame rate %1 fps").arg(rate));
        });
        Q_EMIT requestedFrameRateChanged();
    }
}

std::optional<CodecPolicy::EncoderSettings> VideoStream::encoderSettings() const
{
    return d->encoderSettings;
}

void VideoStream::encoderBackendReported(VideoCodec codec, bool hardware)
{
    qCInfo(KRDP).noquote() << QStringLiteral("Encoder backend: %1 in %2").arg(QLatin1String(VideoCodecSupport::codecName(codec)), hardware ? QStringLiteral("hardware") : QStringLiteral("software"));
    // Stats (every client, a stock codec choice included): the backend that really encodes.
    const std::pair<int, bool> reported{int(familyOf(codec)), hardware};
    if (d->reportedBackend != reported) {
        d->reportedBackend = reported;
        updateStatsBackend();
        d->stats->event(Stats::EventKind::Codec, [this, hardware] {
            return statsDetail(QStringLiteral("encoder backend: %1").arg(hardware ? QStringLiteral("hardware") : QStringLiteral("software")));
        });
    }
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
    updateStatsBackend();
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
    if (!d->codecPolicy.current) {
        return;
    }
    // AUD-FIX14: a client that decodes only AVC gets no codec policy, but its link state still
    // counts (the stats' slow link and limit, the capacity probe): CodecPolicy::stepLink().
    const bool steering = d->codecPolicyActive;
    // CPU guard input: only a software encoder has one.
    const qint64 cpuNs = d->cpuTimeSource ? d->cpuTimeSource() : processCpuNs();
    const int frames = d->framesEncoded.load();
    const auto sampledAt = clk::steady_clock::now();
    if (cpuNs < 0 || (d->cpuNsAtLastSample >= 0 && cpuNs < d->cpuNsAtLastSample)) {
        // OPT-063: the worker's figure is gone or restarted (a new process counts from zero): the
        // intervals before it say nothing about the new encoder, whose start-up is not load either.
        d->encodeLoad.clear();
        d->cpuNsAtLastSample = -1;
    }
    if (steering && !d->codecPolicy.current->hardware && cpuNs >= 0 && d->cpuNsAtLastSample >= 0) {
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
    d->fillSelection(in);
    in.adaptive = d->codecPolicyAdaptive;
    if (network && network->validBandwidthSamples() >= 2) {
        in.bandwidthKbps = network->bandwidth();
    }
    in.congested = congested;
    in.pixels = std::max<qint64>(d->surfacePixels.load(), 1);
    // Software HEVC/AV1 follow adaptive quality through their target bitrate (a CRF change would
    // reopen the encoder; libx265 changes its bitrate in place): CodecPolicy::qualityKbps().
    in.quality = d->quality.load();
    // AUD-FIX5 D2: quality held under its cap while congestion keeps coming back is a slow link.
    in.qualityCap = d->qualityCap.load();
    // AUD-FIX12: a throttled source proves no headroom on a slow link.
    in.throttled = d->deliveryThrottle.active();
    // AUD-FIX13: only congestion with socket evidence is the network's; a client-limited stream's
    // throttle is the client's. judgeLink() ran just before.
    in.networkLimited = d->linkVerdict.network;
    in.throttledByLink = d->linkVerdict.throttledByLink;
    in.clientLimited = d->linkVerdict.clientLimited;
    in.capacityKbps = d->linkCapacityKbps;
    // AUD-FIX14: with socket figures the windowed capacity verdict decides the slow link.
    if (d->linkVerdict.network) {
        in.linkSlow = d->linkVerdict.slow;
        in.linkFast = d->linkVerdict.fast;
        in.linkWhy = d->linkVerdict.linkWhy;
        in.capacityNow = d->linkVerdict.capacityNow;
        in.linkIdle = d->linkVerdict.idle;
    }
    if (!d->codecPolicy.current->hardware) {
        in.encodeLoadP95 = d->encodeLoad.p95();
    }
    const bool wasSlow = d->codecPolicy.slowLink;
    const auto decision = steering ? CodecPolicy::step(d->codecPolicy, in, clk::steady_clock::now()) : CodecPolicy::stepLink(d->codecPolicy, in, clk::steady_clock::now());
    if (!decision.changed && !decision.reason.isEmpty()) {
        // Log only when the reason changes, not on every steering tick of the same wait.
        if (decision.reason != d->lastWaitReason) {
            d->lastWaitReason = decision.reason;
            qCInfo(KRDP).noquote() << "Codec policy:" << decision.reason;
        }
    } else if (decision.changed || decision.reason.isEmpty()) {
        d->lastWaitReason.clear();
    }
    if (d->codecPolicy.slowLink != wasSlow) {
        // AUD-FIX12: at info level (the rollout's slow-link check found no journal line).
        qCInfo(KRDP).noquote() << (d->codecPolicy.slowLink ? QStringLiteral("Video: the link is slow:") : QStringLiteral("Video: the link is no longer slow:"))
                               << (!decision.linkReason.isEmpty() ? decision.linkReason : QStringLiteral("goodput %1 kbit/s").arg(in.bandwidthKbps.value_or(0)));
        d->stats->setSlowLink(d->codecPolicy.slowLink); // samples at 1 Hz on a slow link
        d->stats->event(Stats::EventKind::SlowLink, [this, &decision, &in] {
            const QString goodput = in.bandwidthKbps ? QStringLiteral(" (%1 kbit/s)").arg(*in.bandwidthKbps) : QString();
            if (!decision.linkReason.isEmpty()) {
                return statsDetail(decision.linkReason);
            }
            return statsDetail((d->codecPolicy.slowLink ? QStringLiteral("slow link") : QStringLiteral("link recovered")) + goodput);
        });
    }
    if (decision.capacityProbe) {
        // AUD-FIX14: a keyframe on every surface is a burst TCP can measure the path with.
        const int surfaces = std::max(1, d->surfaceCount.load());
        qCInfo(KRDP).noquote() << QStringLiteral("Video: probing the link's capacity (slow link, no TCP capacity sample): requesting a keyframe for %1 surface(s)").arg(surfaces);
        d->statKeyFrameRequests.fetch_add(1, std::memory_order_relaxed);
        d->stats->event(Stats::EventKind::KeyFrame, [this, surfaces] {
            return statsDetail(QStringLiteral("keyframe requested for %1 surface(s): capacity probe on a slow link").arg(surfaces));
        });
        for (int monitor = 0; monitor < surfaces; ++monitor) {
            Q_EMIT keyFrameRequested(monitor);
        }
    }
    if (steering) {
        applyCodecDecision(decision);
    }
}

std::optional<VideoCodec> VideoStream::negotiatedCodec() const
{
    return d->codecSelection.negotiated();
}

VideoCodec VideoStream::codecForSessions() const
{
    return d->codecSelection.forSessions();
}

bool VideoStream::requestedChroma() const
{
    return d->chromaEnabled.load();
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

void VideoStream::updateDeliveryThrottle(bool windowPressure)
{
    const auto now = clk::steady_clock::now();
    const quint64 acknowledged = d->statAcknowledged.load();
    const quint64 sent = d->statSent.load();
    const int offeredTotal = d->framesEncoded.load();
    bool suspended = false;
    clk::microseconds baseAckLatency{0};
    {
        std::lock_guard lock(d->pendingFramesMutex);
        suspended = d->pendingFrames.suspended();
        baseAckLatency = d->pendingFrames.baseAckLatency(now);
    }
    // AUD-FIX7 F2: a client with a long but steady pipeline is that many frames behind by nature.
    d->backlogFrames = FrameQueuePolicy::backlogFrames(d->requestedFrameRate.load(), d->surfaceCount.load(), baseAckLatency, AdaptiveQuality::BacklogFrames);
    const bool first = d->decisionAt == clk::steady_clock::time_point{};
    const double seconds = first ? 0.0 : clk::duration<double>(now - d->decisionAt).count();
    // What the client took, per surface: acknowledged frames (sent ones while acks are suspended).
    const quint64 taken = suspended ? sent - d->sentAtDecision : acknowledged - d->acknowledgedAtDecision;
    const int offeredFrames = offeredTotal - d->offeredAtDecision;
    d->offeredAtDecision = offeredTotal;
    d->decisionAt = now;
    d->acknowledgedAtDecision = acknowledged;
    d->sentAtDecision = sent;
    if (first || seconds <= 0.0 || d->surfacePixels.load() == 0) {
        return;
    }
    const double delivered = double(taken) / seconds / std::max(1, d->surfaceCount.load());
    const double offered = double(offeredFrames) / seconds / std::max(1, d->surfaceCount.load());
    const bool wasActive = d->deliveryThrottle.active();
    const int before = d->deliveryThrottle.rate(d->policyFrameRate);
    const int after = d->deliveryThrottle.update(now, d->policyFrameRate, offered, delivered, windowPressure);
    if (after != before) {
        qCInfo(KRDP).noquote() << QStringLiteral("Video: %1 the frame rate to %2 fps (the source made %3 fps, the client took %4%5)")
                                      .arg(after < before ? QStringLiteral("throttling") : (wasActive && !d->deliveryThrottle.active() ? QStringLiteral("restoring") : QStringLiteral("raising")))
                                      .arg(after)
                                      .arg(offered, 0, 'f', 1)
                                      .arg(delivered, 0, 'f', 1)
                                      .arg(windowPressure ? QStringLiteral(" behind a full window") : QString());
        refreshFrameRate();
    }
}

void VideoStream::judgeLink(bool congested)
{
    const auto now = clk::steady_clock::now();
    LinkEvidence::Signals in;
    in.at = now;
    in.congested = congested;
    in.throttled = d->deliveryThrottle.active();
    in.slowBelowKbps = CodecPolicy::slowBelowKbps(std::max<qint64>(d->surfacePixels.load(), 1));
    if (auto *network = d->session->networkDetection(); network && network->validBandwidthSamples() >= 2) {
        in.sentKbps = quint32(network->bandwidth());
    }
    if (const auto tcp = d->session->tcpInfo()) {
        d->capacity.sample(now, tcp->deliveryRateBytesPerSecond, tcp->deliveryRateAppLimited);
        LinkEvidence::Socket socket;
        socket.rttUs = tcp->rttUs;
        socket.minRttUs = tcp->minRttUs;
        socket.totalRetransmits = tcp->totalRetransmits;
        socket.queuedBytes = d->session->socketQueuedBytes();
        socket.appLimited = tcp->deliveryRateBytesPerSecond == 0 || tcp->deliveryRateAppLimited;
        socket.busyUs = tcp->busyUs;
        socket.rwndLimitedUs = tcp->rwndLimitedUs;
        in.socket = socket;
    }
    d->linkCapacityKbps = d->capacity.kbps(now);
    in.capacityKbps = d->linkCapacityKbps;
    d->linkVerdict = LinkEvidence::judge(d->linkEvidence, in);

    const auto &current = d->codecPolicy.current;
    const auto p95 = d->codecPolicyActive && current && !current->hardware ? d->encodeLoad.p95() : std::nullopt;
    const bool encoderOverBudget = p95 && *p95 > CodecPolicy::CpuGuardLimit;
    d->limit = LinkEvidence::classify(d->codecPolicy.slowLink, d->linkVerdict, encoderOverBudget);

    if (d->linkVerdict.clientLimitedChanged) {
        const QString reason = d->linkVerdict.clientLimited
            ? QStringLiteral("client-limited: the client takes fewer frames than are sent while the link has headroom (%1)").arg(d->linkVerdict.why)
            : QStringLiteral("no longer client-limited%1").arg(d->linkVerdict.why.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(d->linkVerdict.why));
        qCInfo(KRDP).noquote() << "Video:" << reason;
        d->stats->event(Stats::EventKind::ClientLimited, [this, &reason] {
            return statsDetail(reason);
        });
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
    d->statCongested = windowPressure;
    updateDeliveryThrottle(windowPressure);

    std::optional<bool> policyCongested;
    // The codec policy steps after adaptive quality, on every path out of here, so a software
    // HEVC/AV1 target bitrate follows this interval's quality at once.
    const auto policyStep = qScopeGuard([this, &policyCongested] {
        if (d->surfacePixels.load() > 0) {
            judgeLink(policyCongested.value_or(d->statCongested)); // AUD-FIX13: before the policy reads it
        }
        if (policyCongested) {
            stepCodecPolicy(*policyCongested);
        }
    });
    if (d->codecPolicy.current && d->surfacePixels.load() > 0 && clk::steady_clock::now() - d->streamingSince >= WarmupAfterStreamStart) {
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
            || pending >= d->backlogFrames.load() || windowPressure;
        policyCongested = congested;
        d->statCongested = congested;
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
    const bool backlogged = AdaptiveQuality::backlogIsPressure(now - d->streamingSince, minAfterAck, pendingNow, acksSuspended, d->backlogFrames.load()) || windowPressure;

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
    d->statCongested = d->statCongested || result.congested || backlogged;
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
    d->stats->event(Stats::EventKind::Quality, [&] {
        QStringList what;
        if (bounded != current) {
            what << QStringLiteral("quality %1 -> %2 (cap %3)").arg(current).arg(bounded).arg(d->qualityCap.load());
        }
        if (chromaChanged) {
            what << (result.chromaEnabled ? QStringLiteral("chroma stream restored") : QStringLiteral("chroma stream shed"));
        }
        return statsDetail(what.join(QStringLiteral("; ")) + QStringLiteral(": ")
                           + (result.congested ? QStringLiteral("congested") : (backlogged ? QStringLiteral("the client is behind") : QStringLiteral("clear"))));
    });
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

    qCInfo(KRDP) << "Selected caps:" << capVersionToString(selectedCaps->version);

    // Not reset to -1 on a re-advertisement (see the capsConfirmed branch above):
    // the previous codec stays the best guess until the caps parsed just above
    // settle on a new one a few lines later.
    const auto changed = d->codecSelection.acceptCaps(selectedCaps->version, selectedCaps->capSet.flags);
    const VideoCodec codec = codecForSessions();
    qCInfo(KRDP).noquote() << QStringLiteral("GFX caps confirmed: %1 codec=%2").arg(QLatin1String(capVersionToString(selectedCaps->version)), QLatin1String(VideoCodecSupport::codecName(codec)));
    if (changed) {
        // Gated on the codec actually (re)settling on avc420, not on every advertisement: a client
        // that re-sends CapsAdvertise with the same unsupported result (e.g. mstsc) would otherwise
        // log this every time instead of once per negotiation.
        if (codecPreference() == CodecPreference::Avc444 && codec == VideoCodec::Avc420) {
            qCWarning(KRDP) << "Codec=avc444 requested but client caps or encoder availability require avc420 (caps" << capVersionToString(selectedCaps->version) << ")";
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

    const auto ackedAt = clk::steady_clock::now();
    const auto ack = d->pendingFrames.acknowledge(id, frameAcknowledge->queueDepth, ackedAt);
    d->windowAckLatencyUs = d->pendingFrames.ackLatency(ackedAt).count();
    if ((ack == FrameQueuePolicy::FrameAckTracker::Ack::Acknowledged || ack == FrameQueuePolicy::FrameAckTracker::Ack::Suspended)
        && !d->graphicsDelivered.exchange(true)) {
        qCInfo(KRDP) << "The client acknowledged its first frame";
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

    qCInfo(KRDP) << "Reset graphics desktop" << desktopSize << "with" << monitors.size() << "monitor(s):" << monitorLayoutSummary(monitors);
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
    d->desktopSize = (qint64(desktopSize.width()) << 32) | qint64(quint32(desktopSize.height()));
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
    stats.keyFramesDeferred = d->statKeyFramesDeferred.load();
    stats.acksSuspended = d->statSuspended.load();
    stats.ackLatencyUs = d->windowAckLatencyUs.load();
    stats.frameRate = d->requestedFrameRate.load();
    stats.linkClear = d->windowLinkClear.load();
    return stats;
}

bool VideoStream::windowOpen(clk::steady_clock::time_point now)
{
    // AUD-FIX7 F2: a clear link (the socket sent what it got) lets the byte budget follow the
    // frame window of a client that is merely slow to acknowledge.
    const qint64 socketQueued = d->session->socketQueuedBytes();
    if (!FrameQueuePolicy::linkClear(socketQueued, d->keyFramePeakBytes.load())) {
        d->linkBusyAt = now;
    }
    d->windowLinkClear = now - d->linkBusyAt >= FrameQueuePolicy::LinkClearHold;
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
    return d->pendingFrames.canSend(limits, d->pendingFrames.suspended() ? socketQueued : -1);
}

void VideoStream::holdFrames(clk::steady_clock::time_point now)
{
    // AUD-FIX7 F2: frames wait behind a full window up to keyFrameHoldFrames() (the latency
    // bound); the delivery throttle slows the source meanwhile (updateAdaptiveQuality()). Only a
    // backlog beyond the bound is coalesced, which costs a keyframe: an encoded P-frame cannot be
    // dropped without breaking the chain behind it.
    const int keyFrameHold = FrameQueuePolicy::keyFrameHoldFrames(d->requestedFrameRate.load());
    std::vector<int> starved;
    int dropped = 0;
    {
        std::lock_guard lock(d->frameQueueMutex);
        const auto coalesce = [&](int monitor, int count) {
            const bool drop = FrameQueuePolicy::shouldCoalesce(count, keyFrameHold);
            if (drop) {
                d->countSurfaceCoalesced(monitor, quint64(count));
            }
            return drop;
        };
        dropped = FrameQueuePolicy::coalesceHeldFrames(d->frameQueue, coalesce, starved);
        for (const int monitor : starved) {
            if (monitor < 0) {
                continue;
            }
            d->ensureMonitor(monitor);
            d->starvedMonitors[size_t(monitor)] = 1;
            d->keyFrameBackoff[size_t(monitor)].dropped(now);
        }
    }
    if (dropped > 0) {
        d->statDropped.fetch_add(quint64(dropped), std::memory_order_relaxed);
        d->droppedSinceDecision.fetch_add(dropped, std::memory_order_relaxed);
        d->stats->event(Stats::EventKind::Coalesce, [this, dropped, &starved] {
            QStringList monitors;
            for (const int monitor : starved) {
                monitors << QString::number(monitor);
            }
            return statsDetail(QStringLiteral("the client is behind: dropped %1 queued frame(s) of monitor %2, keyframe requested")
                                   .arg(dropped)
                                   .arg(monitors.join(QLatin1Char(','))));
        });
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
    // AUD-FIX6 F2: a monitor whose keyframe is still unacknowledged asks for none, and requests
    // back off (KeyFrameRequestBackoff; AUD-FIX7: an ack no longer resets that).
    std::vector<char> keyFrameInFlight;
    {
        std::lock_guard lock(d->frameQueueMutex);
        keyFrameInFlight.assign(d->starvedMonitors.size(), 0);
    }
    if (!keyFrameInFlight.empty()) {
        // A starved monitor's P-frames are dropped before they queue, so windowOpen() may not
        // run: expire here too, or a keyframe the client never acknowledges would hold the
        // request back for good instead of for AckTimeout.
        const auto limits = d->windowLimits();
        std::lock_guard lock(d->pendingFramesMutex);
        if (const int timedOut = d->pendingFrames.expire(now, limits); timedOut > 0 && !d->loggedAckTimeout) {
            d->loggedAckTimeout = true;
            qCWarning(KRDP) << "The client left" << timedOut << "frame(s) unacknowledged for"
                            << clk::duration_cast<clk::seconds>(FrameQueuePolicy::AckTimeout).count() << "s; no longer counting them as in flight";
        }
        for (size_t monitor = 0; monitor < keyFrameInFlight.size(); ++monitor) {
            keyFrameInFlight[monitor] = d->pendingFrames.keyFrameInFlight(int(monitor));
        }
    }
    std::vector<int> request;
    std::vector<int> deferred;
    std::vector<std::pair<int, int>> forced; // monitor, forced requests in its silent run
    {
        std::lock_guard lock(d->frameQueueMutex);
        for (size_t monitor = 0; monitor < d->starvedMonitors.size(); ++monitor) {
            if (!d->starvedMonitors[monitor]) {
                continue;
            }
            const bool inFlight = monitor < keyFrameInFlight.size() && keyFrameInFlight[monitor];
            if (d->keyFrameBackoff[monitor].shouldRequest(now, inFlight)) {
                request.push_back(int(monitor));
            } else if (inFlight) {
                deferred.push_back(int(monitor));
            }
        }
        // AUD-FIX12: a surface that has dropped every frame for SilentSurfaceLimit asks again,
        // whatever the back-off or a keyframe in flight says (starved or chain-waiting alike).
        for (size_t monitor = 0; monitor < d->silence.size(); ++monitor) {
            auto &run = d->silence[monitor];
            if (run.shouldForce(now)) {
                if (std::find(request.begin(), request.end(), int(monitor)) == request.end()) {
                    request.push_back(int(monitor));
                }
                forced.emplace_back(int(monitor), run.forced());
                std::erase(deferred, int(monitor));
            }
        }
    }
    for (const auto &[monitor, count] : forced) {
        d->statKeyFramesForced.fetch_add(1, std::memory_order_relaxed);
        if (count == 1) {
            qCInfo(KRDP).noquote() << QStringLiteral("Video: monitor %1 has dropped every frame for %2 ms waiting for a keyframe; asking for one again")
                                          .arg(monitor)
                                          .arg(clk::duration_cast<clk::milliseconds>(FrameQueuePolicy::SilentSurfaceLimit).count());
        } else {
            qCDebug(KRDP) << "Monitor" << monitor << "still has no keyframe; forced request" << count;
        }
    }
    if (!deferred.empty()) {
        d->statKeyFramesDeferred.fetch_add(1, std::memory_order_relaxed);
        if (!d->loggedKeyFrameDeferred) {
            d->loggedKeyFrameDeferred = true;
            qCInfo(KRDP) << "Video: frames of monitor" << deferred.front()
                         << "were coalesced while its keyframe is still unacknowledged; the next keyframe is requested once it is";
        }
    }
    if (!request.empty()) {
        d->stats->event(Stats::EventKind::KeyFrame, [this, &request] {
            QStringList monitors;
            for (const int monitor : request) {
                monitors << QString::number(monitor);
            }
            return statsDetail(QStringLiteral("keyframe requested for monitor %1: its frames were coalesced").arg(monitors.join(QLatin1Char(','))));
        });
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

    // AUD-FIX11 R6: a frame queued before a codec switch is never sent under the new codec's id
    // (queueFrame() checked it against the codec of that moment).
    if (frame.codec && *frame.codec != codec) {
        const int produced = int(*frame.codec);
        if (d->droppedAtSendFamily.exchange(produced) != produced) {
            qCInfo(KRDP) << "Dropping queued" << VideoCodecSupport::codecName(*frame.codec) << "frames: the connection now sends" << VideoCodecSupport::codecName(codec);
        }
        d->statDropped.fetch_add(1, std::memory_order_relaxed);
        d->countSurfaceCoalesced(frame.monitorIndex);
        return true;
    }
    d->droppedAtSendFamily = -1;
    const VideoCodec produced = frame.codec.value_or(codec);
    const int family = codecFamily(produced);

    Surface surface;
    bool requestKeyFrame = false;
    bool send = true;
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
        // AUD-FIX11: HEVC <-> AV1 on a surface whose client decoder is the other one.
        const bool privateDecoderChange = target && family != 0 && target->privateFamily && *target->privateFamily != family;
        if (privateDecoderChange) {
            qCInfo(KRDP) << "Recreating the surfaces for" << VideoCodecSupport::codecName(produced) << "(the client's decoders are for another codec)";
        }
        if (d->pendingReset || d->monitorLayout != plan.monitors || !target || frameSizeChanged || privateDecoderChange) {
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
            d->loggedChainWait = false;
            d->loggedHeaderless = false;

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
            target->chain.broken(); // this frame is part of its encoder's chain
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
        // AUD-FIX11 R6: nothing of a codec before a keyframe of it, with its headers in band, has
        // gone out on this surface; a surface whose chain broke waits for the next one. (A fresh
        // surface after a codec switch or a layout proof otherwise got the first packets of a
        // chain whose start the client never saw: libdav1d rejected them as invalid data.)
        if (!auxOnly) {
            const auto verdict = target->chain.admit(produced, frame.codec.has_value(), frame.isKeyFrame, frame.data);
            if (verdict != SurfaceChain::Verdict::Send) {
                bool &logged = verdict == SurfaceChain::Verdict::WaitForKeyFrame ? d->loggedChainWait : d->loggedHeaderless;
                if (!logged) {
                    logged = true;
                    if (verdict == SurfaceChain::Verdict::WaitForKeyFrame) {
                        qCInfo(KRDP) << "Holding back" << VideoCodecSupport::codecName(produced) << "delta frames on monitor" << frame.monitorIndex
                                     << "until a keyframe of that codec starts its surface";
                    } else {
                        qCWarning(KRDP) << "Dropping a" << VideoCodecSupport::codecName(produced) << "keyframe for monitor" << frame.monitorIndex
                                        << "without in-band codec headers: no client decoder can start from it";
                    }
                }
                d->statDropped.fetch_add(1, std::memory_order_relaxed);
                d->countSurfaceCoalesced(frame.monitorIndex);
                const auto now = clk::steady_clock::now();
                auto &lastRequest = d->lastKeyFrameRequest[frame.monitorIndex];
                if (lastRequest == clk::steady_clock::time_point{} || (now - lastRequest) >= KeyFrameRequestMinInterval) {
                    lastRequest = now;
                    requestKeyFrame = true;
                }
                send = false;
            }
        }
        if (send) {
            if (family != 0 && !target->privateFamily) {
                target->privateFamily = family;
            }
            if (frame.isKeyFrame) {
                target->keyFrameSent = true;
            }
            surface = *target;
        }
    }

    if (requestKeyFrame) {
        // Emitted outside the lock: the slot runs on the session's thread, but
        // signalling under a lock another thread takes is a trap not worth
        // leaving lying around.
        qCDebug(KRDP) << "Surface (re)created on a non-keyframe, requesting a keyframe from the encoder for monitor" << frame.monitorIndex;
        Q_EMIT keyFrameRequested(frame.monitorIndex);
    }
    {
        // AUD-FIX12: the surface's silent run, after layoutMutex is released (lock order above).
        std::lock_guard lock(d->frameQueueMutex);
        if (!send) {
            d->droppedForKeyFrame(frame.monitorIndex, clk::steady_clock::now());
        } else if (frame.monitorIndex >= 0 && size_t(frame.monitorIndex) < d->silence.size()) {
            d->silence[size_t(frame.monitorIndex)].delivered();
        }
    }
    if (!send) {
        return true; // dropped (AUD-FIX11): not a failure to retry
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
        d->pendingFrames.frameSent(frameId, frame.data.size() + frame.aux.size(), clk::steady_clock::now(), frame.isKeyFrame && !frame.data.isEmpty(), frame.monitorIndex);
        if (!frame.isKeyFrame) {
            // AUD-FIX7 F2: what an average frame weighs, for the byte budget of a clear link.
            const qint64 bytes = frame.data.size() + frame.aux.size();
            const qint64 average = d->averageFrameBytes.load(std::memory_order_relaxed);
            d->averageFrameBytes.store(average == 0 ? bytes : average + (bytes - average) / 16, std::memory_order_relaxed);
        }
        const int inFlight = d->pendingFrames.inFlightFrames();
        d->statInFlight = inFlight;
        int seen = d->statMaxInFlight.load(std::memory_order_relaxed);
        while (inFlight > seen && !d->statMaxInFlight.compare_exchange_weak(seen, inFlight, std::memory_order_relaxed)) {
        }
    }
    d->statSent.fetch_add(1, std::memory_order_relaxed);
    d->statBytesSent.fetch_add(quint64(frame.data.size() + frame.aux.size()), std::memory_order_relaxed);
    const bool keyFrame = frame.isKeyFrame && !frame.data.isEmpty();
    if (keyFrame) {
        d->statKeyFramesSent.fetch_add(1, std::memory_order_relaxed);
    }
    if (frame.monitorIndex >= 0 && frame.monitorIndex < MaxMonitorLayoutCount) {
        d->statSurfaceSent[size_t(frame.monitorIndex)].fetch_add(1, std::memory_order_relaxed);
        if (keyFrame) {
            d->statSurfaceKeyFrames[size_t(frame.monitorIndex)].fetch_add(1, std::memory_order_relaxed);
        }
    }

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

uint32_t VideoStream::onQoeFrameAcknowledge(const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU *qoe)
{
    // Stage 7 (MS-RDPEGFX 2.2.2.21): the standard way a client tells the server how long its
    // decode (timeDiffSE) and the step to the screen (timeDiffEDR) took. Recorded for the stats.
    if (!qoe) {
        return CHANNEL_RC_OK;
    }
    d->qoeDecodeMs.fetch_add(qoe->timeDiffSE, std::memory_order_relaxed);
    d->qoeRenderMs.fetch_add(qoe->timeDiffEDR, std::memory_order_relaxed);
    d->qoeLastFrame.store(qoe->frameId, std::memory_order_relaxed);
    d->qoeLastTimes.store((quint32(qoe->timeDiffSE) << 16) | qoe->timeDiffEDR, std::memory_order_relaxed);
    d->qoeFrames.fetch_add(1, std::memory_order_release);
    return CHANNEL_RC_OK;
}

std::optional<VideoStream::ClientQoe> VideoStream::clientQoe() const
{
    if (d->qoeFrames.load(std::memory_order_acquire) == 0) {
        return std::nullopt;
    }
    const quint32 times = d->qoeLastTimes.load(std::memory_order_relaxed);
    return ClientQoe{d->qoeLastFrame.load(std::memory_order_relaxed), quint16(times >> 16), quint16(times & 0xffff)};
}

int VideoStream::setStatsSubscription(int rateHz)
{
    if (rateHz <= 0) {
        d->stats->unsubscribe();
        return 0;
    }
    d->stats->setSlowLink(d->codecPolicy.slowLink);
    return d->stats->subscribe(rateHz);
}

StatsReporter *VideoStream::statsReporter() const
{
    return d->stats.get();
}

bool VideoStream::statsSubscribed() const
{
    return d->stats->subscribed();
}

void VideoStream::addWorkerEncoderStats(quint32 framesEncoded, quint32 framesSkipped, std::optional<double> encodeMs)
{
    if (!d->workerStats) {
        // The first report: the totals start from what this process counted so far, so the
        // next sample's delta is the worker's interval, not everything before it.
        d->workerStats = true;
        d->workerFramesEncoded = quint64(std::max(d->framesEncoded.load(), 0));
    }
    d->workerFramesEncoded += framesEncoded;
    d->workerFramesSkipped += framesSkipped;
    if (encodeMs) {
        setMeasuredEncodeTime(*encodeMs);
    }
}

void VideoStream::setMeasuredEncodeTime(double ms)
{
    if (!std::isfinite(ms) || ms < 0) {
        return;
    }
    d->measuredEncodeMs = ms;
    d->measuredEncodeAt = clk::steady_clock::now();
}

std::optional<bool> VideoStream::statsHardware() const
{
    if (d->codecPolicyActive && d->codecPolicy.current) {
        return d->codecPolicy.current->hardware;
    }
    if (d->reportedBackend && d->reportedBackend->first == int(familyOf(codecForSessions()))) {
        return d->reportedBackend->second;
    }
    return std::nullopt;
}

void VideoStream::updateStatsBackend()
{
    const auto hardware = statsHardware();
    d->statsBackend = hardware ? int(*hardware) : -1;
}

Stats::EventDetail VideoStream::statsDetail(const QString &reason) const
{
    Stats::EventDetail detail;
    detail.reason = reason;
    if (const auto codec = negotiatedCodec()) {
        detail.codec = QString::fromLatin1(CodecPolicy::familyName(familyOf(*codec)));
    }
    if (const int backend = d->statsBackend.load(std::memory_order_relaxed); backend >= 0) {
        detail.hardware = backend != 0;
    }
    return detail;
}

Stats::Snapshot VideoStream::statsSnapshot() const
{
    // Main thread (the reporter's timer): the codec policy state is read where it is written.
    Stats::Snapshot s;
    const auto now = clk::steady_clock::now();
    if (const auto codec = negotiatedCodec()) {
        s.codec = QString::fromLatin1(CodecPolicy::familyName(familyOf(*codec)));
        s.chroma = VideoCodecSupport::isAvc444(*codec) ? QStringLiteral("444") : QStringLiteral("420");
    }
    s.hardware = statsHardware();
    if (s.hardware && !*s.hardware && !s.codec.isEmpty()) {
        s.preset = Stats::encoderPresetName(s.codec, d->encoderSettings ? int(d->encoderSettings->preset) : 0);
    }
    if (const qint64 packed = d->desktopSize.load(); packed > 0) {
        s.size = QSize(int(packed >> 32), int(packed & 0xffffffff));
    }
    s.frameRate = d->requestedFrameRate.load();
    s.frameRateCap = d->policyFrameRate;
    s.quality = d->quality.load();
    s.qualityCap = d->qualityCap.load();
    if (d->encoderSettings && d->encoderSettings->targetKbps > 0) {
        s.targetKbps = d->encoderSettings->targetKbps;
    }
    s.bytesSent = d->statBytesSent.load();
    s.framesSent = d->statSent.load();
    s.framesAcked = d->statAcknowledged.load();
    s.coalesced = d->statDropped.load();
    s.keyframes = d->statKeyFramesSent.load();
    s.keyframeRequests = d->statKeyFrameRequests.load();
    if (d->codecPolicyActive) {
        s.encoderRestarts = quint64(std::max(d->codecPolicy.encoderRestarts, 0));
    }
    s.framesEncoded = d->workerStats ? d->workerFramesEncoded : quint64(std::max(d->framesEncoded.load(), 0));
    if (d->workerStats) {
        s.framesSkipped = d->workerFramesSkipped;
    }
    if (d->codecPolicyActive && d->codecPolicy.current && !d->codecPolicy.current->hardware) {
        s.encodeLoadP95 = d->encodeLoad.p95();
    }
    if (d->measuredEncodeMs && now - d->measuredEncodeAt <= clk::seconds(5)) {
        s.encodeMs = d->measuredEncodeMs;
    } else if (s.encodeLoadP95) {
        s.encodeMs = *s.encodeLoadP95 * 1000.0 / std::max(1, s.frameRate); // the CPU guard's estimate
    }

    s.inFlight = d->statInFlight.load();
    s.window = d->statWindowFrames.load();
    s.windowBytes = d->statWindowBytes.load();
    if (const qint64 ack = d->windowAckLatencyUs.load(); ack > 0) {
        s.ackLatencyMs = double(ack) / 1000.0;
    }
    s.acksSuspended = d->statSuspended.load();
    s.throttled = d->deliveryThrottle.active();
    s.qoeFrames = d->qoeFrames.load(std::memory_order_acquire);
    s.qoeDecodeMs = d->qoeDecodeMs.load(std::memory_order_relaxed);
    s.qoeRenderMs = d->qoeRenderMs.load(std::memory_order_relaxed);

    const auto plausibleMs = [](auto duration) -> std::optional<double> {
        const double ms = clk::duration<double, std::milli>(duration).count();
        return ms > 0 && ms < 60000 ? std::optional<double>(ms) : std::nullopt;
    };
    const auto tcp = d->session->tcpInfo();
    if (auto *network = d->session->networkDetection()) {
        s.rttMs = plausibleMs(network->averageRTT());
        s.rttMinMs = plausibleMs(network->minimumRTT());
        s.sentSamples = network->validBandwidthSamples();
        if (s.sentSamples > 0) {
            s.sentKbps = network->bandwidth();
        }
    }
    if (tcp) {
        if (!s.rttMs && tcp->rttUs > 0) {
            s.rttMs = double(tcp->rttUs) / 1000.0;
        }
        s.rttVarMs = double(tcp->rttVarUs) / 1000.0;
        Stats::applyTcpRtt(s, tcp->rttUs, tcp->minRttUs);
        s.retransmits = tcp->totalRetransmits;
        if (tcp->deliveryRateBytesPerSecond > 0) {
            s.appLimited = tcp->deliveryRateAppLimited;
        }
        d->capacity.sample(now, tcp->deliveryRateBytesPerSecond, tcp->deliveryRateAppLimited);
    }
    // AUD-FIX12: capacity only with evidence: TCP's network-limited delivery rate, else (on a slow
    // link) the highest rate the slow-link probe held without congestion. Never the send rate.
    if (const auto measured = d->capacity.kbps(now)) {
        s.capacityKbps = measured;
        s.capacitySource = QStringLiteral("tcp");
    } else if (d->codecPolicy.slowLink && d->codecPolicy.provenKbps > 0) {
        s.capacityKbps = d->codecPolicy.provenKbps;
        s.capacitySource = QStringLiteral("probe");
    }
    if (const qint64 queued = d->session->socketQueuedBytes(); queued >= 0) {
        s.sendQueueBytes = queued;
    }
    s.congested = d->statCongested;
    s.slow = d->codecPolicy.slowLink;
    s.limit = QString::fromLatin1(LinkEvidence::limitName(d->limit));

    s.mode = QString::fromLatin1(CodecPolicy::softwareEncodingName(d->softwareEncoding));
    s.decode = d->clientDecode;
    if (s.codec == QLatin1String("av1") && d->encoderSettings) {
        s.av1Tiles = d->encoderSettings->av1Tiles;
    }
    if (d->codecPolicyActive) {
        s.adaptive = d->codecPolicyAdaptive;
        std::optional<qint64> retry;
        for (const auto family : {CodecPolicy::Family::Avc, CodecPolicy::Family::Hevc, CodecPolicy::Family::Av1}) {
            const auto until = d->codecPolicy.softwareBlockedUntil[size_t(family)];
            if (now < until) {
                s.heldBack << QString::fromLatin1(CodecPolicy::familyName(family));
                const qint64 seconds = clk::duration_cast<clk::seconds>(until - now + clk::milliseconds(999)).count();
                retry = retry ? std::min(*retry, seconds) : seconds;
            }
        }
        if (retry) {
            s.retryInS = int(std::min<qint64>(*retry, std::numeric_limits<int>::max()));
        }
        const auto &current = d->codecPolicy.current;
        const bool stepped = current && !current->hardware
            && ((current->family != CodecPolicy::Family::Avc && d->codecPolicy.preset != CodecPolicy::Preset::Efficient) || d->codecPolicy.guardFrameRate);
        s.guardState = stepped ? QStringLiteral("stepped") : !s.heldBack.isEmpty() ? QStringLiteral("holding") : QStringLiteral("ok");
    }

    const int surfaces = d->surfacePixels.load() > 0 ? std::clamp(d->surfaceCount.load(), 1, MaxMonitorLayoutCount) : 0;
    s.surfaces.reserve(surfaces);
    for (int i = 0; i < surfaces; ++i) {
        s.surfaces.append(Stats::SurfaceCounters{
            d->statSurfaceSent[size_t(i)].load(std::memory_order_relaxed),
            d->statSurfaceCoalesced[size_t(i)].load(std::memory_order_relaxed),
            d->statSurfaceKeyFrames[size_t(i)].load(std::memory_order_relaxed),
        });
    }
    return s;
}
}

#include "moc_VideoStream.cpp"
