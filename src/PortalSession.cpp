// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PortalSession.h"

#include <QGuiApplication>
#include <QMimeData>
#include <QMouseEvent>
#include <QQueue>
#include <QRect>

#include <linux/input.h>
#include <chrono>
#include <optional>
#include <utility>

#include <KConfigGroup>
#include <KSharedConfig>
#include <KSystemClipboard>

#include "PortalSession_p.h"
#include "VideoFrame.h"
#include "krdp_logging.h"
#include "xdp_dbus_remotedesktop_interface.h"
#include "xdp_dbus_screencast_interface.h"

using namespace Qt::StringLiterals;

namespace KRdp
{

static const QString dbusService = QStringLiteral("org.freedesktop.portal.Desktop");
static const QString dbusPath = QStringLiteral("/org/freedesktop/portal/desktop");
static const QString dbusRequestInterface = QStringLiteral("org.freedesktop.portal.Request");
static const QString dbusResponse = QStringLiteral("Response");
static const QString dbusSessionInterface = QStringLiteral("org.freedesktop.portal.Session");

namespace
{
struct EncodedPacketMetadata {
    QSize size;
    QRegion damage;
    std::chrono::system_clock::time_point presentationTimeStamp;
    bool hasSize = false;
    bool hasDamage = false;
    bool hasPresentationTimeStamp = false;
};

struct PendingEncodedPacket {
    PipeWireEncodedStream::Packet packet;
    std::chrono::steady_clock::time_point queuedAt;
};

constexpr int MaxPendingFrameMetadata = 128;
constexpr int MaxPendingPacketsWithoutMetadata = 8;
constexpr auto MetadataPairWaitBudget = std::chrono::milliseconds(12);

QRegion fullFrameDamage(const QSize &size)
{
    if (size.isEmpty()) {
        return {};
    }
    return QRegion(QRect(QPoint(0, 0), size));
}

QRegion clippedDamage(const QRegion &damage, const QSize &size)
{
    if (size.isEmpty()) {
        return {};
    }
    auto clipped = damage.intersected(QRect(QPoint(0, 0), size));
    return clipped.isEmpty() ? fullFrameDamage(size) : clipped;
}

template<typename Stream>
void enableDamageMetadataIfSupported(Stream *stream)
{
    if constexpr (requires(Stream *s) {
                      s->setDamageEnabled(true);
                  }) {
        stream->setDamageEnabled(true);
    } else {
        qCWarning(KRDP) << "KPipeWire does not expose encoded damage metadata, using full-frame updates";
    }
}

template<typename Stream>
void setFullColorRangeIfSupported(Stream *stream)
{
    if constexpr (requires(Stream *s) {
                      s->setColorRange(typename Stream::ColorRange{});
                      Stream::ColorRange::Full;
                  }) {
        stream->setColorRange(Stream::ColorRange::Full);
    }
}

template<typename Stream>
void setPreferredH264Encoder(Stream *stream)
{
    auto encoder = PipeWireEncodedStream::H264Baseline;
    if constexpr (requires(Stream *s) {
                      s->suggestedEncoders();
                  }) {
        const auto suggested = stream->suggestedEncoders();
        if (suggested.contains(PipeWireEncodedStream::H264Main)) {
            encoder = PipeWireEncodedStream::H264Main;
        }
    }
    stream->setEncoder(encoder);
    qCDebug(KRDP) << "Using PipeWire H264 encoder profile:" << (encoder == PipeWireEncodedStream::H264Main ? "Main" : "Baseline");
}

template<typename Stream, typename Receiver, typename Callback>
bool connectFrameMetadataIfSupported(Stream *stream, Receiver *receiver, Callback &&callback)
{
    if constexpr (requires {
                      &Stream::frameMetadata;
                  }) {
        const auto connection = QObject::connect(stream, &Stream::frameMetadata, receiver, std::forward<Callback>(callback));
        return static_cast<bool>(connection);
    }
    return false;
}
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalSessionStream &stream)
{
    arg.beginStructure();
    arg >> stream.nodeId;

    arg.beginMap();
    while (!arg.atEnd()) {
        QString key;
        QVariant map;
        arg.beginMapEntry();
        arg >> key >> map;
        arg.endMapEntry();
        stream.map.insert(key, map);
    }
    arg.endMap();
    arg.endStructure();

    return arg;
}

void PortalRequest::onStarted(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QDBusObjectPath> reply = *watcher;
    if (!reply.isError()) {
        QDBusConnection::sessionBus().connect(QString{}, reply.value().path(), dbusRequestInterface, dbusResponse, this, SLOT(onFinished(uint, QVariantMap)));
    } else {
        m_callback(-1, {{QStringLiteral("errorMessage"), reply.error().message()}});
    }
    watcher->deleteLater();
}

void PortalRequest::onFinished(uint code, const QVariantMap &result)
{
    if (m_context) {
        m_callback(code, result);
    }
    deleteLater();
}

class KRDP_NO_EXPORT PortalSession::Private
{
public:
    Server *server = nullptr;

