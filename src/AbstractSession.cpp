// SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleix.pol_gonzalez@mercedes-benz.com>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "AbstractSession.h"
#include <PipeWireEncodedStream>
#include <PipeWireSourceStream>
#include <QSet>

#include "krdp_logging.h"

namespace KRdp
{

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
    bool softwareFallbackAttempted = false;
    bool softwareFallbackRetryPending = false;
    bool softwareFallbackRetryInProgress = false;
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

bool AbstractSession::streamingEnabled() const
{
    if (d->encodedStream) {
        return d->encodedStream->isActive();
    }
    return false;
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

QSize AbstractSession::size() const
{
    return d->size;
}

PipeWireEncodedStream *AbstractSession::stream()
{
    if (!d->encodedStream) {
        d->encodedStream = std::make_unique<PipeWireEncodedStream>();
        connect(d->encodedStream.get(), &PipeWireBaseEncodedStream::errorFound, this, &AbstractSession::handleStreamError);
        connect(d->encodedStream.get(), &PipeWireBaseEncodedStream::stateChanged, this, &AbstractSession::handleStreamStateChanged);
        connect(d->encodedStream.get(), &PipeWireBaseEncodedStream::activeChanged, this, &AbstractSession::handleStreamActiveChanged);
        if (d->frameRate) {
            d->encodedStream->setMaxFramerate({d->frameRate.value(), 1});
        }
        if (d->quality) {
            d->encodedStream->setQuality(d->quality.value());
        }
    }
    return d->encodedStream.get();
}

void AbstractSession::handleStreamError(const QString &errorMessage)
{
    const auto forcedEncoder = qgetenv("KPIPEWIRE_FORCE_ENCODER").trimmed().toLower();
    const bool alreadyForcedSoftware = (forcedEncoder == "libx264");
    if (d->softwareFallbackAttempted || alreadyForcedSoftware) {
        qCWarning(KRDP) << "PipeWire encoder failed and no additional fallback is available:" << errorMessage;
        Q_EMIT error();
        return;
    }

    d->softwareFallbackAttempted = true;
    d->softwareFallbackRetryPending = true;
    qputenv("KPIPEWIRE_FORCE_ENCODER", "libx264");
    qCWarning(KRDP) << "PipeWire encoder initialization failed; forcing software fallback to libx264:" << errorMessage;

    if (d->encodedStream->state() == PipeWireBaseEncodedStream::Idle) {
        handleStreamStateChanged();
        return;
    }

    d->encodedStream->stop();
}

void AbstractSession::handleStreamStateChanged()
{
    if (!d->encodedStream || !d->softwareFallbackRetryPending) {
        return;
    }
    if (d->encodedStream->state() != PipeWireBaseEncodedStream::Idle) {
        return;
    }
    if (!d->enabled) {
        return;
    }

    d->softwareFallbackRetryPending = false;
    d->softwareFallbackRetryInProgress = true;
    qCInfo(KRDP) << "Retrying PipeWire stream with forced software encoder libx264";
    d->encodedStream->start();
}

void AbstractSession::handleStreamActiveChanged(bool active)
{
    if (!active || !d->softwareFallbackRetryInProgress) {
        return;
    }

    d->softwareFallbackRetryInProgress = false;
    qCInfo(KRDP) << "Software encoder fallback active for this session";
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

#include "AbstractSession.moc"
