// SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleix.pol_gonzalez@mercedes-benz.com>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "RelativePointerEvent.h"
#include "PlasmaScreencastV1Session.h"

#include "EncoderFailurePolicy.h"
#include "EncoderWatchdog.h"

#include <QGuiApplication>
#include <QMimeData>
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
#include <wayland-client-core.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>
#include <chrono>
#include <algorithm>
#include <optional>
#include <utility>

#include <KSystemClipboard>

#include "qwayland-fake-input.h"
#include "qwayland-wayland.h"
#include "screencasting_p.h"

#include "EncoderSelection.h"
#include "PressedInputTracker.h"
#include "ScreencastTarget.h"
#include "StreamRecoveryPolicy.h"
#include "StreamResumePolicy.h"
#include "VideoStream.h"
#include "WaylandRequestVersion.h"
#include "WorkspaceFrameGeometry.h"
#include "krdp_logging.h"

namespace KRdp
{

class FakeInput : public QWaylandClientExtensionTemplate<FakeInput>, public QtWayland::org_kde_kwin_fake_input
{
public:
    FakeInput()
        // Ask for the version that has the destructor request; Qt binds the
        // lower of this and what the compositor advertises, so an older KWin
        // still gets a v4 bind and teardown drops the proxy instead.
        : QWaylandClientExtensionTemplate<FakeInput>(ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION)
    {
        initialize();
        if (isActive()) {
            auto appId = qGuiApp->desktopFileName();
            if (appId.isEmpty()) {
                appId = QStringLiteral("io.github.westers.farside.server");
            }
            authenticate(appId, QStringLiteral("KRDP remote control"));
            if (!supports(ORG_KDE_KWIN_FAKE_INPUT_KEYBOARD_KEY_SINCE_VERSION)) {
                qCWarning(KRDP) << "org_kde_kwin_fake_input is bound at version" << boundVersion()
                                << "which predates keyboard_key; keyboard input is disabled";
            }
        }
    }

    ~FakeInput() override
    {
        // AUD-P2: the session owns this object. AUD-FIX F1: the destructor
        // request is since="5" in fake-input.xml; sending it on an older bind
        // is a protocol error that makes KWin disconnect krdpserver. On an
        // older bind just free the client-side proxy (the caller has already
        // sent the key/button releases and flushed).
        switch (WaylandRequestVersion::teardownFor(boundVersion(), ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION)) {
        case WaylandRequestVersion::Teardown::SendDestructor:
            destroy();
            break;
        case WaylandRequestVersion::Teardown::DropProxy:
            wl_proxy_destroy(reinterpret_cast<wl_proxy *>(object()));
            init(static_cast<struct ::org_kde_kwin_fake_input *>(nullptr));
            break;
        case WaylandRequestVersion::Teardown::None:
            break;
        }
    }

    uint32_t boundVersion() const
    {
        return object() ? wl_proxy_get_version(reinterpret_cast<wl_proxy *>(const_cast<struct ::org_kde_kwin_fake_input *>(object()))) : 0;
    }

    bool supports(uint32_t sinceVersion) const
    {
        return WaylandRequestVersion::supports(boundVersion(), sinceVersion);
    }

