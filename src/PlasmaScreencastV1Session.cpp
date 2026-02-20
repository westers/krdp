// SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleix.pol_gonzalez@mercedes-benz.com>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PlasmaScreencastV1Session.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QQueue>
#include <QRect>
#include <QRegion>
#include <QScreen>
#include <QWaylandClientExtensionTemplate>
#include <qpa/qplatformnativeinterface.h>

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>
#include <chrono>
#include <algorithm>
#include <optional>
#include <utility>

#include "qwayland-fake-input.h"
#include "qwayland-wayland.h"
#include "screencasting_p.h"

#include "VideoStream.h"
#include "krdp_logging.h"

namespace KRdp
{

class FakeInput : public QWaylandClientExtensionTemplate<FakeInput>, public QtWayland::org_kde_kwin_fake_input
{
public:
    FakeInput()
        : QWaylandClientExtensionTemplate<FakeInput>(4)
    {
        initialize();
        if (isActive()) {
            auto appId = qGuiApp->desktopFileName();
            if (appId.isEmpty()) {
                appId = QStringLiteral("org.kde.krdpserver");
            }
            authenticate(appId, QStringLiteral("KRDP remote control"));
        }
        Q_ASSERT(isActive());
    }
};

namespace
{
struct XKBStateDeleter {
    void operator()(struct xkb_state *state) const
    {
        xkb_state_unref(state);
    }
};
struct XKBKeymapDeleter {
    void operator()(struct xkb_keymap *keymap) const
    {
        xkb_keymap_unref(keymap);
    }
};
struct XKBContextDeleter {
    void operator()(struct xkb_context *context) const
    {
        xkb_context_unref(context);
    }
};
using ScopedXKBState = std::unique_ptr<struct xkb_state, XKBStateDeleter>;
using ScopedXKBKeymap = std::unique_ptr<struct xkb_keymap, XKBKeymapDeleter>;
using ScopedXKBContext = std::unique_ptr<struct xkb_context, XKBContextDeleter>;

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

QRect logicalRectForStream(int streamIndex)
{
    const auto screens = qGuiApp->screens();
    if (screens.isEmpty()) {
        return {};
    }

    QRect logicalRect;
    if (streamIndex < 0 || streamIndex >= screens.size()) {
        QRegion logicalRegion;
        for (auto *screen : screens) {
            logicalRegion += screen->geometry();
        }
        logicalRect = logicalRegion.boundingRect();
    } else {
        logicalRect = screens.at(streamIndex)->geometry();
    }
    return logicalRect;
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
class Xkb : public QtWayland::wl_keyboard
{
public:
    struct Code {
        const uint32_t level;
        const uint32_t code;
    };
    std::optional<Code> keycodeFromKeysym(xkb_keysym_t keysym)
    {
        /* The offset between KEY_* numbering, and keycodes in the XKB evdev
         * dataset. */
        static const uint EVDEV_OFFSET = 8;

        auto layout = xkb_state_serialize_layout(m_state.get(), XKB_STATE_LAYOUT_EFFECTIVE);
        const xkb_keycode_t max = xkb_keymap_max_keycode(m_keymap.get());
        for (xkb_keycode_t keycode = xkb_keymap_min_keycode(m_keymap.get()); keycode < max; keycode++) {
            uint levelCount = xkb_keymap_num_levels_for_key(m_keymap.get(), keycode, layout);
            for (uint currentLevel = 0; currentLevel < levelCount; currentLevel++) {
                const xkb_keysym_t *syms;
                uint num_syms = xkb_keymap_key_get_syms_by_level(m_keymap.get(), keycode, layout, currentLevel, &syms);
                for (uint sym = 0; sym < num_syms; sym++) {
                    if (syms[sym] == keysym) {
                        return Code{currentLevel, keycode - EVDEV_OFFSET};
                    }
                }
            }
        }
        return {};
    }

    static Xkb *self()
    {
        static Xkb self;
        return &self;
    }

private:
    Xkb()
    {
        m_ctx.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
        if (!m_ctx) {
            qCWarning(KRDP) << "Failed to create xkb context";
            return;
        }
        m_keymap.reset(xkb_keymap_new_from_names(m_ctx.get(), nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS));
        if (!m_keymap) {
            qCWarning(KRDP) << "Failed to create the keymap";
            return;
        }
        m_state.reset(xkb_state_new(m_keymap.get()));
        if (!m_state) {
            qCWarning(KRDP) << "Failed to create the xkb state";
            return;
        }

        QPlatformNativeInterface *nativeInterface = qGuiApp->platformNativeInterface();
        auto seat = static_cast<wl_seat *>(nativeInterface->nativeResourceForIntegration("wl_seat"));
        init(wl_seat_get_keyboard(seat));
    }

    void keyboard_keymap(uint32_t format, int32_t fd, uint32_t size) override
    {
        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
            qCWarning(KRDP) << "unknown keymap format:" << format;
            close(fd);
            return;
        }

        char *map_str = static_cast<char *>(mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
        if (map_str == MAP_FAILED) {
            close(fd);
            return;
        }

        m_keymap.reset(xkb_keymap_new_from_string(m_ctx.get(), map_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS));
        munmap(map_str, size);
        close(fd);

        if (m_keymap)
            m_state.reset(xkb_state_new(m_keymap.get()));
        else
            m_state.reset(nullptr);
    }

    ScopedXKBContext m_ctx;
    ScopedXKBKeymap m_keymap;
    ScopedXKBState m_state;
};

class KRDP_NO_EXPORT PlasmaScreencastV1Session::Private
{
public:
    Server *server = nullptr;

    Screencasting m_screencasting;
    ScreencastingStream *request = nullptr;
    FakeInput *remoteInterface = nullptr;
    QRect logicalRect;
    QQueue<EncodedPacketMetadata> pendingFrameMetadata;
    QQueue<PendingEncodedPacket> pendingPackets;
    bool metadataSignalAvailable = false;
    bool metadataSeen = false;
    std::chrono::steady_clock::time_point lastMetadataMissLog;
};

PlasmaScreencastV1Session::PlasmaScreencastV1Session()
    : AbstractSession()
    , d(std::make_unique<Private>())
{
    d->remoteInterface = new FakeInput();
}

PlasmaScreencastV1Session::~PlasmaScreencastV1Session()
{
    qCDebug(KRDP) << "Closing Plasma Remote Session";
}

void PlasmaScreencastV1Session::start()
{
    if (auto vm = virtualMonitor()) {
        d->request = d->m_screencasting.createVirtualMonitorStream(vm->name, vm->size, vm->dpr, Screencasting::Metadata);
        d->logicalRect = QRect(QPoint(0, 0), vm->size);
        qCDebug(KRDP) << "Using virtual monitor stream" << vm->name << "logical rect" << d->logicalRect;
    } else {
        const auto screens = qGuiApp->screens();
        const auto streamIndex = activeStream();
        if (streamIndex >= 0 && streamIndex < screens.size()) {
            d->request = d->m_screencasting.createOutputStream(screens.at(streamIndex), Screencasting::Metadata);
            d->logicalRect = logicalRectForStream(streamIndex);
            qCDebug(KRDP) << "Using output stream index" << streamIndex << "screen" << screens.at(streamIndex)->name() << "logical rect" << d->logicalRect;
        } else {
            d->request = d->m_screencasting.createWorkspaceStream(Screencasting::Metadata);
            d->logicalRect = logicalRectForStream(-1);
            qCDebug(KRDP) << "Using workspace stream logical rect" << d->logicalRect;
        }
    }

    if (!d->request) {
        Q_EMIT error();
        return;
    }
    connect(d->request, &ScreencastingStream::failed, this, &PlasmaScreencastV1Session::error);
    connect(d->request, &ScreencastingStream::created, this, [this](uint nodeId) {
        qCDebug(KRDP) << "Started Plasma session";
        if (!d->logicalRect.isEmpty()) {
            setLogicalSize(d->logicalRect.size());
        } else {
            setLogicalSize(d->request->size());
        }
        qCDebug(KRDP) << "Plasma stream sizes: request" << d->request->size() << "logical" << logicalSize();
        auto encodedStream = stream();
        d->pendingFrameMetadata.clear();
        d->pendingPackets.clear();
        d->metadataSeen = false;
        d->lastMetadataMissLog = {};
        encodedStream->setNodeId(nodeId);
        encodedStream->setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed);
        setFullColorRangeIfSupported(encodedStream);
        setPreferredH264Encoder(encodedStream);
        enableDamageMetadataIfSupported(encodedStream);
        connect(encodedStream, &PipeWireEncodedStream::newPacket, this, &PlasmaScreencastV1Session::onPacketReceived);
        connect(encodedStream, &PipeWireEncodedStream::sizeChanged, this, &PlasmaScreencastV1Session::setSize);
        connect(encodedStream, &PipeWireEncodedStream::cursorChanged, this, &PlasmaScreencastV1Session::cursorUpdate);
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
        setStarted(true);
    });
}

void PlasmaScreencastV1Session::sendEvent(const std::shared_ptr<QEvent> &event)
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
        d->remoteInterface->button(button, state);
        break;
    }
    case QEvent::MouseMove: {
        auto me = std::static_pointer_cast<QMouseEvent>(event);
        auto position = me->position();
        if (size().isEmpty() || logicalSize().isEmpty()) {
            return;
        }
        const auto inputWidth = std::max(1, size().width() - 1);
        const auto inputHeight = std::max(1, size().height() - 1);
        const auto logicalWidth = std::max(1, logicalSize().width() - 1);
        const auto logicalHeight = std::max(1, logicalSize().height() - 1);
        const auto normalizedX = std::clamp(position.x() / double(inputWidth), 0.0, 1.0);
        const auto normalizedY = std::clamp(position.y() / double(inputHeight), 0.0, 1.0);
        auto logicalPosition = QPointF{normalizedX * logicalWidth + d->logicalRect.x(), normalizedY * logicalHeight + d->logicalRect.y()};
        d->remoteInterface->pointer_motion_absolute(wl_fixed_from_double(logicalPosition.x()), wl_fixed_from_double(logicalPosition.y()));
        break;
    }
    case QEvent::Wheel: {
        auto we = std::static_pointer_cast<QWheelEvent>(event);
        auto delta = we->angleDelta();
        if (delta.y() != 0) {
            d->remoteInterface->axis(WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(delta.y() / 120.0));
        }
        if (delta.x() != 0) {
            d->remoteInterface->axis(WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(delta.x() / 120.0));
        }
        break;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        auto ke = std::static_pointer_cast<QKeyEvent>(event);
        auto state = ke->type() == QEvent::KeyPress ? 1 : 0;

        if (ke->nativeScanCode()) {
            d->remoteInterface->keyboard_key(ke->nativeScanCode(), state);
        } else {
            auto keycode = Xkb::self()->keycodeFromKeysym(ke->nativeVirtualKey());
            if (!keycode) {
                qCWarning(KRDP) << "Failed to convert keysym into keycode" << ke->nativeVirtualKey();
                return;
            }

            auto sendKey = [this, state](int keycode) {
                d->remoteInterface->keyboard_key(keycode, state);
            };
            switch (keycode->level) {
            case 0:
                break;
            case 1:
                sendKey(KEY_LEFTSHIFT);
                break;
            case 2:
                sendKey(KEY_RIGHTALT);
                break;
            default:
                qCWarning(KRDP) << "Unsupported key level" << keycode->level;
                break;
            }
            sendKey(keycode->code);
        }
        break;
    }
    default:
        break;
    }
}

void PlasmaScreencastV1Session::setClipboardData(std::unique_ptr<QMimeData> data)
{
    Q_UNUSED(data);
}

void PlasmaScreencastV1Session::processPendingPackets()
{
    auto emitFrame = [this](const PipeWireEncodedStream::Packet &packet, const EncodedPacketMetadata *metadata) {
        VideoFrame frameData;
        frameData.size = size();
        frameData.data = packet.data();
        frameData.isKeyFrame = packet.isKeyFrame();
        frameData.damage = fullFrameDamage(frameData.size);

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

void PlasmaScreencastV1Session::onPacketReceived(const PipeWireEncodedStream::Packet &data)
{
    d->pendingPackets.enqueue(PendingEncodedPacket{
        .packet = data,
        .queuedAt = std::chrono::steady_clock::now(),
    });
    processPendingPackets();
}

}
