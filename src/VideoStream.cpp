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
#include <cstdint>
#include <condition_variable>
#include <limits>
#include <vector>

#include <QDateTime>
#include <QQueue>
#include <QRect>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

#include "NetworkDetection.h"
#include "PeerContext_p.h"
#include "RdpConnection.h"
#include "VideoCodecSupport.h"

#include "krdp_logging.h"

namespace KRdp
{

namespace clk = std::chrono;

constexpr clk::system_clock::duration FrameRateEstimateAveragePeriod = clk::seconds(1);
constexpr int MaxCoalescedDamageRects = 64;
constexpr int MaxDamageRectCount = 128;
constexpr int MaxQueuedFrames = 8;
constexpr int ActivityTileSize = 64;
constexpr uint8_t ActivityDecayPerFrame = 1;
constexpr uint8_t ActivityBoostPerDamage = 6;
constexpr int ActivityStaticThreshold = 2;
constexpr int ActivityTransientThreshold = 8;
constexpr int StableFramesBeforeRefinement = 3;
constexpr auto RefinementCooldown = clk::milliseconds(600);
constexpr int MaxCongestionQpBias = 8;
constexpr uint16_t MaxRdpCoordinate = std::numeric_limits<uint16_t>::max();
constexpr int MinimumFrameRate = 5;
constexpr int MaxFramesBetweenFullDamage = 8;
constexpr double FullDamageCoverageThreshold = 0.15;

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

struct RectEncodingQuality {
    uint8_t qp = 22;
    uint8_t quality = 100;
};

RectEncodingQuality qualityForDamageRect(const RECTANGLE_16 &rect,
                                         const QSize &frameSize,
                                         bool isKeyFrame,
                                         bool isRefinementFrame,
                                         int activityScore,
                                         int congestionQpBias)
{
    if (isKeyFrame || frameSize.isEmpty()) {
        return {};
    }

    if (isRefinementFrame) {
        return {
            .qp = 16,
            .quality = 100,
        };
    }

    const auto frameArea = std::max(1, frameSize.width() * frameSize.height());
    const auto rectArea = std::max<int>(1, (rect.right - rect.left) * (rect.bottom - rect.top));
    const auto coverage = double(rectArea) / double(frameArea);

    // Bias for crisp quality on small UI updates and better compression on large
    // motion updates.
    int qp = 22;
    int quality = 90;
    if (coverage <= 0.03) {
        qp = 18;
        quality = 100;
    } else if (coverage <= 0.20) {
        qp = 21;
        quality = 92;
    }

    // Keep static/text-like areas crisp while compressing repeated motion
    // regions more aggressively.
    if (activityScore <= ActivityStaticThreshold && coverage <= 0.20) {
        qp -= 3;
        quality += 8;
    } else if (activityScore >= ActivityTransientThreshold) {
        qp += 3;
        quality -= 8;
        if (activityScore >= (ActivityTransientThreshold * 2)) {
            qp += 2;
            quality -= 6;
        }
    }

    // Under congestion, bias larger/motion updates toward lower bitrate while
    // keeping tiny UI regions readable.
    const auto effectiveCongestionBias = (coverage <= 0.03) ? (congestionQpBias / 2) : congestionQpBias;
    qp += effectiveCongestionBias;
    quality -= effectiveCongestionBias * 2;

    return {
        .qp = static_cast<uint8_t>(std::clamp(qp, 10, 40)),
        .quality = static_cast<uint8_t>(std::clamp(quality, 70, 100)),
    };
}

std::vector<RECTANGLE_16> toDamageRects(const VideoFrame &frame)
{
    std::vector<RECTANGLE_16> rects;

    if (frame.size.isEmpty()) {
        return rects;
    }

    const QRect frameBounds(QPoint(0, 0), frame.size);
    const auto fullRect = toRdpRect(frameBounds);

    if (frame.isKeyFrame || frame.damage.isEmpty()) {
        rects.push_back(fullRect);
        return rects;
    }

    const auto clippedDamage = frame.damage.intersected(frameBounds);
    QVector<QRect> damageRects;
    const auto sourceRects = clippedDamage.rects();
    damageRects.reserve(sourceRects.size());
    for (const auto &rect : sourceRects) {
        damageRects.append(rect);
    }
    if (damageRects.isEmpty() || damageRects.size() > MaxDamageRectCount) {
        rects.push_back(fullRect);
        return rects;
    }

    // Merge nearby/overlapping rectangles to reduce metadata overhead while
    // preserving partial update behavior.
    bool merged = true;
    while (merged && damageRects.size() > MaxCoalescedDamageRects) {
        merged = false;
        for (int i = 0; i < damageRects.size() - 1; ++i) {
            for (int j = i + 1; j < damageRects.size(); ++j) {
                const auto &a = damageRects.at(i);
                const auto &b = damageRects.at(j);
                const auto joined = a.united(b);
                if (joined.width() * joined.height() <= (a.width() * a.height() + b.width() * b.height()) * 3 / 2) {
                    damageRects[i] = joined;
                    damageRects.removeAt(j);
                    merged = true;
                    break;
                }
            }
            if (merged) {
                break;
            }
        }
    }
    if (damageRects.size() > MaxDamageRectCount) {
        rects.push_back(fullRect);
        return rects;
    }

    rects.reserve(damageRects.size());
    for (const auto &damageRect : damageRects) {
        const auto boundedRect = damageRect.intersected(frameBounds);
        if (boundedRect.isEmpty()) {
            continue;
        }
        rects.push_back(toRdpRect(boundedRect));
    }

    if (rects.empty()) {
        rects.push_back(fullRect);
    }

    return rects;
}

struct RdpCapsInformation {
    uint32_t version;
    RDPGFX_CAPSET capSet;
    bool avcSupported : 1 = false;
    bool yuv420Supported : 1 = false;
    bool avc444Supported : 1 = false;
    bool avc444v2Supported : 1 = false;
};

enum class StreamCodec {
    Avc420,
    Avc444,
    Avc444v2,
};

uint16_t toCodecId(StreamCodec codec)
{
    switch (codec) {
    case StreamCodec::Avc444:
        return RDPGFX_CODECID_AVC444;
    case StreamCodec::Avc444v2:
        return RDPGFX_CODECID_AVC444v2;
    case StreamCodec::Avc420:
    default:
        return RDPGFX_CODECID_AVC420;
    }
}

const char *codecToString(StreamCodec codec)
{
    switch (codec) {
    case StreamCodec::Avc444:
        return "AVC444";
    case StreamCodec::Avc444v2:
        return "AVC444v2";
    case StreamCodec::Avc420:
    default:
        return "AVC420";
    }
}

bool capSupportsCodec(const RdpCapsInformation &caps, StreamCodec codec)
{
    switch (codec) {
    case StreamCodec::Avc444v2:
        return caps.avcSupported && caps.avc444v2Supported;
    case StreamCodec::Avc444:
        return caps.avcSupported && caps.avc444Supported;
    case StreamCodec::Avc420:
    default:
        return caps.avcSupported && caps.yuv420Supported;
    }
}

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
    bool capsConfirmed = false;
    StreamCodec selectedCodec = StreamCodec::Avc420;