    Q_DISABLE_COPY_MOVE(FakeInput)
};

namespace
{
// Push queued requests (the releases sent on teardown) to the compositor now:
// a session destroyed at shutdown may not see another event-loop flush.
void flushWaylandDisplay()
{
    if (auto *native = qGuiApp ? qGuiApp->platformNativeInterface() : nullptr) {
        if (auto *display = static_cast<wl_display *>(native->nativeResourceForIntegration("wl_display"))) {
            wl_display_flush(display);
        }
    }
}
}

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

// Closed-stream recovery; see StreamRecoveryPolicy.h.
constexpr int MaxRecoveryAttempts = StreamRecoveryPolicy::MaxAttempts;
constexpr int RecoveryIntervalMs = StreamRecoveryPolicy::IntervalMs;
constexpr int RecoverySettleMs = StreamRecoveryPolicy::SettleMs;

// KPipeWire tears its produce thread down asynchronously after stop(): start()
// is a no-op until that thread is gone, and the node ID is cleared once it is.
// Poll for that before attaching a replacement node.
constexpr int StreamRestartPollMs = 10;
constexpr auto StreamRestartTimeout = std::chrono::milliseconds(5000);
// AUD-FIX12: how long a watchdog restart lets the wedged producer pause before stopping it again.
constexpr auto ForcedTeardownAfter = std::chrono::milliseconds(60);

// How long a requested virtual output may take to show up as a QScreen. The
// spike saw it within one screencast round trip; 5 s is generous.
constexpr int VirtualScreenTimeoutMs = 5000;
// KWin names the output it creates for a virtual monitor stream after the
// requested name, with this prefix (verified: "Virtual-1280x720@1").
const QLatin1String VirtualOutputPrefix("Virtual-");

QRegion fullFrameDamage(const QSize &size)
{
    if (size.isEmpty()) {
        return {};
    }
    return QRegion(QRect(QPoint(0, 0), size));
}

// A snapshot of Qt's screen list for ScreencastTarget::resolve(), plus the
// QScreen pointers at the same indices.
QList<ScreencastTarget::Screen> screenSnapshot(QList<QScreen *> *pointers = nullptr)
{
    QList<ScreencastTarget::Screen> result;
    const auto screens = qGuiApp->screens();
    const auto *primaryScreen = qGuiApp->primaryScreen();
    for (auto *screen : screens) {
        result.push_back(ScreencastTarget::Screen{.name = screen->name(), .geometry = screen->geometry(), .primary = screen == primaryScreen});
    }
    if (pointers) {
        *pointers = screens;
    }
    return result;
}

/**
 * The AVC444 chroma (aux) half of a packet, with a KPipeWire that has it (the private build);
 * stock KPipeWire's Packet has no aux(). A template, so the requires-expression is checked on
 * substitution instead of making a stock build ill-formed.
 */
template<typename Packet>
void copyAuxIfSupported(VideoFrame &frame, const Packet &packet)
{
    if constexpr (requires(const Packet &p) {
                      p.aux();
                      p.auxIsKey();
                  }) {
        frame.aux = packet.aux();
        frame.auxIsKeyFrame = packet.auxIsKey();
    }
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
    // Declared before pressedInput: its destructor releases through this.
    std::unique_ptr<FakeInput> remoteInterface;
    // The bound fake-input object, or null when the compositor withheld it (no grant): every request goes through this.
    FakeInput *fake() const
    {
        return remoteInterface && remoteInterface->isActive() ? remoteInterface.get() : nullptr;
    }
    bool reportedUnavailable = false;
    PressedInputTracker pressedInput{[this](PressedInputTracker::Kind kind, uint32_t code) {
        if (!remoteInterface || !remoteInterface->isActive()) {
            return;
        }
        if (kind == PressedInputTracker::Kind::Button) {
            remoteInterface->button(code, 0);
        } else {
            if (remoteInterface->supports(ORG_KDE_KWIN_FAKE_INPUT_KEYBOARD_KEY_SINCE_VERSION)) {
                remoteInterface->keyboard_key(code, 0);
            }
        }
    }};
    StreamTarget streamTarget = StreamTarget::None;
    QPointer<QScreen> outputScreen = nullptr;
    // The screen an Output target captures, and the stream index it was
    // resolved for; a recovery re-finds the screen by this name (AUD-P1).
    QString targetScreenName;
    int targetStreamIndex = -1;
    QRect logicalRect;
    QVector<VideoMonitor> monitorLayout;
    std::optional<double> workspaceFrameScaleHint;
    bool streamConfigured = false;
    bool streamSignalsConnected = false;
    bool startedSignalEmitted = false;
    QTimer recoveryTimer;
    int recoveryAttempt = 0;
    QTimer streamRestartTimer;
    quint64 resizeRestartEpoch = 0;
    // Virtual-monitor target only: the QScreen KWin created for our request.
    // Input is mapped through logicalRect, which for a virtual output is only
    // known once that screen exists (KWin places it, we do not).
    QString virtualScreenName;
    QPointer<QScreen> virtualScreen;
    bool virtualGeometryResolved = false;
    QMetaObject::Connection virtualScreenAddedConnection;
    QMetaObject::Connection virtualScreenRemovedConnection;
    QMetaObject::Connection virtualScreenGeometryConnection;
    QTimer virtualScreenTimer;
    bool loggedGatedInput = false;
    uint pendingNodeId = 0;
    // The node of the live screencast request: it outlives the consumer, which KPipeWire resets to node 0 on stop().
    uint screencastNodeId = 0;
    std::chrono::steady_clock::time_point streamRestartWaitStarted;
    // Latch so a session whose stream is down logs one line for the whole
    // inactive period instead of one per dropped event; cleared as soon as the
    // stream carries an event again.
    bool loggedInactiveGlobalEvents = false;
    // Guard the KSystemClipboard::changed handler against the server's own
    // write of client-originated data. KGuiAddons emits changed(Clipboard)
    // synchronously inside setMimeData() on this stack (verified in KGuiAddons
    // 6.24 waylandclipboard.cpp: DataControlDevice::setSelection emits
    // selectionChanged directly and the connection to changed() is a plain
    // direct connection), so a flag set across the write and cleared right
    // after drops that echo. lastClientText is the belt for any later
    // asynchronous changed that still carries the bytes we just wrote.
    bool writingClientClipboard = false;
    QString lastClientText;
    // AUD-FIX12: requests the encoder never answers restart it (EncoderWatchdog.h).
    EncoderWatchdog::Watchdog watchdog;
    // OPT-055 K4: what to do when KPipeWire reports the encoder dead (restart twice a minute, then close).
    EncoderFailurePolicy failurePolicy;
    QTimer watchdogTimer;
    int watchdogRestarts = 0;
    // A watchdog restart: the old producer is wedged and never finishes draining its queue on
    // its own, so it is told to stop a second time once it has paused (see pollEncoderWatchdog()).
    bool forceTeardown = false;
    bool forcedSecondStop = false;

    // AUD-FIX14: KWin 6.6 records a changed cursor only with its next move or repaint of the
    // output (ScreenCastStream::invalidateCursor only marks the bitmap stale). An application sets
    // its cursor (Konsole's I-beam: a cursor surface or a cursor-shape-v1 shape) only after it
    // heard of a move, i.e. after KWin recorded that move, so with the pointer at rest the new
    // shape would never reach the client. Once the injected pointer input pauses, the pointer is
    // moved by one wl_fixed step (1/256 logical px) and straight back, CursorSettleDelaysMs after
    // the last event: KWin records the cursor again, with the new bitmap, and the pointer ends
    // where the client put it.
    static constexpr int CursorSettleDelaysMs[] = {60, 300};
    QTimer cursorSettleTimer;
    int cursorSettleStep = 0;
    std::optional<QPointF> lastPointer; // KWin-global logical, as fake input got it
};

PlasmaScreencastV1Session::PlasmaScreencastV1Session()
    : AbstractSession()
    , d(std::make_unique<Private>())
{
    d->remoteInterface = std::make_unique<FakeInput>();
    if (!d->fake()) {
        // Not fatal: the session cannot inject input, and start() reports the missing screencast too.
        qCWarning(KRDP) << "The compositor did not grant org_kde_kwin_fake_input; check X-KDE-Wayland-Interfaces in the .desktop file. Remote input is disabled for this session";
    }

    connect(KSystemClipboard::instance(), &KSystemClipboard::changed, this, [this](auto mode) {
        // The clipboard is workspace-wide, but MonitorMode=multi runs one
        // session per RDPGFX surface and SessionWrapper connects every
        // session's clipboardDataChanged to the connection's cliprdr. Only
        // the surface-0 session announces, so one copy is one format list.
        if (monitorIndex() != 0) {
            return;
        }

        if (mode != QClipboard::Clipboard) {
            return;
        }

        // Do not announce the server's own write of the client's clipboard
        // back to the client: that echo, doubled by two data requests per
        // copy, was the announce storm (see side-clipboard-plasma). The flag
        // covers the synchronous changed emitted inside setClipboardData();
        // the content compare covers any later asynchronous one.
        if (d->writingClientClipboard) {
            return;
        }

        auto data = KSystemClipboard::instance()->mimeData(mode);
        if (!data) {
            return;
        }

        if (data->hasText() && data->text() == d->lastClientText) {
            qCDebug(KRDP) << "Skipping announce of clipboard change that echoes the last client write";
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

        qCDebug(KRDP) << "Announcing system clipboard change to the client, formats:" << formats;
        Q_EMIT clipboardDataChanged(newData);
    });

    d->cursorSettleTimer.setSingleShot(true);
    connect(&d->cursorSettleTimer, &QTimer::timeout, this, &PlasmaScreencastV1Session::settleCursor);

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
        const auto waited = std::chrono::steady_clock::now() - d->streamRestartWaitStarted;
        if (!tornDown && d->forceTeardown && !d->forcedSecondStop && waited >= ForcedTeardownAfter) {
            // AUD-FIX12: KPipeWire's producer waits for its queues to drain before it tears
            // down, and a wedged one (a dead filter graph) never drains. Once paused, a second
            // stop() destroys it at once (PipeWireProduce::deactivate on a paused stream).
            d->forcedSecondStop = true;
            qCInfo(KRDP) << "Monitor" << monitorIndex() << ": the wedged encoder did not drain; tearing it down";
            encodedStream->stop();
            return;
        }
        if (!tornDown && waited < StreamRestartTimeout) {
            return;
        }
        d->streamRestartTimer.stop();
        d->forceTeardown = false;
        d->forcedSecondStop = false;
        if (!tornDown) {
            if (d->resizeRestartEpoch) {
                // Do not label a still-running old encoder a new capture
                // epoch. The resize owner times out with input/frames gated.
                qCWarning(KRDP) << "Resize encoder teardown timed out; capture remains gated";
                const auto epoch = std::exchange(d->resizeRestartEpoch, quint64(0));
                Q_EMIT captureRestartFailed(epoch);
                return;
            }
            // OPT-055 K4.3: the old producer's thread is stuck (a driver call that never returns).
            // KPipeWire hands it to its process-wide list and returns the stream to Idle, so the new
            // attach starts a fresh producer instead of silently doing nothing or inheriting the
            // old thread's finished handler. A KPipeWire without the API keeps the old behaviour.
            const bool abandoned = abandonStreamProducer();
            qCWarning(KRDP) << "Encoded stream did not shut down within" << StreamRestartTimeout.count() << "ms,"
                            << (abandoned ? "abandoning its producer and" : "") << "attaching new node anyway";
        }
        if (d->resizeRestartEpoch) {
            const auto epoch = std::exchange(d->resizeRestartEpoch, quint64(0));
            // nodeId becomes zero only in KPipeWire's producer-thread finished
            // handler, after the old producer and its queued packets retire.
            // A starting producer's activeChanged(true) is NOT this boundary.
            const QPointer<PlasmaScreencastV1Session> alive(this);
            const uint nodeId = d->pendingNodeId;
            Q_EMIT captureRestartReady(epoch);
            if (!alive || d->resizeRestartEpoch || d->streamRestartTimer.isActive()
                || d->pendingNodeId != nodeId || !streamingRequested() || stream()->nodeId() != 0) return;
        }
        attachEncodedStream(d->pendingNodeId, true);
    });

