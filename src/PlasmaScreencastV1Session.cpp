// SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleix.pol_gonzalez@mercedes-benz.com>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PlasmaScreencastV1Session.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QQueue>
#include <QRect>
#include <QRegion>
#include <QScreen>
#include <QTimer>
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

// Closed-stream recovery. A DPMS wake makes KWin tear down and re-add every
// output, so wait for the output set to settle, keep retrying for a while, and
// only settle for a workspace stream in the last few attempts.
constexpr int MaxRecoveryAttempts = 24;
constexpr int RecoveryIntervalMs = 500;
constexpr int RecoverySettleMs = 750;
constexpr int WorkspaceFallbackAttempts = 4;

// KPipeWire tears its produce thread down asynchronously after stop(): start()
// is a no-op until that thread is gone, and the node ID is cleared once it is.
// Poll for that before attaching a replacement node.
constexpr int StreamRestartPollMs = 10;
constexpr auto StreamRestartTimeout = std::chrono::milliseconds(5000);

QRegion fullFrameDamage(const QSize &size)
{
    if (size.isEmpty()) {
        return {};
    }
    return QRegion(QRect(QPoint(0, 0), size));
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

QVector<VideoMonitor> monitorLayoutForStream(int streamIndex, const QRect &logicalRect)
{
    QVector<VideoMonitor> monitors;
    const auto screens = qGuiApp->screens();
    if (screens.isEmpty()) {
        return monitors;
    }

    auto primaryScreen = qGuiApp->primaryScreen();
    if (streamIndex >= 0 && streamIndex < screens.size()) {
        const auto geometry = screens.at(streamIndex)->geometry().translated(-logicalRect.topLeft());
        monitors.push_back(VideoMonitor{
            .geometry = geometry,
            .primary = (screens.at(streamIndex) == primaryScreen),
        });
    } else {
        monitors.reserve(screens.size());
        for (auto *screen : screens) {
            monitors.push_back(VideoMonitor{
                .geometry = screen->geometry().translated(-logicalRect.topLeft()),
                .primary = (screen == primaryScreen),
            });
        }
    }

    if (!std::any_of(monitors.cbegin(), monitors.cend(), [](const auto &monitor) {
            return monitor.primary;
        })
        && !monitors.isEmpty()) {
        monitors.first().primary = true;
    }

    return monitors;
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

template<typename Stream>
bool requestKeyFrameIfSupported(Stream *stream)
{
    if constexpr (requires(Stream *s) { s->requestKeyFrame(); }) {
        stream->requestKeyFrame();
        return true;
    } else {
        return false;
    }
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
    enum class StreamTarget {
        None,
        Output,
        Workspace,
        Virtual,
    };

    Server *server = nullptr;

    Screencasting m_screencasting;
    ScreencastingStream *request = nullptr;
    FakeInput *remoteInterface = nullptr;
    StreamTarget streamTarget = StreamTarget::None;
    QPointer<QScreen> outputScreen = nullptr;
    QString targetScreenName;
    QRect logicalRect;
    QVector<VideoMonitor> monitorLayout;
    bool streamConfigured = false;
    bool streamSignalsConnected = false;
    bool startedSignalEmitted = false;
    QTimer recoveryTimer;
    int recoveryAttempt = 0;
    QTimer streamRestartTimer;
    uint pendingNodeId = 0;
    std::chrono::steady_clock::time_point streamRestartWaitStarted;
};

PlasmaScreencastV1Session::PlasmaScreencastV1Session()
    : AbstractSession()
    , d(std::make_unique<Private>())
{
    d->remoteInterface = new FakeInput();

    d->recoveryTimer.setSingleShot(true);
    connect(&d->recoveryTimer, &QTimer::timeout, this, [this]() {
        attemptStreamRecovery(d->recoveryAttempt);
    });
    // While recovering, every output change pushes the next attempt out until the set is stable.
    auto settleRecovery = [this](QScreen *) {
        if (d->recoveryTimer.isActive()) {
            d->recoveryTimer.start(RecoverySettleMs);
        }
    };
    connect(qGuiApp, &QGuiApplication::screenAdded, this, settleRecovery);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, settleRecovery);

    d->streamRestartTimer.setInterval(StreamRestartPollMs);
    connect(&d->streamRestartTimer, &QTimer::timeout, this, [this]() {
        auto encodedStream = stream();
        const bool tornDown = encodedStream->nodeId() == 0;
        if (!tornDown && (std::chrono::steady_clock::now() - d->streamRestartWaitStarted) < StreamRestartTimeout) {
            return;
        }
        d->streamRestartTimer.stop();
        if (!tornDown) {
            qCWarning(KRDP) << "Encoded stream did not shut down within" << StreamRestartTimeout.count() << "ms, attaching new node anyway";
        }
        attachEncodedStream(d->pendingNodeId, true);
    });
}

PlasmaScreencastV1Session::~PlasmaScreencastV1Session()
{
    qCDebug(KRDP) << "Closing Plasma Remote Session";
}

void PlasmaScreencastV1Session::start()
{
    if (!setupScreencastRequest()) {
        Q_EMIT error();
    }
}

void PlasmaScreencastV1Session::refreshDisplayConfiguration()
{
    if (virtualMonitor()) {
        return;
    }

    // Re-create the screencast for the new topology. The stream restart goes
    // through the deferred attach in onScreencastCreated() (it waits for
    // KPipeWire's produce thread to tear down before calling start() again).
    if (!setupScreencastRequest()) {
        qCWarning(KRDP) << "Unable to refresh display configuration after topology change (session kept alive)";
    }
}

void PlasmaScreencastV1Session::scheduleStreamRecovery(int attempt, int delayMs)
{
    d->recoveryAttempt = attempt;
    d->recoveryTimer.start(delayMs);
}

void PlasmaScreencastV1Session::attemptStreamRecovery(int attempt)
{
    const auto screens = qGuiApp->screens();
    const bool screensAvailable = !screens.isEmpty() && screens.first()->geometry().isValid();
    // Hold out for the configured output; only the final attempts may settle for the workspace.
    const bool allowWorkspaceFallback = attempt >= MaxRecoveryAttempts - WorkspaceFallbackAttempts;

    bool recovered = false;
    if (screensAvailable) {
        qCInfo(KRDP) << "Attempting to recover display stream (attempt" << (attempt + 1) << "of" << MaxRecoveryAttempts << ", workspace fallback:" << allowWorkspaceFallback << ")";
        recovered = setupScreencastRequest(allowWorkspaceFallback);
    } else {
        qCInfo(KRDP) << "No screens available yet (attempt" << (attempt + 1) << "of" << MaxRecoveryAttempts << ")";
    }
    if (recovered) {
        return;
    }

    if (attempt + 1 < MaxRecoveryAttempts) {
        qCInfo(KRDP) << "Retrying display stream recovery in" << RecoveryIntervalMs << "ms";
        scheduleStreamRecovery(attempt + 1, RecoveryIntervalMs);
        return;
    }
    qCWarning(KRDP) << "Display stream recovery failed after" << MaxRecoveryAttempts << "attempts (session kept alive)";
}

bool PlasmaScreencastV1Session::setupScreencastRequest(bool allowWorkspaceFallback)
{
    Private::StreamTarget target = Private::StreamTarget::Workspace;
    QPointer<QScreen> outputScreen = nullptr;
    if (virtualMonitor()) {
        target = Private::StreamTarget::Virtual;
    } else {
        const auto screens = qGuiApp->screens();
        const auto streamIndex = activeStream();
        // On recovery, resolve by saved screen name to survive screen list reordering.
        if (!d->targetScreenName.isEmpty()) {
            for (auto *screen : screens) {
                if (screen->name() == d->targetScreenName) {
                    target = Private::StreamTarget::Output;
                    outputScreen = screen;
                    break;
                }
            }
            if (!outputScreen) {
                if (!allowWorkspaceFallback) {
                    qCInfo(KRDP) << "Target screen" << d->targetScreenName << "not available yet, waiting for it to return";
                    return false;
                }
                qCWarning(KRDP) << "Target screen" << d->targetScreenName << "no longer available, falling back to workspace";
            }
        } else if (streamIndex >= 0 && streamIndex < screens.size()) {
            target = Private::StreamTarget::Output;
            outputScreen = screens.at(streamIndex);
            d->targetScreenName = outputScreen->name();
        }
    }

    QRect targetLogicalRect;
    QVector<VideoMonitor> targetMonitorLayout;
    if (target == Private::StreamTarget::Virtual) {
        auto vm = virtualMonitor();
        targetLogicalRect = QRect(QPoint(0, 0), vm->size);
        targetMonitorLayout = {
            VideoMonitor{
                .geometry = targetLogicalRect,
                .primary = true,
            },
        };
    } else if (target == Private::StreamTarget::Output) {
        targetLogicalRect = logicalRectForStream(activeStream());
        targetMonitorLayout = monitorLayoutForStream(activeStream(), targetLogicalRect);
    } else {
        targetLogicalRect = logicalRectForStream(-1);
        targetMonitorLayout = monitorLayoutForStream(-1, targetLogicalRect);
    }

    const bool targetChanged = (d->streamTarget != target);
    const bool outputChanged = (target == Private::StreamTarget::Output) && (d->outputScreen != outputScreen);
    const bool logicalRectChanged = (d->logicalRect != targetLogicalRect);
    const bool requiresRecreate = !d->request || targetChanged || outputChanged || logicalRectChanged;
    d->logicalRect = targetLogicalRect;
    d->monitorLayout = targetMonitorLayout;
    if (!d->logicalRect.isEmpty()) {
        setLogicalSize(d->logicalRect.size());
    }
    d->streamTarget = target;
    d->outputScreen = outputScreen;

    if (!requiresRecreate) {
        return true;
    }

    if (d->request) {
        disconnect(d->request, nullptr, this, nullptr);
        d->request->deleteLater();
        d->request = nullptr;
    }

    if (target == Private::StreamTarget::Virtual) {
        auto vm = virtualMonitor();
        d->request = d->m_screencasting.createVirtualMonitorStream(vm->name, vm->size, vm->dpr, Screencasting::Metadata);
        qCDebug(KRDP) << "Using virtual monitor stream" << vm->name << "logical rect" << d->logicalRect;
    } else if (target == Private::StreamTarget::Output) {
        d->request = d->m_screencasting.createOutputStream(outputScreen, Screencasting::Metadata);
        if (!d->request && !allowWorkspaceFallback) {
            // No wl_output behind the screen yet (placeholder or mid-teardown); let recovery retry.
            qCInfo(KRDP) << "Output stream for screen" << (outputScreen ? outputScreen->name() : QStringLiteral("<unknown>")) << "not creatable yet, waiting";
            return false;
        }
        if (!d->request) {
            qCWarning(KRDP) << "Failed to create output stream for screen"
                            << (outputScreen ? outputScreen->name() : QStringLiteral("<unknown>"))
                            << ", falling back to workspace stream";
            target = Private::StreamTarget::Workspace;
            d->streamTarget = target;
            d->outputScreen = nullptr;
            d->logicalRect = logicalRectForStream(-1);
            d->monitorLayout = monitorLayoutForStream(-1, d->logicalRect);
            if (!d->logicalRect.isEmpty()) {
                setLogicalSize(d->logicalRect.size());
            }
        } else {
            qCDebug(KRDP) << "Using output stream index" << activeStream() << "screen" << outputScreen->name() << "logical rect" << d->logicalRect;
        }
    }

    if (!d->request && target == Private::StreamTarget::Workspace) {
        d->request = d->m_screencasting.createWorkspaceStream(Screencasting::Metadata);
        qCDebug(KRDP) << "Using workspace stream logical rect" << d->logicalRect;
    }

    if (!d->request) {
        return false;
    }
    // A stream now exists again (whichever path created it), so any pending retry is moot.
    d->recoveryTimer.stop();

    connect(d->request, &ScreencastingStream::failed, this, &PlasmaScreencastV1Session::error);
    connect(d->request, &ScreencastingStream::closed, this, [this]() {
        qCWarning(KRDP) << "Screencast stream closed, deferring recovery to let compositor settle";
        d->request = nullptr;
        scheduleStreamRecovery(0, RecoverySettleMs);
    });
    connect(d->request, &ScreencastingStream::created, this, &PlasmaScreencastV1Session::onScreencastCreated);

    return true;
}

void PlasmaScreencastV1Session::onScreencastCreated(uint nodeId)
{
    if (!d->logicalRect.isEmpty()) {
        setLogicalSize(d->logicalRect.size());
    } else if (d->request) {
        setLogicalSize(d->request->size());
    }
    if (d->request && !d->request->size().isEmpty()) {
        setSize(d->request->size());
    }
    qCDebug(KRDP) << "Plasma stream sizes: request" << (d->request ? d->request->size() : QSize()) << "logical" << logicalSize();

    auto encodedStream = stream();

    // A previous node is still being torn down (see StreamRestartPollMs): attach
    // once it is gone, otherwise start() silently does nothing and the node ID is
    // wiped when the old thread exits.
    if (encodedStream->isActive() || d->streamRestartTimer.isActive()) {
        restartEncodedStream(nodeId);
        return;
    }

    attachEncodedStream(nodeId, false);
}

void PlasmaScreencastV1Session::restartEncodedStream(uint nodeId)
{
    auto encodedStream = stream();
    if (encodedStream->isActive()) {
        encodedStream->stop();
    }
    d->pendingNodeId = nodeId;
    if (!d->streamRestartTimer.isActive()) {
        d->streamRestartWaitStarted = std::chrono::steady_clock::now();
        d->streamRestartTimer.start();
    }
}

void PlasmaScreencastV1Session::requestKeyFrame()
{
    auto encodedStream = stream();
    const uint nodeId = encodedStream->nodeId();
    if (!d->streamConfigured || nodeId == 0 || !streamingRequested()) {
        return;
    }
    if (requestKeyFrameIfSupported(encodedStream)) {
        qCDebug(KRDP) << "Requested a keyframe from the encoder for the new surface";
        return;
    }
    // Stock KPipeWire 6.6 cannot be asked for an IDR mid-stream, but a restarted
    // encoded stream always opens with one. Re-attach the same PipeWire node
    // through the deferred restart (KWin keeps the screencast source alive;
    // only the KPipeWire consumer/encoder is recreated).
    if (d->streamRestartTimer.isActive()) {
        // A restart is already in flight; it will deliver a keyframe.
        return;
    }
    qCDebug(KRDP) << "Restarting encoded stream on node" << nodeId << "to obtain a keyframe for the new surface";
    restartEncodedStream(nodeId);
}

void PlasmaScreencastV1Session::attachEncodedStream(uint nodeId, bool streamWasActive)
{
    auto encodedStream = stream();
    const bool shouldResumeStreaming = d->streamConfigured && (streamWasActive || streamingRequested());

    encodedStream->setNodeId(nodeId);
    encodedStream->setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed);
    if (!d->streamConfigured) {
        setFullColorRangeIfSupported(encodedStream);
        setPreferredH264Encoder(encodedStream);
    }

    if (!d->streamSignalsConnected) {
        connect(encodedStream, &PipeWireEncodedStream::newPacket, this, &PlasmaScreencastV1Session::onPacketReceived);
        connect(encodedStream, &PipeWireEncodedStream::sizeChanged, this, &PlasmaScreencastV1Session::setSize);
        connect(encodedStream, &PipeWireEncodedStream::cursorChanged, this, &PlasmaScreencastV1Session::cursorUpdate);
        d->streamSignalsConnected = true;
    }

    d->streamConfigured = true;
    if (shouldResumeStreaming) {
        qCDebug(KRDP) << "Restarting encoded stream on node" << nodeId;
        encodedStream->start();
    }

    if (!d->startedSignalEmitted) {
        qCDebug(KRDP) << "Started Plasma session";
        d->startedSignalEmitted = true;
        setStarted(true);
    } else {
        qCDebug(KRDP) << "Re-attached Plasma screencast stream on node" << nodeId;
    }
}