    std::jthread frameSubmissionThread;
    std::mutex frameQueueMutex;
    std::condition_variable frameQueueCondition;

    QQueue<VideoFrame> frameQueue;
    int droppedQueuedFrames = 0;
    clk::system_clock::time_point lastDropLogTime;
    QSet<uint32_t> pendingFrames;
    QSize activityFrameSize;
    int activityTileColumns = 0;
    int activityTileRows = 0;
    QVector<uint8_t> activityTiles;

    int maximumFrameRate = 120;
    int requestedFrameRate = 60;
    QQueue<FrameRateEstimate> frameRateEstimates;
    clk::system_clock::time_point lastFrameRateEstimation;

    std::atomic_int encodedFrames = 0;
    std::atomic_int frameDelay = 0;
    std::atomic_int decoderQueueDepth = 0;
    int framesSinceFullDamage = 0;
    bool refinementPending = false;
    int stableFramesSinceMotion = 0;
    clk::system_clock::time_point lastRefinementFrameTime;
    int congestionQpBias = 0;
    clk::milliseconds previousRtt = clk::milliseconds(0);

    void resetActivityGrid(const QSize &size)
    {
        if (size == activityFrameSize && !activityTiles.isEmpty()) {
            return;
        }

        activityFrameSize = size;
        activityTileColumns = std::max(1, (size.width() + ActivityTileSize - 1) / ActivityTileSize);
        activityTileRows = std::max(1, (size.height() + ActivityTileSize - 1) / ActivityTileSize);
        activityTiles.fill(0, activityTileColumns * activityTileRows);
    }

