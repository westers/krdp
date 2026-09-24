// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>
#include <unistd.h>

#include <QAction>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFile>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>

#include <PlasmaScreencastV1Session.h>
#include <KGlobalAccel>

#include "ConsoleWorkerWire.h"
#include "ConsoleInputState.h"
#include "ConsoleResizeSession.h"
#include "VirtualResizeSession.h"
#include "H264KeyframeSize.h"
#include "TakeoverDetector.h"
#include "PipeWireAudioPlayback.h"
#include "ConsoleMicrophoneSession.h"
#include "CaptureWorkerMode.h"
#include "RetainedMultiCapture.h"
#include "RetainedMultiInput.h"
#include "RetainedKScreenReadback.h"
#include "ConsoleTopologyReadback.h"
#include "ConsoleTopologyPlan.h"
#include "ConsoleTopologyLease.h"
#include "RetainedMultiResizePlan.h"
#include "RetainedMultiPositionPlan.h"
#include "RetainedMultiFitPlan.h"
#include "RetainedMultiPrimaryPlan.h"
#include "RetainedMultiMixedPlan.h"
#include "RetainedMultiMixedCreatePlan.h"

using namespace KRdp;

namespace
{
std::shared_ptr<QEvent> eventFor(const ConsoleWorkerWire::Input &input)
{
    switch (input.type) {
    case ConsoleWorkerWire::Input::Type::Mouse:
        if (input.eventType != QEvent::MouseMove && input.eventType != QEvent::MouseButtonPress && input.eventType != QEvent::MouseButtonRelease) {
            return {};
        }
        return std::make_shared<QMouseEvent>(input.eventType, input.position, QPointF{}, input.button, input.buttons, Qt::NoModifier);
    case ConsoleWorkerWire::Input::Type::Wheel:
        if (input.eventType != QEvent::Wheel) {
            return {};
        }
        return std::make_shared<QWheelEvent>(input.position, QPointF{}, QPoint{}, input.angleDelta, input.buttons, Qt::NoModifier, Qt::NoScrollPhase, false);
    case ConsoleWorkerWire::Input::Type::Key:
        if (input.eventType != QEvent::KeyPress && input.eventType != QEvent::KeyRelease) {
            return {};
        }
        return std::make_shared<QKeyEvent>(input.eventType, 0, Qt::NoModifier, input.nativeScanCode, input.nativeVirtualKey, 0, input.text);
    }
    return {};
}

class Worker : public QObject
{
    struct PendingPosition {
        quint64 requestId = 0;
        quint64 generation = 0;
        bool batch = false;
        QVector<RetainedKScreenReadback::Placement> targets;
    };
public:
    Worker(const QString &socketName, const CaptureWorkerMode &mode, quint32 uid, const QByteArray &token, bool desktop, QObject *parent = nullptr)
        : QObject(parent)
        , m_socketName(socketName)
        , m_sessionId(mode.sessionId)
        , m_uid(uid)
        , m_token(token)
        , m_mode(mode)
        , m_authenticatedDesktop(desktop)
        , m_microphone(desktop)
    {
        connect(&m_microphone, &ConsoleMicrophoneSession::result, this, [this](const auto &result) {
            if (!m_stopping && m_socket.state() == QLocalSocket::ConnectedState)
                m_socket.write(ConsoleWorkerWire::frame(result));
        });
        m_clock.start();
        connect(&m_resize, &ConsoleResizeSession::mutationStarting, this, [this]() {
            releaseInput();
            m_lastPhysicalKeyframe.reset();
            m_takeover.outputMoved(m_clock.elapsed());
            if (m_multiMode) {
                m_multiReady = false;
                m_multiCapture.invalidate();
                m_multiPublishedFrames.clear();
                m_multiResizeNeedsRestart = true;
            }
        });
        connect(&m_resize, &ConsoleResizeSession::keyframeNeeded, this, [this] {
            if (m_multiMode) {
                m_physicalResizeReadyForCapture = true;
                releaseInput();
                m_multiReady = false;
                m_multiCapture.invalidate();
                m_multiPublishedFrames.clear();
                m_multiResizeNeedsRestart = true;
                m_multiSettle.start(0); // A mode change can leave logical geometry unchanged.
            } else m_session.requestKeyFrame();
        });
        connect(&m_resize, &ConsoleResizeSession::result, this, [this](const auto &result) {
            m_physicalResizeReadyForCapture = false;
            if (m_socket.state() == QLocalSocket::ConnectedState) {
                m_socket.write(ConsoleWorkerWire::frame(result));
            }
        });
        connect(&m_virtualResize, &VirtualResizeSession::mutationStarting, this, &Worker::releaseInput);
        connect(&m_virtualResize, &VirtualResizeSession::keyframeNeeded, &m_session, &AbstractSession::requestKeyFrame);
        connect(&m_virtualResize, &VirtualResizeSession::captureRefreshNeeded, this, [this](quint64 epoch) {
            m_virtualCaptureEpoch = epoch;
            m_session.setWorkspaceFrameScaleHint(m_virtualResize.outputScale());
            if (!m_session.restartCaptureForResize(epoch)) m_virtualResize.captureRestartFailed(epoch);
        });
        connect(&m_session, &PlasmaScreencastV1Session::captureRestartReady, &m_virtualResize, &VirtualResizeSession::captureRestartReady);
        connect(&m_session, &PlasmaScreencastV1Session::captureRestartFailed, &m_virtualResize, &VirtualResizeSession::captureRestartFailed);
        connect(&m_virtualResize, &VirtualResizeSession::result, this, [this](const auto &result) {
            if (m_socket.state() == QLocalSocket::ConnectedState) m_socket.write(ConsoleWorkerWire::frame(result));
            if (m_mode.virtualSession) m_multiSettle.start();
        });
        const auto resizedStopped = [this](const QString &error) {
            if (!error.isEmpty()) {
                qWarning().noquote() << "Worker output recovery:" << error;
            }
            m_session.setStreamingEnabled(false);
            for (const auto &session : m_multiSessions) session->setStreamingEnabled(false);
            QCoreApplication::exit(error.isEmpty() ? m_exitCode : 1);
        };
        connect(&m_resize, &ConsoleResizeSession::stopped, this, resizedStopped);
        connect(&m_virtualResize, &VirtualResizeSession::stopped, this, resizedStopped);
        // Use Plasma's shortcut service, never a raw keyboard grab. SDDM need
        // not provide it; do not auto-start desktop services in the greeter.
        if (m_mode.physicalActions() && QDBusConnection::sessionBus().interface()
            && QDBusConnection::sessionBus().interface()->isServiceRegistered(QStringLiteral("org.kde.kglobalaccel"))) {
            m_reclaimAction.setText(QStringLiteral("Reclaim physical console"));
            m_reclaimAction.setObjectName(QStringLiteral("reclaim-console"));
            m_reclaimAction.setProperty("componentName", QStringLiteral("krdp-console-worker"));
            m_reclaimAction.setProperty("componentDisplayName", QStringLiteral("KRDP Physical Console"));
            m_reclaimAction.setEnabled(false);
            connect(&m_reclaimAction, &QAction::triggered, this, &Worker::reclaimConsole);
            const QKeySequence shortcut(Qt::META | Qt::CTRL | Qt::ALT | Qt::Key_T);
            KGlobalAccel::self()->setDefaultShortcut(&m_reclaimAction, {shortcut});
            KGlobalAccel::self()->setShortcut(&m_reclaimAction, {shortcut});
        }
        connect(&m_session, &AbstractSession::cursorUpdate, this, [this](const PipeWireCursor &cursor) {
            if (m_mode.physicalActions() && m_control.active && !m_resize.changing() && m_session.outputGeometryResolved()
                && m_takeover.observed(m_session.mapToGlobal(cursor.position).toPoint(), m_clock.elapsed())) {
                reclaimConsole();
            }
        });
        connect(&m_session, &AbstractSession::outputGeometryChanged, this, [this](const QRect &) {
            m_takeover.outputMoved(m_clock.elapsed());
        });
        connect(&m_socket, &QLocalSocket::connected, this, [this]() {
            m_connectTimeout.stop();
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{m_sessionId, m_uid, m_token}));
            startCapture();
        });
        connect(&m_socket, &QLocalSocket::readyRead, this, &Worker::readBroker);
        connect(&m_socket, &QLocalSocket::disconnected, this, [this]() {
            shutdown(1);
        });
        connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
            if (m_socket.state() == QLocalSocket::UnconnectedState) {
                shutdown(1);
            }
        });
        m_positionDeadline.setSingleShot(true);
        m_positionDeadline.setInterval(12000);
        connect(&m_positionDeadline, &QTimer::timeout, this, [this] {
            if (!m_positionPending) return;
            finishPosition(QStringLiteral("post-position capture timed out"));
            m_socket.disconnectFromServer(); // No stale video/input after an unverified mutation.
        });
        m_multiResizeDeadline.setSingleShot(true);
        m_multiResizeDeadline.setInterval(15000);
        connect(&m_multiResizeDeadline, &QTimer::timeout, this, [this] {
            if (!m_multiResizePending) return;
            finishMultiResize(QStringLiteral("post-resize capture/readback timed out"));
            m_socket.disconnectFromServer();
        });
        m_multiFitDeadline.setSingleShot(true);
        m_multiFitDeadline.setInterval(15000);
        connect(&m_multiFitDeadline, &QTimer::timeout, this, [this] {
            if (!m_multiFitPending) return;
            finishMultiFit(QStringLiteral("post-Fit capture/readback timed out"));
            m_socket.disconnectFromServer();
        });
        m_primaryDeadline.setSingleShot(true);
        m_primaryDeadline.setInterval(15000);
        connect(&m_primaryDeadline, &QTimer::timeout, this, [this] {
            if (!m_primaryPending) return;
            finishPrimary(QStringLiteral("post-primary capture/readback timed out"));
            m_socket.disconnectFromServer();
        });
        m_mixedDeadline.setSingleShot(true);
        m_mixedDeadline.setInterval(15000);
        connect(&m_mixedDeadline, &QTimer::timeout, this, [this] {
            if (!m_mixedPending) return;
            finishMixed(QStringLiteral("post-mixed-layout capture/readback timed out"));
            m_socket.disconnectFromServer();
        });
        m_mixedCreateDeadline.setSingleShot(true);
        m_mixedCreateDeadline.setInterval(30000); // Creator, KScreen apply, then three or more fresh captures.
        connect(&m_mixedCreateDeadline, &QTimer::timeout, this, [this] {
            if (!m_mixedCreatePending) return;
            finishMixedCreate(QStringLiteral("mixed creation/capture timed out"));
            m_socket.disconnectFromServer();
        });
        m_physicalDeadline.setSingleShot(true);
        m_physicalDeadline.setInterval(15000);
        connect(&m_physicalDeadline, &QTimer::timeout, this, [this] {
            if (m_physicalPending) failPhysical(QStringLiteral("physical layout capture timed out"));
        });
        connect(&m_session, &PlasmaScreencastV1Session::captureRestartFailed, this, [this](quint64 epoch) {
            if (m_physicalPending && epoch == m_physicalCaptureEpoch)
                failPhysical(QStringLiteral("physical layout capture restart failed"));
        });
        connect(&m_session, &PlasmaScreencastV1Session::captureRestartReady, this, [this](quint64 epoch) {
            if (m_physicalPending && epoch == m_physicalCaptureEpoch)
                m_physicalCaptureRestartReady = true;
        });
        m_addDeadline.setSingleShot(true);
        m_addDeadline.setInterval(15000);
        connect(&m_addDeadline, &QTimer::timeout, this, [this] {
            if (!m_addPending) return;
            finishAdd(QStringLiteral("virtual output creation/readback timed out"));
            m_socket.disconnectFromServer(); // The creator is released as the worker exits.
        });
        m_removeDeadline.setSingleShot(true);
        m_removeDeadline.setInterval(15000);
        connect(&m_removeDeadline, &QTimer::timeout, this, [this] {
            if (!m_removePending) return;
            finishRemove(QStringLiteral("virtual output removal/readback timed out"));
            m_socket.disconnectFromServer();
        });
        connect(&m_session, &AbstractSession::streamActiveChanged, this, [this](bool active) {
            if (m_mode.virtualSession) m_virtualResize.captureStateChanged(m_virtualCaptureEpoch, active);
            if (active && !m_captureReady) {
                m_captureReady = true;
                m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
            }
        });
        connect(&m_session, &AbstractSession::frameReceived, this, [this](const VideoFrame &frame) {
            if (m_captureReady && !m_multiMode) {
                // Use the same ordering and coordinates as the captured frame,
                // not a root-side guess about the user's monitor setup.
                const auto screens = qGuiApp->screens();
                ConsoleWorkerWire::Outputs outputs;
                QRect workspace;
                for (const auto *screen : screens) {
                    workspace = workspace.united(screen->geometry());
                }
                if (m_mode.virtualSession && !screens.isEmpty()) outputs.compositorOrigin = workspace.topLeft();
                if (screens.size() == frame.monitors.size()) {
                    for (qsizetype i = 0; i < screens.size(); ++i) {
                        if (screens[i]->geometry().translated(-workspace.topLeft()) != frame.monitors[i].geometry) {
                            outputs.monitors.clear();
                            break; // Wait until capture and screen discovery agree after hotplug.
                        }
                        double scale = screens[i]->devicePixelRatio();
                        if (m_mode.virtualSession) {
                            const auto inferred = VirtualResize::frameScale(frame.size, frame.monitors[i].geometry.size());
                            if (!inferred) {
                                outputs.monitors.clear();
                                break; // A frame with inconsistent pixel/logical axes cannot prove Fit.
                            }
                            const auto verified = m_virtualResize.outputScale();
                            // A completed Fit carries KScreen's exact scale. For an
                            // independently changed output, use the ratio that agrees
                            // with this frame's pixels and logical rectangle.
                            scale = verified && VirtualResize::geometryMatchesScale(frame.size, frame.monitors[i].geometry.size(), *verified)
                                ? *verified : *inferred;
                        }
                        outputs.monitors.append({screens[i]->name(), frame.monitors[i].geometry,
                                                 scale, frame.monitors[i].primary});
                    }
                }
                if (outputs.monitors.isEmpty()) m_lastPhysicalKeyframe.reset();
                if (m_mode.virtualSession) {
                    const QSize payloadPixels = m_virtualResize.changing() && frame.isKeyFrame
                        ? h264KeyframeSize(frame.data).value_or(QSize{}) : QSize{};
                    if (m_virtualResize.changing() && frame.isKeyFrame)
                        qInfo() << "Virtual Fit keyframe epoch" << m_virtualCaptureEpoch
                                << "payload" << payloadPixels << "metadata" << frame.size
                                << "screen" << (screens.isEmpty() ? QRect{} : screens.first()->geometry())
                                << "frame monitor" << (frame.monitors.isEmpty() ? QRect{} : frame.monitors.first().geometry)
                                << "outputs" << outputs.monitors.size()
                                << "scale" << (outputs.monitors.isEmpty() ? 0.0 : outputs.monitors.first().scale)
                                << "target scale" << m_virtualResize.outputScale().value_or(0.0);
                    m_virtualResize.captured(outputs, frame.size, payloadPixels, frame.isKeyFrame, m_virtualCaptureEpoch);
                    // A failed Fit may report its error while a rollback producer
                    // still needs the exact fractional scale for its recovery frame.
                    if (!m_virtualResize.changing()) m_session.setWorkspaceFrameScaleHint(std::nullopt);
                    if (!m_virtualResize.framesAllowed()) return; // Never forward stale-size encoded packets during Fit/recovery.
                }
                if (!m_mode.virtualSession && m_physicalPending) {
                    if (!m_physicalCaptureRestartReady || !frame.isKeyFrame || outputs.monitors.isEmpty()) return;
                    const auto json = readKScreenJson();
                    const auto kscreen = json ? RetainedKScreenReadback::parse(*json, m_sessionId) : std::nullopt;
                    if (!json || !kscreen || !ConsoleTopologyReadback::confirmed(*kscreen, outputs, frame)
                        || !m_physicalPlan || !m_physicalBefore || !m_physicalSelected
                        || !ConsoleTopologyPlan::matchesApplied(*m_physicalPlan, *m_physicalBefore,
                            *m_physicalSelected, *json)
                        || !m_control.active || m_control.generation != m_physicalPending->controlGeneration) {
                        failPhysical(QStringLiteral("physical layout differs after captured readback"));
                        return;
                    }
                    finishPhysical({});
                }
                if (!outputs.monitors.isEmpty() && outputs != m_outputs) {
                    m_outputs = outputs;
                    m_lastPhysicalKeyframe.reset();
                    m_socket.write(ConsoleWorkerWire::frame(outputs));
                }
                if (m_mode.virtualSession && outputs.monitors.size() == 1) {
                    // Capture/input use KWin's logical output rectangle, but
                    // RDPGFX monitor coordinates describe the encoded pixel
                    // surface. At fractional scale those are different axes.
                    VideoFrame rdpFrame = frame;
                    rdpFrame.monitors = {VideoMonitor{.geometry = QRect(QPoint(0, 0), frame.size), .primary = true}};
                    m_socket.write(ConsoleWorkerWire::frame(rdpFrame));
                } else {
                    m_socket.write(ConsoleWorkerWire::frame(frame));
                }
                // Only this frame's validated output geometry may complete Fit,
                // never metadata cached before a resize or a compositor handoff.
                if (!m_mode.virtualSession) {
                    m_resize.captured(outputs, frame.isKeyFrame);
                    if (frame.isKeyFrame && !m_resize.changing() && !outputs.monitors.isEmpty()) {
                        m_lastPhysicalKeyframe = frame;
                        if (m_topologyQueryPending) {
                            m_topologyQueryPending = false;
                            const auto topology = physicalTopology(outputs, frame);
                            // Empty explicitly retires an earlier inventory when
                            // fresh compositor readback and capture disagree.
                            m_socket.write(ConsoleWorkerWire::frame(topology.value_or(ConsoleWorkerWire::Topology{})));
                        }
                    }
                }
            }
        });
        connect(&m_session, &AbstractSession::error, this, [this]() {
            if (m_multiMode) return; // The per-output producers now own capture.
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Error, QByteArrayLiteral("screencast failed")));
            m_socket.disconnectFromServer();
        });
        m_multiSettle.setSingleShot(true);
        m_multiSettle.setInterval(400);
        connect(&m_multiSettle, &QTimer::timeout, this, &Worker::syncCaptureMode);
        connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, [this] { m_multiSettle.start(); });
        connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
            watchScreen(screen);
            m_multiSettle.start();
        });
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
            m_watchedScreens.remove(screen);
            m_multiSettle.start();
        });
        m_connectTimeout.setSingleShot(true);
        connect(&m_connectTimeout, &QTimer::timeout, qApp, []() { QCoreApplication::exit(1); });
        m_audioTimer.setInterval(20);
        connect(&m_audioTimer, &QTimer::timeout, this, [this]() {
            if (!m_audio || !m_captureReady) {
                return;
            }
            const QByteArray pcm = m_audio->take();
            if (!pcm.isEmpty()) {
                m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Audio{pcm}));
            }
        });
    }

    void connectToBroker()
    {
        m_connectTimeout.start(10000);
        m_socket.connectToServer(m_socketName);
    }

