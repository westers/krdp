// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>
#include <array>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QElapsedTimer>

#include "ConsoleWorkerWire.h"

namespace KRdp
{
class ConsoleWorkerEndpoint;
class ConsoleWorkerSession;
class VideoStream;

/**
 * AUD-FIX7: the codec policy of a connection whose encoder runs in a console or virtual worker.
 *
 * The policy stays where krdpserver has it: in the connection's VideoStream in the broker, which
 * has the link measurements (goodput, RTT, the in-flight window) the slow-link rule and the
 * delivery throttle need, and the KRDPCTL channel for `codec` replies and pushes. The worker is
 * the encoder:
 * - its own probe (EncoderCaps, right after Hello) replaces the broker's estimate
 *   (VideoStream::updateEncoderPolicy()): it runs as the desktop's user, on the render node the
 *   desktop really has, in the process that encodes;
 * - the stream's codec, encoder settings and frame rate go to it (EncoderConfig) for the control
 *   generation this bridge is bound with;
 * - its encoder events (EncoderReport) come back as the proxy session's AbstractSession signals,
 *   connected to the stream exactly as krdpserver connects a local session's;
 * - its process CPU time (EncoderLoad) is the CPU guard's input;
 * - while the connection's KRDPCTL client is subscribed to stats, EncoderConfig asks for
 *   EncoderStats (STATS-S6), which feed the stream's `stats-sample` records.
 */
class WorkerCodecBridge : public QObject
{
    Q_OBJECT
public:
    WorkerCodecBridge(VideoStream *stream, ConsoleWorkerSession *session, QObject *parent = nullptr);
    ~WorkerCodecBridge() override;

    /** Start steering \a endpoint's worker for control \a generation; sends the current config. */
    void bind(ConsoleWorkerEndpoint *endpoint, quint64 generation);
    /** Stop steering (the worker resets itself when its control generation changes). */
    void unbind();
    bool bound() const;
    /** Send the config again even if unchanged (a new worker behind the same endpoint). */
    void resend();
    /** What would be sent now (tests). */
    std::optional<ConsoleWorkerWire::EncoderConfig> config() const;
    /** Validated connection policy; unchanged on invalid input. Main thread. */
    bool setChromaPolicy(const ChromaPolicy &policy);
    ChromaPolicy chromaPolicy() const { return m_chromaPolicy; }
    bool setCapturePolicy(const MonitorCapturePolicy &policy);
    // OPT-060: while Replace is enabled the host's physical screens are off and the worker captures its own
    // configured outputs, so the user's MonitorMode (a choice among physical outputs) is moot for this
    // connection. The stored preference is kept and takes effect again when Replace ends or fails open.
    MonitorCapturePolicy capturePolicy() const { return m_consoleVirtualPolicy.enabled ? MonitorCapturePolicy{} : m_capturePolicy; }
    bool setConsoleVirtualPolicy(const ConsoleVirtualOutputPolicy &policy);
    ConsoleVirtualOutputPolicy consoleVirtualPolicy() const { return m_consoleVirtualPolicy; }

private:
    void send(bool force);
    void applyCaps(const ConsoleWorkerWire::EncoderCaps &caps);

    QPointer<VideoStream> m_stream;
    QPointer<ConsoleWorkerSession> m_session;
    QPointer<ConsoleWorkerEndpoint> m_endpoint;
    quint64 m_generation = 0;
    ChromaPolicy m_chromaPolicy;
    MonitorCapturePolicy m_capturePolicy;
    ConsoleVirtualOutputPolicy m_consoleVirtualPolicy;
    std::optional<ConsoleWorkerWire::EncoderConfig> m_sent;
    QList<QMetaObject::Connection> m_endpointConnections;
    std::array<QElapsedTimer, 16> m_costLogs;
};
}
