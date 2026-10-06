// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once
#include "ConsoleMicrophoneWire.h"
#include "ConsoleCameraWire.h"

#include <memory>

#include <QObject>
#include <QPointer>
#include <QTimer>

#include "ConsoleHandoff.h"
#include <QJsonObject>
#include "ConsoleWorkerWire.h"

class QLocalServer;
class QLocalSocket;
class ConsoleWorkerEndpointHardeningTest;

namespace KRdp
{
/**
 * Broker-side endpoint for one greeter or desktop capture worker.
 *
 * The caller supplies an unguessable per-launch token and a socket pathname in
 * that worker's runtime directory. A connection cannot send a frame or accept
 * input until its Hello matches both the selected logind session and token.
 */
class ConsoleWorkerEndpoint : public QObject
{
    Q_OBJECT

public:
    explicit ConsoleWorkerEndpoint(QObject *parent = nullptr);
    ~ConsoleWorkerEndpoint() override;

    /// Pre-authentication limits (AUD-C-9).
    static constexpr int AuthenticationTimeoutMs = 5000;
    static constexpr qsizetype MaxPreAuthenticationBytes = 64 * 1024;
    /// AUD-FIX8: encoder reports held between Hello and Ready (applied right after Ready).
    static constexpr qsizetype MaxEarlyReports = 64;

    bool listen(const QString &socketName, const ConsoleHandoff::Target &target, const QByteArray &token, QString *error = nullptr);
    void close();
    bool ready() const;
    bool authenticated() const;
    /** Stop was requested but could not be delivered yet (worker not authenticated). */
    bool stopPending() const;
    void setAuthenticationTimeout(int milliseconds);
    QString socketName() const;
    ConsoleHandoff::Target target() const;