private:
    void shutdown(int code)
    {
        if (m_stopping) {
            return;
        }
        m_stopping = true;
        m_exitCode = code;
        if (m_physicalPending) failPhysical(QStringLiteral("physical Console worker stopped during layout change"));
        releasePhysicalLease();
        releaseInput();
        m_audioTimer.stop();
        m_audio.reset();
        m_microphone.stop();
        // Keep the event loop alive until in-flight mutations settle. Successful
        // virtual geometry is retained; only an unfinished Fit may roll back.
        if (m_mode.virtualSession) m_virtualResize.stop();
        else m_resize.stop();
    }

    PlasmaScreencastV1Session *multiInputSession() const
    {
        for (const auto &session : m_multiSessions) {
            if (session->streamActive()) return session.get();
        }
        return nullptr;
    }

    void releaseInput()
    {
        const auto releases = m_inputState.releaseAll();
        if (!releases.isEmpty()) {
            qInfo() << "Worker releasing" << releases.size() << "held console input(s)";
        }
        for (const auto &input : releases) {
            if (const auto event = eventFor(input)) {
                if (m_multiMode) {
                    if (auto *session = multiInputSession()) session->sendGlobalEvent(event);
                } else m_session.sendEvent(event);
            }
        }
    }

    void startCapture()
    {
        m_session.setActiveStream(-1); // Capture this compositor's complete workspace.
        m_session.setVideoCodec(VideoCodec::Avc420);
        // Match the desktop server's quality baseline. Leaving this unset
        // selects libx264's CRF35 fallback, visibly damaging desktop text.
        m_session.setVideoQuality(80);
        for (auto *screen : qGuiApp->screens()) watchScreen(screen);
        if (qGuiApp->screens().size() > 1) {
            syncCaptureMode(); // Each output gets its own encoder and independently checked keyframe.
        } else {
            m_session.setStreamingEnabled(true);
            m_multiSettle.start();
        }
    }

    void watchScreen(QScreen *screen)
    {
        if (!screen || m_watchedScreens.contains(screen)) return;
        m_watchedScreens.insert(screen);
        connect(screen, &QScreen::geometryChanged, this, [this] { m_multiSettle.start(); });
        connect(screen, &QObject::destroyed, this, [this, screen] { m_watchedScreens.remove(screen); });
    }

    void syncCaptureMode()
    {
        if (m_stopping) return;
        const auto screens = qGuiApp->screens();
        for (auto *screen : screens) watchScreen(screen);
        if (!m_mode.virtualSession && m_multiMode && m_resize.changing() && !m_physicalResizeReadyForCapture) {
            m_multiSettle.start(200); // Wait for the physical KScreen executor's readback before recapture.
            return;
        }
        if (m_removePending) {
            if (!m_control.active || m_control.generation != m_removePending->generation) {
                finishRemove(QStringLiteral("virtual output authority changed during removal"));
                m_socket.disconnectFromServer();
                return;
            }
            const auto current = readKScreen();
            if (!current || current->outputs.size() == m_removeBefore.outputs.size()) {
                m_multiSettle.start(200); // KWin has not retired the output yet.
                return;
            }
            if (!removeMatches(*current)) {
                finishRemove(QStringLiteral("removing the output changed the remaining compositor layout"));
                m_socket.disconnectFromServer();
                return;
            }
            const bool screensAgree = screens.size() == current->outputs.size()
                && std::all_of(current->outputs.cbegin(), current->outputs.cend(), [&screens](const auto &output) {
                    return std::any_of(screens.cbegin(), screens.cend(), [&output](const auto *screen) {
                        return screen->name() == output.backendKey && screen->geometry() == output.logicalGeometry;
                    });
                });
            if (!screensAgree) { m_multiSettle.start(200); return; }
        }
        if (m_mixedCreatePending && !m_mixedCreateApplied) {
            if (!m_control.active || m_control.generation != m_mixedCreatePending->generation) {
                finishMixedCreate(QStringLiteral("mixed creation authority changed"));
                m_socket.disconnectFromServer();
                return;
            }
            const auto json = readKScreenJson();
            const auto current = json ? RetainedKScreenReadback::parse(*json, m_sessionId) : std::nullopt;
            if (!current || current->outputs.size() == m_mixedCreatePlan->before.outputs.size()) {
                m_multiSettle.start(200);
                return;
            }
            if (!RetainedMultiMixedCreatePlan::matchesCreated(*m_mixedCreatePlan, *current)
                || !applyMixedCreateAfterCreation(*current, *json)) {
                finishMixedCreate(QStringLiteral("mixed created output or apply differs from preview"));
                m_socket.disconnectFromServer();
                return;
            }
            m_multiSettle.start(200);
            return;
        }
        if (m_addPending) {
            if (!m_control.active || m_control.generation != m_addPending->generation) {
                finishAdd(QStringLiteral("virtual output authority changed during creation"));
                m_socket.disconnectFromServer();
                return;
            }
            const auto current = readKScreen();
            if (!current || current->outputs.size() == m_addBefore.outputs.size()) {
                m_multiSettle.start(200); // KWin has not exposed the new output yet.
                return;
            }
            if (!addMatches(*current, false)) {
                finishAdd(QStringLiteral("virtual output changed the existing compositor layout"));
                m_socket.disconnectFromServer();
                return;
            }
            if (!m_addPlaced) {
                const auto &request = *m_addPending;
                const auto created = std::find_if(current->outputs.cbegin(), current->outputs.cend(), [&request](const auto &output) {
                    return output.backendKey == request.output;
                });
                if (created == current->outputs.cend()) {
                    finishAdd(QStringLiteral("new virtual output was not found"));
                    m_socket.disconnectFromServer();
                    return;
                }
                if (created->logicalGeometry.topLeft() != request.globalLogical) {
                    const auto arguments = RetainedKScreenReadback::positionArguments(*current,
                        {{request.output, request.globalLogical}});
                    if (!arguments) {
                        finishAdd(QStringLiteral("invalid new-output position"));
                        m_socket.disconnectFromServer();
                        return;
                    }
                    QProcess command;
                    command.start(QStringLiteral("kscreen-doctor"), *arguments);
                    const bool finished = command.waitForStarted(1000) && command.waitForFinished(3000);
                    if (!finished) { command.kill(); command.waitForFinished(1000); }
                    const auto positioned = readKScreen();
                    if (!finished || command.exitStatus() != QProcess::NormalExit || command.exitCode() != 0
                        || !positioned || !addMatches(*positioned, true)) {
                        finishAdd(QStringLiteral("new virtual output position did not land"));
                        m_socket.disconnectFromServer();
                        return;
                    }
                }
                m_addPlaced = true;
            }
            if (!addMatches(*current, true)) {
                m_multiSettle.start(200); // KScreen accepted a move; QScreen may still be catching up.
                return;
            }
            const bool screensAgree = std::all_of(current->outputs.cbegin(), current->outputs.cend(), [&screens](const auto &output) {
                return std::any_of(screens.cbegin(), screens.cend(), [&output](const auto *screen) {
                    return screen->name() == output.backendKey && screen->geometry() == output.logicalGeometry;
                });
            });
            if (!screensAgree) { m_multiSettle.start(200); return; }
        }
        if (screens.size() < 2) {
            if (m_mixedCreatePending) {
                finishMixedCreate(QStringLiteral("multi-output capture disappeared during mixed creation"));
                m_socket.disconnectFromServer();
                return;
            }
            if (m_mixedPending) {
                finishMixed(QStringLiteral("multi-output capture disappeared during mixed layout"));
                m_socket.disconnectFromServer();
                return;
            }
            if (m_primaryPending) {
                finishPrimary(QStringLiteral("multi-output capture disappeared during primary selection"));
                m_socket.disconnectFromServer();
                return;
            }
            if (m_multiFitPending) {
                finishMultiFit(QStringLiteral("multi-output capture disappeared during Fit"));
                m_socket.disconnectFromServer();
                return;
            }
            if (m_multiResizePending) {
                finishMultiResize(QStringLiteral("multi-output capture disappeared during resize"));
                m_socket.disconnectFromServer();
                return;
            }
            if (m_multiMode) {
                releaseInput();
                ++m_multiEpoch;
                m_multiReady = false;
                m_multiMode = false;
                m_multiSessions.clear();
                m_multiCapture.invalidate();
                m_multiPublishedFrames.clear();
                m_wireAtlas.clear();
                m_logicalOutputs.clear();
                m_outputs = {};
                m_lastPhysicalKeyframe.reset();
                m_session.setStreamingEnabled(true);
            }
            return;
        }
        if (m_virtualResize.changing()) {
            m_multiSettle.start(500);
            return; // Do not replace the verified single-output Fit producer mid-transaction.
        }
        QVector<RetainedMultiCapture::Screen> inventory;
        inventory.reserve(screens.size());
        const auto *primary = qGuiApp->primaryScreen() ? qGuiApp->primaryScreen() : screens.first();
        for (const auto *screen : screens) inventory.append({screen->name(), screen->geometry(), screen == primary});
        if (m_multiResizePending && m_multiResizePlan) {
            const bool screensAgree = std::all_of(m_multiResizePlan->after.cbegin(), m_multiResizePlan->after.cend(), [&screens](const auto &output) {
                return std::any_of(screens.cbegin(), screens.cend(), [&output](const auto *screen) {
                    return screen->name() == output.backendKey && screen->geometry() == output.logicalGeometry;
                });
            });
            if (!screensAgree) { m_multiSettle.start(200); return; }
        }
        if (m_multiFitPending && m_multiFitPlan) {
            const bool screensAgree = std::all_of(m_multiFitPlan->after.cbegin(), m_multiFitPlan->after.cend(), [&screens](const auto &output) {
                return std::any_of(screens.cbegin(), screens.cend(), [&output](const auto *screen) {
                    return screen->name() == output.backendKey && screen->geometry() == output.logicalGeometry;
                });
            });
            if (!screensAgree) { m_multiSettle.start(200); return; }
        }
        if (m_primaryPending && m_primaryPlan) {
            const bool screensAgree = std::all_of(m_primaryPlan->after.cbegin(), m_primaryPlan->after.cend(),
                [&screens, primary](const auto &output) {
                    return std::any_of(screens.cbegin(), screens.cend(), [&output, primary](const auto *screen) {
                        return screen->name() == output.backendKey && screen->geometry() == output.logicalGeometry
                            && (screen == primary) == output.primary;
                    });
                });
            if (!screensAgree) { m_multiSettle.start(200); return; }
        }
        if (m_mixedPending && m_mixedPlan) {
            const bool screensAgree = screens.size() == m_mixedPlan->after.size()
                && std::all_of(m_mixedPlan->after.cbegin(), m_mixedPlan->after.cend(),
                    [&screens, primary](const auto &output) {
                        return std::any_of(screens.cbegin(), screens.cend(), [&output, primary](const auto *screen) {
                            return screen->name() == output.backendKey && screen->geometry() == output.logicalGeometry
                                && (screen == primary) == output.primary;
                        });
                    });
            if (!screensAgree) { m_multiSettle.start(200); return; }
        }
        if (m_mixedCreatePending && m_mixedCreateApplied) {
            const bool screensAgree = screens.size() == m_mixedCreateApplied->after.size()
                && std::all_of(m_mixedCreateApplied->after.cbegin(), m_mixedCreateApplied->after.cend(),
                    [&screens, primary](const auto &output) {
                        return std::any_of(screens.cbegin(), screens.cend(), [&output, primary](const auto *screen) {
                            return screen->name() == output.backendKey && screen->geometry() == output.logicalGeometry
                                && (screen == primary) == output.primary;
                        });
                    });
            if (!screensAgree) { m_multiSettle.start(200); return; }
        }
        if (m_multiMode && m_multiCapture.screens() == inventory && !m_multiResizeNeedsRestart) return;
        m_multiResizeNeedsRestart = false;
        releaseInput();
        ++m_multiEpoch;
        m_multiReady = false;
        m_multiMode = true;
        m_lastPhysicalKeyframe.reset();
        m_multiSessions.clear();
        m_multiPublishedFrames.clear();
        m_wireAtlas.clear();
        m_logicalOutputs.clear();
        QRect workspace;
        for (const auto *screen : screens) workspace |= screen->geometry();
        m_workspaceOrigin = workspace.topLeft();
        if (!m_multiCapture.configure(inventory)) {
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Error, QByteArrayLiteral("unsupported retained monitor arrangement")));
            m_socket.disconnectFromServer();
            return;
        }
        const auto epoch = m_multiEpoch;
        for (qsizetype i = 0; i < screens.size(); ++i) {
            auto session = std::make_unique<PlasmaScreencastV1Session>();
            session->setParent(this);
            session->setActiveStream(int(i));
            session->setMonitorIndex(int(i));
            session->setVideoCodec(VideoCodec::Avc420);
            session->setVideoQuality(m_multiQuality);
            connect(session.get(), &AbstractSession::frameReceived, this, [this, i, epoch](const VideoFrame &frame) {
                if (m_multiMode && epoch == m_multiEpoch) onMultiFrame(i, frame);
            });
            connect(session.get(), &AbstractSession::error, this, [this, epoch] {
                if (epoch != m_multiEpoch || m_stopping) return;
                m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Error, QByteArrayLiteral("per-output capture failed")));
                m_socket.disconnectFromServer();
            });
            if (!m_mode.virtualSession) {
                auto *producer = session.get();
                connect(producer, &AbstractSession::cursorUpdate, this, [this, producer](const PipeWireCursor &cursor) {
                    if (m_mode.physicalActions() && m_control.active && !m_resize.changing()
                        && producer->outputGeometryResolved()
                        && m_takeover.observed(producer->mapToGlobal(cursor.position).toPoint(), m_clock.elapsed()))
                        reclaimConsole();
                });
                connect(producer, &AbstractSession::outputGeometryChanged, this, [this](const QRect &) {
                    m_takeover.outputMoved(m_clock.elapsed());
                });
            }
            m_multiSessions.push_back(std::move(session));
        }
        for (const auto &session : m_multiSessions) session->setStreamingEnabled(true);
    }

    void onMultiFrame(qsizetype index, const VideoFrame &frame)
    {
        const auto result = m_multiCapture.submit(index, frame);
        if (result.reset) {
            releaseInput();
            m_multiReady = false;
            m_multiPublishedFrames.clear();
            for (const auto &session : m_multiSessions) session->requestKeyFrame();
        }
        if (result.becameReady) {
            // QScreen and encoded packets agree, but neither is an independent
            // compositor configuration readback. Query KScreen from THIS
            // private worker runtime before publishing the inventory. A failed
            // query/mismatch must not masquerade as topology success.
            const auto kscreenJson = readKScreenJson();
            const auto kscreen = kscreenJson
                ? RetainedKScreenReadback::parse(*kscreenJson, m_sessionId) : std::nullopt;
            if (!kscreen || !RetainedKScreenReadback::matchesPublished(*kscreen, result.outputs, result.frames)) {
                qWarning() << "KScreen readback did not match all captured outputs";
                if (m_physicalPending) {
                    failPhysical(QStringLiteral("physical layout differs from independent captured outputs"));
                    return;
                }
                if (m_mixedCreatePending)
                    finishMixedCreate(QStringLiteral("mixed-created capture differs from compositor readback"));
                m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Error,
                    QByteArrayLiteral("retained compositor readback/capture mismatch")));
                m_socket.disconnectFromServer();
                return;
            }
            if (!m_mode.virtualSession) {
                m_resize.captured(result.outputs, true);
                if (m_resize.changing()) {
                    // An intermediate frame must not be shown at stale RDP
                    // coordinates while the legacy resize is unresolved.
                    return;
                }
                if (m_physicalPending) {
                    if (!kscreenJson || !m_physicalPlan || !m_physicalBefore || !m_physicalSelected
                        || !ConsoleTopologyPlan::matchesApplied(*m_physicalPlan, *m_physicalBefore,
                            *m_physicalSelected, *kscreenJson)
                        || !m_control.active || m_control.generation != m_physicalPending->controlGeneration) {
                        failPhysical(QStringLiteral("physical layout differs after per-output captured readback"));
                        return;
                    }
                    finishPhysical({});
                }
            }
            if (m_addPending && (!m_addPlaced || !addMatches(*kscreen, true)
                || !m_control.active || m_control.generation != m_addPending->generation)) {
                finishAdd(QStringLiteral("created output differs from preview after captured readback"));
                m_socket.disconnectFromServer();
                return;
            }
            if (m_removePending && (!removeMatches(*kscreen) || !m_control.active
                || m_control.generation != m_removePending->generation)) {
                finishRemove(QStringLiteral("remaining outputs differ from preview after captured readback"));
                m_socket.disconnectFromServer();
                return;
            }
            if (m_multiResizePending && (!m_multiResizePlan || !RetainedMultiResizePlan::matches(*m_multiResizePlan, *kscreen)
                || !m_control.active || m_control.generation != m_multiResizePending->generation)) {
                finishMultiResize(QStringLiteral("resized output or peers differ after captured readback"));
                m_socket.disconnectFromServer();
                return;
            }
            if (m_multiFitPending && (!m_multiFitPlan || !RetainedMultiFitPlan::matches(*m_multiFitPlan, *kscreen)
                || !m_control.active || m_control.generation != m_multiFitPending->generation)) {
                finishMultiFit(QStringLiteral("Fit output or dependents differ after captured readback"));
                m_socket.disconnectFromServer();
                return;
            }
            if (m_primaryPending && (!m_primaryPlan || !RetainedMultiPrimaryPlan::matches(*m_primaryPlan, *kscreen)
                || !kscreenJson || RetainedMultiPrimaryPlan::priorities(*kscreenJson, *kscreen) != m_primaryPlan->requested
                || !m_control.active || m_control.generation != m_primaryPending->generation)) {
                finishPrimary(QStringLiteral("primary or peer outputs differ after captured readback"));
                m_socket.disconnectFromServer();
                return;
            }
            if (m_mixedPending) {
                const auto priorities = kscreenJson
                    ? RetainedMultiPrimaryPlan::priorities(*kscreenJson, *kscreen) : std::nullopt;
                if (!m_mixedPlan || !priorities
                    || !RetainedMultiMixedPlan::matches(*m_mixedPlan, *kscreen, *priorities)
                    || !m_control.active || m_control.generation != m_mixedPending->generation) {
                    finishMixed(QStringLiteral("mixed layout or peers differ after captured readback"));
                    m_socket.disconnectFromServer();
                    return;
                }
            }
            if (m_mixedCreatePending) {
                const auto priorities = kscreenJson
                    ? RetainedMultiPrimaryPlan::priorities(*kscreenJson, *kscreen) : std::nullopt;
                if (!m_mixedCreateApplied || !priorities
                    || !RetainedMultiMixedPlan::matches(*m_mixedCreateApplied, *kscreen, *priorities)
                    || !m_control.active || m_control.generation != m_mixedCreatePending->generation) {
                    finishMixedCreate(QStringLiteral("mixed-created layout differs after captured readback"));
                    m_socket.disconnectFromServer();
                    return;
                }
            }
            if (m_positionPending && (!m_positionPlan || !RetainedMultiPositionPlan::matches(*m_positionPlan, *kscreen)
                || !m_control.active || m_control.generation != m_positionPending->generation)) {
                finishPosition(QStringLiteral("positioned output or peers differ after captured readback"));
                m_socket.disconnectFromServer();
                return;
            }
            qInfo() << "Retained KScreen readback confirmed" << kscreen->outputs.size() << "independent captured outputs";
            m_outputs = result.outputs;
            m_multiPublishedFrames = result.frames;
            m_wireAtlas = result.atlas;
            m_logicalOutputs.clear();
            for (qsizetype i = 0; i < result.outputs.monitors.size(); ++i) {
                const auto &output = result.outputs.monitors[i];
                m_logicalOutputs.append({output.geometry.topLeft(), result.frames[i].size, output.scale, output.primary});
            }
            m_multiReady = true;
            if (!m_captureReady) {
                m_captureReady = true;
                m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
            }
            m_socket.write(ConsoleWorkerWire::frame(result.outputs));
            m_session.setStreamingEnabled(false); // No oversized workspace encoder in multi mode.
            if (!m_mode.virtualSession && m_topologyQueryPending) {
                m_topologyQueryPending = false;
                const auto topology = physicalTopology(result.outputs, result.frames);
                m_socket.write(ConsoleWorkerWire::frame(topology.value_or(ConsoleWorkerWire::Topology{})));
            }
        }
        if (!m_multiReady) return;
        for (const auto &packet : result.frames) m_socket.write(ConsoleWorkerWire::frame(packet));
        if (result.becameReady && m_positionPending) {
            const auto request = *m_positionPending;
            if (!m_control.active || m_control.generation != request.generation) {
                finishPosition(QStringLiteral("position changed or authority lost during capture"));
            } else finishPosition({});
        }
        if (result.becameReady && m_addPending) finishAdd({});
        if (result.becameReady && m_removePending) finishRemove({});
        if (result.becameReady && m_multiResizePending) finishMultiResize({});
        if (result.becameReady && m_multiFitPending) finishMultiFit({});
        if (result.becameReady && m_primaryPending) finishPrimary({});
        if (result.becameReady && m_mixedPending) finishMixed({});
        if (result.becameReady && m_mixedCreatePending) finishMixedCreate({});
    }

    std::optional<QByteArray> readKScreenJson() const
    {
        QProcess readback;
        readback.start(QStringLiteral("kscreen-doctor"), {QStringLiteral("-j")});
        const bool finished = readback.waitForStarted(1000) && readback.waitForFinished(3000);
        if (!finished) {
            readback.kill();
            readback.waitForFinished(1000);
        }
        if (!finished || readback.exitStatus() != QProcess::NormalExit || readback.exitCode() != 0) return {};
        const auto json = readback.readAllStandardOutput();
        return json.size() <= 1024 * 1024 ? std::optional<QByteArray>(json) : std::nullopt;
    }

    std::optional<RetainedKScreenReadback::Snapshot> readKScreen() const
    {
        const auto json = readKScreenJson();
        return json ? RetainedKScreenReadback::parse(*json, m_sessionId) : std::nullopt;
    }

    std::optional<ConsoleWorkerWire::Topology> physicalTopology(const ConsoleWorkerWire::Outputs &outputs,
        const VideoFrame &frame) const
    {
        const auto json = readKScreenJson();
        const auto kscreen = json ? RetainedKScreenReadback::parse(*json, m_sessionId) : std::nullopt;
        const auto captured = kscreen ? ConsoleTopologyReadback::confirmed(*kscreen, outputs, frame) : std::nullopt;
        return captured ? ConsoleTopologyReadback::withPriorities(*captured, *json, *kscreen) : std::nullopt;
    }

    std::optional<ConsoleWorkerWire::Topology> physicalTopology(const ConsoleWorkerWire::Outputs &outputs,
        const QVector<VideoFrame> &frames) const
    {
        const auto json = readKScreenJson();
        const auto kscreen = json ? RetainedKScreenReadback::parse(*json, m_sessionId) : std::nullopt;
        const auto captured = kscreen ? ConsoleTopologyReadback::confirmedMulti(*kscreen, outputs, frames) : std::nullopt;
        return captured ? ConsoleTopologyReadback::withPriorities(*captured, *json, *kscreen) : std::nullopt;
    }

    void finishPhysical(const QString &error)
    {
        if (!m_physicalPending) return;
        const auto request = *m_physicalPending;
        if (error.isEmpty() && m_physicalCandidate) {
            m_physicalLease = std::move(m_physicalCandidate);
            m_physicalLeaseGeneration = request.controlGeneration;
        }
        m_physicalPending.reset();
        m_physicalCandidate.reset();
        m_physicalPlan.reset();
        m_physicalBefore.reset();
        m_physicalSelected.reset();
        m_physicalDeadline.stop();
        m_physicalCaptureRestartReady = false;
        m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PhysicalLayoutResult{
            request.requestId, request.controlGeneration, error}));
    }

    void failPhysical(const QString &reason)
    {
        if (!m_physicalPending) return;
        releaseInput();
        m_multiReady = false;
        m_lastPhysicalKeyframe.reset();
        QString error = reason;
        if (m_physicalPlan && m_physicalBefore && m_physicalSelected) {
            const auto current = readKScreenJson();
            const auto restore = current ? ConsoleTopologyPlan::recoveryArguments(*m_physicalPlan,
                *m_physicalBefore, *m_physicalSelected, *current) : std::nullopt;
            if (restore) {
                if (!restore->isEmpty()) runKScreenCommand(*restore);
                const auto verified = readKScreenJson();
                error += verified && ConsoleTopologyPlan::recoveryVerified(*m_physicalPlan,
                    *m_physicalBefore, *m_physicalSelected, *current, *verified)
                    ? QStringLiteral("; owned fields reconciled")
                    : QStringLiteral("; recovery not fully verified");
            } else {
                error += QStringLiteral("; recovery unavailable");
            }
        }
        finishPhysical(error);
        m_socket.disconnectFromServer(); // Never show stale video or accept stale input after failure.
    }

    bool releasePhysicalLease()
    {
        if (!m_physicalLease) return true;
        releaseInput();
        m_multiReady = false;
        m_lastPhysicalKeyframe.reset();
        m_multiPublishedFrames.clear();
        const auto current = readKScreenJson();
        const auto restore = current ? ConsoleTopologyPlan::recoveryArguments(m_physicalLease->cumulative,
            m_physicalLease->original, m_physicalLease->selected, *current) : std::nullopt;
        if (restore && !restore->isEmpty()) runKScreenCommand(*restore);
        const auto verified = restore ? readKScreenJson() : std::nullopt;
        const bool okay = current && verified && ConsoleTopologyPlan::recoveryVerified(
            m_physicalLease->cumulative, m_physicalLease->original, m_physicalLease->selected,
            *current, *verified);
        qInfo() << "Physical Console lease released; conditional KScreen reconciliation verified:" << okay;
        if (m_socket.state() == QLocalSocket::ConnectedState) {
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PhysicalLeaseReleased{
                m_physicalLeaseGeneration, okay}));
            m_socket.flush();
            m_socket.waitForBytesWritten(1000);
        }
        m_physicalLease.reset();
        m_physicalLeaseGeneration = 0;
        return okay;
    }

    void physicalLayout(const ConsoleWorkerWire::PhysicalLayout &request)
    {
        const auto reject = [this, &request](const QString &error) {
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PhysicalLayoutResult{
                request.requestId, request.controlGeneration, error}));
        };
        if (m_mode.virtualSession || !m_authenticatedDesktop || m_stopping || m_physicalPending || !m_control.active
            || m_control.generation != request.controlGeneration || m_resize.changing()
            || m_outputs.monitors.isEmpty()
            || (m_multiMode ? (!m_multiReady || m_multiPublishedFrames.size() != m_outputs.monitors.size())
                            : !m_lastPhysicalKeyframe)) {
            reject(QStringLiteral("physical layout requires an idle authenticated Console capture"));
            return;
        }
        const auto plan = ConsoleTopologyPlan::fromWire(request);
        if (!plan) {
            reject(QStringLiteral("physical layout draft is invalid"));
            return;
        }
        const auto json = readKScreenJson();
        const auto kscreen = json ? RetainedKScreenReadback::parse(*json, m_sessionId) : std::nullopt;
        const auto captured = kscreen
            ? (m_multiMode ? ConsoleTopologyReadback::confirmedMulti(*kscreen, m_outputs, m_multiPublishedFrames)
                           : ConsoleTopologyReadback::confirmed(*kscreen, m_outputs, *m_lastPhysicalKeyframe))
            : std::nullopt;
        if (!captured) {
            reject(QStringLiteral("fresh physical KScreen readback differs from captured desktop"));
            return;
        }
        const auto before = ConsoleTopologyPlan::inventory(*plan, *json);
        const auto selected = before ? ConsoleTopologyPlan::selectedModes(*plan, *before) : std::nullopt;
        const auto args = selected
            ? ConsoleTopologyPlan::arguments(*plan, before->states, before->priorities, *selected) : std::nullopt;
        if (!args) {
            reject(QStringLiteral("physical layout or advertised modes changed before apply"));
            return;
        }
        if (args->isEmpty()) {
            reject({}); // A verified no-op must not churn the compositor.
            return;
        }
        const auto candidate = m_physicalLease
            ? (m_physicalLeaseGeneration == request.controlGeneration
                ? ConsoleTopologyLease::advance(*m_physicalLease, *plan, *before, *selected)
                : std::nullopt)
            : ConsoleTopologyLease::start(*plan, *before, *selected);
        if (!candidate) {
            reject(QStringLiteral("physical layout lease baseline or control generation changed"));
            return;
        }
        releaseInput();
        m_physicalPending = request;
        m_physicalCandidate = *candidate;
        m_physicalPlan = *plan;
        m_physicalBefore = *before;
        m_physicalSelected = *selected;
        m_physicalCaptureRestartReady = false;
        m_lastPhysicalKeyframe.reset();
        if (m_multiMode) {
            m_multiReady = false;
            m_multiCapture.invalidate();
            m_multiPublishedFrames.clear();
        }
        // KScreen's exit code is not authoritative. It can report apply failure
        // and still exit zero; always compare a second complete readback.
        runKScreenCommand(*args);
        const auto applied = readKScreenJson();
        if (!applied || !ConsoleTopologyPlan::matchesApplied(*plan, *before, *selected, *applied)) {
            failPhysical(QStringLiteral("physical layout did not match requested full readback"));
            return;
        }
        m_physicalDeadline.start();
        if (m_multiMode) {
            m_multiResizeNeedsRestart = true;
            m_multiSettle.start(0);
        } else if (!m_session.restartCaptureForResize(++m_physicalCaptureEpoch)) {
            failPhysical(QStringLiteral("physical layout capture cannot restart"));
        }
    }

    bool runKScreenCommand(const QStringList &arguments) const
    {
        if (arguments.isEmpty()) return false;
        QProcess command;
        command.start(QStringLiteral("kscreen-doctor"), arguments);
        const bool finished = command.waitForStarted(1000) && command.waitForFinished(3000);
        if (!finished) {
            command.kill();
            command.waitForFinished(1000);
        }
        return finished && command.exitStatus() == QProcess::NormalExit && command.exitCode() == 0;
    }

    void finishMultiResize(const QString &error)
    {
        if (!m_multiResizePending) return;
        const auto request = *m_multiResizePending;
        m_multiResizePending.reset();
        m_multiResizePlan.reset();
        m_multiResizeNeedsRestart = false;
        m_multiResizeDeadline.stop();
        m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ResizeResult{request.requestId, request.generation, error}));
    }

    void finishMultiFit(const QString &error)
    {
        if (!m_multiFitPending) return;
        const auto request = *m_multiFitPending;
        QString finalError = error;
        if (!error.isEmpty() && m_multiFitPlan && m_multiFitOriginalState && m_multiFitAppliedMode
            && m_control.active && m_control.generation == request.generation) {
            const auto currentJson = readKScreenJson();
            const auto current = currentJson ? RetainedKScreenReadback::parse(*currentJson, m_sessionId) : std::nullopt;
            const auto selected = currentJson
                ? VirtualResize::snapshotForOutput(*currentJson, request.output, nullptr) : std::nullopt;
            if (current && selected) {
                const auto restore = RetainedMultiFitPlan::recoveryArguments(*m_multiFitPlan, *current,
                    *selected, *m_multiFitOriginalState, *m_multiFitAppliedMode);
                if (restore && (restore->isEmpty() || runKScreenCommand(*restore))) {
                    const auto verifiedJson = readKScreenJson();
                    const auto verified = verifiedJson
                        ? RetainedKScreenReadback::parse(*verifiedJson, m_sessionId) : std::nullopt;
                    const auto restoredMode = verifiedJson
                        ? VirtualResize::snapshotForOutput(*verifiedJson, request.output, nullptr) : std::nullopt;
                    if (verified && verified->outputs == m_multiFitPlan->before.outputs && restoredMode
                        && restoredMode->current.id == m_multiFitOriginalState->current.id
                        && VirtualResize::sameScale(restoredMode->scale, m_multiFitOriginalState->scale)) {
                        finalError += QStringLiteral("; original layout restored");
                    }
                }
            }
        }
        m_multiFitPending.reset();
        m_multiFitPlan.reset();
        m_multiFitOriginalState.reset();
        m_multiFitAppliedMode.reset();
        m_multiFitDeadline.stop();
        m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ManagedFitResult{request.requestId, request.generation, finalError}));
    }

    void managedFit(const ConsoleWorkerWire::ManagedFit &request)
    {
        const auto reject = [this, &request](const QString &error) {
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ManagedFitResult{request.requestId, request.generation, error}));
        };
        if (!m_mode.virtualSession || !m_multiMode || !m_multiReady || m_multiPublishedFrames.size() < 2
            || !m_control.active || m_control.generation != request.generation || m_positionPending
            || m_addPending || m_removePending || m_multiResizePending || m_multiFitPending || m_primaryPending || m_mixedPending || m_mixedCreatePending) {
            reject(QStringLiteral("managed Fit unavailable or not authorized"));
            return;
        }
        const auto beforeJson = readKScreenJson();
        const auto before = beforeJson ? RetainedKScreenReadback::parse(*beforeJson, m_sessionId) : std::nullopt;
        if (!before || !RetainedKScreenReadback::matchesPublished(*before, m_outputs, m_multiPublishedFrames)) {
            reject(QStringLiteral("fresh compositor readback differs from capture"));
            return;
        }
        QVector<RemoteTopologyFit::Relation> relations;
        for (const auto &relation : request.relations) {
            relations.append({relation.parent, relation.child,
                static_cast<RemoteTopologyFit::Relation::Edge>(relation.edge), relation.offset});
        }
        const auto plan = RetainedMultiFitPlan::make(*before, m_sessionId, request.output, request.pixels, request.scale, relations);
        if (!plan) {
            reject(QStringLiteral("managed Fit conflicts with retained output arrangement"));
            return;
        }
        if (!plan->changed) { reject({}); return; }
        QString parseError;
        auto state = VirtualResize::snapshotForOutput(*beforeJson, request.output, &parseError);
        if (!state) { reject(QStringLiteral("Fit target mode inventory unavailable")); return; }
        const auto originalState = *state;
        auto mode = VirtualResize::matchingMode(*state, request.pixels, state->current.refresh);
        if (plan->resizeChanged && !mode) {
            if (state->modes.size() >= 64 || !runKScreenCommand(VirtualResize::add({*state, request.pixels, request.scale}))) {
                reject(QStringLiteral("Fit custom mode could not be added"));
                return;
            }
            const auto addedJson = readKScreenJson();
            const auto unchanged = addedJson ? RetainedKScreenReadback::parse(*addedJson, m_sessionId) : std::nullopt;
            state = addedJson ? VirtualResize::snapshotForOutput(*addedJson, request.output, &parseError) : std::nullopt;
            if (!unchanged || unchanged->outputs != before->outputs || !state
                || !VirtualResize::sameOutput(*state, originalState)
                || state->current.id != originalState.current.id || state->current.pixels != originalState.current.pixels
                || state->current.refresh != originalState.current.refresh
                || !VirtualResize::sameScale(state->scale, originalState.scale)) {
                reject(QStringLiteral("Fit custom mode changed the active arrangement"));
                return;
            }
            mode = VirtualResize::matchingMode(*state, request.pixels, state->current.refresh);
            if (!mode) { reject(QStringLiteral("Fit custom mode not advertised")); return; }
        }
        const auto args = RetainedMultiFitPlan::arguments(*plan, *state, mode.value_or(state->current));
        if (!args || args->isEmpty()) { reject(QStringLiteral("Fit command preflight failed")); return; }
        releaseInput();
        m_multiReady = false;
        ++m_multiEpoch;
        m_multiSessions.clear();
        m_multiCapture.invalidate();
        m_multiPublishedFrames.clear();
        m_multiFitPending = request;
        m_multiFitPlan = *plan;
        m_multiFitOriginalState = originalState;
        m_multiFitAppliedMode = mode.value_or(state->current);
        m_multiFitDeadline.start();
        if (!runKScreenCommand(*args)) {
            finishMultiFit(QStringLiteral("managed Fit compositor apply failed"));
            m_socket.disconnectFromServer();
            return;
        }
        const auto after = readKScreen();
        if (!after || !RetainedMultiFitPlan::matches(*plan, *after)) {
            finishMultiFit(QStringLiteral("managed Fit readback differs from preview"));
            m_socket.disconnectFromServer();
            return;
        }
        m_multiResizeNeedsRestart = true;
        m_multiSettle.start(200);
    }

    void finishPrimary(const QString &error)
    {
        if (!m_primaryPending) return;
        const auto request = *m_primaryPending;
        QString finalError = error;
        if (!error.isEmpty() && m_primaryPlan && m_control.active && m_control.generation == request.generation) {
            const auto json = readKScreenJson();
            const auto current = json ? RetainedKScreenReadback::parse(*json, m_sessionId) : std::nullopt;
            const auto currentPriority = current ? RetainedMultiPrimaryPlan::priorities(*json, *current) : std::nullopt;
            if (current && currentPriority) {
                const auto restore = RetainedMultiPrimaryPlan::recoveryArguments(*m_primaryPlan, *current, *currentPriority);
                if (restore && (restore->isEmpty() || runKScreenCommand(*restore))) {
                    const auto verifiedJson = readKScreenJson();
                    const auto verified = verifiedJson
                        ? RetainedKScreenReadback::parse(*verifiedJson, m_sessionId) : std::nullopt;
                    const auto restoredPriority = verified
                        ? RetainedMultiPrimaryPlan::priorities(*verifiedJson, *verified) : std::nullopt;
                    if (verified && restoredPriority && verified->outputs == m_primaryPlan->before.outputs
                        && *restoredPriority == m_primaryPlan->original)
                        finalError += QStringLiteral("; original priority order restored");
                }
            }
        }
        m_primaryPending.reset();
        m_primaryPlan.reset();
        m_primaryDeadline.stop();
        m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PrimaryResult{
            request.requestId, request.generation, finalError}));
    }

    void primary(const ConsoleWorkerWire::Primary &request)
    {
        const auto reject = [this, &request](const QString &error) {
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PrimaryResult{
                request.requestId, request.generation, error}));
        };
        if (!m_mode.virtualSession || !m_multiMode || !m_multiReady || m_multiPublishedFrames.size() < 2
            || !m_control.active || m_control.generation != request.generation || m_positionPending
            || m_addPending || m_removePending || m_multiResizePending || m_multiFitPending || m_primaryPending || m_mixedPending || m_mixedCreatePending) {
            reject(QStringLiteral("primary unavailable or not authorized"));
            return;
        }
        const auto json = readKScreenJson();
        const auto before = json ? RetainedKScreenReadback::parse(*json, m_sessionId) : std::nullopt;
        if (!before || !RetainedKScreenReadback::matchesPublished(*before, m_outputs, m_multiPublishedFrames)) {
            reject(QStringLiteral("fresh compositor readback differs from capture"));
            return;
        }
        const auto priorities = RetainedMultiPrimaryPlan::priorities(*json, *before);
        const auto plan = priorities
            ? RetainedMultiPrimaryPlan::make(*before, m_sessionId, request.output, *priorities) : std::nullopt;
        if (!plan) { reject(QStringLiteral("invalid owned primary selection")); return; }
        if (!plan->changed) { reject({}); return; }
        releaseInput();
        m_multiReady = false;
        ++m_multiEpoch;
        m_multiSessions.clear();
        m_multiCapture.invalidate();
        m_multiPublishedFrames.clear();
        m_primaryPending = request;
        m_primaryPlan = *plan;
        m_primaryDeadline.start();
        if (!runKScreenCommand(RetainedMultiPrimaryPlan::arguments(plan->requested))) {
            finishPrimary(QStringLiteral("primary compositor apply failed"));
            m_socket.disconnectFromServer();
            return;
        }
        const auto afterJson = readKScreenJson();
        const auto after = afterJson ? RetainedKScreenReadback::parse(*afterJson, m_sessionId) : std::nullopt;
        if (!after || !RetainedMultiPrimaryPlan::matches(*plan, *after)
            || RetainedMultiPrimaryPlan::priorities(*afterJson, *after) != plan->requested) {
            finishPrimary(QStringLiteral("primary readback differs from preview"));
            m_socket.disconnectFromServer();
            return;
        }
        m_multiResizeNeedsRestart = true;
        m_multiSettle.start(200);
    }

    void finishMixed(const QString &error)
    {
        if (!m_mixedPending) return;
        const auto request = *m_mixedPending;
        QString finalError = error;
        if (!error.isEmpty() && m_mixedPlan && m_control.active
            && m_control.generation == request.generation) {
            const auto json = readKScreenJson();
            const auto current = json ? RetainedKScreenReadback::parse(*json, m_sessionId) : std::nullopt;
            const auto priorities = current
                ? RetainedMultiPrimaryPlan::priorities(*json, *current) : std::nullopt;
            QMap<QString, VirtualResize::Snapshot> states;
            if (current && priorities) {
                for (auto it = m_mixedPlan->resizes.cbegin(); it != m_mixedPlan->resizes.cend(); ++it) {
                    const auto state = VirtualResize::snapshotForOutput(*json, it.key(), nullptr);
                    if (!state) break;
                    states.insert(it.key(), *state);
                }
                const auto restore = RetainedMultiMixedPlan::recoveryArguments(*m_mixedPlan, *current,
                    *priorities, states, m_mixedOriginalStates, m_mixedAppliedModes);
                if (restore && (restore->isEmpty() || runKScreenCommand(*restore))) {
                    const auto verifiedJson = readKScreenJson();
                    const auto verified = verifiedJson
                        ? RetainedKScreenReadback::parse(*verifiedJson, m_sessionId) : std::nullopt;
                    const auto verifiedPriorities = verified
                        ? RetainedMultiPrimaryPlan::priorities(*verifiedJson, *verified) : std::nullopt;
                    bool modesRestored = verifiedJson.has_value();
                    for (auto it = m_mixedOriginalStates.cbegin(); modesRestored && it != m_mixedOriginalStates.cend(); ++it) {
                        const auto mode = VirtualResize::snapshotForOutput(*verifiedJson, it.key(), nullptr);
                        modesRestored = mode && mode->current.id == it.value().current.id
                            && VirtualResize::sameScale(mode->scale, it.value().scale);
                    }
                    if (verified && verifiedPriorities && modesRestored
                        && verified->outputs == m_mixedPlan->before.outputs
                        && *verifiedPriorities == m_mixedPlan->originalPriorities)
                        finalError += QStringLiteral("; original mixed layout restored");
                }
            }
        }
        m_mixedPending.reset();
        m_mixedPlan.reset();
        m_mixedOriginalStates.clear();
        m_mixedAppliedModes.clear();
        m_mixedDeadline.stop();
        m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::MixedResult{
            request.requestId, request.generation, finalError}));
    }

    void finishMixedCreate(const QString &error)
    {
        if (!m_mixedCreatePending) return;
        const auto request = *m_mixedCreatePending;
        if (!error.isEmpty() && m_mixedCreateApplied && m_control.active
            && m_control.generation == request.generation) {
            const auto json = readKScreenJson();
            const auto current = json ? RetainedKScreenReadback::parse(*json, m_sessionId) : std::nullopt;
            const auto priorities = current ? RetainedMultiPrimaryPlan::priorities(*json, *current) : std::nullopt;
            QMap<QString, VirtualResize::Snapshot> states;
            if (current && priorities) {
                for (auto it = m_mixedCreateApplied->resizes.cbegin(); it != m_mixedCreateApplied->resizes.cend(); ++it) {
                    const auto state = VirtualResize::snapshotForOutput(*json, it.key(), nullptr);
                    if (!state) break;
                    states.insert(it.key(), *state);
                }
                const auto restore = RetainedMultiMixedPlan::recoveryArguments(*m_mixedCreateApplied,
                    *current, *priorities, states, m_mixedCreateOriginalStates, m_mixedCreateAppliedModes);
                if (restore && (restore->isEmpty() || runKScreenCommand(*restore))) {
                    const auto verifiedJson = readKScreenJson();
                    const auto verified = verifiedJson
                        ? RetainedKScreenReadback::parse(*verifiedJson, m_sessionId) : std::nullopt;
                    const auto verifiedPriorities = verified
                        ? RetainedMultiPrimaryPlan::priorities(*verifiedJson, *verified) : std::nullopt;
                    bool modesRestored = verifiedJson.has_value();
                    for (auto it = m_mixedCreateOriginalStates.cbegin(); modesRestored && it != m_mixedCreateOriginalStates.cend(); ++it) {
                        const auto mode = VirtualResize::snapshotForOutput(*verifiedJson, it.key(), nullptr);
                        modesRestored = mode && mode->current.id == it.value().current.id
                            && VirtualResize::sameScale(mode->scale, it.value().scale);
                    }
                    if (verified && verifiedPriorities && modesRestored
                        && verified->outputs == m_mixedCreateApplied->before.outputs
                        && *verifiedPriorities == m_mixedCreateApplied->originalPriorities)
                        qInfo() << "Mixed-create existing outputs restored before creator release";
                }
            }
        }
        m_mixedCreatePending.reset();
        m_mixedCreateDeadline.stop();
        m_mixedCreatePlan.reset();
        m_mixedCreateApplied.reset();
        m_mixedCreateOriginalStates.clear();
        m_mixedCreateAppliedModes.clear();
        if (error.isEmpty()) m_ownedCreators.emplace_back(request.newOutput, std::move(m_mixedCreateCreator));
        // On failure the caller closes the socket; the worker then releases
        // this creator. Never report a rollback that has not been read back.
        m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::MixedCreateResult{
            request.requestId, request.generation, error}));
    }

    bool applyMixedCreateAfterCreation(const RetainedKScreenReadback::Snapshot &created,
        const QByteArray &createdJson)
    {
        if (!m_mixedCreatePlan || !m_mixedCreatePending) return false;
        const auto priorities = RetainedMultiPrimaryPlan::priorities(createdJson, created);
        const auto plan = priorities ? RetainedMultiMixedCreatePlan::afterCreation(*m_mixedCreatePlan,
            created, *priorities) : std::nullopt;
        if (!plan) return false;
        QMap<QString, VirtualResize::Snapshot> originals;
        QMap<QString, VirtualResize::Snapshot> states;
        QMap<QString, VirtualResize::Mode> modes;
        for (auto it = plan->resizes.cbegin(); it != plan->resizes.cend(); ++it) {
            auto state = VirtualResize::snapshotForOutput(createdJson, it.key(), nullptr);
            if (!state) return false;
            originals.insert(it.key(), *state);
            auto mode = VirtualResize::matchingMode(*state, it.value().first, state->current.refresh);
            if (!mode) {
                if (state->modes.size() >= 64 || !runKScreenCommand(VirtualResize::add({*state,
                    it.value().first, it.value().second}))) return false;
                const auto addedJson = readKScreenJson();
                const auto unchanged = addedJson
                    ? RetainedKScreenReadback::parse(*addedJson, m_sessionId) : std::nullopt;
                const auto unchangedPriorities = unchanged
                    ? RetainedMultiPrimaryPlan::priorities(*addedJson, *unchanged) : std::nullopt;
                state = addedJson ? VirtualResize::snapshotForOutput(*addedJson, it.key(), nullptr) : std::nullopt;
                if (!unchanged || unchanged->outputs != created.outputs || unchangedPriorities != priorities
                    || !state || !VirtualResize::sameOutput(*state, originals.value(it.key()))
                    || state->current.id != originals.value(it.key()).current.id) return false;
                mode = VirtualResize::matchingMode(*state, it.value().first, state->current.refresh);
                if (!mode) return false;
            }
            states.insert(it.key(), *state);
            modes.insert(it.key(), *mode);
        }
        const auto freshJson = readKScreenJson();
        const auto fresh = freshJson ? RetainedKScreenReadback::parse(*freshJson, m_sessionId) : std::nullopt;
        const auto freshPriorities = fresh
            ? RetainedMultiPrimaryPlan::priorities(*freshJson, *fresh) : std::nullopt;
        if (!fresh || fresh->outputs != created.outputs || freshPriorities != priorities) return false;
        for (auto it = plan->resizes.cbegin(); it != plan->resizes.cend(); ++it) {
            const auto state = VirtualResize::snapshotForOutput(*freshJson, it.key(), nullptr);
            if (!state || !VirtualResize::sameOutput(*state, originals.value(it.key()))
                || state->current.id != originals.value(it.key()).current.id) return false;
            states.insert(it.key(), *state);
        }
        const auto args = RetainedMultiMixedPlan::arguments(*plan, states, modes);
        if (!args) return false;
        m_mixedCreateApplied = *plan;
        m_mixedCreateOriginalStates = originals;
        m_mixedCreateAppliedModes = modes;
        if (!args->isEmpty() && !runKScreenCommand(*args)) return false;
        const auto afterJson = readKScreenJson();
        const auto after = afterJson ? RetainedKScreenReadback::parse(*afterJson, m_sessionId) : std::nullopt;
        const auto afterPriorities = after
            ? RetainedMultiPrimaryPlan::priorities(*afterJson, *after) : std::nullopt;
        if (!after || !afterPriorities || !RetainedMultiMixedPlan::matches(*plan, *after, *afterPriorities)) return false;
        m_multiResizeNeedsRestart = true;
        m_multiSettle.start(200);
        return true;
    }

    void mixedCreate(const ConsoleWorkerWire::MixedCreate &request)
    {
        const auto reject = [this, &request](const QString &error) {
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::MixedCreateResult{
                request.requestId, request.generation, error}));
        };
        if (!m_mode.virtualSession || !m_multiMode || !m_multiReady || m_multiPublishedFrames.size() < 2
            || !m_control.active || m_control.generation != request.generation || m_positionPending
            || m_addPending || m_removePending || m_multiResizePending || m_multiFitPending
            || m_primaryPending || m_mixedPending || m_mixedCreatePending) {
            reject(QStringLiteral("mixed creation unavailable or not authorized"));
            return;
        }
        const auto beforeJson = readKScreenJson();
        const auto before = beforeJson ? RetainedKScreenReadback::parse(*beforeJson, m_sessionId) : std::nullopt;
        if (!before || !RetainedKScreenReadback::matchesPublished(*before, m_outputs, m_multiPublishedFrames)) {
            reject(QStringLiteral("fresh compositor readback differs from capture"));
            return;
        }
        const auto priorities = RetainedMultiPrimaryPlan::priorities(*beforeJson, *before);
        if (!priorities) { reject(QStringLiteral("mixed creation priority inventory unavailable")); return; }
        const QString temporaryId = QStringLiteral("new:worker");
        RemoteTopologyDraft::Operation add;
        add.kind = RemoteTopologyDraft::Operation::Kind::AddVirtual;
        add.id = temporaryId;
        add.position = request.globalLogical;
        add.pixels = request.pixels;
        add.scale = request.scale;
        QVector<RemoteTopologyDraft::Operation> operations{add};
        for (const auto &wire : request.changes) {
            if (wire.output == request.newOutput && wire.kind != ConsoleWorkerWire::MixedOperation::Kind::Primary) {
                reject(QStringLiteral("new output mode and position belong in creator request"));
                return;
            }
            RemoteTopologyDraft::Operation operation;
            operation.id = wire.output == request.newOutput ? temporaryId : wire.output;
            operation.position = wire.globalLogical;
            operation.pixels = wire.pixels;
            operation.scale = wire.scale;
            switch (wire.kind) {
            case ConsoleWorkerWire::MixedOperation::Kind::Move:
                operation.kind = RemoteTopologyDraft::Operation::Kind::Move;
                break;
            case ConsoleWorkerWire::MixedOperation::Kind::Resize:
                operation.kind = RemoteTopologyDraft::Operation::Kind::Resize;
                break;
            case ConsoleWorkerWire::MixedOperation::Kind::Primary:
                operation.kind = RemoteTopologyDraft::Operation::Kind::SetPrimary;
                break;
            }
            operations.append(operation);
        }
        const auto plan = RetainedMultiMixedCreatePlan::make(*before, m_sessionId,
            *priorities, request.newOutput, operations);
        if (!plan) { reject(QStringLiteral("mixed creation conflicts with retained output arrangement")); return; }
        releaseInput();
        m_multiReady = false;
        ++m_multiEpoch;
        m_multiSessions.clear();
        m_multiCapture.invalidate();
        m_multiPublishedFrames.clear();
        m_mixedCreatePending = request;
        m_mixedCreatePlan = *plan;
        m_mixedCreateDeadline.start();
        m_mixedCreateCreator = std::make_unique<PlasmaScreencastV1Session>();
        m_mixedCreateCreator->setVirtualMonitor(VirtualMonitor{request.newOutput.mid(8), request.pixels, request.scale});
        m_mixedCreateCreator->setVideoCodec(VideoCodec::Avc420);
        connect(m_mixedCreateCreator.get(), &AbstractSession::error, this, [this] {
            if (!m_mixedCreatePending) return;
            finishMixedCreate(QStringLiteral("KWin rejected mixed virtual output creation"));
            m_socket.disconnectFromServer();
        });
        connect(m_mixedCreateCreator.get(), &AbstractSession::virtualOutputUnresolved, this, [this] {
            if (!m_mixedCreatePending) return;
            finishMixedCreate(QStringLiteral("mixed new KWin output did not resolve"));
            m_socket.disconnectFromServer();
        });
        m_mixedCreateCreator->setStreamingEnabled(true);
        m_multiSettle.start(200);
    }

    void mixed(const ConsoleWorkerWire::Mixed &request)
    {
        const auto reject = [this, &request](const QString &error) {
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::MixedResult{
                request.requestId, request.generation, error}));
        };
        if (!m_mode.virtualSession || !m_multiMode || !m_multiReady || m_multiPublishedFrames.size() < 2
            || !m_control.active || m_control.generation != request.generation || m_positionPending
            || m_addPending || m_removePending || m_multiResizePending || m_multiFitPending
            || m_primaryPending || m_mixedPending || m_mixedCreatePending) {
            reject(QStringLiteral("mixed layout unavailable or not authorized"));
            return;
        }
        const auto beforeJson = readKScreenJson();
        const auto before = beforeJson ? RetainedKScreenReadback::parse(*beforeJson, m_sessionId) : std::nullopt;
        if (!before || !RetainedKScreenReadback::matchesPublished(*before, m_outputs, m_multiPublishedFrames)) {
            reject(QStringLiteral("fresh compositor readback differs from capture"));
            return;
        }
        const auto priorities = RetainedMultiPrimaryPlan::priorities(*beforeJson, *before);
        if (!priorities) { reject(QStringLiteral("mixed layout priority inventory unavailable")); return; }
        QVector<RemoteTopologyDraft::Operation> operations;
        for (const auto &wire : request.operations) {
            RemoteTopologyDraft::Operation operation;
            operation.id = wire.output;
            operation.position = wire.globalLogical;
            operation.pixels = wire.pixels;
            operation.scale = wire.scale;
            switch (wire.kind) {
            case ConsoleWorkerWire::MixedOperation::Kind::Move:
                operation.kind = RemoteTopologyDraft::Operation::Kind::Move;
                break;
            case ConsoleWorkerWire::MixedOperation::Kind::Resize:
                operation.kind = RemoteTopologyDraft::Operation::Kind::Resize;
                break;
            case ConsoleWorkerWire::MixedOperation::Kind::Primary:
                operation.kind = RemoteTopologyDraft::Operation::Kind::SetPrimary;
                break;
            }
            operations.append(operation);
        }
        const auto plan = RetainedMultiMixedPlan::make(*before, m_sessionId, *priorities, operations);
        if (!plan) { reject(QStringLiteral("mixed layout conflicts with retained output arrangement")); return; }
        if (!plan->changed) { reject({}); return; }
        QMap<QString, VirtualResize::Snapshot> originalStates;
        QMap<QString, VirtualResize::Snapshot> states;
        QMap<QString, VirtualResize::Mode> selectedModes;
        for (auto it = plan->resizes.cbegin(); it != plan->resizes.cend(); ++it) {
            auto state = VirtualResize::snapshotForOutput(*beforeJson, it.key(), nullptr);
            if (!state) { reject(QStringLiteral("mixed target mode inventory unavailable")); return; }
            originalStates.insert(it.key(), *state);
            auto mode = VirtualResize::matchingMode(*state, it.value().first, state->current.refresh);
            if (!mode) {
                if (state->modes.size() >= 64 || !runKScreenCommand(VirtualResize::add({*state,
                    it.value().first, it.value().second}))) {
                    reject(QStringLiteral("mixed custom mode could not be added"));
                    return;
                }
                const auto addedJson = readKScreenJson();
                const auto unchanged = addedJson ? RetainedKScreenReadback::parse(*addedJson, m_sessionId) : std::nullopt;
                const auto unchangedPriorities = unchanged
                    ? RetainedMultiPrimaryPlan::priorities(*addedJson, *unchanged) : std::nullopt;
                state = addedJson ? VirtualResize::snapshotForOutput(*addedJson, it.key(), nullptr) : std::nullopt;
                if (!unchanged || unchanged->outputs != before->outputs || unchangedPriorities != priorities
                    || !state || !VirtualResize::sameOutput(*state, originalStates.value(it.key()))
                    || state->current.id != originalStates.value(it.key()).current.id) {
                    reject(QStringLiteral("mixed custom mode changed the active arrangement"));
                    return;
                }
                mode = VirtualResize::matchingMode(*state, it.value().first, state->current.refresh);
                if (!mode) { reject(QStringLiteral("mixed custom mode not advertised")); return; }
            }
            states.insert(it.key(), *state);
            selectedModes.insert(it.key(), *mode);
        }
        const auto finalJson = readKScreenJson();
        const auto stillBefore = finalJson ? RetainedKScreenReadback::parse(*finalJson, m_sessionId) : std::nullopt;
        const auto stillPriorities = stillBefore
            ? RetainedMultiPrimaryPlan::priorities(*finalJson, *stillBefore) : std::nullopt;
        if (!stillBefore || stillBefore->outputs != before->outputs || stillPriorities != priorities) {
            reject(QStringLiteral("mixed layout changed during mode preparation"));
            return;
        }
        for (auto it = plan->resizes.cbegin(); it != plan->resizes.cend(); ++it) {
            const auto fresh = VirtualResize::snapshotForOutput(*finalJson, it.key(), nullptr);
            if (!fresh || !VirtualResize::sameOutput(*fresh, originalStates.value(it.key()))
                || fresh->current.id != originalStates.value(it.key()).current.id) {
                reject(QStringLiteral("mixed mode inventory changed during preparation"));
                return;
            }
            states.insert(it.key(), *fresh);
        }
        const auto args = RetainedMultiMixedPlan::arguments(*plan, states, selectedModes);
        if (!args || args->isEmpty()) { reject(QStringLiteral("mixed layout command preflight failed")); return; }
        releaseInput();
        m_multiReady = false;
        ++m_multiEpoch;
        m_multiSessions.clear();
        m_multiCapture.invalidate();
        m_multiPublishedFrames.clear();
        m_mixedPending = request;
        m_mixedPlan = *plan;
        m_mixedOriginalStates = originalStates;
        m_mixedAppliedModes = selectedModes;
        m_mixedDeadline.start();
        if (!runKScreenCommand(*args)) {
            finishMixed(QStringLiteral("mixed layout compositor apply failed"));
            m_socket.disconnectFromServer();
            return;
        }
        const auto afterJson = readKScreenJson();
        const auto after = afterJson ? RetainedKScreenReadback::parse(*afterJson, m_sessionId) : std::nullopt;
        const auto afterPriorities = after
            ? RetainedMultiPrimaryPlan::priorities(*afterJson, *after) : std::nullopt;
        if (!after || !afterPriorities || !RetainedMultiMixedPlan::matches(*plan, *after, *afterPriorities)) {
            finishMixed(QStringLiteral("mixed layout readback differs from preview"));
            m_socket.disconnectFromServer();
            return;
        }
        m_multiResizeNeedsRestart = true;
        m_multiSettle.start(200);
    }

    void multiResize(const ConsoleWorkerWire::Resize &request)
    {
        const auto reject = [this, &request](const QString &error) {
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ResizeResult{request.requestId, request.generation, error}));
        };
        if (!m_mode.virtualSession || !m_multiMode || !m_multiReady || m_multiPublishedFrames.size() < 2
            || !m_control.active || m_control.generation != request.generation
            || m_positionPending || m_addPending || m_removePending || m_multiResizePending || m_multiFitPending || m_primaryPending || m_mixedPending || m_mixedCreatePending) {
            reject(QStringLiteral("multi-output resize unavailable or not authorized"));
            return;
        }
        const auto beforeJson = readKScreenJson();
        const auto before = beforeJson ? RetainedKScreenReadback::parse(*beforeJson, m_sessionId) : std::nullopt;
        if (!before || !RetainedKScreenReadback::matchesPublished(*before, m_outputs, m_multiPublishedFrames)) {
            reject(QStringLiteral("fresh compositor readback differs from capture"));
            return;
        }
        const auto plan = RetainedMultiResizePlan::make(*before, m_sessionId, request.output, request.pixels, request.scale);
        if (!plan) {
            reject(QStringLiteral("resize overlaps or exceeds retained-output limits"));
            return;
        }
        if (!plan->changed) {
            reject({}); // Full fresh readback and capture already agree; no mutation.
            return;
        }
        QString parseError;
        auto state = VirtualResize::snapshotForOutput(*beforeJson, request.output, &parseError);
        const auto target = std::find_if(before->outputs.cbegin(), before->outputs.cend(), [&request](const auto &output) {
            return output.backendKey == request.output;
        });
        if (!state || target == before->outputs.cend() || state->position != target->logicalGeometry.topLeft()
            || state->current.pixels != target->nativePixels || !VirtualResize::sameScale(state->scale, target->scale)) {
            reject(QStringLiteral("target mode inventory differs from full compositor readback"));
            return;
        }
        const auto originalState = *state;
        auto mode = VirtualResize::matchingMode(*state, request.pixels, state->current.refresh);
        if (!mode) {
            if (state->modes.size() >= 64 || !runKScreenCommand(VirtualResize::add({*state, request.pixels, request.scale}))) {
                reject(QStringLiteral("requested custom mode could not be added"));
                return;
            }
            const auto addedJson = readKScreenJson();
            const auto unchanged = addedJson ? RetainedKScreenReadback::parse(*addedJson, m_sessionId) : std::nullopt;
            state = addedJson ? VirtualResize::snapshotForOutput(*addedJson, request.output, &parseError) : std::nullopt;
            if (!unchanged || unchanged->outputs != before->outputs || !state
                || !VirtualResize::sameOutput(*state, originalState)
                || state->current.id != originalState.current.id || state->current.pixels != originalState.current.pixels
                || state->current.refresh != originalState.current.refresh
                || !VirtualResize::sameScale(state->scale, originalState.scale)) {
                reject(QStringLiteral("custom mode changed the active arrangement"));
                return;
            }
            mode = VirtualResize::matchingMode(*state, request.pixels, state->current.refresh);
            if (!mode) {
                reject(QStringLiteral("new custom mode was not advertised"));
                return;
            }
        }
        releaseInput();
        m_multiReady = false;
        ++m_multiEpoch;
        m_multiSessions.clear();
        m_multiCapture.invalidate();
        m_multiPublishedFrames.clear();
        m_multiResizePending = request;
        m_multiResizePlan = *plan;
        m_multiResizeDeadline.start();
        if (!runKScreenCommand(VirtualResize::select(*state, *mode, request.scale))) {
            finishMultiResize(QStringLiteral("virtual mode/scale apply failed"));
            m_socket.disconnectFromServer();
            return;
        }
        const auto after = readKScreen();
        if (!after || !RetainedMultiResizePlan::matches(*plan, *after)) {
            finishMultiResize(QStringLiteral("mode/scale readback differs from preview"));
            m_socket.disconnectFromServer();
            return;
        }
        m_multiResizeNeedsRestart = true;
        m_multiSettle.start(200);
    }

    bool addMatches(const RetainedKScreenReadback::Snapshot &current, bool position) const
    {
        if (!m_addPending || current.outputs.size() != m_addBefore.outputs.size() + 1) return false;
        for (const auto &old : m_addBefore.outputs) {
            const auto found = std::find_if(current.outputs.cbegin(), current.outputs.cend(), [&old](const auto &output) {
                return output.backendKey == old.backendKey;
            });
            if (found == current.outputs.cend() || found->nativePixels != old.nativePixels
                || found->logicalGeometry != old.logicalGeometry || found->primary != old.primary
                || !VirtualResize::sameScale(found->scale, old.scale)) return false;
        }
        const auto &request = *m_addPending;
        const auto created = std::find_if(current.outputs.cbegin(), current.outputs.cend(), [&request](const auto &output) {
            return output.backendKey == request.output;
        });
        return created != current.outputs.cend() && !created->primary && created->nativePixels == request.pixels
            && VirtualResize::sameScale(created->scale, request.scale)
            && (!position || created->logicalGeometry.topLeft() == request.globalLogical);
    }

    void finishAdd(const QString &error)
    {
        if (!m_addPending) return;
        const auto request = *m_addPending;
        m_addPending.reset();
        m_addDeadline.stop();
        m_addPlaced = false;
        if (error.isEmpty()) m_ownedCreators.emplace_back(request.output, std::move(m_addCreator));
        m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::AddVirtualResult{request.requestId, request.generation, error}));
    }

    bool removeMatches(const RetainedKScreenReadback::Snapshot &current) const
    {
        if (!m_removePending || current.outputs.size() + 1 != m_removeBefore.outputs.size()) return false;
        for (const auto &old : m_removeBefore.outputs) {
            const auto found = std::find_if(current.outputs.cbegin(), current.outputs.cend(), [&old](const auto &output) {
                return output.backendKey == old.backendKey;
            });
            if (old.backendKey == m_removePending->output) {
                if (found != current.outputs.cend()) return false;
            } else if (found == current.outputs.cend() || found->nativePixels != old.nativePixels
                || found->logicalGeometry != old.logicalGeometry || found->primary != old.primary
                || !VirtualResize::sameScale(found->scale, old.scale)) return false;
        }
        return true;
    }

    void finishRemove(const QString &error)
    {
        if (!m_removePending) return;
        const auto request = *m_removePending;
        m_removePending.reset();
        m_removeDeadline.stop();
        m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::RemoveVirtualResult{request.requestId, request.generation, error}));
    }

    void removeVirtual(const ConsoleWorkerWire::RemoveVirtual &request)
    {
        const auto reject = [this, &request](const QString &error) {
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::RemoveVirtualResult{request.requestId, request.generation, error}));
        };
        if (!m_mode.virtualSession || !m_multiMode || !m_multiReady || !m_control.active
            || m_control.generation != request.generation || m_positionPending || m_addPending || m_removePending || m_multiResizePending
            || m_multiFitPending || m_primaryPending || m_mixedPending || m_mixedCreatePending || m_multiPublishedFrames.size() < 3) {
            reject(QStringLiteral("virtual output removal unavailable or not authorized"));
            return;
        }
        const auto creator = std::find_if(m_ownedCreators.begin(), m_ownedCreators.end(), [&request](const auto &owned) {
            return owned.first == request.output;
        });
        if (creator == m_ownedCreators.end()) {
            reject(QStringLiteral("worker does not own this virtual output"));
            return;
        }
        const auto before = readKScreen();
        if (!before || !RetainedKScreenReadback::matchesPublished(*before, m_outputs, m_multiPublishedFrames)
            || before->outputs.size() < 3
            || std::none_of(before->outputs.cbegin(), before->outputs.cend(), [&request](const auto &output) {
                return output.backendKey == request.output && !output.primary;
            })) {
            reject(QStringLiteral("fresh compositor readback refused removal"));
            return;
        }
        releaseInput();
        m_multiReady = false;
        m_multiPublishedFrames.clear();
        m_removeBefore = *before;
        m_removePending = request;
        m_removeDeadline.start();
        m_ownedCreators.erase(creator); // Release only this worker-owned KWin output.
        m_multiSettle.start(200);
    }

    void addVirtual(const ConsoleWorkerWire::AddVirtual &request)
    {
        const auto reject = [this, &request](const QString &error) {
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::AddVirtualResult{request.requestId, request.generation, error}));
        };
        if (!m_mode.virtualSession || !m_multiMode || !m_multiReady || !m_control.active
            || m_control.generation != request.generation || m_positionPending || m_addPending || m_removePending || m_multiResizePending
            || m_multiFitPending || m_primaryPending || m_mixedPending || m_mixedCreatePending || m_multiPublishedFrames.size() < 2) {
            reject(QStringLiteral("virtual output creation unavailable or not authorized"));
            return;
        }
        const auto before = readKScreen();
        if (!before || !RetainedKScreenReadback::matchesPublished(*before, m_outputs, m_multiPublishedFrames)
            // Private KWin reports maxActiveOutputsCount as its *current*
            // virtual-output count and increments it on creation; it is not
            // a fixed capacity. The capture/wire cap is checked separately.
            || before->outputs.size() >= 16
            || std::any_of(before->outputs.cbegin(), before->outputs.cend(), [&request](const auto &output) {
                return output.backendKey == request.output;
            })) {
            reject(QStringLiteral("fresh compositor readback or output limit refused creation"));
            return;
        }
        releaseInput();
        m_multiReady = false;
        m_multiPublishedFrames.clear();
        m_addBefore = *before;
        m_addPending = request;
        m_addPlaced = false;
        m_addDeadline.start();
        m_addCreator = std::make_unique<PlasmaScreencastV1Session>();
        m_addCreator->setVirtualMonitor(VirtualMonitor{request.output.mid(8), request.pixels, request.scale});
        m_addCreator->setVideoCodec(VideoCodec::Avc420);
        connect(m_addCreator.get(), &AbstractSession::error, this, [this] {
            if (!m_addPending) return;
            finishAdd(QStringLiteral("KWin rejected virtual output creation"));
            m_socket.disconnectFromServer();
        });
        connect(m_addCreator.get(), &AbstractSession::virtualOutputUnresolved, this, [this] {
            if (!m_addPending) return;
            finishAdd(QStringLiteral("new KWin output did not resolve"));
            m_socket.disconnectFromServer();
        });
        m_addCreator->setStreamingEnabled(true);
        m_multiSettle.start(200);
    }

    void finishPosition(const QString &error)
    {
        if (!m_positionPending) return;
        const auto request = *m_positionPending;
        m_positionPending.reset();
        m_positionPlan.reset();
        m_positionDeadline.stop();
        if (request.batch) m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PositionBatchResult{
            request.requestId, request.generation, error}));
        else m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PositionResult{
            request.requestId, request.generation, error}));
    }

    void position(const ConsoleWorkerWire::Position &request)
    {
        positionTargets({request.requestId, request.generation, false,
            {{request.output, request.globalLogical}}});
    }

    void positionBatch(const ConsoleWorkerWire::PositionBatch &request)
    {
        QVector<RetainedKScreenReadback::Placement> targets;
        for (const auto &target : request.targets) targets.append({target.output, target.globalLogical});
        positionTargets({request.requestId, request.generation, true, std::move(targets)});
    }

    void positionTargets(const PendingPosition &request)
    {
        const auto reject = [this, &request](const QString &error) {
            if (request.batch) m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PositionBatchResult{
                request.requestId, request.generation, error}));
            else m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PositionResult{
                request.requestId, request.generation, error}));
        };
        if (!m_mode.virtualSession || !m_multiMode || !m_multiReady || m_multiPublishedFrames.size() < 2
            || !m_control.active || m_control.generation != request.generation
            || m_positionPending || m_addPending || m_removePending || m_multiResizePending || m_multiFitPending || m_primaryPending || m_mixedPending || m_mixedCreatePending) {
            reject(QStringLiteral("position unavailable or not authorized"));
            return;
        }
        const auto before = readKScreen();
        if (!before || !RetainedKScreenReadback::matchesPublished(*before, m_outputs, m_multiPublishedFrames)) {
            reject(QStringLiteral("fresh compositor readback differs from capture"));
            return;
        }
        const auto plan = RetainedMultiPositionPlan::make(*before, m_sessionId, request.targets);
        if (!plan) {
            reject(QStringLiteral("invalid or overlapping position"));
            return;
        }
        if (!plan->changed) {
            reject({}); // Fresh readback still agrees with captured keyframes; no mutation.
            return;
        }
        const auto arguments = RetainedKScreenReadback::positionArguments(*before, request.targets);
        releaseInput();
        m_multiReady = false;
        m_multiPublishedFrames.clear();
        m_positionPending = request;
        m_positionPlan = *plan;
        m_positionDeadline.start();
        QProcess command;
        command.start(QStringLiteral("kscreen-doctor"), *arguments);
        const bool finished = command.waitForStarted(1000) && command.waitForFinished(3000);
        if (!finished) {
            command.kill();
            command.waitForFinished(1000);
        }
        const auto after = readKScreen();
        const bool landed = after && RetainedMultiPositionPlan::matches(*plan, *after);
        if (!finished || command.exitStatus() != QProcess::NormalExit || command.exitCode() != 0
            || !landed) {
            finishPosition(QStringLiteral("position readback differs from request"));
            m_socket.flush();
            m_socket.disconnectFromServer();
            return;
        }
        m_multiSettle.start(0); // KWin/QScreen reconfiguration must precede capture success.
    }

    void reclaimConsole()
    {
        if (!m_mode.physicalActions() || !m_control.active) {
            return;
        }
        releaseInput();
        m_socket.write(ConsoleWorkerWire::frame(m_control, ConsoleWorkerWire::Kind::LocalTakeover));
        m_control.active = false; // Gate immediately, before the host's acknowledgement.
        if (m_physicalLease) {
            releasePhysicalLease();
            m_socket.disconnectFromServer(); // A new worker must prove fresh capture before any new grant.
        }
        m_microphone.setControl(m_control);
        m_session.setVideoQuality(80);
        m_resize.setControl(m_control);
        m_reclaimAction.setEnabled(false);
        m_takeover.latch();
    }

    void readBroker()
    {
        if (m_stopping) {
            return;
        }
        m_deframer.feed(m_socket.readAll());
        while (const auto record = m_deframer.next()) {
            if (const auto control = ConsoleWorkerWire::controlState(*record)) {
                if (*control != m_control) {
                    if (m_physicalPending) {
                        failPhysical(QStringLiteral("physical layout authority changed during capture"));
                        return;
                    }
                    if (m_physicalLease && (!control->active || control->generation != m_physicalLeaseGeneration)) {
                        releasePhysicalLease();
                        m_socket.disconnectFromServer(); // Never reuse pre-restoration capture metadata.
                        return;
                    }
                    const bool multiNewGrant = m_multiMode && m_multiReady
                        && !m_control.active && control->active
                        && !m_positionPending && !m_addPending && !m_removePending
                        && !m_multiResizePending && !m_multiFitPending && !m_primaryPending
                        && !m_mixedPending && !m_mixedCreatePending;
                    releaseInput();
                    m_session.setVideoQuality(80);
                    m_multiQuality = 80;
                    for (const auto &session : m_multiSessions) session->setVideoQuality(80);
                    m_control = *control;
                    m_microphone.setControl(*control);
                    if (m_mode.virtualSession) m_virtualResize.setControl(*control);
                    else m_resize.setControl(*control);
                    m_reclaimAction.setEnabled(m_mode.physicalActions() && control->active);
                    m_takeover = {};
                    if (control->active) {
                        m_takeover.armed(m_clock.elapsed());
                    }
                    if (multiNewGrant) {
                        // An idle compositor may not deliver any new damage after
                        // readiness or after a former RDP client exits. A keyframe request cannot
                        // recover when the old encoder has no reusable last frame.
                        // Recreate only the private capture streams; the retained
                        // compositor, outputs and apps are left untouched. Hold
                        // input until fresh per-output packets and KScreen agree.
                        ++m_multiEpoch;
                        m_multiReady = false;
                        m_multiCapture.invalidate();
                        m_multiPublishedFrames.clear();
                        m_multiResizeNeedsRestart = true;
                        m_multiSettle.start(0);
                        qInfo() << "Refreshing per-output captures for a new client";
                    }
                }
                continue;
            }
            if (record->kind == ConsoleWorkerWire::Kind::Stop && record->payload.isEmpty()) {
                shutdown(0);
                return;
            }
            if (const auto quality = ConsoleWorkerWire::videoQuality(*record)) {
                if (ConsoleWorkerWire::mayApplyQuality(*quality, m_control)) {
                    m_session.setVideoQuality(quality->quality);
                    m_multiQuality = quality->quality;
                    for (const auto &session : m_multiSessions) session->setVideoQuality(quality->quality);
                }
                continue;
            }
            if (const auto policy = ConsoleWorkerWire::microphonePolicy(*record)) {
                m_microphone.request(*policy);
                continue;
            }
            if (const auto audio = ConsoleWorkerWire::microphoneAudio(*record)) {
                m_microphone.audio(*audio);
                continue;
            }
            if (const auto request = ConsoleWorkerWire::resize(*record)) {
                if (m_physicalPending) {
                    m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ResizeResult{
                        request->requestId, request->generation,
                        QStringLiteral("physical layout transaction is in progress")}));
                    continue;
                }
                releaseInput();
                if (m_mode.virtualSession && m_multiMode) multiResize(*request);
                else if (m_mode.virtualSession) m_virtualResize.request(*request);
                else m_resize.request(*request);
                continue;
            }
            if (const auto request = ConsoleWorkerWire::addVirtual(*record)) {
                addVirtual(*request);
                continue;
            }
            if (const auto request = ConsoleWorkerWire::removeVirtual(*record)) {
                removeVirtual(*request);
                continue;
            }
            if (const auto request = ConsoleWorkerWire::position(*record)) {
                position(*request);
                continue;
            }
            if (const auto request = ConsoleWorkerWire::positionBatch(*record)) {
                positionBatch(*request);
                continue;
            }
            if (const auto request = ConsoleWorkerWire::managedFit(*record)) {
                managedFit(*request);
                continue;
            }
            if (const auto request = ConsoleWorkerWire::primary(*record)) {
                primary(*request);
                continue;
            }
            if (const auto request = ConsoleWorkerWire::mixed(*record)) {
                mixed(*request);
                continue;
            }
            if (const auto request = ConsoleWorkerWire::mixedCreate(*record)) {
                mixedCreate(*request);
                continue;
            }
            if (const auto request = ConsoleWorkerWire::physicalLayout(*record)) {
                physicalLayout(*request);
                continue;
            }
            if (record->kind == ConsoleWorkerWire::Kind::RequestKeyFrame && record->payload.isEmpty()) {
                if (m_multiMode) {
                    for (const auto &session : m_multiSessions) session->requestKeyFrame();
                } else m_session.requestKeyFrame();
                continue;
            }
            if (record->kind == ConsoleWorkerWire::Kind::TopologyQuery && record->payload.isEmpty()) {
                if (!m_mode.virtualSession) {
                    if (!m_resize.changing()) {
                        const auto topology = m_multiMode && m_multiReady
                            ? physicalTopology(m_outputs, m_multiPublishedFrames)
                            : (!m_multiMode && m_lastPhysicalKeyframe
                                ? physicalTopology(m_outputs, *m_lastPhysicalKeyframe) : std::nullopt);
                        if (topology) {
                            m_socket.write(ConsoleWorkerWire::frame(*topology));
                            continue; // Idle KWin need not produce damage.
                        }
                    }
                    m_topologyQueryPending = true;
                    if (m_multiMode) {
                        releaseInput();
                        m_multiReady = false;
                        m_multiCapture.invalidate();
                        m_multiPublishedFrames.clear();
                        m_multiResizeNeedsRestart = true;
                        m_multiSettle.start(0);
                    } else m_session.requestKeyFrame();
                }
                continue;
            }
            if (const auto media = ConsoleWorkerWire::media(*record)) {
                m_audioTimer.stop();
                m_audio.reset();
                if (media->playback) {
                    m_audio = std::make_unique<PipeWireAudioPlayback>();
                    const bool started = media->silenceHost ? m_audio->startIsolated(QStringLiteral("console-%1").arg(m_sessionId))
                                                            : m_audio->start(QStringLiteral("@DEFAULT_AUDIO_SINK@"));
                    if (!started) {
                        qWarning("Cannot capture console-session PipeWire audio");
                        m_audio.reset();
                    } else {
                        m_audioTimer.start();
                    }
                }
                continue;
            }
            if (const auto input = ConsoleWorkerWire::input(*record)) {
                if (!m_control.active || m_physicalPending
                    || !(m_mode.virtualSession ? m_virtualResize.inputAllowed() : m_resize.inputAllowed())
                    || (m_multiMode && !m_multiReady)) {
                    continue;
                }
                const auto mapped = m_multiMode
                    ? RetainedMultiInput::toCompositor(*input, m_wireAtlas, m_logicalOutputs, m_workspaceOrigin)
                    : std::optional(*input);
                if (!mapped) continue;
                if (const auto event = eventFor(*mapped)) {
                    if (m_multiMode) {
                        auto *session = multiInputSession();
                        if (!session) continue;
                        if (!m_mode.virtualSession && mapped->type == ConsoleWorkerWire::Input::Type::Mouse
                            && mapped->eventType == QEvent::MouseMove)
                            m_takeover.injected(mapped->position.toPoint(), m_clock.elapsed());
                        if (RetainedMultiInput::positionBeforeDispatch(*mapped)) {
                            auto motion = *mapped;
                            motion.type = ConsoleWorkerWire::Input::Type::Mouse;
                            motion.eventType = QEvent::MouseMove;
                            motion.button = Qt::NoButton;
                            if (!m_mode.virtualSession)
                                m_takeover.injected(motion.position.toPoint(), m_clock.elapsed());
                            session->sendGlobalEvent(eventFor(motion));
                        }
                        session->sendGlobalEvent(event);
                    } else {
                        if (input->type == ConsoleWorkerWire::Input::Type::Mouse && input->eventType == QEvent::MouseMove) {
                            m_takeover.injected(m_session.mapToGlobal(input->position).toPoint(), m_clock.elapsed());
                        }
                        m_session.sendEvent(event);
                    }
                    m_inputState.record(*mapped);
                    continue;
                }
            }
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Error, QByteArrayLiteral("invalid broker record")));
            m_socket.disconnectFromServer();
            return;
        }
        if (m_deframer.overflowed() || m_deframer.takeInvalidCount() != 0) {
            m_socket.disconnectFromServer();
        }
    }

    QString m_socketName;
    QString m_sessionId;
    quint32 m_uid = 0;
    QByteArray m_token;
    CaptureWorkerMode m_mode;
    bool m_authenticatedDesktop = false;
    QLocalSocket m_socket;
    ConsoleWorkerWire::Deframer m_deframer;
    PlasmaScreencastV1Session m_session;
    RetainedMultiCapture m_multiCapture;
    std::vector<std::unique_ptr<PlasmaScreencastV1Session>> m_multiSessions;
    QVector<VideoMonitor> m_wireAtlas;
    QVector<RemoteMonitorGeometry::Output> m_logicalOutputs;
    QPoint m_workspaceOrigin;
    QSet<QScreen *> m_watchedScreens;
    QTimer m_multiSettle;
    QTimer m_physicalDeadline;
    std::optional<ConsoleWorkerWire::PhysicalLayout> m_physicalPending;
    std::optional<ConsoleTopologyPlan::Plan> m_physicalPlan;
    std::optional<ConsoleTopologyPlan::Inventory> m_physicalBefore;
    std::optional<QMap<QString, ConsoleTopologyPlan::Mode>> m_physicalSelected;
    std::optional<ConsoleTopologyLease::State> m_physicalCandidate;
    std::optional<ConsoleTopologyLease::State> m_physicalLease;
    quint64 m_physicalLeaseGeneration = 0;
    quint64 m_physicalCaptureEpoch = 0;
    bool m_physicalCaptureRestartReady = false;
    QTimer m_positionDeadline;
    std::optional<PendingPosition> m_positionPending;
    std::optional<RetainedMultiPositionPlan::Plan> m_positionPlan;
    QTimer m_multiResizeDeadline;
    std::optional<ConsoleWorkerWire::Resize> m_multiResizePending;
    std::optional<RetainedMultiResizePlan::Plan> m_multiResizePlan;
    QTimer m_multiFitDeadline;
    std::optional<ConsoleWorkerWire::ManagedFit> m_multiFitPending;
    std::optional<RetainedMultiFitPlan::Plan> m_multiFitPlan;
    std::optional<VirtualResize::Snapshot> m_multiFitOriginalState;
    std::optional<VirtualResize::Mode> m_multiFitAppliedMode;
    QTimer m_primaryDeadline;
    std::optional<ConsoleWorkerWire::Primary> m_primaryPending;
    std::optional<RetainedMultiPrimaryPlan::Plan> m_primaryPlan;
    QTimer m_mixedDeadline;
    std::optional<ConsoleWorkerWire::Mixed> m_mixedPending;
    std::optional<RetainedMultiMixedPlan::Plan> m_mixedPlan;
    QMap<QString, VirtualResize::Snapshot> m_mixedOriginalStates;
    QMap<QString, VirtualResize::Mode> m_mixedAppliedModes;
    QTimer m_mixedCreateDeadline;
    std::optional<ConsoleWorkerWire::MixedCreate> m_mixedCreatePending;
    std::optional<RetainedMultiMixedCreatePlan::Plan> m_mixedCreatePlan;
    std::optional<RetainedMultiMixedPlan::Plan> m_mixedCreateApplied;
    QMap<QString, VirtualResize::Snapshot> m_mixedCreateOriginalStates;
    QMap<QString, VirtualResize::Mode> m_mixedCreateAppliedModes;
    std::unique_ptr<PlasmaScreencastV1Session> m_mixedCreateCreator;
    bool m_multiResizeNeedsRestart = false;
    QTimer m_addDeadline;
    std::optional<ConsoleWorkerWire::AddVirtual> m_addPending;
    RetainedKScreenReadback::Snapshot m_addBefore;
    std::unique_ptr<PlasmaScreencastV1Session> m_addCreator;
    std::vector<std::pair<QString, std::unique_ptr<PlasmaScreencastV1Session>>> m_ownedCreators;
    bool m_addPlaced = false;
    QTimer m_removeDeadline;
    std::optional<ConsoleWorkerWire::RemoveVirtual> m_removePending;
    RetainedKScreenReadback::Snapshot m_removeBefore;
    QVector<VideoFrame> m_multiPublishedFrames;
    quint64 m_multiEpoch = 0;
    quint8 m_multiQuality = 80;
    bool m_multiMode = false;
    bool m_multiReady = false;
    bool m_physicalResizeReadyForCapture = false;
    std::unique_ptr<PipeWireAudioPlayback> m_audio;
    ConsoleMicrophoneSession m_microphone;
    QTimer m_connectTimeout;
    QTimer m_audioTimer;
    bool m_captureReady = false;
    ConsoleWorkerWire::Outputs m_outputs;
    bool m_topologyQueryPending = false;
    std::optional<VideoFrame> m_lastPhysicalKeyframe;
    ConsoleInputState m_inputState;
    QElapsedTimer m_clock;
    Takeover::Detector m_takeover;
    QAction m_reclaimAction;
    ConsoleWorkerWire::ControlState m_control;
    ConsoleResizeSession m_resize;
    VirtualResizeSession m_virtualResize;
    quint64 m_virtualCaptureEpoch = 0;
    bool m_stopping = false;
    int m_exitCode = 0;
};
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    application.setDesktopFileName(QStringLiteral("org.kde.krdpconsoleworker"));
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption socketOption(QStringLiteral("socket"), QStringLiteral("Broker socket path."), QStringLiteral("path"));
    // QGuiApplication consumes --session for its own session restoration.
    const QCommandLineOption sessionOption(QStringLiteral("logind-session"), QStringLiteral("logind session id."), QStringLiteral("id"));
    const QCommandLineOption virtualOption(QStringLiteral("virtual-session"), QStringLiteral("Authenticated virtual registry session UUID; exclusive with --logind-session."), QStringLiteral("id"));
    const QCommandLineOption uidOption(QStringLiteral("uid"), QStringLiteral("Authenticated desktop owner's uid."), QStringLiteral("uid"));
    const QCommandLineOption tokenOption(QStringLiteral("token-hex"), QStringLiteral("Per-launch broker token."), QStringLiteral("token"));
    const QCommandLineOption tokenFdOption(QStringLiteral("token-fd"), QStringLiteral("Read the per-launch broker token once from this inherited fd."), QStringLiteral("fd"));
    const QCommandLineOption desktopOption(QStringLiteral("desktop-media"), QStringLiteral("Broker verified an authenticated user desktop (never a greeter)."));
    parser.addOptions({socketOption, sessionOption, virtualOption, uidOption, tokenOption, tokenFdOption, desktopOption});
    parser.process(application);

    bool uidOk = false;
    const quint32 uid = parser.value(uidOption).toUInt(&uidOk);
    const auto mode = CaptureWorkerMode::parse(parser.value(sessionOption), parser.value(virtualOption));
    QByteArray token;
    if (parser.isSet(tokenFdOption)) {
        bool fdOk = false;
        const int fd = parser.value(tokenFdOption).toInt(&fdOk);
        QFile tokenFile;
        if (!fdOk || fd < 0 || !tokenFile.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            parser.showHelp(1);
        }
        token = tokenFile.readAll();
    } else {
        token = QByteArray::fromHex(parser.value(tokenOption).toLatin1());
    }
    if (parser.isSet(tokenOption) == parser.isSet(tokenFdOption) || parser.isSet(sessionOption) == parser.isSet(virtualOption)
        || !mode || !uidOk || uid == 0 || uid != getuid() || uid != geteuid() || parser.value(socketOption).isEmpty() || token.size() < 16) {
        parser.showHelp(1);
    }

    Worker worker(parser.value(socketOption), *mode, uid, token, parser.isSet(desktopOption));
    worker.connectToBroker();
    return application.exec();
}