    connect(this, &AbstractSession::encoderFailureReported, this, &PlasmaScreencastV1Session::onEncoderFailed);
    d->watchdogTimer.setInterval(EncoderWatchdog::Watchdog::TickInterval);
    connect(&d->watchdogTimer, &QTimer::timeout, this, &PlasmaScreencastV1Session::pollEncoderWatchdog);

    d->virtualScreenTimer.setSingleShot(true);
    connect(&d->virtualScreenTimer, &QTimer::timeout, this, [this]() {
        qCWarning(KRDP) << "Virtual output" << d->virtualScreenName << "did not appear within" << VirtualScreenTimeoutMs << "ms; input stays gated";
        Q_EMIT virtualOutputUnresolved();
    });
}

PlasmaScreencastV1Session::~PlasmaScreencastV1Session()
{
    qCInfo(KRDP) << "Closing Plasma Remote Session";
    // AUD-P2: a client that drops, or a rebuild that replaces this session,
    // must not leave a key or button held down in the compositor.
    if (!d->pressedInput.empty()) {
        qCInfo(KRDP) << "Releasing keys and buttons still held by the closing session";
        d->pressedInput.releaseAll();
        flushWaylandDisplay();
    }
    d->remoteInterface.reset();
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

bool PlasmaScreencastV1Session::restartCaptureForResize(quint64 epoch)
{
    auto encodedStream = stream();
    const uint nodeId = d->streamRestartTimer.isActive() ? d->pendingNodeId : encodedStream->nodeId();
    if (!epoch || !d->streamConfigured || !streamingRequested() || nodeId == 0) return false;
    d->resizeRestartEpoch = epoch;
    // stop() also queues deactivation for a producer which is still starting.
    // isActive() alone cannot distinguish that state from complete teardown.
    if (!encodedStream->isActive()) encodedStream->stop();
    restartEncodedStream(nodeId);
    return true;
}

void PlasmaScreencastV1Session::setWorkspaceFrameScaleHint(std::optional<double> scale)
{
    d->workspaceFrameScaleHint = scale && std::isfinite(*scale) && *scale >= 1 && *scale <= 4 ? scale : std::nullopt;
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
    const bool allowWorkspaceFallback = StreamRecoveryPolicy::allowWorkspaceFallback(attempt);

    bool recovered = false;
    if (screensAvailable) {
        qCInfo(KRDP) << "Attempting to recover display stream (attempt" << (attempt + 1) << "of" << MaxRecoveryAttempts << ", workspace fallback:" << allowWorkspaceFallback << ")";
        recovered = setupScreencastRequest(true, allowWorkspaceFallback);
    } else {
        qCInfo(KRDP) << "No screens available yet (attempt" << (attempt + 1) << "of" << MaxRecoveryAttempts << ")";
    }
    switch (StreamRecoveryPolicy::next(attempt, recovered)) {
    case StreamRecoveryPolicy::Next::Done:
        return;
    case StreamRecoveryPolicy::Next::Retry:
        qCInfo(KRDP) << "Retrying display stream recovery in" << RecoveryIntervalMs << "ms";
        scheduleStreamRecovery(attempt + 1, RecoveryIntervalMs);
        return;
    case StreamRecoveryPolicy::Next::GiveUp:
        // AUD-P4: do not keep a connection that streams nothing. error() makes
        // the controller close it with a reason the client shows (or, in
        // multi mode, drop just this monitor).
        qCWarning(KRDP) << "Display stream recovery failed after" << MaxRecoveryAttempts << "attempts; reporting the session as failed";
        Q_EMIT error();
        return;
    }
}

bool PlasmaScreencastV1Session::setupScreencastRequest(bool recovery, bool allowWorkspaceFallback)
{
    if (!d->m_screencasting.isAvailable()) {
        // The compositor withheld zkde_screencast_unstable_v1: nothing here can ever succeed, so end the
        // session (never the process) once, with the remedy.
        if (!std::exchange(d->reportedUnavailable, true)) {
            const QString reason = QStringLiteral("the compositor did not grant zkde_screencast_unstable_v1; check X-KDE-Wayland-Interfaces in the .desktop file of %1")
                                       .arg(qGuiApp && !qGuiApp->desktopFileName().isEmpty() ? qGuiApp->desktopFileName() : QCoreApplication::applicationName());
            qCWarning(KRDP) << "Screencast unavailable:" << reason;
            Q_EMIT captureUnavailable(reason);
        }
        d->recoveryTimer.stop();
        return false;
    }
    Private::StreamTarget target = Private::StreamTarget::Workspace;
    QPointer<QScreen> outputScreen = nullptr;
    QRect targetLogicalRect;
    QVector<VideoMonitor> targetMonitorLayout;
    if (virtualMonitor()) {
        target = Private::StreamTarget::Virtual;
        auto vm = virtualMonitor();
        targetLogicalRect = QRect(QPoint(0, 0), vm->size);
        targetMonitorLayout = {
            VideoMonitor{
                .geometry = targetLogicalRect,
                .primary = true,
            },
        };
    } else {
        // The captured screen and the rect input is mapped through come from
        // the same resolution (AUD-P1). A recovery holds on to the remembered
        // screen name; an explicit start/refresh follows the stream index.
        QList<QScreen *> screens;
        const auto snapshot = screenSnapshot(&screens);
        const auto resolution =
            ScreencastTarget::resolve(snapshot, activeStream(), d->targetScreenName, d->targetStreamIndex, recovery, allowWorkspaceFallback);
        if (resolution.kind == ScreencastTarget::Kind::Wait) {
            qCInfo(KRDP) << "Target screen" << resolution.name << "not available yet, waiting for it to return";
            return false;
        }
        if (resolution.kind == ScreencastTarget::Kind::Output) {
            target = Private::StreamTarget::Output;
            outputScreen = screens.at(resolution.screenIndex);
            d->targetScreenName = resolution.name;
            d->targetStreamIndex = activeStream();
        } else {
            if (recovery && !d->targetScreenName.isEmpty()) {
                qCWarning(KRDP) << "Target screen" << d->targetScreenName << "no longer available, falling back to workspace";
            }
            d->targetScreenName.clear();
            d->targetStreamIndex = -1;
        }
        targetLogicalRect = resolution.logicalRect;
        targetMonitorLayout = resolution.monitors;
    }

    const bool targetChanged = (d->streamTarget != target);
    const bool outputChanged = (target == Private::StreamTarget::Output) && (d->outputScreen != outputScreen);
    // A virtual output's logicalRect is KWin's placement of it, not the
    // provisional (0,0) rect computed here, so it must not trip a recreate.
    const bool logicalRectChanged = (target != Private::StreamTarget::Virtual) && (d->logicalRect != targetLogicalRect);
    const bool requiresRecreate = !d->request || targetChanged || outputChanged || logicalRectChanged;
    if (!(target == Private::StreamTarget::Virtual && d->virtualGeometryResolved)) {
        // A resolved virtual session keeps the real rect across a no-op refresh.
        d->logicalRect = targetLogicalRect;
        d->monitorLayout = targetMonitorLayout;
    }
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
        d->screencastNodeId = 0;
    }

    if (target == Private::StreamTarget::Virtual) {
        auto vm = virtualMonitor();
        d->request = d->m_screencasting.createVirtualMonitorStream(vm->name, vm->size, vm->dpr, Screencasting::Metadata);
        qCInfo(KRDP) << "Using virtual monitor stream" << vm->name << "logical rect" << d->logicalRect;
        watchForVirtualScreen();
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
            const auto workspace = ScreencastTarget::resolve(screenSnapshot(), -1, {}, -1, false, true);
            d->targetScreenName.clear();
            d->targetStreamIndex = -1;
            d->logicalRect = workspace.logicalRect;
            d->monitorLayout = workspace.monitors;
            if (!d->logicalRect.isEmpty()) {
                setLogicalSize(d->logicalRect.size());
            }
        } else {
            qCInfo(KRDP) << "Using output stream index" << activeStream() << "screen" << outputScreen->name() << "logical rect" << d->logicalRect;
        }
    }

    if (!d->request && target == Private::StreamTarget::Workspace) {
        d->request = d->m_screencasting.createWorkspaceStream(Screencasting::Metadata);
        qCInfo(KRDP) << "Using workspace stream logical rect" << d->logicalRect;
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
        d->screencastNodeId = 0;
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
    d->screencastNodeId = nodeId;

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
    d->watchdog.keyFrameRequested(std::chrono::steady_clock::now());
    startEncoderWatchdog();
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

void PlasmaScreencastV1Session::restartStreamForCodecChange()
{
    auto encodedStream = stream();
    const uint nodeId = encodedStream->nodeId();
    if (!d->streamConfigured || nodeId == 0 || !streamingRequested()) {
        return;
    }
    if (d->streamRestartTimer.isActive()) {
        // Already restarting; it picks the new mode up.
        return;
    }
    encoderReconfigured();
    qCInfo(KRDP) << "Restarting encoded stream on node" << nodeId << "for the codec change";
    restartEncodedStream(nodeId); // setChromaMode() is applied at the next start(); the new stream opens with an IDR
}

void PlasmaScreencastV1Session::resumeStreaming()
{
    auto encodedStream = stream();
    const auto state = [&] {
        switch (encodedStream->state()) {
        case PipeWireBaseEncodedStream::Recording:
            return StreamResumePolicy::State::Recording;
        case PipeWireBaseEncodedStream::Rendering:
            return StreamResumePolicy::State::Rendering;
        default:
            return StreamResumePolicy::State::Idle;
        }
    }();
    const auto decision = StreamResumePolicy::decide(encodedStream->nodeId(), state, d->streamRestartTimer.isActive(), d->screencastNodeId, d->request != nullptr);
    switch (decision.action) {
    case StreamResumePolicy::Action::None:
        return;
    case StreamResumePolicy::Action::Start:
        encodedStream->start();
        return;
    case StreamResumePolicy::Action::Reattach:
        // start() on a stopped stream is a silent no-op (no node, or the old producer is still exiting):
        // the deferred restart waits for the producer to end, then sets the node and starts.
        qCInfo(KRDP) << "Resuming the stopped encoded stream on node" << decision.node;
        restartEncodedStream(decision.node);
        return;
    case StreamResumePolicy::Action::Recreate:
        qCWarning(KRDP) << "Resuming a stopped stream whose screencast is gone: creating a new one";
        start(); // reports error() when it cannot
        return;
    }
}

void PlasmaScreencastV1Session::attachEncodedStream(uint nodeId, bool streamWasActive)
{
    auto encodedStream = stream();
    const bool shouldResumeStreaming = d->streamConfigured && (streamWasActive || streamingRequested());

    encodedStream->setNodeId(nodeId);
    encodedStream->setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed);
    // setEncoder() must happen before start(), including a deferred restart after a negotiated
    // private-codec change; KPipeWire keeps it as the next produce's encoder choice. apply() also
    // sets the backend policy (before setEncoder()) and the codec's colour range: full for AVC,
    // limited for HEVC/AV1 (see EncoderSelection::colorRangeFor()).
    const auto hardware = encoderHardware();
    const bool encoderMatches = EncoderSelection::apply(encodedStream, videoCodec(), hardware);
    qCInfo(KRDP) << "Using PipeWire encoder for" << VideoCodecSupport::codecName(videoCodec()) << ':' << int(encodedStream->encoder()) << "backend"
                  << (hardware ? (*hardware ? "hardware" : "software") : "default");
    if (!encoderMatches) {
        // Never label one codec's bytes with another's id: have the connection move off it now,
        // before start() (direct connection), then the stream restarts with the new codec.
        qCCritical(KRDP) << "Codec mismatch: the encoder cannot produce" << VideoCodecSupport::codecName(videoCodec())
                         << "(encoder" << int(encodedStream->encoder()) << "); falling back";
        Q_EMIT encoderUnavailable(videoCodec());
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
        qCInfo(KRDP) << "Started Plasma session";
        d->startedSignalEmitted = true;
        setStarted(true);
    } else {
        qCInfo(KRDP) << "Re-attached Plasma screencast stream on node" << nodeId;
    }
}

