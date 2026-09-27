// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>
#include <optional>
#include <thread>

#include <QJsonObject>
#include <QObject>

#include <freerdp/freerdp.h>

#include "ClientDisplayInfo.h"
#include "DeviceControl.h"
#include "krdp_export.h"

namespace KRdp
{

class InputHandler;
class Server;
class VideoStream;
class Cursor;
class NetworkDetection;
class Clipboard;

/**
 * Select a VAAPI driver (LIBVA_DRIVER_NAME) for the current configuration.
 *
 * Honours KRDP_FORCE_VAAPI_DRIVER / KRDP_AUTO_VAAPI_DRIVER (set from the
 * VaapiDriverMode config key) and, in auto mode, avoids decode-only NVIDIA
 * VAAPI backends on mixed-GPU systems. Safe to call at startup and again on
 * config change; an externally-provided LIBVA_DRIVER_NAME is always respected.
 */
KRDP_EXPORT void selectVaapiDriver();

/**
 * An RDP session.
 *
 * This represents an RDP session, that is, a connection between an RDP client
 * and the server. It primarily takes care of the RDP communication side of
 * things.
 *
 * Note that this class starts its own thread for performing the actual
 * communication.
 */
class KRDP_EXPORT RdpConnection : public QObject
{
    Q_OBJECT

public:
    /**
     * Session state.
     */
    enum class State {
        Initial,
        Starting,
        Running,
        Streaming,
        Closed,
    };

    /**
     * Reasons for closing the stream.
     */
    enum class CloseReason {
        None, ///< No particular reason, e.g. closing due to normal operation
              ///  like client disconnect.
        VideoInitFailed, ///< VideoStream failed to initialize.
        AuthenticationFailed, ///< The credentials were refused: the client is told with the standard
                              ///  Set Error Info ERRINFO_SERVER_INSUFFICIENT_PRIVILEGES (KRDPCTL-V2-CONTRACT.md §c).
    };

    /**
     * Constructor.
     *
     * \param server The KRdp::Server instance this session is part of.
     * \param socketHandle A file handle to the socket this session should use
     *                     for communication. -1 makes a detached object that
     *                     never initializes (used by tests); for any other
     *                     handle a failed initialization closes the socket
     *                     and moves the connection to Closed.
     */
    explicit RdpConnection(Server *server, qintptr socketHandle);
    ~RdpConnection() override;

    /**
     * The current session state.
     */
    State state() const;
    Q_SIGNAL void stateChanged(State newState);

    /** OS identity established by successful PAM authentication/account checks.
     * Thread-safe; empty before completed authentication, for configured test
     * accounts, and after close. Never derived from a control-channel record.
     * UID0 is still subject to the virtual-session manager's root refusal.
     */
    std::optional<quint32> authenticatedPamUid() const;

    /** Whether PostConnect authentication (PAM or a configured user) has
     * succeeded on this connection. Thread-safe; never reset. Until then no
     * channel data, input or control record from the client is delivered.
     */
    bool isAuthenticated() const;

    /** FreeRDP context of this peer (null before it exists), so a host can
     * set a standard error-info disconnect reason before close(). */
    rdpContext *freerdpContext() const;

    /**
     * Close the connection
     *
     * \param reason The reason to close the connection. May set error state if
     *               it is something different than CloseReason::None.
     */
    void close(CloseReason reason = CloseReason::None);

    /**
     * Close the connection with a standard MS-RDPBCGR Set Error Info code
     * (an `ERRINFO_*` value), sent at once and again by the close sequence,
     * so that any stock client (mstsc, FreeRDP, Remmina) shows the reason.
     * Callable from the main thread, like close().
     */
    void closeWithErrorInfo(quint32 errorInfo);

    /**
     * Offer the standard Display Control channel (MS-RDPEDISP) once DRDYNVC is
     * ready, so a client can ask for a new desktop size by resizing its window.
     * Off by default; call before the client authenticates. The requests
     * arrive as displayLayoutRequested().
     */
    void setDisplayControlEnabled(bool enabled);
    /**
     * A DISPLAYCONTROL_MONITOR_LAYOUT_PDU from the client: the monitors it
     * would like, in its own coordinates (already range-checked by FreeRDP).
     * Emitted on the Display Control channel's thread; connect with
     * Qt::QueuedConnection.
     */
    Q_SIGNAL void displayLayoutRequested(const QList<KRdp::VideoMonitor> &monitors);