void PlasmaScreencastV1Session::sendEvent(const std::shared_ptr<QEvent> &event)
{
    auto encodedStream = stream();
    if (!encodedStream || !encodedStream->isActive()) {
        return;
    }

    if (event->type() == QEvent::MouseMove) {
        // The position is relative to this session's own captured output, in
        // capture pixels; normalise it and map it onto the output's place in
        // the KWin-global logical coordinate space fake input expects.
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
        return;
    }

    injectNonMotionEvent(event);
}

void PlasmaScreencastV1Session::sendGlobalEvent(const std::shared_ptr<QEvent> &event)
{
    auto encodedStream = stream();
    if (!encodedStream || !encodedStream->isActive()) {
        return;
    }

    if (event->type() == QEvent::MouseMove) {
        // The position is already in KWin-global logical coordinates, so it
        // must NOT be normalised against this session's own output: fake
        // input's pointer_motion_absolute addresses the whole workspace, and
        // clamping here would pin the pointer to the captured output.
        auto me = std::static_pointer_cast<QMouseEvent>(event);
        const auto position = me->position();
        d->remoteInterface->pointer_motion_absolute(wl_fixed_from_double(position.x()), wl_fixed_from_double(position.y()));
        return;
    }

    injectNonMotionEvent(event);
}