void PlasmaScreencastV1Session::sendEvent(const std::shared_ptr<QEvent> &event)
{
    if (!d->fake()) {
        return; // no fake-input grant: warned once at construction
    }
    auto encodedStream = stream();
    if (!encodedStream || !encodedStream->isActive()) {
        return;
    }

    if (virtualMonitor() && !d->virtualGeometryResolved && event->type() == QEvent::MouseMove) {
        // Until KWin tells us where the virtual output sits, a pointer position
        // would be mapped onto (0,0) - Steve's real primary monitor. Drop it.
        if (!d->loggedGatedInput) {
            d->loggedGatedInput = true;
            qCInfo(KRDP) << "Dropping pointer motion until the virtual output geometry is known";
        }
        return;
    }

    if (event->type() == RelativePointerEvent::EventType) {
        const auto relative = std::static_pointer_cast<RelativePointerEvent>(event);
        // Cursor-shape refresh nudges are absolute warps: they break games' pointer locks.
        d->cursorSettleTimer.stop();
        d->lastPointer.reset();
        if (relative->action == QEvent::MouseMove)
            d->remoteInterface->pointer_motion(wl_fixed_from_double(relative->delta.x()), wl_fixed_from_double(relative->delta.y()));
        else
            injectNonMotionEvent(relative->nonMotionEvent());
        return;
    }

    if (event->type() == QEvent::MouseMove) {
        // The position is relative to this session's own captured output, in
        // capture pixels; mapToGlobal() normalises it and maps it onto the
        // output's place (d->logicalRect, via outputGeometry()) in the
        // KWin-global logical coordinate space fake input expects.
        auto me = std::static_pointer_cast<QMouseEvent>(event);
        if (size().isEmpty() || logicalSize().isEmpty()) {
            return;
        }
        const QPointF logicalPosition = mapToGlobal(me->position());
        if (d->remoteInterface->supports(ORG_KDE_KWIN_FAKE_INPUT_POINTER_MOTION_ABSOLUTE_SINCE_VERSION)) {
            d->remoteInterface->pointer_motion_absolute(wl_fixed_from_double(logicalPosition.x()), wl_fixed_from_double(logicalPosition.y()));
            d->lastPointer = logicalPosition;
            armCursorSettle();
        }
        return;
    }

    injectNonMotionEvent(event);
}

