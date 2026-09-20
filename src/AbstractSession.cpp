// SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleix.pol_gonzalez@mercedes-benz.com>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "AbstractSession.h"

#include <algorithm>

#include <PipeWireEncodedStream>
#include <PipeWireSourceStream>
#include <QSet>

#include "krdp_logging.h"

namespace KRdp
{

namespace
{
template<typename Stream>
void applyChromaModeIfSupported(Stream *stream, VideoCodec codec)
{
    if constexpr (requires(Stream *s) { s->setChromaMode(typename Stream::ChromaMode{}); }) {
        using Mode = typename Stream::ChromaMode;
        stream->setChromaMode(codec == VideoCodec::Avc444v2 ? Mode::Yuv444v2 : codec == VideoCodec::Avc444 ? Mode::Yuv444v1 : Mode::Yuv420);
    } else if (codec != VideoCodec::Avc420) {
        qCWarning(KRDP) << "This KPipeWire cannot encode 4:4:4; the connection negotiated" << VideoCodecSupport::codecName(codec) << "and will receive luma-only frames";
    }
}
template<typename Stream>
bool chromaModeMatches(Stream *stream, VideoCodec codec)
{
    if constexpr (requires(Stream *s) { s->chromaMode(); }) {
        using Mode = typename Stream::ChromaMode;
        const Mode wanted = codec == VideoCodec::Avc444v2 ? Mode::Yuv444v2 : codec == VideoCodec::Avc444 ? Mode::Yuv444v1 : Mode::Yuv420;
        return stream->chromaMode() == wanted;
    } else {
        return true;
    }
}
template<typename Stream>
void applyAuxEnabledIfSupported(Stream *stream, bool enabled)
{
    if constexpr (requires(Stream *s) { s->setAuxStreamEnabled(true); }) {
        stream->setAuxStreamEnabled(enabled);
    }
}
template<typename Stream>
void applyChromaPolicyIfSupported(Stream *stream, const ChromaPolicy &policy)
{
    if constexpr (requires(Stream *s) { s->setChromaPolicy(typename Stream::ChromaPolicy{}); }) {
        using Policy = typename Stream::ChromaPolicy;
        stream->setChromaPolicy(Policy{policy.motionGapMs, policy.restMs, policy.maxGapMs});
    }
}
template<typename Stream>
const char *activeChromaName(Stream *stream)
{
    if constexpr (requires(Stream *s) { s->activeChromaMode(); }) {
        using Mode = typename Stream::ChromaMode;
        switch (stream->activeChromaMode()) {
        case Mode::Yuv444v2:
            return "444v2";
        case Mode::Yuv444v1:
            return "444v1";
        default:
            return "420";
        }
    } else {
        return "420 (KPipeWire without AVC444)";
    }
}
template<typename Stream>
bool chromaActive(Stream *stream)
{
    if constexpr (requires(Stream *s) { s->activeChromaMode(); }) {
        return stream->activeChromaMode() != Stream::ChromaMode::Yuv420;
    } else {
        return false;
    }
}
template<typename Stream, typename Session>
void connectActiveChromaModeIfSupported(Stream *stream, Session *session)
{
    if constexpr (requires(Stream *s) { s->activeChromaMode(); }) {
        // Once at start and on the encoder's mid-session fallback to 4:2:0 (SPS mismatch): the
        // "Encoder running" line from onStreamActiveChanged() would otherwise stay wrong all session.
        QObject::connect(stream, &Stream::activeChromaModeChanged, session, [session, stream](typename Stream::ChromaMode mode) {
            qCInfo(KRDP) << "Encoder chroma mode:" << activeChromaName(stream)
                         << (mode == Stream::ChromaMode::Yuv420 && session->videoCodec() != VideoCodec::Avc420 ? "(fell back to 4:2:0)" : "");
            Q_EMIT session->chromaCapabilityChanged(mode != Stream::ChromaMode::Yuv420);
        });
    }
}
template<typename Stream, typename Session>
void connectChromaTimingIfSupported(Stream *stream, Session *session)
{
    if constexpr (requires { typename Stream::ChromaTiming; }) {
        QObject::connect(stream, &Stream::chromaTimingReported, session, [session](const typename Stream::ChromaTiming &t) {
            ChromaTimingReport r;
            r.frames = t.frames;
            r.auxSent = t.auxSent;
            r.auxSkippedMotion = t.auxSkippedMotion;
            r.auxRestRefresh = t.auxRestRefresh;
            r.rewriteFailures = t.rewriteFailures;
            r.splitVariant = QString::fromLatin1(t.splitVariant);
            r.downloadAvg = t.downloadAvg;
            r.downloadMax = t.downloadMax;
            r.splitAvg = t.splitAvg;
            r.splitMax = t.splitMax;
            r.uploadAvg = t.uploadAvg;
            r.uploadMax = t.uploadMax;
            r.encodeMainAvg = t.encodeMainAvg;
            r.encodeMainMax = t.encodeMainMax;
            r.encodeAuxAvg = t.encodeAuxAvg;
            r.encodeAuxMax = t.encodeAuxMax;
            Q_EMIT session->chromaTimingReported(r);
        });
    }
}
}

class KRDP_NO_EXPORT AbstractSession::Private
{
public:
    std::unique_ptr<PipeWireEncodedStream> encodedStream;