    /**
     * The InputHandler instance associated with this session.
     */
    InputHandler *inputHandler() const;
    /**
     * The VideoStream instance associated with this session.
     */
    VideoStream *videoStream() const;
    /**
     * The Cursor instance associated with this session.
     */
    Cursor *cursor() const;

    Clipboard *clipboard() const;

    /** The display the client asked for in its connect data. Valid once clientDisplayInfoReceived() fired; a copy, safe from any thread. */
    ClientDisplay::Info clientDisplayInfo() const;
    /**
     * Emitted on the session thread once PostConnect authentication has
     * succeeded (and again after a reactivation's capabilities exchange).
     * Nothing the client sent before authentication reached any handler, and
     * the `KRDPCTL` channel, if the client joined it, is open by now: this is
     * the earliest point a control record (e.g. `capabilities`) can be sent.
     * Connect with Qt::QueuedConnection.
     */
    Q_SIGNAL void clientDisplayInfoReceived();

    NetworkDetection *networkDetection() const;

    /**
     * Whether the client has a usable `KRDPCTL` static virtual channel (slice
     * 2c layout control, OPT-044): it joined it and the server-side open
     * succeeded. The channel is only opened once the client authenticated
     * (AUD-S1): valid from the moment clientDisplayInfoReceived() fires.
     * Safe from any thread.
     */
    bool hasControlChannel() const;
    /**
     * Send one `KRDPCTL` record (framed by LayoutControl::frame()). Callable
     * from any thread: the write only queues the data with FreeRDP's channel
     * manager, and the session thread's loop puts it on the wire. Dropped,
     * with a warning, when the client has no control channel or the
     * connection is already closed.
     */
    void sendControlRecord(const QJsonObject &record);
    /**
     * A KRDPCTL `device` request (DEVICES-DESIGN.md §4). Safe from any thread:
     * it only stores the device's consent (a fresh DeviceConsent generation
     * for every `on` and `reselect`) and \a requestId; the session loop's
     * reconcilers do the standard channel work on their next iteration.
     *
     * The answer is a deviceState() carrying \a requestId, emitted once the
     * request is settled: `on` once the channel is ready and the PipeWire node
     * exists, `off` once teardown finished, `error` (with a code) otherwise,
     * or after the 5 s deadline. A `query` is answered at once with the last
     * state (possibly `starting`). An empty \a requestId still gets its
     * answer, uncorrelated (KRDPCTL-V2-CONTRACT.md §a).
     * \a silenceHost only applies to a playback `on`.
     */
    void requestDevice(MediaDevice device, DeviceControl::Action action, bool silenceHost, const QString &requestId);
    /**
     * Brokers: switch a device's consent without a request (idempotent: the
     * generation only changes when the enabled state does). Any thread. The
     * console and virtual brokers answer their clients themselves.
     */
    void setDeviceEnabled(MediaDevice device, bool enabled, bool silenceHost = false);
    /**
     * StandardClientMedia (DEVICES-DESIGN.md §1): if Server::standardClientMedia()
     * is set, treat standard negotiation as consent - playback if the client
     * joined RDPSND, the microphone and camera if it has dynamic channels (the
     * client's accept of the AUDIN / RDPECAM channel is its consent). Only for
     * a connection that sent no `device` record. Any thread; returns whether
     * it was applied.
     */
    bool applyStandardConsent();
    /** The standard media channels a client joined (MCS join, fixed after connect). */
    struct StandardMediaChannels {
        bool playback = false; // RDPSND
        bool dynamic = false; // DRDYNVC: AUDIN (and RDPECAM) can be offered
    };
    /**
     * Brokers (DEVICES-DESIGN.md §1): StandardClientMedia for a connection whose
     * devices a broker runs through its worker. Empty if Server::standardClientMedia()
     * is off; otherwise the standard channels the client joined (known once it
     * authenticated; nothing before). Any thread.
     */
    std::optional<StandardMediaChannels> standardMediaChannels() const;
    /** The last state deviceState() reported for \a device. Any thread. */
    DeviceStatus deviceStatus(MediaDevice device) const;
    /**
     * Whether the peer is our own client: it joined `KRDPCTL`, got
     * `capabilities` and sent a record back. Only such a peer gets the
     * non-essential RDPSND SNDC_CLOSE when playback is switched off
     * (DEVICES-DESIGN.md §7 risk 1). Any thread.
     */
    bool isOwnClient() const;
    /**
     * A device's state changed, or a request was settled (\a requestId set:
     * the answer to that request; empty: an unsolicited push, e.g. `inUse`, a
     * camera the client removed). Emitted on the session thread (a `query`:
     * on the caller's); connect with Qt::QueuedConnection.
     */
    Q_SIGNAL void deviceState(KRdp::MediaDevice device, const KRdp::DeviceStatus &status, const QString &requestId);
    void setAudioPriority(bool enabled);
    void setAudioPriorityDefault(bool enabled);
    void clearAudioPriorityOverride();
    /** Select a worker-bound microphone destination before initialization. Never opens host PipeWire. */
    bool enableExternalMicrophone();
    /** Drain at most 20ms of fresh, currently consented 48kHz stereo S16 PCM. */
    QByteArray takeExternalMicrophone();
    bool audioPriorityActive() const;
    /** Select PCM supplied by a session worker instead of this process's PipeWire graph. */
    void setExternalAudioPlayback(bool enabled);
    /** Thread-safe 44.1 kHz stereo S16 PCM from a trusted capture worker. */
    void submitExternalAudio(const QByteArray &pcm);
    /**
     * One complete `KRDPCTL` record from the client. Emitted on the session
     * thread; connect with Qt::QueuedConnection.
     */
    Q_SIGNAL void controlRecordReceived(const QJsonObject &record);

private:
    friend BOOL peerCapabilities(freerdp_peer *);
    friend BOOL peerActivate(freerdp_peer *);
    friend BOOL peerPostConnect(freerdp_peer *);
    friend BOOL suppressOutput(rdpContext *, uint8_t, const RECTANGLE_16 *);