void PlasmaScreencastV1Session::sendGlobalEvent(const std::shared_ptr<QEvent> &event)
{
    if (!d->fake()) {
        return; // no fake-input grant: warned once at construction
    }
    auto encodedStream = stream();
    if (!encodedStream || !encodedStream->isActive()) {
        // Fake input would work here - it addresses the whole workspace, not
        // this session's output - but an inactive stream means this session is
        // being recovered, and injecting through it is not what the caller
        // asked for. The caller picks a session with a live stream
        // (SessionWrapper::inputSession()); this is what a period with none
        // looks like, and it is silent input, so say so once.
        if (!d->loggedInactiveGlobalEvents) {
            d->loggedInactiveGlobalEvents = true;
            qCDebug(KRDP) << "Dropping input: this session's encoded stream is not active";
        }
        return;
    }
    d->loggedInactiveGlobalEvents = false;

    if (event->type() == RelativePointerEvent::EventType) {
        const auto relative = std::static_pointer_cast<RelativePointerEvent>(event);
        // Cursor-shape refresh nudges are absolute warps: they break games' pointer locks.
        d->cursorSettleTimer.stop();
        d->lastPointer.reset();
        if (relative->action == QEvent::MouseMove)
            d->remoteInterface->pointer_motion(wl_fixed_from_double(relative->delta.x()), wl_fixed_from_double(relative->delta.y()));
        else
            injectNonMotionEvent(relative->nonMotionEvent());
        return;
    }

    if (event->type() == QEvent::MouseMove) {
        // The position is already in KWin-global logical coordinates, so it
        // must NOT be normalised against this session's own output: fake
        // input's pointer_motion_absolute addresses the whole workspace, and
        // clamping here would pin the pointer to the captured output.
        auto me = std::static_pointer_cast<QMouseEvent>(event);
        const auto position = me->position();
        // Debug-gated. In multi mode this is the only place the workspace-global
        // pointer position can be checked against the monitor the click was
        // meant for, so it is what tells a seam bug from a capture bug.
        qCDebug(KRDP) << "Global pointer motion to" << position << "(workspace logical)";
        if (d->remoteInterface->supports(ORG_KDE_KWIN_FAKE_INPUT_POINTER_MOTION_ABSOLUTE_SINCE_VERSION)) {
            d->remoteInterface->pointer_motion_absolute(wl_fixed_from_double(position.x()), wl_fixed_from_double(position.y()));
            d->lastPointer = position;
            armCursorSettle();
        }
        return;
    }

    injectNonMotionEvent(event);
}