    void decayActivity()
    {
        for (auto &activity : activityTiles) {
            if (activity > ActivityDecayPerFrame) {
                activity -= ActivityDecayPerFrame;
            } else {
                activity = 0;
            }
        }
    }

    template<typename TileFunc>
    void forEachTileInRect(const RECTANGLE_16 &rect, TileFunc &&tileFunc) const
    {
        if (activityTiles.isEmpty()) {
            return;
        }

        const auto left = std::clamp<int>(rect.left / ActivityTileSize, 0, activityTileColumns - 1);
        const auto top = std::clamp<int>(rect.top / ActivityTileSize, 0, activityTileRows - 1);
        const auto right = std::clamp<int>(std::max<int>(int(rect.right) - 1, int(rect.left)) / ActivityTileSize, 0, activityTileColumns - 1);
        const auto bottom = std::clamp<int>(std::max<int>(int(rect.bottom) - 1, int(rect.top)) / ActivityTileSize, 0, activityTileRows - 1);

        for (auto y = top; y <= bottom; ++y) {
            for (auto x = left; x <= right; ++x) {
                tileFunc(y * activityTileColumns + x);
            }
        }
    }

    int activityForRect(const RECTANGLE_16 &rect) const
    {
        if (activityTiles.isEmpty()) {
            return 0;
        }

        int sum = 0;
        int count = 0;
        forEachTileInRect(rect, [this, &sum, &count](int index) {
            sum += activityTiles[index];
            count++;
        });

        return count > 0 ? (sum / count) : 0;
    }

    void markDamageActivity(const std::vector<RECTANGLE_16> &rects)
    {
        for (const auto &rect : rects) {
            forEachTileInRect(rect, [this](int index) {
                const auto boosted = int(activityTiles[index]) + int(ActivityBoostPerDamage);
                activityTiles[index] = static_cast<uint8_t>(std::min(boosted, 255));
            });
        }
    }
};

VideoStream::VideoStream(RdpConnection *session)
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
    d->session = session;
}

VideoStream::~VideoStream()
{
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
            VideoFrame nextFrame;
            {
                std::unique_lock lock(d->frameQueueMutex);
                auto frameInterval = std::chrono::milliseconds(1000 / std::max(d->requestedFrameRate, 1));
                d->frameQueueCondition.wait_for(lock, frameInterval, [this, token]() {
                    return token.stop_requested() || !d->frameQueue.isEmpty();
                });
                if (token.stop_requested()) {
                    break;
                }
                if (d->frameQueue.isEmpty()) {
                    continue;
                }
                nextFrame = d->frameQueue.takeLast();
                const auto staleFrames = d->frameQueue.size();
                if (staleFrames > 0) {
                    d->frameQueue.clear();
                    d->droppedQueuedFrames += staleFrames;
                }

                auto now = clk::system_clock::now();
                if (d->droppedQueuedFrames > 0
                    && (d->lastDropLogTime.time_since_epoch().count() == 0 || (now - d->lastDropLogTime) >= clk::seconds(2))) {
                    qCDebug(KRDP) << "Dropped stale queued frames:" << d->droppedQueuedFrames;
                    d->droppedQueuedFrames = 0;
                    d->lastDropLogTime = now;
                }
            }
            sendFrame(nextFrame);
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

    d->gfxContext->Close(d->gfxContext.get());

    if (d->frameSubmissionThread.joinable()) {
        d->frameSubmissionThread.request_stop();
        d->frameQueueCondition.notify_all();
        d->frameSubmissionThread.join();
    }

    Q_EMIT closed();
}

