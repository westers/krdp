// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <chrono>
#include <filesystem>
#include <memory>

#include <QTcpServer>

#include <freerdp/settings.h>

#include "krdp_export.h"

namespace KRdp
{

class RdpConnection;

/**
 * Data required per user that is allowed to connect to the server.
 */
struct User {
    QString name; ///< The user name used to connect.
    QString password; ///< The password for the user.
    bool readOnly = false; ///< Whether this user is allowed to control the session.
};

/**
 * Core RDP server class.
 *
 * This class listens for TCP connections and creates a new @c Session for each
 * incoming connection. It takes care of basic system initialisation. It also
 * stores connection and security settings.
 */
class KRDP_EXPORT Server : public QTcpServer
{
    Q_OBJECT

public:
    explicit Server(QObject *parent = nullptr);
    ~Server() override;

    /**
     * Start listening for incoming connections.
     *
     * Note that `address` and `port` should be set before calling this,
     * changing them after the server has started listening has no effect.
     */
    bool start();
    /**
     * Whether \a certificatePath and \a keyPath can be read and parse as a
     * certificate and a private key. start() refuses to listen otherwise.
     */
    static bool tlsFilesUsable(const std::filesystem::path &certificatePath, const std::filesystem::path &keyPath);
    /**
     * Stop listening for incoming connections.
     */
    void stop();

    /**
     * The host address to listen on.
     *
     * Set this to an appropriate address for the server to listen on. Common
     * options are `0.0.0.0` to listen on all interfaces and accept all incoming
     * connections or `127.0.0.1` to only listen on the loopback interface and
     * only allow local connections.
     *
     * By default the address is set to QHostAddress::LocalHost
     */
    QHostAddress address() const;
    void setAddress(const QHostAddress &newAddress);

    /**
     * The port to listen on.
     *
     * By default this is set to 3389, which is the standard port used for RDP.
     */
    quint16 port() const;
    void setPort(quint16 newPort);

    /**
     * The list of users allowed to log in to the server.
     *
     * At least one user is required for the server to work.
     */
    QList<User> users() const;
    void setUsers(const QList<User> &users);
    void addUser(const User &user);
    /**
     * Whether @p name and @p password match one of users(). An entry with an
     * empty name or password never matches and is skipped (AUD-S3): it does
     * not keep the entries after it from logging in.
     */
    bool matchesConfiguredUser(const QString &name, const QString &password) const;

    /**
     * How long a connection may take from accept to successful PostConnect
     * authentication before it is closed (AUD-S2). Default 15 s.
     */
    std::chrono::milliseconds handshakeTimeout() const;
    void setHandshakeTimeout(std::chrono::milliseconds timeout);

    /** Whether to authenticate against PAM for the user running the daemon
     */
    bool usePAMAuthentication() const;
    void setUsePAMAuthentication(bool usePAM);
    bool allowAnyPAMUser() const;
    void setAllowAnyPAMUser(bool allow);

    /**
     * The path of a certificate file to use for encrypting communications.
     *
     * This is required to be set to a valid file, as the RDP login process only
     * works over an encrypted connection.
     */
    std::filesystem::path tlsCertificate() const;
    void setTlsCertificate(const std::filesystem::path &newTlsCertificate);

    /**
     * The path of a certificate key to use for encrypting communications.
     *
     * This is required to be set to a valid file, as the RDP login process only
     * works over an encrypted connection.
     */
    std::filesystem::path tlsCertificateKey() const;
    void setTlsCertificateKey(const std::filesystem::path &newTlsCertificateKey);

    /** Optional V4L2 loopback device for remote cameras (for applications which
     * cannot consume PipeWire camera sources directly). */
    QString cameraLoopbackDevice() const;
    void setCameraLoopbackDevice(const QString &device);

    /**
     * StandardClientMedia (DEVICES-DESIGN.md §1, default true): a client that
     * never sends a KRDPCTL `device` record gets playback, the microphone and
     * the camera through standard RDP negotiation alone (RdpConnection::
     * applyStandardConsent()). Thread-safe.
     */
    bool standardClientMedia() const;
    void setStandardClientMedia(bool enabled);

    /**
     * Emitted whenever a new connection is started.
     *
     * \param connection The new connection that was just started.
     */
    Q_SIGNAL void newConnectionCreated(RdpConnection *connection);

protected:
    /**
     * Overridden from QTcpServer
     */
    void incomingConnection(qintptr handle) override;

private:
    friend class RdpConnection;
    rdp_settings *rdpSettings() const;

    class Private;
    const std::unique_ptr<Private> d;
};

}