void PlasmaScreencastV1Session::armCursorSettle()
{
    // Keys do not arm it: a nudge is a pointer motion, and an application that hides its pointer
    // while one types (Konsole can) would show it again.
    if (!d->lastPointer) {
        return;
    }
    d->cursorSettleStep = 0;
    d->cursorSettleTimer.start(Private::CursorSettleDelaysMs[0]);
}

void PlasmaScreencastV1Session::settleCursor()
{
    auto encodedStream = stream();
    if (!d->lastPointer || !encodedStream || !encodedStream->isActive() || !d->remoteInterface->isActive()
        || !d->remoteInterface->supports(ORG_KDE_KWIN_FAKE_INPUT_POINTER_MOTION_ABSOLUTE_SINCE_VERSION)) {
        return;
    }
    const wl_fixed_t x = wl_fixed_from_double(d->lastPointer->x());
    const wl_fixed_t y = wl_fixed_from_double(d->lastPointer->y());
    // Right, not left: a pointer on the workspace's left edge would be clamped and not move. A
    // client's rightmost pixel maps to at most width - 1 logical px, so this stays on the output.
    d->remoteInterface->pointer_motion_absolute(x + 1, y);
    d->remoteInterface->pointer_motion_absolute(x, y);
    if (++d->cursorSettleStep < int(std::size(Private::CursorSettleDelaysMs))) {
        d->cursorSettleTimer.start(Private::CursorSettleDelaysMs[d->cursorSettleStep]);
    }
}