void VideoStream::queueFrame(const KRdp::VideoFrame &frame)
{
    if (d->session->state() != RdpConnection::State::Streaming || !d->enabled) {
        return;
    }

    {
        std::lock_guard lock(d->frameQueueMutex);
        while (d->frameQueue.size() >= MaxQueuedFrames) {
            d->frameQueue.removeFirst();
            d->droppedQueuedFrames++;
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
                caps.avc444Supported = true;
                // Per MS-RDPEGFX, AVC444v2 is implied from 10.1+.
                caps.avc444v2Supported = set.version >= RDPGFX_CAPVERSION_101;
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

        qCDebug(KRDP) << " " << capVersionToString(caps.version) << "AVC:" << caps.avcSupported << "YUV420:" << caps.yuv420Supported << "AVC444:"
                      << caps.avc444Supported << "AVC444v2:" << caps.avc444v2Supported;

        capsInformation.push_back(caps);
    }

    const auto settings = d->session->rdpPeerContext()->settings;
    const bool wantsAvc444 = freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444);
    const bool wantsAvc444v2 = freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444v2);

    auto preferredCodec = StreamCodec::Avc420;
    if (wantsAvc444v2) {
        preferredCodec = StreamCodec::Avc444v2;
    } else if (wantsAvc444) {
        preferredCodec = StreamCodec::Avc444;
    }

    if ((preferredCodec != StreamCodec::Avc420) && !LocalAvc444EncodingAvailable) {
        qCDebug(KRDP) << "Client supports" << codecToString(preferredCodec) << "but local encoder path is AVC420-only, falling back";
        preferredCodec = StreamCodec::Avc420;
    }

    auto findBestCapsForCodec = [&](StreamCodec codec) {
        const auto itr = std::max_element(capsInformation.begin(),
                                          capsInformation.end(),
                                          [codec](const auto &first, const auto &second) {
                                              const auto firstSupported = capSupportsCodec(first, codec);
                                              const auto secondSupported = capSupportsCodec(second, codec);
                                              if (firstSupported != secondSupported) {
                                                  return !firstSupported;
                                              }
                                              return first.version < second.version;
                                          });
        if (itr == capsInformation.end() || !capSupportsCodec(*itr, codec)) {
            return capsInformation.end();
        }
        return itr;
    };

    auto selectedCaps = findBestCapsForCodec(preferredCodec);
    if (selectedCaps == capsInformation.end() && preferredCodec != StreamCodec::Avc420) {
        qCDebug(KRDP) << "Client did not advertise usable" << codecToString(preferredCodec) << "caps, falling back to AVC420";
        preferredCodec = StreamCodec::Avc420;
        selectedCaps = findBestCapsForCodec(preferredCodec);
    }

    if (selectedCaps == capsInformation.end()) {
        qCWarning(KRDP) << "Client does not support H.264 in YUV420 mode!";
        d->session->close(RdpConnection::CloseReason::VideoInitFailed);
        return CHANNEL_RC_INITIALIZATION_ERROR;
    }

    d->selectedCodec = preferredCodec;
    qCDebug(KRDP) << "Selected caps:" << capVersionToString(selectedCaps->version) << "codec:" << codecToString(d->selectedCodec);

    RDPGFX_CAPS_CONFIRM_PDU capsConfirmPdu;
    capsConfirmPdu.capsSet = &(selectedCaps->capSet);
    d->gfxContext->CapsConfirm(d->gfxContext.get(), &capsConfirmPdu);

    d->capsConfirmed = true;

    return CHANNEL_RC_OK;
}