    std::unique_ptr<OrgFreedesktopPortalRemoteDesktopInterface> remoteInterface;
    std::unique_ptr<OrgFreedesktopPortalScreenCastInterface> screencastInterface;

    bool ignoreNextSystemClipboardChange = false;

    QDBusObjectPath sessionPath;
    QQueue<EncodedPacketMetadata> pendingFrameMetadata;
    QQueue<PendingEncodedPacket> pendingPackets;
    bool metadataSignalAvailable = false;
    bool metadataSeen = false;
    std::chrono::steady_clock::time_point lastMetadataMissLog;
    QVector<VideoMonitor> monitorLayout;
};

QString createHandleToken()
{
    return QStringLiteral("krdp%1").arg(QRandomGenerator::global()->generate());
}

PortalSession::PortalSession()
    : AbstractSession()
    , d(std::make_unique<Private>())
{
    d->remoteInterface = std::make_unique<OrgFreedesktopPortalRemoteDesktopInterface>(dbusService, dbusPath, QDBusConnection::sessionBus());
    d->screencastInterface = std::make_unique<OrgFreedesktopPortalScreenCastInterface>(dbusService, dbusPath, QDBusConnection::sessionBus());

    connect(KSystemClipboard::instance(), &KSystemClipboard::changed, this, [this](auto mode) {
        if (mode != QClipboard::Clipboard) {
            return;
        }

        auto data = KSystemClipboard::instance()->mimeData(mode);
        if (!data) {
            return;
        }

        // KSystemClipboard takes ownership of any QMimeData passed to it but
        // does not relinquish ownership over anything it returns. So manually
        // copy over the contents to a new instance of QMimeData so we can keep
        // the semantics the same.
        auto newData = new QMimeData();
        const auto formats = data->formats();
        for (auto format : formats) {
            newData->setData(format, data->data(format));
        }

        Q_EMIT clipboardDataChanged(newData);
    });

    if (!d->remoteInterface->isValid() || !d->screencastInterface->isValid()) {
        qCWarning(KRDP) << "Could not connect to Freedesktop Remote Desktop Portal";
        return;
    }
}

PortalSession::~PortalSession()
{
    // Make sure to clear any modifier keys that were pressed when the session closed, otherwise
    // we risk those keys getting stuck and the original session becoming unusable.
    for (auto keycode : {KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA}) {
        auto call = d->remoteInterface->NotifyKeyboardKeycode(d->sessionPath, QVariantMap{}, keycode, 0);
        call.waitForFinished();
    }

    auto closeMessage = QDBusMessage::createMethodCall(dbusService, d->sessionPath.path(), dbusSessionInterface, QStringLiteral("Close"));
    QDBusConnection::sessionBus().asyncCall(closeMessage);

    qCDebug(KRDP) << "Closing Freedesktop Portal Session";
}

void PortalSession::start()
{
    qCDebug(KRDP) << "Initializing Freedesktop Portal Session";

    auto parameters = QVariantMap{
        {QStringLiteral("handle_token"), createHandleToken()},
        {QStringLiteral("session_handle_token"), createHandleToken()},
    };
    new PortalRequest(d->remoteInterface->CreateSession(parameters), this, &PortalSession::onCreateSession);
}

void PortalSession::sendEvent(const std::shared_ptr<QEvent> &event)
{
    auto encodedStream = stream();
    if (!encodedStream || !encodedStream->isActive()) {
        return;
    }

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        auto me = std::static_pointer_cast<QMouseEvent>(event);
        int button = 0;
        if (me->button() == Qt::LeftButton) {
            button = BTN_LEFT;
        } else if (me->button() == Qt::MiddleButton) {
            button = BTN_MIDDLE;
        } else if (me->button() == Qt::RightButton) {
            button = BTN_RIGHT;
        } else {
            qCWarning(KRDP) << "Unsupported mouse button" << me->button();
            return;
        }
        uint state = me->type() == QEvent::MouseButtonPress ? 1 : 0;
        d->remoteInterface->NotifyPointerButton(d->sessionPath, QVariantMap{}, button, state);
        break;
    }
    case QEvent::MouseMove: {
        auto me = std::static_pointer_cast<QMouseEvent>(event);
        auto position = me->position();
        auto logicalPosition = QPointF{(position.x() / size().width()) * logicalSize().width(), (position.y() / size().height()) * logicalSize().height()};
        d->remoteInterface->NotifyPointerMotionAbsolute(d->sessionPath, QVariantMap{}, encodedStream->nodeId(), logicalPosition.x(), logicalPosition.y());
        break;
    }
    case QEvent::Wheel: {
        auto we = std::static_pointer_cast<QWheelEvent>(event);
        auto delta = we->angleDelta();
        if (delta.y() != 0) {
            d->remoteInterface->NotifyPointerAxisDiscrete(d->sessionPath, QVariantMap{}, 0 /* Vertical */, delta.y() / 120);
        }
        if (delta.x() != 0) {
            d->remoteInterface->NotifyPointerAxisDiscrete(d->sessionPath, QVariantMap{}, 1 /* Horizontal */, delta.x() / 120);
        }
        break;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        auto ke = std::static_pointer_cast<QKeyEvent>(event);
        auto state = ke->type() == QEvent::KeyPress ? 1 : 0;

        if (ke->nativeScanCode()) {
            d->remoteInterface->NotifyKeyboardKeycode(d->sessionPath, QVariantMap{}, ke->nativeScanCode(), state);
        } else {
            d->remoteInterface->NotifyKeyboardKeysym(d->sessionPath, QVariantMap{}, ke->nativeVirtualKey(), state);
        }
        break;
    }
    default:
        break;
    }
}

