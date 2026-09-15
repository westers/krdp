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

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

#include "NetworkDetection.h"
#include "PeerContext_p.h"
#include "RdpConnection.h"

#include "krdp_logging.h"

namespace KRdp
{

namespace clk = std::chrono;

constexpr clk::system_clock::duration FrameRateEstimateAveragePeriod = clk::seconds(1);
constexpr uint16_t MaxRdpCoordinate = std::numeric_limits<uint16_t>::max();
constexpr int MinimumFrameRate = 5;
constexpr int MaxMonitorLayoutCount = 16;
constexpr auto KeyFrameRequestMinInterval = clk::seconds(2);

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
    uint16_t id;
    QSize size;
};

struct FrameRateEstimate {
    clk::system_clock::time_point timeStamp;
    int estimate = 0;
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
    Surface surface;

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

    int maximumFrameRate = 120;
    std::atomic_int requestedFrameRate = 60;
    QQueue<FrameRateEstimate> frameRateEstimates;
    clk::system_clock::time_point lastFrameRateEstimation;

    std::atomic_int encodedFrames = 0;
    std::atomic_int frameDelay = 0;
    std::atomic_int decoderQueueDepth = 0;
    clk::milliseconds previousRtt = clk::milliseconds(0);
    QVector<VideoMonitor> monitorLayout;
    // Submission-thread only: rate-limits keyFrameRequested so a reset storm
    // (e.g. repeated caps re-advertisement) does not restart the encoder more
    // than once per KeyFrameRequestMinInterval.
    clk::steady_clock::time_point lastKeyFrameRequest;
};


VideoStream::VideoStream(RdpConnection *session)
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
    d->session = session;
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

    connect(d->session->networkDetection(), &NetworkDetection::rttChanged, this, &VideoStream::updateRequestedFrameRate);

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

    return true;
}

void VideoStream::close()
{
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
        d->surface = Surface{};
        d->monitorLayout.clear();
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.clear();
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

    if (frameAcknowledge->queueDepth & SUSPEND_FRAME_ACKNOWLEDGEMENT) {
        qDebug() << "suspend frame ack";
        d->decoderQueueDepth = 16;
    } else if (frameAcknowledge->queueDepth != QUEUE_DEPTH_UNAVAILABLE) {
        d->decoderQueueDepth = static_cast<int>(frameAcknowledge->queueDepth);
    }

    d->frameDelay = d->encodedFrames - frameAcknowledge->totalFramesDecoded;
    d->pendingFrames.erase(itr);

    return CHANNEL_RC_OK;
}

void VideoStream::performReset(const QSize &size, const QVector<VideoMonitor> &monitors)
{
    RDPGFX_RESET_GRAPHICS_PDU resetGraphicsPdu;
    resetGraphicsPdu.width = size.width();
    resetGraphicsPdu.height = size.height();
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

    qCDebug(KRDP) << "Reset graphics monitor layout:" << monitorLayoutSummary(monitors);
    d->gfxContext->ResetGraphics(d->gfxContext.get(), &resetGraphicsPdu);

    RDPGFX_CREATE_SURFACE_PDU createSurfacePdu;
    createSurfacePdu.width = size.width();
    createSurfacePdu.height = size.height();
    uint16_t surfaceId = d->nextSurfaceId++;
    createSurfacePdu.surfaceId = surfaceId;
    createSurfacePdu.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;
    d->gfxContext->CreateSurface(d->gfxContext.get(), &createSurfacePdu);

    d->surface = Surface{
        .id = surfaceId,
        .size = size,
    };

    RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU mapSurfaceToOutputPdu;
    mapSurfaceToOutputPdu.outputOriginX = 0;
    mapSurfaceToOutputPdu.outputOriginY = 0;
    mapSurfaceToOutputPdu.surfaceId = surfaceId;
    d->gfxContext->MapSurfaceToOutput(d->gfxContext.get(), &mapSurfaceToOutputPdu);
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

    const auto monitorLayout = monitorLayoutForReset(frame);
    const bool monitorLayoutChanged = (d->monitorLayout != monitorLayout);
    if (d->pendingReset || monitorLayoutChanged || d->surface.size != frame.size) {
        d->pendingReset = false;
        d->monitorLayout = monitorLayout;
        performReset(frame.size, monitorLayout);

        // A freshly created surface has no reference picture. If the frame we
        // are about to send is not a keyframe (e.g. after a caps
        // re-advertisement the queue holds P-frames), ask the session for one
        // now instead of waiting for the next organic IDR, which on a static
        // desktop can be seconds away (gop 100, frames only on damage).
        if (!frame.isKeyFrame) {
            const auto now = clk::steady_clock::now();
            if (d->lastKeyFrameRequest == clk::steady_clock::time_point{} || (now - d->lastKeyFrameRequest) >= KeyFrameRequestMinInterval) {
                d->lastKeyFrameRequest = now;
                qCDebug(KRDP) << "Surface (re)created on a non-keyframe, requesting a keyframe from the encoder";
                Q_EMIT keyFrameRequested();
            }
        }
    }

    d->session->networkDetection()->startBandwidthMeasure();

    auto frameId = d->frameId++;

    d->encodedFrames++;

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
    surfaceCommand.surfaceId = d->surface.id;
    surfaceCommand.codecId = RDPGFX_CODECID_AVC420;
    surfaceCommand.format = PIXEL_FORMAT_BGRX32;
    surfaceCommand.left = 0;
    surfaceCommand.top = 0;
    surfaceCommand.right = frame.size.width();
    surfaceCommand.bottom = frame.size.height();
    surfaceCommand.length = 0;
    surfaceCommand.data = nullptr;

    RDPGFX_AVC420_BITMAP_STREAM avcStream = {};
    surfaceCommand.extra = &avcStream;
    avcStream.data = (BYTE *)frame.data.data();
    avcStream.length = frame.data.length();

    avcStream.meta.numRegionRects = 1;
    RECTANGLE_16 rect = {0, 0, static_cast<UINT16>(frame.size.width()), static_cast<UINT16>(frame.size.height())};
    avcStream.meta.regionRects = &rect;
    RDPGFX_H264_QUANT_QUALITY quality = {22, 0, 100};
    avcStream.meta.quantQualityVals = &quality;

    d->gfxContext->StartFrame(d->gfxContext.get(), &startFramePdu);
    d->gfxContext->SurfaceCommand(d->gfxContext.get(), &surfaceCommand);
    d->gfxContext->EndFrame(d->gfxContext.get(), &endFramePdu);

    d->session->networkDetection()->stopBandwidthMeasure();
    return true;
}