// Buttons, wheel and keys carry no position, so they are identical for the
// output-local and the workspace-global entry points.
void PlasmaScreencastV1Session::injectNonMotionEvent(const std::shared_ptr<QEvent> &event)
{
    if (!d->fake()) {
        return; // no fake-input grant: warned once at construction
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
        d->pressedInput.button(button, state);
        armCursorSettle(); // a click changes cursors too (a drag, a busy application)
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
        armCursorSettle(); // scrolling moves content, and another cursor, under the pointer
        break;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        if (!d->remoteInterface->supports(ORG_KDE_KWIN_FAKE_INPUT_KEYBOARD_KEY_SINCE_VERSION)) {
            return; // warned once when the interface was bound
        }
        auto ke = std::static_pointer_cast<QKeyEvent>(event);
        auto state = ke->type() == QEvent::KeyPress ? 1 : 0;

        if (ke->nativeScanCode()) {
            d->remoteInterface->keyboard_key(ke->nativeScanCode(), state);
            d->pressedInput.key(ke->nativeScanCode(), state);
        } else {
            auto keycode = Xkb::self()->keycodeFromKeysym(ke->nativeVirtualKey());
            if (!keycode) {
                qCWarning(KRDP) << "Failed to convert keysym into keycode" << ke->nativeVirtualKey();
                return;
            }

            auto sendKey = [this, state](int keycode) {
                d->remoteInterface->keyboard_key(keycode, state);
                d->pressedInput.key(keycode, state);
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

bool PlasmaScreencastV1Session::outputGeometryResolved() const
{
    return !virtualMonitor() || d->virtualGeometryResolved;
}

void PlasmaScreencastV1Session::watchForVirtualScreen()
{
    const auto vm = virtualMonitor();
    if (!vm) {
        return;
    }
    d->virtualScreenName = VirtualOutputPrefix + vm->name;
    d->virtualGeometryResolved = false;
    d->virtualScreen = nullptr;
    disconnect(d->virtualScreenAddedConnection);
    disconnect(d->virtualScreenRemovedConnection);
    disconnect(d->virtualScreenGeometryConnection);

    d->virtualScreenRemovedConnection = connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
        if (screen != d->virtualScreen) {
            return;
        }
        // KWin dropped our output (stream closed, or a stale same-name output
        // going away before the new one arrives). Wait for it to come back.
        qCInfo(KRDP) << "Virtual output" << d->virtualScreenName << "removed; waiting for it to reappear";
        d->virtualScreen = nullptr;
        d->virtualGeometryResolved = false;
        d->virtualScreenTimer.start(VirtualScreenTimeoutMs);
    });
    d->virtualScreenAddedConnection = connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
        adoptVirtualScreen(screen);
    });

    for (auto *screen : qGuiApp->screens()) {
        if (adoptVirtualScreen(screen)) {
            return;
        }
    }
    d->virtualScreenTimer.start(VirtualScreenTimeoutMs);
}

bool PlasmaScreencastV1Session::adoptVirtualScreen(QScreen *screen)
{
    if (!screen || screen->name() != d->virtualScreenName || d->virtualScreen == screen) {
        return false;
    }
    d->virtualScreenTimer.stop();
    d->virtualScreen = screen;
    d->virtualGeometryResolved = true;
    d->loggedGatedInput = false;
    disconnect(d->virtualScreenGeometryConnection);
    d->virtualScreenGeometryConnection = connect(screen, &QScreen::geometryChanged, this, [this, screen](const QRect &geometry) {
        if (screen == d->virtualScreen) {
            updateVirtualGeometry(geometry);
        }
    });
    updateVirtualGeometry(screen->geometry(), true);
    return true;
}

void PlasmaScreencastV1Session::updateVirtualGeometry(const QRect &geometry, bool adopted)
{
    // An adopted screen is always announced, even at the provisional rect:
    // KWin replays a remembered arrangement for a known output set, so the
    // output can well appear exactly where setupScreencastRequest() guessed,
    // and the controller waits for this to learn that it is resolved.
    if (geometry.isEmpty() || (!adopted && d->logicalRect == geometry)) {
        return;
    }
    // logicalRect is KWin-global (input mapping); the monitor layout handed
    // to the RDP side stays local to this output, as for a physical one.
    // The logical size follows KWin's placement too: at a scale other than 1
    // it is the requested pixel size divided by the scale, and sendEvent()
    // spans pointer positions over it.
    d->logicalRect = geometry;
    setLogicalSize(geometry.size());
    d->monitorLayout = {
        VideoMonitor{
            .geometry = QRect(QPoint(0, 0), geometry.size()),
            .primary = true,
        },
    };
    qCInfo(KRDP) << "Virtual output" << d->virtualScreenName << "resolved at" << geometry;
    Q_EMIT outputGeometryChanged(geometry);
}

void PlasmaScreencastV1Session::setClipboardData(std::unique_ptr<QMimeData> data)
{
    // Remember what the client sent so the changed handler can drop the echo
    // this write triggers (see the KSystemClipboard::changed connection).
    d->lastClientText = data && data->hasText() ? data->text() : QString();

    d->writingClientClipboard = true;
    // KSystemClipboard takes ownership
    if (data) {
        KSystemClipboard::instance()->setMimeData(data.release(), QClipboard::Clipboard);
    } else {
        KSystemClipboard::instance()->clear(QClipboard::Clipboard);
    }
    d->writingClientClipboard = false;
}