void PortalSession::setClipboardData(std::unique_ptr<QMimeData> data)
{
    // KSystemClipboard takes ownership
    if (data) {
        KSystemClipboard::instance()->setMimeData(data.release(), QClipboard::Clipboard);
    } else {
        KSystemClipboard::instance()->clear(QClipboard::Clipboard);
    }
}

void PortalSession::onCreateSession(uint code, const QVariantMap &result)
{
    if (code != 0) {
        qCWarning(KRDP) << "Could not open a new remote desktop session, error code" << code;
        Q_EMIT error();
        return;
    }

    d->sessionPath = QDBusObjectPath(result.value(QStringLiteral("session_handle")).toString());

    static const uint PermissionsPersistUntilExplicitlyRevoked = 2;

    auto parameters = QVariantMap{
        {QStringLiteral("types"), 7u},
        {QStringLiteral("handle_token"), createHandleToken()},
        {QStringLiteral("persist_mode"), PermissionsPersistUntilExplicitlyRevoked},
    };
    // name is set explicitly as this is also used by the KCM
    KConfigGroup restorationGroup = KSharedConfig::openStateConfig(QStringLiteral("krdp-serverstaterc"))->group(QStringLiteral("General"));
    QString restoreToken = restorationGroup.readEntry(QStringLiteral("restorationToken"));

    // this is a compatibility path for krdp < 6.3 that used a different name and in .config
    // in 6.4 onwards it can be killed
    if (restoreToken.isEmpty()) {
        KConfigGroup restorationGroup = KSharedConfig::openConfig(QStringLiteral("krdp-serverrc"))->group(QStringLiteral("General"));
        restoreToken = restorationGroup.readEntry(QStringLiteral("restorationToken"));
    } // end compat

    if (!restoreToken.isEmpty()) {
        parameters[QStringLiteral("restore_token")] = restoreToken;
    }

    new PortalRequest(d->remoteInterface->SelectDevices(d->sessionPath, parameters), this, &PortalSession::onDevicesSelected);
}