uint32_t VideoStream::onFrameAcknowledge(const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge)
{
    auto id = frameAcknowledge->frameId;

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

void VideoStream::performReset(QSize size)
{
    RDPGFX_RESET_GRAPHICS_PDU resetGraphicsPdu;
    resetGraphicsPdu.width = size.width();
    resetGraphicsPdu.height = size.height();
    resetGraphicsPdu.monitorCount = 1;

    auto monitors = new MONITOR_DEF[1];
    monitors[0].left = 0;
    monitors[0].right = size.width();
    monitors[0].top = 0;
    monitors[0].bottom = size.height();
    monitors[0].flags = MONITOR_PRIMARY;
    resetGraphicsPdu.monitorDefArray = monitors;
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

void VideoStream::sendFrame(const VideoFrame &frame)
{
    if (!d->gfxContext || !d->capsConfirmed) {
        return;
    }

    if (frame.data.size() == 0) {
        return;
    }

    if (d->pendingReset) {
        d->pendingReset = false;
        performReset(frame.size);
    }

    d->session->networkDetection()->startBandwidthMeasure();

    auto frameId = d->frameId++;

    d->encodedFrames++;

    d->pendingFrames.insert(frameId);

    RDPGFX_START_FRAME_PDU startFramePdu;
    RDPGFX_END_FRAME_PDU endFramePdu;

    auto now = QDateTime::currentDateTimeUtc().time();
    startFramePdu.timestamp = now.hour() << 22 | now.minute() << 16 | now.second() << 10 | now.msec();

    startFramePdu.frameId = frameId;
    endFramePdu.frameId = frameId;

    RDPGFX_SURFACE_COMMAND surfaceCommand;
    surfaceCommand.surfaceId = d->surface.id;
    surfaceCommand.codecId = toCodecId(d->selectedCodec);
    surfaceCommand.format = PIXEL_FORMAT_BGRX32;
    surfaceCommand.length = 0;
    surfaceCommand.data = nullptr;

    RDPGFX_AVC420_BITMAP_STREAM avcStream;
    surfaceCommand.extra = &avcStream;

    avcStream.data = (BYTE *)frame.data.data();
    avcStream.length = frame.data.length();

    auto damageRects = toDamageRects(frame);
    const auto trackedDamageRects = damageRects;
    if (damageRects.empty()) {
        return;
    }

    const auto fullRect = toRdpRect(QRect(QPoint(0, 0), frame.size));
    const auto frameArea = std::max(1, frame.size.width() * frame.size.height());
    int damageArea = 0;
    for (const auto &rect : damageRects) {
        damageArea += std::max<int>(1, (rect.right - rect.left) * (rect.bottom - rect.top));
    }
    const auto damageCoverage = double(damageArea) / double(frameArea);
    const auto delayedFrames = std::max(d->frameDelay.load(), 0);
    const bool highMotionUpdate = (damageCoverage >= FullDamageCoverageThreshold) || (damageRects.size() > 8);

    if (highMotionUpdate || delayedFrames >= 1) {
        d->refinementPending = true;
        d->stableFramesSinceMotion = 0;
    } else if (d->refinementPending && damageCoverage <= 0.03 && delayedFrames == 0) {
        d->stableFramesSinceMotion++;
    } else {
        d->stableFramesSinceMotion = 0;
    }

    const auto cooldownElapsed =
        (d->lastRefinementFrameTime.time_since_epoch().count() == 0) || ((clk::system_clock::now() - d->lastRefinementFrameTime) >= RefinementCooldown);
    const bool shouldSendRefinement = d->refinementPending && (d->stableFramesSinceMotion >= StableFramesBeforeRefinement) && (delayedFrames == 0)
        && !frame.isKeyFrame && cooldownElapsed;

    bool useFullDamage = frame.isKeyFrame
        || shouldSendRefinement
        || (damageCoverage >= FullDamageCoverageThreshold)
        || (delayedFrames >= 1)
        || (damageRects.size() > 8)
        || (d->framesSinceFullDamage >= MaxFramesBetweenFullDamage);
    const bool isRefinementFrame = shouldSendRefinement;

    if (useFullDamage) {
        damageRects.clear();
        damageRects.push_back(fullRect);
        d->framesSinceFullDamage = 0;
    } else {
        d->framesSinceFullDamage++;
    }

    avcStream.meta.numRegionRects = static_cast<decltype(avcStream.meta.numRegionRects)>(damageRects.size());
    auto rects = std::make_unique<RECTANGLE_16[]>(damageRects.size());
    std::copy(damageRects.begin(), damageRects.end(), rects.get());
    avcStream.meta.regionRects = rects.get();

    auto damageBounds = damageRects.front();
    for (const auto &rect : damageRects) {
        damageBounds.left = std::min(damageBounds.left, rect.left);
        damageBounds.top = std::min(damageBounds.top, rect.top);
        damageBounds.right = std::max(damageBounds.right, rect.right);
        damageBounds.bottom = std::max(damageBounds.bottom, rect.bottom);
    }
    surfaceCommand.left = damageBounds.left;
    surfaceCommand.top = damageBounds.top;
    surfaceCommand.right = damageBounds.right;
    surfaceCommand.bottom = damageBounds.bottom;

    auto qualities = std::make_unique<RDPGFX_H264_QUANT_QUALITY[]>(damageRects.size());
    avcStream.meta.quantQualityVals = qualities.get();
    d->resetActivityGrid(frame.size);
    d->decayActivity();
    std::vector<int> rectActivityScores;
    rectActivityScores.reserve(damageRects.size());
    for (const auto &rect : damageRects) {
        rectActivityScores.push_back(d->activityForRect(rect));
    }
    for (size_t i = 0; i < damageRects.size(); ++i) {
        const auto quality =
            qualityForDamageRect(damageRects[i], frame.size, frame.isKeyFrame, isRefinementFrame, rectActivityScores[i], d->congestionQpBias);
        qualities[i].qp = quality.qp;
        qualities[i].p = 0;
        qualities[i].qualityVal = quality.quality;
    }
    d->markDamageActivity(trackedDamageRects);

    if (isRefinementFrame) {
        d->refinementPending = false;
        d->stableFramesSinceMotion = 0;
        d->lastRefinementFrameTime = clk::system_clock::now();
        qCDebug(KRDP) << "Sent progressive refinement frame";
    }

    d->gfxContext->StartFrame(d->gfxContext.get(), &startFramePdu);
    d->gfxContext->SurfaceCommand(d->gfxContext.get(), &surfaceCommand);

    d->gfxContext->EndFrame(d->gfxContext.get(), &endFramePdu);

    d->session->networkDetection()->stopBandwidthMeasure();
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

    int targetQpBias = 0;
    if (delayedFrames >= 6 || decoderQueueDepth >= 8 || rttRiseMs >= 12) {
        targetQpBias = 8;
    } else if (delayedFrames >= 3 || decoderQueueDepth >= 5 || rttRiseMs >= 8) {
        targetQpBias = 5;
    } else if (delayedFrames >= 1 || decoderQueueDepth >= 2 || rttRiseMs >= 4) {
        targetQpBias = 2;
    }
    targetQpBias = std::clamp(targetQpBias, 0, MaxCongestionQpBias);
    if (targetQpBias > d->congestionQpBias) {
        d->congestionQpBias = targetQpBias;
    } else if (targetQpBias < d->congestionQpBias) {
        d->congestionQpBias = std::max(targetQpBias, d->congestionQpBias - 1);
    }
}
}

#include "moc_VideoStream.cpp"