    void stopWorker();
    void requestKeyFrame();
    bool requestTopology();
    void sendInput(const ConsoleWorkerWire::Input &input);
    void setMedia(const ConsoleWorkerWire::Media &media);
    void reclaimConsole(quint64 generation);
    void setControlState(const ConsoleWorkerWire::ControlState &state);
    bool setPointerCapture(const QJsonObject &request);
    QJsonObject pointerState() const { return m_pointerState; }
    /** Authenticated demand, including before Ready when DPMS can block capture.
     * Latest desired state is sent when Hello authenticates. No input grant. */
    bool setDisplayPolicy(bool active, bool wakeEnabled);
    bool setVideoQuality(const ConsoleWorkerWire::VideoQuality &quality);
    /** AUD-FIX7: the controlling connection's codec, encoder settings and frame rate. */
    bool setEncoderConfig(const ConsoleWorkerWire::EncoderConfig &config);
    /** AUD-FIX7: the worker's own encoder probe (sent after Hello); nullopt until it arrives. */
    std::optional<ConsoleWorkerWire::EncoderCaps> encoderCaps() const
    {
        return m_encoderCaps;
    }
    /** AUD-FIX7: the worker's last reported process CPU time (ns), -1 = none yet. */
    qint64 workerCpuNs() const
    {
        return m_workerCpuNs;
    }
    /** FIX-CURSOR: the worker's current cursor shape (latest Cursor record); nullopt until one arrives. */
    std::optional<ConsoleWorkerWire::CursorShape> cursorShape() const
    {
        return m_cursorShape;
    }
    bool setMicrophone(const ConsoleWorkerWire::MicrophonePolicy &policy);
    bool sendMicrophoneAudio(const ConsoleWorkerWire::MicrophoneAudio &audio);
    bool setCamera(const ConsoleWorkerWire::CameraPolicy &policy);
    bool setCameraFormat(const ConsoleWorkerWire::CameraFormat &format);
    bool sendCameraFrame(const ConsoleWorkerWire::CameraFrame &sample);
    bool resize(const ConsoleWorkerWire::Resize &request);
    bool position(const ConsoleWorkerWire::Position &request);
    bool positionBatch(const ConsoleWorkerWire::PositionBatch &request);
    bool managedFit(const ConsoleWorkerWire::ManagedFit &request);
    bool primary(const ConsoleWorkerWire::Primary &request);
    bool mixed(const ConsoleWorkerWire::Mixed &request);
    bool mixedCreate(const ConsoleWorkerWire::MixedCreate &request);
    bool physicalLayout(const ConsoleWorkerWire::PhysicalLayout &request);
    bool addVirtual(const ConsoleWorkerWire::AddVirtual &request);
    bool removeVirtual(const ConsoleWorkerWire::RemoveVirtual &request);

Q_SIGNALS:
    void workerAuthenticated(const KRdp::ConsoleHandoff::Target &target);
    void workerReady(const KRdp::ConsoleHandoff::Target &target);
    void workerStopped();
    void frameReceived(const KRdp::VideoFrame &frame);
    void inputReceived(const KRdp::ConsoleWorkerWire::Input &input);
    void audioReceived(const KRdp::ConsoleWorkerWire::Audio &audio);
    void outputsReceived(const KRdp::ConsoleWorkerWire::Outputs &outputs);
    void topologyReceived(const KRdp::ConsoleWorkerWire::Topology &topology);
    void localTakeover(quint64 generation);
    /** M-8: the worker restored the host's screens because a host screen was added (wire 15). */
    void hostScreensChanged(quint64 generation);
    void resizeFinished(const KRdp::ConsoleWorkerWire::ResizeResult &result);
    void positionFinished(const KRdp::ConsoleWorkerWire::PositionResult &result);
    void positionBatchFinished(const KRdp::ConsoleWorkerWire::PositionBatchResult &result);
    void managedFitFinished(const KRdp::ConsoleWorkerWire::ManagedFitResult &result);
    void primaryFinished(const KRdp::ConsoleWorkerWire::PrimaryResult &result);
    void mixedFinished(const KRdp::ConsoleWorkerWire::MixedResult &result);
    void mixedCreateFinished(const KRdp::ConsoleWorkerWire::MixedCreateResult &result);
    void physicalLayoutFinished(const KRdp::ConsoleWorkerWire::PhysicalLayoutResult &result);
    void physicalLeaseReleased(const KRdp::ConsoleWorkerWire::PhysicalLeaseReleased &result);
    void addVirtualFinished(const KRdp::ConsoleWorkerWire::AddVirtualResult &result);
    void removeVirtualFinished(const KRdp::ConsoleWorkerWire::RemoveVirtualResult &result);
    void microphoneFinished(const KRdp::ConsoleWorkerWire::MicrophoneResult &result);
    void cameraFinished(const KRdp::ConsoleWorkerWire::CameraResult &result);
    void cameraDemand(const KRdp::ConsoleWorkerWire::CameraDemand &demand);
    void encoderCapsReceived(const KRdp::ConsoleWorkerWire::EncoderCaps &caps);
    void encoderReported(const KRdp::ConsoleWorkerWire::EncoderReport &report);
    /** STATS-S6: the worker's EncoderStats (only after Ready; one before Ready fails the worker). */
    void encoderStatsReceived(const KRdp::ConsoleWorkerWire::EncoderStats &stats);
    void chromaTimingReceived(const KRdp::ConsoleWorkerWire::ChromaTiming &timing);
    /** FIX-CURSOR: the desktop's cursor shape changed (also in cursorShape()). */
    void pointerStateReceived(const QJsonObject &state);
    void cursorShapeReceived(const KRdp::ConsoleWorkerWire::CursorShape &shape);
    void protocolError(const QString &message);
    /** The worker speaks another paired wire version; always followed by protocolError. */
    void versionMismatch(quint16 workerVersion);

private:
    friend class ::ConsoleWorkerEndpointHardeningTest;
    void acceptConnection();
    void readWorker();
    bool processRecords();
    void workerDisconnected();
    void send(ConsoleWorkerWire::Kind kind);
    void fail(const QString &message);
    void dropWorker();

    std::unique_ptr<QLocalServer> m_server;
    QPointer<QLocalSocket> m_worker;
    ConsoleWorkerWire::Deframer m_deframer;
    ConsoleWorkerWire::ControlState m_pointerControl;
    QJsonObject m_pointerState;
    ConsoleHandoff::Target m_target;
    QByteArray m_token;
    bool m_authenticated = false;
    bool m_ready = false;
    bool m_stopRequested = false;
    QTimer m_authenticationDeadline;
    std::optional<ConsoleWorkerWire::EncoderCaps> m_encoderCaps;
    QVector<ConsoleWorkerWire::EncoderReport> m_earlyReports;
    qint64 m_workerCpuNs = -1;
    std::optional<ConsoleWorkerWire::CursorShape> m_cursorShape;
    std::optional<ConsoleWorkerWire::DisplayPolicy> m_displayPolicy;
};
}