void PortalSession::onDevicesSelected(uint code, const QVariantMap & /*result*/)
{
    if (code != 0) {
        qCWarning(KRDP) << "Could not select devices for remote desktop session, error code" << code;
        Q_EMIT error();
        return;
    }

    QVariantMap parameters;
    if (virtualMonitor()) {
        parameters = {{QStringLiteral("types"), 4u}}; // VIRTUAL
    } else {
        parameters = {{QStringLiteral("types"), 1u}, // MONITOR
                      {QStringLiteral("multiple"), activeStream() >= 0}};
    }

    new PortalRequest(d->screencastInterface->SelectSources(d->sessionPath, parameters), this, &PortalSession::onSourcesSelected);
}

void PortalSession::onSourcesSelected(uint code, const QVariantMap & /*result*/)
{
    if (code != 0) {
        qCWarning(KRDP) << "Could not select sources for screencast session, error code" << code;
        Q_EMIT error();
        return;
    }

    auto parameters = QVariantMap{
        {QStringLiteral("handle_token"), createHandleToken()},
    };
    new PortalRequest(d->remoteInterface->Start(d->sessionPath, QString{}, parameters), this, &PortalSession::onSessionStarted);
}

void KRdp::PortalSession::onSessionStarted(uint code, const QVariantMap &result)
{
    if (code != 0) {
        qCWarning(KRDP) << "Could not start screencast session, error code" << code;
        Q_EMIT error();
        return;
    }

    if (result.value(QStringLiteral("devices")).toUInt() == 0) {
        qCWarning(KRDP) << "No devices were granted" << result;
        Q_EMIT error();
        return;
    }

    KConfigGroup restorationGroup = KSharedConfig::openStateConfig(QStringLiteral("krdp-serverstaterc"))->group(QStringLiteral("General"));
    restorationGroup.writeEntry("restorationToken", result.value(QStringLiteral("restore_token")));

    const auto streams = qdbus_cast<QList<PortalSessionStream>>(result.value(QStringLiteral("streams")));
    if (streams.isEmpty()) {
        qCWarning(KRDP) << "No screencast streams supplied";
        Q_EMIT error();
        return;
    }

    auto watcher = new QDBusPendingCallWatcher(d->screencastInterface->OpenPipeWireRemote(d->sessionPath, QVariantMap{}));
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, streams](QDBusPendingCallWatcher *watcher) {
        auto reply = QDBusReply<QDBusUnixFileDescriptor>(*watcher);
        if (reply.isValid()) {
            qCDebug(KRDP) << "Started Freedesktop Portal session";

            if (activeStream() >= streams.size()) {
                qCWarning(KRDP) << "Requested monitor index out of range, using first monitor";
                setActiveStream(0);
            }
            auto stream = streams.at(activeStream() >= 0 ? activeStream() : 0);

            setLogicalSize(qdbus_cast<QSize>(stream.map.value(u"size"_s)));
            d->monitorLayout = {
                VideoMonitor{
                    .geometry = QRect(QPoint(0, 0), logicalSize()),
                    .primary = true,
                },
            };
            auto fd = reply.value();
            auto encodedStream = this->stream();
            d->pendingFrameMetadata.clear();
            d->pendingPackets.clear();
            d->metadataSeen = false;
            d->lastMetadataMissLog = {};
            encodedStream->setNodeId(stream.nodeId);
            encodedStream->setFd(fd.takeFileDescriptor());
            encodedStream->setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed);
            // Ensure we encode in full color range when the KPipeWire API supports it.
            setFullColorRangeIfSupported(encodedStream);
            setPreferredH264Encoder(encodedStream);
            enableDamageMetadataIfSupported(encodedStream);
            connect(encodedStream, &PipeWireEncodedStream::newPacket, this, &PortalSession::onPacketReceived);
            connect(encodedStream, &PipeWireEncodedStream::sizeChanged, this, &PortalSession::setSize);
            connect(encodedStream, &PipeWireEncodedStream::cursorChanged, this, &PortalSession::cursorUpdate);
            d->metadataSignalAvailable = connectFrameMetadataIfSupported(encodedStream, this, [this](const auto &meta) {
                EncodedPacketMetadata frameMetadata;
                frameMetadata.size = meta.size;
                frameMetadata.hasSize = !meta.size.isEmpty();
                if (meta.hasDamage) {
                    frameMetadata.damage = meta.damage;
                    frameMetadata.hasDamage = true;
                }
                if (meta.hasPts) {
                    frameMetadata.presentationTimeStamp = std::chrono::system_clock::time_point{std::chrono::nanoseconds(meta.ptsNs)};
                    frameMetadata.hasPresentationTimeStamp = true;
                }
                d->pendingFrameMetadata.enqueue(frameMetadata);
                while (d->pendingFrameMetadata.size() > MaxPendingFrameMetadata) {
                    d->pendingFrameMetadata.dequeue();
                }
                d->metadataSeen = true;
                processPendingPackets();
            });
            QDBusConnection::sessionBus().connect(u"org.freedesktop.portal.Desktop"_s,
                                                  d->sessionPath.path(),
                                                  u"org.freedesktop.portal.Session"_s,
                                                  u"Closed"_s,
                                                  this,
                                                  SLOT(onSessionClosed()));

            setStarted(true);
        } else {
            qCWarning(KRDP) << "Could not open pipewire remote";
            Q_EMIT error();
        }
        watcher->deleteLater();
    });
}

