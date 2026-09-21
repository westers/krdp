// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>
#include <thread>

#include <QJsonObject>
#include <QObject>

#include <freerdp/freerdp.h>

#include "ClientDisplayInfo.h"
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
    };

    /**
     * Constructor.
     *
     * \param server The KRdp::Server instance this session is part of.
     * \param socketHandle A file handle to the socket this session should use
     *                     for communication.
     */
    explicit RdpConnection(Server *server, qintptr socketHandle);
    ~RdpConnection() override;

    /**
     * The current session state.
     */
    State state() const;
    Q_SIGNAL void stateChanged(State newState);

    /**
     * Close the connection
     *
     * \param reason The reason to close the connection. May set error state if
     *               it is something different than CloseReason::None.
     */
    void close(CloseReason reason = CloseReason::None);

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
    /** Emitted on the session thread, once per connection, at the end of the capabilities exchange. Connect with Qt::QueuedConnection. */
    Q_SIGNAL void clientDisplayInfoReceived();

    NetworkDetection *networkDetection() const;

    /**
     * Whether the client has a usable `KRDPCTL` static virtual channel (slice
     * 2c layout control, OPT-044): it joined it and the server-side open
     * succeeded. Known once the MCS channel join is done, which precedes the
     * capabilities exchange: valid from the moment clientDisplayInfoReceived()
     * fires. Safe from any thread.
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
     * Apply the own client's explicit conferencing consent. Safe from any
     * thread: the session loop opens the selected RDP channels on its next
     * iteration, after the connection is fully active.
     */
    void setMediaPolicy(bool remoteAudioPlayback, bool microphone, bool camera, bool silenceHostAudio = false);
    void setAudioPriority(bool enabled);
    void setAudioPriorityDefault(bool enabled);
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
    bool onSuppressOutput(uint8_t allow);
    /** Session thread: create/open the standard audio channels once joined. */
    bool initializeAudioChannels();
    /** Session thread: open `KRDPCTL` once the client has joined it. */
    void openControlChannel();
    /** Session thread: hand every queued `KRDPCTL` message to the deframer. False on a protocol violation. */
    bool readControlChannel();

    class Private;
    const std::unique_ptr<Private> d;
};

}