    std::optional<int> activeStream;
    std::optional<VirtualMonitor> virtualMonitor;
    bool started = false;
    bool enabled = false;
    QSize size;
    QSize logicalSize;
    std::optional<quint32> frameRate = 60;
    std::optional<quint8> quality;
    QSet<QObject *> enableRequests;
    int monitorIndex = 0;
    VideoCodec codec = VideoCodec::Avc420;
    bool chromaEnabled = true;
    ChromaPolicy chromaPolicy;
};

AbstractSession::AbstractSession()
    : QObject()
    , d(std::make_unique<Private>())
{
}

AbstractSession::~AbstractSession()
{
    if (d->encodedStream) {
        d->encodedStream->stop();
    }
}

QSize AbstractSession::logicalSize() const
{
    return d->logicalSize;
}

bool AbstractSession::streamingRequested() const
{
    return d->enabled;
}

int AbstractSession::activeStream() const
{
    return d->activeStream.value_or(-1);
}

std::optional<VirtualMonitor> AbstractSession::virtualMonitor() const
{
    return d->virtualMonitor;
}

void AbstractSession::setActiveStream(int stream)
{
    Q_ASSERT(!d->virtualMonitor);
    d->activeStream = stream;
}

void AbstractSession::setVirtualMonitor(const VirtualMonitor &virtualMonitor)
{
    Q_ASSERT(!d->activeStream.has_value());
    d->virtualMonitor = virtualMonitor;
}

void AbstractSession::setVideoQuality(quint8 quality)
{
    d->quality = quality;
    if (d->encodedStream) {
        d->encodedStream->setQuality(quality);
    }
}

void AbstractSession::setVideoCodec(VideoCodec codec)
{
    const bool changed = d->codec != codec;
    d->codec = codec;
    if (!d->encodedStream) {
        return;
    }
    if (!changed && chromaModeMatches(d->encodedStream.get(), codec)) {
        return;
    }
    applyChromaModeIfSupported(d->encodedStream.get(), codec);
    if (d->encodedStream->isActive()) {
        qCInfo(KRDP) << "Codec changed to" << VideoCodecSupport::codecName(codec) << "on a running stream; restarting the encoder";
        restartStreamForCodecChange();
    }
}

VideoCodec AbstractSession::videoCodec() const
{
    return d->codec;
}

void AbstractSession::setChromaEnabled(bool enabled)
{
    d->chromaEnabled = enabled;
    if (d->encodedStream) {
        applyAuxEnabledIfSupported(d->encodedStream.get(), enabled);
    }
}

void AbstractSession::setChromaPolicy(const ChromaPolicy &policy)
{
    d->chromaPolicy = policy;
    if (d->encodedStream) {
        applyChromaPolicyIfSupported(d->encodedStream.get(), policy);
    }
}

void AbstractSession::restartStreamForCodecChange()
{
    qCDebug(KRDP) << "This session type cannot restart its stream; the codec change applies to its next start";
}

void AbstractSession::refreshDisplayConfiguration()
{
}

void AbstractSession::sendGlobalEvent(const std::shared_ptr<QEvent> &event)
{
    sendEvent(event);
}

void AbstractSession::setMonitorIndex(int index)
{
    d->monitorIndex = index;
}

int AbstractSession::monitorIndex() const
{
    return d->monitorIndex;
}

QRect AbstractSession::outputGeometry() const
{
    return QRect(QPoint(0, 0), logicalSize());
}

QPointF AbstractSession::mapToGlobal(const QPointF &local) const
{
    const QPoint origin = outputGeometry().topLeft();
    const QSize pixels = pixelSize();
    const QSize logical = logicalSize();
    if (pixels.isEmpty() || logical.isEmpty()) {
        return local + origin;
    }
    // The last pixel of the capture is the last logical unit of the output,
    // which is why both spans are one less than the size.
    const auto inputWidth = std::max(1, pixels.width() - 1);
    const auto inputHeight = std::max(1, pixels.height() - 1);
    const auto logicalWidth = std::max(1, logical.width() - 1);
    const auto logicalHeight = std::max(1, logical.height() - 1);
    const auto normalizedX = std::clamp(local.x() / double(inputWidth), 0.0, 1.0);
    const auto normalizedY = std::clamp(local.y() / double(inputHeight), 0.0, 1.0);
    return QPointF{normalizedX * logicalWidth + origin.x(), normalizedY * logicalHeight + origin.y()};
}

void AbstractSession::requestKeyFrame()
{
    qCDebug(KRDP) << "Keyframe requested but this session type cannot obtain one; waiting for the next organic keyframe";
}

bool AbstractSession::streamActive() const
{
    if (d->encodedStream) {
        return d->encodedStream->isActive();
    }
    return false;
}

// The pre-existing spelling of the same question, kept so nothing outside has
// to change; there is one implementation, above.
bool AbstractSession::streamingEnabled() const
{
    return streamActive();
}

void AbstractSession::setStreamingEnabled(bool enable)
{
    d->enabled = enable;

    if (enable && !d->started) {
        start();
        return;
    }

    if (d->encodedStream) {
        if (enable && d->started) {
            d->encodedStream->start();
        } else {
            d->encodedStream->stop();
        }
    }
}

void AbstractSession::setVideoFrameRate(quint32 framerate)
{
    d->frameRate = framerate;
    if (d->encodedStream) {
        d->encodedStream->setMaxFramerate({framerate, 1});
        // this buffers 1 second of frames and drops after that
        d->encodedStream->setMaxPendingFrames(framerate);
    }
}

void AbstractSession::setSize(QSize size)
{
    d->size = size;
}

void AbstractSession::setLogicalSize(QSize size)
{
    d->logicalSize = size;
}

QSize AbstractSession::pixelSize() const
{
    return d->size;
}

// The protected spelling subclasses already use; one implementation, above.
QSize AbstractSession::size() const
{
    return pixelSize();
}

PipeWireEncodedStream *AbstractSession::stream()
{
    if (!d->encodedStream) {
        d->encodedStream = std::make_unique<PipeWireEncodedStream>();
        connect(d->encodedStream.get(), &PipeWireEncodedStream::activeChanged, this, &AbstractSession::onStreamActiveChanged);
        if (d->frameRate) {
            d->encodedStream->setMaxFramerate({d->frameRate.value(), 1});
        }
        if (d->quality) {
            d->encodedStream->setQuality(d->quality.value());
        }
        applyChromaModeIfSupported(d->encodedStream.get(), d->codec);
        applyAuxEnabledIfSupported(d->encodedStream.get(), d->chromaEnabled);
        applyChromaPolicyIfSupported(d->encodedStream.get(), d->chromaPolicy);
        connectChromaTimingIfSupported(d->encodedStream.get(), this);
        connectActiveChromaModeIfSupported(d->encodedStream.get(), this);
    }
    return d->encodedStream.get();
}

void AbstractSession::onStreamActiveChanged(bool active)
{
    if (active) {
        qCInfo(KRDP) << "Encoder running: codec" << VideoCodecSupport::codecName(d->codec) << "chroma" << activeChromaName(d->encodedStream.get());
        Q_EMIT chromaCapabilityChanged(chromaActive(d->encodedStream.get()));
    }
    Q_EMIT streamActiveChanged(active);
}

void AbstractSession::setStarted(bool s)
{
    d->started = s;
    if (s) {
        if (d->enabled) {
            d->encodedStream->start();
        }
        Q_EMIT started();
    }
}

void AbstractSession::requestStreamingEnable(QObject *requester)
{
    d->enableRequests.insert(requester);
    connect(requester, &QObject::destroyed, this, &AbstractSession::requestStreamingDisable);
    setStreamingEnabled(true);
}

void AbstractSession::requestStreamingDisable(QObject *requester)
{
    if (!d->enableRequests.contains(requester)) {
        return;
    }
    disconnect(requester, &QObject::destroyed, this, &AbstractSession::requestStreamingDisable);
    d->enableRequests.remove(requester);
    if (d->enableRequests.size() == 0) {
        setStreamingEnabled(false);
    }
}

}