void VideoStream::updateRequestedFrameRate()
{
    auto rtt = std::max(clk::duration_cast<clk::milliseconds>(d->session->networkDetection()->averageRTT()), clk::milliseconds(1));
    auto now = clk::system_clock::now();
    const auto delayedFrames = std::max(d->frameDelay.load(), 0);
    const auto decoderQueueDepth = std::max(d->decoderQueueDepth.load(), 0);

    int rttRiseMs = 0;
    if (d->previousRtt.count() > 0) {
        rttRiseMs = std::max(0, int((rtt - d->previousRtt).count()));
    }
    d->previousRtt = rtt;

    FrameRateEstimate estimate;
    estimate.timeStamp = now;
    const auto baseline = double(clk::milliseconds(1000).count()) / double(rtt.count());
    const auto delayPenalty = 1.0 + (double(delayedFrames) * 0.75);
    const auto queuePenalty = 1.0 + (double(std::min(decoderQueueDepth, 12)) * 0.25);
    const auto rttTrendPenalty = 1.0 + (double(std::clamp(rttRiseMs, 0, 20)) / 20.0);
    estimate.estimate = std::clamp(int(baseline / (delayPenalty * queuePenalty * rttTrendPenalty)), MinimumFrameRate, d->maximumFrameRate);
    d->frameRateEstimates.append(estimate);

    if (now - d->lastFrameRateEstimation < FrameRateEstimateAveragePeriod) {
        return;
    }

    d->lastFrameRateEstimation = now;

    d->frameRateEstimates.erase(std::remove_if(d->frameRateEstimates.begin(),
                                               d->frameRateEstimates.end(),
                                               [now](const auto &estimate) {
                                                   return (now - estimate.timeStamp) > FrameRateEstimateAveragePeriod;
                                               }),
                                d->frameRateEstimates.cend());

    auto sum = std::accumulate(d->frameRateEstimates.cbegin(), d->frameRateEstimates.cend(), 0, [](int acc, const auto &estimate) {
        return acc + estimate.estimate;
    });
    auto average = sum / d->frameRateEstimates.size();

    // Keep headroom so we can drain delay quickly when congestion appears.
    constexpr qreal targetFrameRateSaturation = 0.8;
    auto targetFrameRate = std::clamp(int(average * targetFrameRateSaturation), MinimumFrameRate, d->maximumFrameRate);

    // Hard clamps when decoder backlog is growing.
    if (delayedFrames >= 8 || decoderQueueDepth >= 10) {
        targetFrameRate = std::min(targetFrameRate, 10);
    } else if (delayedFrames >= 4 || decoderQueueDepth >= 6) {
        targetFrameRate = std::min(targetFrameRate, 20);
    } else if (delayedFrames >= 2 || decoderQueueDepth >= 3) {
        targetFrameRate = std::min(targetFrameRate, 30);
    }

    if (rttRiseMs >= 12) {
        targetFrameRate = std::min(targetFrameRate, 24);
    } else if (rttRiseMs >= 6) {
        targetFrameRate = std::min(targetFrameRate, 36);
    }

    int nextFrameRate = d->requestedFrameRate;
    if (targetFrameRate < d->requestedFrameRate) {
        // React quickly on congestion to avoid lag buildup.
        if (delayedFrames >= 2 || decoderQueueDepth >= 3 || rttRiseMs >= 8) {
            nextFrameRate = targetFrameRate;
        } else {
            nextFrameRate = std::max(targetFrameRate, d->requestedFrameRate - 5);
        }
    } else if (targetFrameRate > d->requestedFrameRate) {
        // Recover conservatively to prevent oscillation.
        nextFrameRate = std::min(targetFrameRate, d->requestedFrameRate + 2);
    }

    nextFrameRate = std::clamp(nextFrameRate, MinimumFrameRate, d->maximumFrameRate);

    if (nextFrameRate != d->requestedFrameRate) {
        d->requestedFrameRate = nextFrameRate;
        Q_EMIT requestedFrameRateChanged();
    }
}
}

#include "moc_VideoStream.cpp"