void PlasmaScreencastV1Session::onPacketReceived(const PipeWireEncodedStream::Packet &data)
{
    // KWin can resize an existing single-output workspace stream in place.
    // PipeWire updates size(), but no new screencast request is created, so
    // the request-time logical geometry otherwise stays stale indefinitely.
    // Publish new coordinates only once the captured pixel size agrees with
    // the current output; leave ambiguous multi-output transitions gated.
    const auto screens = qGuiApp->screens();
    if (d->streamTarget == Private::StreamTarget::Workspace && screens.size() == 1) {
        const auto *screen = screens.first();
        const QRect geometry = screen->geometry();
        const bool matchingPixels = d->workspaceFrameScaleHint
            ? WorkspaceFrameGeometry::matches(size(), geometry.size(), *d->workspaceFrameScaleHint)
            : geometry.size() * screen->devicePixelRatio() == size();
        if (matchingPixels && d->logicalRect != geometry) {
            d->logicalRect = geometry;
            d->monitorLayout = ScreencastTarget::resolve(screenSnapshot(), -1, {}, -1, false, true).monitors;
            setLogicalSize(geometry.size());
            Q_EMIT outputGeometryChanged(geometry);
        }
    }
    // KPipeWire's encoded stream carries no per-frame damage, so every packet is
    // a full-frame update. Keep the multi-monitor layout for the RDPGFX reset.
    VideoFrame frameData;
    frameData.size = size();
    frameData.data = data.data();
    frameData.isKeyFrame = data.isKeyFrame();
    copyAuxIfSupported(frameData, data);
    frameData.codec = EncoderSelection::producedCodec(stream(), videoCodec());
    frameData.monitors = d->monitorLayout;
    frameData.monitorIndex = monitorIndex();
    frameData.damage = fullFrameDamage(frameData.size);

    if (frameData.monitors.isEmpty() && !frameData.size.isEmpty()) {
        frameData.monitors.push_back(VideoMonitor{
            .geometry = QRect(QPoint(0, 0), frameData.size),
            .primary = true,
        });
    }

    d->watchdog.packet(std::chrono::steady_clock::now(), frameData.isKeyFrame);
    Q_EMIT frameReceived(frameData);
}

void PlasmaScreencastV1Session::encoderReconfigured()
{
    d->watchdog.arm(std::chrono::steady_clock::now());
    startEncoderWatchdog();
}

void PlasmaScreencastV1Session::startEncoderWatchdog()
{
    if (!d->watchdogTimer.isActive()) {
        d->watchdogTimer.start();
    }
}

void PlasmaScreencastV1Session::onEncoderFailed(const QString &reason)
{
    // The latched encoder is dead; whatever the keyframe watchdog was waiting for is moot (E44).
    d->watchdog.reset();
    switch (d->failurePolicy.onFailure(std::chrono::steady_clock::now(), d->streamRestartTimer.isActive(), streamingRequested())) {
    case EncoderFailurePolicy::Action::Ignore:
        qCDebug(KRDP) << "Monitor" << monitorIndex() << ": ignoring encoder failure while restarting or not streaming:" << reason;
        return;
    case EncoderFailurePolicy::Action::Restart: {
        const uint nodeId = stream()->nodeId();
        qCWarning(KRDP) << "Monitor" << monitorIndex() << ": the encoder failed (" << reason << "); restarting it";
        if (nodeId == 0) {
            return;
        }
        d->forceTeardown = true;
        d->forcedSecondStop = false;
        restartEncodedStream(nodeId);
        return;
    }
    case EncoderFailurePolicy::Action::Close:
        qCWarning(KRDP) << "Monitor" << monitorIndex() << ": the encoder failed again (" << reason << "); closing the session";
        Q_EMIT encoderGaveUp(reason);
        Q_EMIT error();
        return;
    }
}

void PlasmaScreencastV1Session::pollEncoderWatchdog()
{
    const auto now = std::chrono::steady_clock::now();
    auto encodedStream = stream();
    const uint nodeId = encodedStream->nodeId();
    const bool running = d->streamConfigured && nodeId != 0 && streamingRequested();
    if (!running || d->streamRestartTimer.isActive()) {
        // Stopped: nothing is owed. Restarting: the new stream opens with an IDR.
        if (!running) {
            d->watchdog.reset();
        }
        if (!d->watchdog.needsTimer(now)) {
            d->watchdogTimer.stop();
        }
        return;
    }
    switch (d->watchdog.poll(now)) {
    case EncoderWatchdog::Action::None:
        break;
    case EncoderWatchdog::Action::Probe:
        qCDebug(KRDP) << "Monitor" << monitorIndex() << "went silent after a reconfiguration; asking the encoder for a keyframe";
        requestKeyFrameIfSupported(encodedStream);
        break;
    case EncoderWatchdog::Action::Retry:
        qCDebug(KRDP) << "Monitor" << monitorIndex() << ": no keyframe yet; asking the encoder again";
        requestKeyFrameIfSupported(encodedStream);
        break;
    case EncoderWatchdog::Action::Restart:
        ++d->watchdogRestarts;
        qCWarning(KRDP) << "Monitor" << monitorIndex() << ": the encoder answered no keyframe request within"
                        << std::chrono::duration_cast<std::chrono::milliseconds>(EncoderWatchdog::RestartAfter).count()
                        << "ms; restarting its encoder (restart" << d->watchdogRestarts << "of this session)";
        Q_EMIT encoderWatchdogRestarted();
        d->forceTeardown = true;
        d->forcedSecondStop = false;
        restartEncodedStream(nodeId);
        break;
    case EncoderWatchdog::Action::GiveUp:
        qCWarning(KRDP) << "Monitor" << monitorIndex() << ": the restarted encoder produced no keyframe either; waiting for the next request";
        break;
    }
    if (!d->watchdog.needsTimer(now)) {
        d->watchdogTimer.stop();
    }
}

}