    friend class Cursor;
    friend class VideoStream;
    friend class NetworkDetection;
    friend class Clipboard;

    void setState(State newState);
    void initialize();
    void run(std::stop_token stopToken);

    freerdp_peer *rdpPeer() const;
    rdpContext *rdpPeerContext() const;

    bool onCapabilities();
    bool onActivate();
    bool onPostConnect();
    bool onClose();
    /** Session thread, from onPostConnect(): open the pre-authentication gate. */
    void onAuthenticated();
    bool onSuppressOutput(uint8_t allow);
    /** Session thread: bring each device's channels in line with its consent. False only on a fatal error. */
    bool reconcileDevices();
    bool reconcilePlayback();
    bool reconcileMicrophone();
    bool reconcileCamera();
    /** Session thread: close the RDPECAM enumerator, then every camera (each joins its channel thread before its PipeWire node goes). */
    void closeCameras();
    /** Session thread: close AUDIN (joins its thread), then drop the PipeWire source. */
    bool retireMicrophone();
    /** Session thread: record \a status; with \a settledGeneration, answer the requests up to it, else push a change. */
    void publishDevice(MediaDevice device, const DeviceStatus &status, std::optional<uint64_t> settledGeneration = std::nullopt);
    /** Session thread: open `KRDPCTL` once the client has joined it. */
    void openControlChannel();
    /** Session thread: hand every queued `KRDPCTL` message to the deframer. False on a protocol violation. */
    bool readControlChannel();

    /** Session thread: open MS-RDPEDISP once DRDYNVC is ready (setDisplayControlEnabled()). */
    void openDisplayControl();
    void closeDisplayControl();

    class Private;
    const std::unique_ptr<Private> d;
};

}