void PortalSession::onSessionClosed()
{
    qCWarning(KRDP) << "Portal session was closed!";
    Q_EMIT error();
}

void PortalSession::processPendingPackets()
{
    auto emitFrame = [this](const PipeWireEncodedStream::Packet &packet, const EncodedPacketMetadata *metadata) {
        VideoFrame frameData;
        frameData.size = size();
        frameData.data = packet.data();
        frameData.isKeyFrame = packet.isKeyFrame();
        frameData.monitors = d->monitorLayout;
        frameData.damage = fullFrameDamage(frameData.size);

        if (frameData.monitors.isEmpty() && !frameData.size.isEmpty()) {
            frameData.monitors.push_back(VideoMonitor{
                .geometry = QRect(QPoint(0, 0), frameData.size),
                .primary = true,
            });
        }

        const bool metadataApplied = metadata != nullptr;
        if (metadata) {
            if (metadata->hasSize && !metadata->size.isEmpty()) {
                frameData.size = metadata->size;
            }
            if (metadata->hasPresentationTimeStamp) {
                frameData.presentationTimeStamp = metadata->presentationTimeStamp;
            }
            if (metadata->hasDamage) {
                frameData.damage = clippedDamage(metadata->damage, frameData.size);
            }
        }

        if (!metadataApplied || frameData.isKeyFrame || frameData.damage.isEmpty()) {
            frameData.damage = fullFrameDamage(frameData.size);
        }

        Q_EMIT frameReceived(frameData);
    };

    while (!d->pendingPackets.isEmpty()) {
        if (!d->pendingFrameMetadata.isEmpty()) {
            auto packet = d->pendingPackets.dequeue().packet;
            auto metadata = d->pendingFrameMetadata.dequeue();
            emitFrame(packet, &metadata);
            continue;
        }

        auto pendingPacket = d->pendingPackets.head();
        const bool shouldSendWithoutMetadata = !d->metadataSignalAvailable || !d->metadataSeen || pendingPacket.packet.isKeyFrame();
        if (shouldSendWithoutMetadata) {
            d->pendingPackets.dequeue();
            emitFrame(pendingPacket.packet, nullptr);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const bool waitedTooLong = (now - pendingPacket.queuedAt) >= MetadataPairWaitBudget;
        const bool queueTooDeep = d->pendingPackets.size() > MaxPendingPacketsWithoutMetadata;
        if (waitedTooLong || queueTooDeep) {
            if (d->lastMetadataMissLog.time_since_epoch().count() == 0 || (now - d->lastMetadataMissLog) >= std::chrono::seconds(2)) {
                qCDebug(KRDP) << "No matching damage metadata for encoded packet, using full-frame update";
                d->lastMetadataMissLog = now;
            }
            d->pendingPackets.dequeue();
            emitFrame(pendingPacket.packet, nullptr);
            continue;
        }

        // Leave packet queued briefly so late metadata can still be paired.
        break;
    }
}

void PortalSession::onPacketReceived(const PipeWireEncodedStream::Packet &data)
{
    d->pendingPackets.enqueue(PendingEncodedPacket{
        .packet = data,
        .queuedAt = std::chrono::steady_clock::now(),
    });
    processPendingPackets();
}

}

#include "moc_PortalSession_p.cpp"

#include "moc_PortalSession.cpp"
