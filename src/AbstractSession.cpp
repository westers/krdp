// SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleix.pol_gonzalez@mercedes-benz.com>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "AbstractSession.h"
#include <PipeWireEncodedStream>
#include <PipeWireSourceStream>
#include <QSet>
#include <QTimer>

#include "krdp_logging.h"

namespace KRdp
{

class KRDP_NO_EXPORT AbstractSession::Private
{
public:
    static constexpr int FirstPacketTimeoutMs = 1500;
    static constexpr int PacketStallTimeoutMs = 3000;
    static constexpr int HardwareRetryDelayMs = 8000;
    static constexpr int MaxHardwareRetryAttempts = 3;

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
    bool softwareFallbackRetryPending = false;
    bool softwareFallbackRetryInProgress = false;
    bool softwareFallbackActive = false;
    bool hardwareRetryPending = false;
    bool hardwareRetryInProgress = false;
    bool hardwareRetryScheduled = false;
    int hardwareRetryAttempts = 0;
    quint64 hardwareRetryScheduleGeneration = 0;
    bool receivedPacketSinceActivation = false;
    quint64 streamActivationGeneration = 0;
    quint64 packetSequence = 0;
    bool temporarySoftwareEncoderOverride = false;
    bool hadPreviousForcedEncoder = false;
    QByteArray previousForcedEncoder;
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
    restoreForcedEncoderOverride();
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

void AbstractSession::refreshDisplayConfiguration()
{
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
            d->softwareFallbackRetryPending = false;
            d->softwareFallbackRetryInProgress = false;
            d->softwareFallbackActive = false;
            d->hardwareRetryPending = false;
            d->hardwareRetryInProgress = false;
            d->hardwareRetryScheduled = false;
            d->hardwareRetryAttempts = 0;
            ++d->hardwareRetryScheduleGeneration;
            d->encodedStream->stop();
            restoreForcedEncoderOverride();
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
        connect(d->encodedStream.get(), &PipeWireEncodedStream::newPacket, this, [this](const PipeWireEncodedStream::Packet &) {
            handleEncodedPacket();
        });
        if (d->frameRate) {
            d->encodedStream->setMaxFramerate({d->frameRate.value(), 1});
        }
        if (d->quality) {
            d->encodedStream->setQuality(d->quality.value());
        }
    }
    return d->encodedStream.get();
}

bool AbstractSession::requestSoftwareFallback(const QString &reason, const QString &context)
{
    const auto forcedEncoder = qgetenv("KPIPEWIRE_FORCE_ENCODER").trimmed().toLower();
    const bool alreadyForcedSoftware = (forcedEncoder == "libx264");
    if (d->softwareFallbackActive) {
        qCWarning(KRDP) << context << reason << "(software fallback already active)";
        scheduleHardwareEncoderRetry();
        return true;
    }

    if (alreadyForcedSoftware && !d->temporarySoftwareEncoderOverride) {
        qCWarning(KRDP) << context << reason << "(software encoder forced externally)";
        return true;
    }

    d->hardwareRetryPending = false;
    d->hardwareRetryInProgress = false;
    d->hardwareRetryScheduled = false;
    ++d->hardwareRetryScheduleGeneration;
    d->softwareFallbackRetryPending = true;
    if (!d->temporarySoftwareEncoderOverride) {
        d->hadPreviousForcedEncoder = qEnvironmentVariableIsSet("KPIPEWIRE_FORCE_ENCODER");
        d->previousForcedEncoder = qgetenv("KPIPEWIRE_FORCE_ENCODER");
        d->temporarySoftwareEncoderOverride = true;
    }
    qputenv("KPIPEWIRE_FORCE_ENCODER", "libx264");
    qCWarning(KRDP) << context << reason;

    if (d->encodedStream && d->encodedStream->state() == PipeWireBaseEncodedStream::Idle) {
        handleStreamStateChanged();
    } else if (d->encodedStream) {
        d->encodedStream->stop();
    }
    return true;
}

void AbstractSession::restoreForcedEncoderOverride()
{
    if (!d->temporarySoftwareEncoderOverride) {
        return;
    }

    if (d->hadPreviousForcedEncoder) {
        qputenv("KPIPEWIRE_FORCE_ENCODER", d->previousForcedEncoder);
    } else {
        qunsetenv("KPIPEWIRE_FORCE_ENCODER");
    }

    d->temporarySoftwareEncoderOverride = false;
    d->hadPreviousForcedEncoder = false;
    d->previousForcedEncoder.clear();
}

void AbstractSession::handleStreamError(const QString &errorMessage)
{
    if (!requestSoftwareFallback(errorMessage, QStringLiteral("PipeWire encoder initialization failed; forcing software fallback to libx264:"))) {
        qCWarning(KRDP) << "PipeWire encoder failed and no additional fallback is available:" << errorMessage;
        restoreForcedEncoderOverride();
        Q_EMIT error();
    }
}

void AbstractSession::handleStreamStateChanged()
{
    if (!d->encodedStream) {
        return;
    }
    if (d->encodedStream->state() != PipeWireBaseEncodedStream::Idle || !d->enabled) {
        return;
    }

    if (d->softwareFallbackRetryPending) {
        d->softwareFallbackRetryPending = false;
        d->softwareFallbackRetryInProgress = true;
        qCInfo(KRDP) << "Retrying PipeWire stream with forced software encoder libx264";
        d->encodedStream->start();
        return;
    }

    if (d->hardwareRetryPending) {
        d->hardwareRetryPending = false;
        d->hardwareRetryInProgress = true;
        qCInfo(KRDP) << "Retrying PipeWire stream with hardware encoder";
        d->encodedStream->start();
    }
}

void AbstractSession::handleStreamActiveChanged(bool active)
{
    if (!active) {
        d->receivedPacketSinceActivation = false;
        return;
    }

    d->receivedPacketSinceActivation = false;
    const auto generation = ++d->streamActivationGeneration;
    schedulePacketStallWatchdog();
    QTimer::singleShot(Private::FirstPacketTimeoutMs, this, [this, generation]() {
        if (!d->encodedStream || !d->enabled) {
            return;
        }
        if (generation != d->streamActivationGeneration) {
            return;
        }
        if (!d->encodedStream->isActive() || d->receivedPacketSinceActivation) {
            return;
        }

        if (!requestSoftwareFallback(QStringLiteral("No encoded packets received from PipeWire within %1 ms").arg(Private::FirstPacketTimeoutMs),
                                     QStringLiteral("PipeWire stream stalled before first packet; forcing software fallback to libx264:"))) {
            qCWarning(KRDP) << "PipeWire stream stalled and no additional fallback is available";
            restoreForcedEncoderOverride();
            Q_EMIT error();
        }
    });

    if (d->softwareFallbackRetryInProgress) {
        d->softwareFallbackRetryInProgress = false;
        d->softwareFallbackActive = true;
        qCInfo(KRDP) << "Software encoder fallback active for this session";
        restoreForcedEncoderOverride();
        scheduleHardwareEncoderRetry();
        return;
    }

    if (d->hardwareRetryInProgress) {
        d->hardwareRetryInProgress = false;
        d->softwareFallbackActive = false;
        d->hardwareRetryAttempts = 0;
        qCInfo(KRDP) << "Hardware encoder recovered; leaving software fallback";
        return;
    }

    if (d->softwareFallbackActive) {
        scheduleHardwareEncoderRetry();
    }
}

void AbstractSession::handleEncodedPacket()
{
    d->receivedPacketSinceActivation = true;
    ++d->packetSequence;
    schedulePacketStallWatchdog();
}

void AbstractSession::schedulePacketStallWatchdog()
{
    if (!d->encodedStream || !d->enabled || !d->encodedStream->isActive()) {
        return;
    }

    const auto activationGeneration = d->streamActivationGeneration;
    const auto packetSequence = d->packetSequence;
    QTimer::singleShot(Private::PacketStallTimeoutMs, this, [this, activationGeneration, packetSequence]() {
        if (!d->encodedStream || !d->enabled) {
            return;
        }
        if (activationGeneration != d->streamActivationGeneration) {
            return;
        }
        if (!d->encodedStream->isActive()) {
            return;
        }
        if (packetSequence != d->packetSequence) {
            return;
        }

        if (!requestSoftwareFallback(QStringLiteral("No encoded packets received for %1 ms").arg(Private::PacketStallTimeoutMs),
                                     QStringLiteral("PipeWire stream stalled during active session; forcing software fallback to libx264:"))) {
            qCWarning(KRDP) << "PipeWire stream stalled during active session and no additional fallback is available";
            restoreForcedEncoderOverride();
            Q_EMIT error();
        }
    });
}

void AbstractSession::scheduleHardwareEncoderRetry()
{
    if (!d->softwareFallbackActive || d->hardwareRetryScheduled || d->hardwareRetryInProgress || !d->enabled || !d->encodedStream) {
        return;
    }
    if (d->hardwareRetryAttempts >= Private::MaxHardwareRetryAttempts) {
        return;
    }

    d->hardwareRetryScheduled = true;
    const auto retryAttempt = d->hardwareRetryAttempts + 1;
    const auto generation = ++d->hardwareRetryScheduleGeneration;
    qCInfo(KRDP) << "Scheduling hardware encoder retry in" << Private::HardwareRetryDelayMs << "ms (attempt" << retryAttempt << "of"
                 << Private::MaxHardwareRetryAttempts << ')';

    QTimer::singleShot(Private::HardwareRetryDelayMs, this, [this, generation]() {
        if (!d->encodedStream || !d->enabled) {
            return;
        }
        if (generation != d->hardwareRetryScheduleGeneration) {
            return;
        }
        if (!d->softwareFallbackActive || d->hardwareRetryInProgress || d->hardwareRetryAttempts >= Private::MaxHardwareRetryAttempts) {
            return;
        }

        d->hardwareRetryScheduled = false;
        ++d->hardwareRetryAttempts;
        d->hardwareRetryPending = true;
        qCInfo(KRDP) << "Attempting hardware encoder recovery (attempt" << d->hardwareRetryAttempts << "of" << Private::MaxHardwareRetryAttempts << ')';
        restoreForcedEncoderOverride();

        if (d->encodedStream->state() == PipeWireBaseEncodedStream::Idle) {
            handleStreamStateChanged();
        } else {
            d->encodedStream->stop();
        }
    });
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