// Buttons, wheel and keys carry no position, so they are identical for the
// output-local and the workspace-global entry points.
void PlasmaScreencastV1Session::injectNonMotionEvent(const std::shared_ptr<QEvent> &event)
{
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

QRect PlasmaScreencastV1Session::outputGeometry() const
{
    return d->logicalRect;
}

void PlasmaScreencastV1Session::setClipboardData(std::unique_ptr<QMimeData> data)
{
    Q_UNUSED(data);
}

void PlasmaScreencastV1Session::onPacketReceived(const PipeWireEncodedStream::Packet &data)
{
    // KPipeWire's encoded stream carries no per-frame damage, so every packet is
    // a full-frame update. Keep the multi-monitor layout for the RDPGFX reset.
    VideoFrame frameData;
    frameData.size = size();
    frameData.data = data.data();
    frameData.isKeyFrame = data.isKeyFrame();
    frameData.monitors = d->monitorLayout;
    frameData.monitorIndex = monitorIndex();
    frameData.damage = fullFrameDamage(frameData.size);

    if (frameData.monitors.isEmpty() && !frameData.size.isEmpty()) {
        frameData.monitors.push_back(VideoMonitor{
            .geometry = QRect(QPoint(0, 0), frameData.size),
            .primary = true,
        });
    }

    Q_EMIT frameReceived(frameData);
}

}
