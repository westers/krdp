// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "Server.h"
#include "StallPhase.h"

#include <atomic>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <vector>

#include <QCoreApplication>
#include <QSocketNotifier>
#include <QTimer>

#include <freerdp/channels/channels.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/freerdp.h>
#include <winpr/ssl.h>

#include "PipeWireAudioPlayback.h"
#include "RdpConnection.h"

#include "krdp_logging.h"

using namespace KRdp;

class KRDP_NO_EXPORT Server::Private
{
public:
    std::vector<std::unique_ptr<RdpConnection>> sessions;
    rdp_settings *settings = nullptr;

    QHostAddress address = QHostAddress::LocalHost;
    quint16 port = 3389;

    QList<User> users;
    bool usePamAuthentication = false;
    bool allowAnyPamUser = false;
    std::function<bool(quint32)> pamAdmission;
    std::function<std::optional<quint32>(const QString &, const QString &)> mappedCredentials;

    std::filesystem::path tlsCertificate;
    std::filesystem::path tlsCertificateKey;
    QString cameraLoopbackDevice;
    std::atomic<bool> standardClientMedia = true;
    std::chrono::milliseconds handshakeTimeout = std::chrono::seconds(15);

    // OPT-055 K3.1: accept-error recovery. Qt pauses accepting after an error
    // such as EMFILE and never resumes by itself.
    using Clock = std::chrono::steady_clock;
    QTimer acceptRetry;
    std::chrono::milliseconds acceptBackoff = std::chrono::seconds(1);
    bool acceptEpisode = false; // an error episode is being reported
    bool acceptResumed = false; // resumeAccepting() ran, no connection since
    int acceptErrors = 0;
    int acceptedSinceError = 0;
    bool leaveScheduled = false;
    // I1: Qt 6.10 latches TemporaryError after the first accept, so an EMFILE
    // then neither pauses the listener nor emits acceptError and the listener's
    // read notifier spins. This second notifier notices the shortage and pauses
    // accepting ourselves. It watches a dup() of the listening socket: Qt's own
    // UNIX dispatcher (QT_NO_GLIB=1) supports only one notifier per descriptor
    // number and would otherwise stop the listener's accepts (N1).
    std::unique_ptr<QSocketNotifier> acceptWatch;
    qintptr watchedDescriptor = -1; // the listener's descriptor the duplicate was made from
    int watchDuplicate = -1;

    void dropAcceptWatch()
    {
        acceptWatch.reset(); // unregisters before the descriptor is closed
        if (watchDuplicate >= 0) {
            ::close(watchDuplicate);
            watchDuplicate = -1;
        }
    }
    DIR *fdDir = nullptr; // kept open: /proc/self/fd cannot be opened at EMFILE

    void endAcceptEpisode()
    {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - acceptEpisodeStart).count();
        qCWarning(KRDP) << "Accepting connections again after" << acceptErrors << "errors in" << seconds << "s";
        acceptEpisode = false;
        acceptResumed = false;
        acceptedSinceError = 0;
    }
    Clock::time_point acceptEpisodeStart;
    Clock::time_point lastAcceptError;
    bool haveAcceptError = false;
    // K3.2
    Clock::time_point lastPressureWarning;
    bool pressureWarned = false;
};

namespace
{
constexpr std::chrono::milliseconds kAcceptBackoffStart = std::chrono::seconds(1);
constexpr std::chrono::milliseconds kAcceptBackoffMax = std::chrono::seconds(10);
constexpr std::chrono::seconds kAcceptBackoffReset = std::chrono::seconds(60);
constexpr std::chrono::minutes kPressureWarningInterval = std::chrono::minutes(10);

int openFileDescriptorCount(DIR *dir)
{
    if (!dir) {
        return -1;
    }
    ::rewinddir(dir);
    int count = 0;
    while (::readdir(dir)) {
        ++count;
    }
    return count - 3; // ".", ".." and the directory stream itself
}

rlim_t softFileLimit()
{
    rlimit limit{};
    return ::getrlimit(RLIMIT_NOFILE, &limit) == 0 ? limit.rlim_cur : RLIM_INFINITY;
}
}

Server::Server(QObject *parent)
    : QTcpServer(parent)
    , d(std::make_unique<Private>())
{
    winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());

    d->fdDir = ::opendir("/proc/self/fd");
    d->acceptRetry.setSingleShot(true);
    connect(&d->acceptRetry, &QTimer::timeout, this, [this]() {
        d->acceptResumed = true;
        if (d->acceptWatch) {
            d->acceptWatch->setEnabled(true);
        }
        if (isListening()) {
            resumeAccepting();
        }
    });
    // K3.1/K3.2 bookkeeping runs on newConnection (emitted after every
    // incomingConnection, including overrides).
    connect(this, &QTcpServer::newConnection, this, [this]() {
        const auto now = Private::Clock::now();
        ensureAcceptWatch();
        ++d->acceptedSinceError;
        if (d->acceptEpisode && d->acceptResumed) {
            if (!d->leaveScheduled) {
                // Confirmed one event-loop turn later: an error right after this
                // connection means the episode is not over.
                d->leaveScheduled = true;
                QTimer::singleShot(0, this, [this]() {
                    d->leaveScheduled = false;
                    if (d->acceptEpisode && d->acceptResumed) {
                        d->endAcceptEpisode();
                    }
                });
            }
        }
        // K3.2: warn once per interval when descriptors run short.
        const rlim_t limit = softFileLimit();
        if (limit != RLIM_INFINITY && limit > 0) {
            const int open = openFileDescriptorCount(d->fdDir);
            if (open >= 0 && qulonglong(open) * 10 >= qulonglong(limit) * 8 && (!d->pressureWarned || now - d->lastPressureWarning >= kPressureWarningInterval)) {
                d->pressureWarned = true;
                d->lastPressureWarning = now;
                qCWarning(KRDP) << "File descriptor pressure:" << open << "of" << qulonglong(limit) << "in use";
            }
        }
    });
    connect(this, &QTcpServer::acceptError, this, [this](QAbstractSocket::SocketError error) {
        const auto now = Private::Clock::now();
        // Qt paused the listener: our watch must not spin on its readable
        // backlog meanwhile. The retry timer re-enables it.
        if (d->acceptWatch) {
            d->acceptWatch->setEnabled(false);
        }
        // With descriptors to spare and connections accepted since the last
        // error this is not EMFILE: Qt 6 reports a stale error after draining
        // the backlog that followed an earlier one, and pauses again. Resume
        // soon and silently; an open episode is over.
        const int openFds = openFileDescriptorCount(d->fdDir);
        const rlim_t limit = softFileLimit();
        const bool headroom = openFds >= 0 && limit != RLIM_INFINITY && qulonglong(openFds) + 4 <= qulonglong(limit);
        if (d->acceptedSinceError > 0 && headroom) {
            d->acceptedSinceError = 0;
            if (d->acceptEpisode) {
                d->endAcceptEpisode();
            }
            d->haveAcceptError = true;
            d->lastAcceptError = now;
            d->acceptBackoff = kAcceptBackoffStart;
            if (!d->acceptRetry.isActive()) {
                d->acceptRetry.start(d->acceptBackoff);
            }
            return;
        }
        reportAcceptFailure(errorString(), int(error), openFds);
    });
}

void Server::ensureAcceptWatch()
{
    const qintptr descriptor = isListening() ? socketDescriptor() : -1;
    if (descriptor == d->watchedDescriptor && (d->acceptWatch || descriptor < 0)) {
        return;
    }
    d->dropAcceptWatch();
    d->watchedDescriptor = descriptor;
    if (descriptor < 0) {
        return;
    }
    // The duplicate is a real descriptor and is counted by openFileDescriptorCount
    // like any other, so the headroom checks below stay exact. It exists before
    // any shortage (created right after the first accept); if even this fails
    // the watch is skipped and the next newConnection retries.
    d->watchDuplicate = ::fcntl(int(descriptor), F_DUPFD_CLOEXEC, 3);
    if (d->watchDuplicate < 0) {
        d->watchedDescriptor = -1;
        return;
    }
    d->acceptWatch = std::make_unique<QSocketNotifier>(d->watchDuplicate, QSocketNotifier::Read);
    connect(d->acceptWatch.get(), &QSocketNotifier::activated, this, [this]() {
        // Cheap when descriptors are available. A failing accept needs one
        // free descriptor for the connection (Qt's reserved spare is not ours
        // to count on).
        const int openFds = openFileDescriptorCount(d->fdDir);
        const rlim_t limit = softFileLimit();
        if (openFds < 0 || limit == RLIM_INFINITY || qulonglong(openFds) + 2 <= qulonglong(limit)) {
            return;
        }
        pauseAccepting();
        d->acceptWatch->setEnabled(false);
        reportAcceptFailure(QStringLiteral("too many open files"), int(QAbstractSocket::SocketResourceError), openFds);
    });
}

void Server::reportAcceptFailure(const QString &what, int error, int openFds)
{
    const auto now = Private::Clock::now();
    if (d->acceptWatch) {
        d->acceptWatch->setEnabled(false); // re-enabled by the retry timer
    }
    d->acceptedSinceError = 0;
    if (!d->acceptEpisode) {
        if (!d->haveAcceptError || now - d->lastAcceptError > kAcceptBackoffReset) {
            d->acceptBackoff = kAcceptBackoffStart;
        }
        d->acceptEpisode = true;
        d->acceptErrors = 0;
        d->acceptEpisodeStart = now;
        qCWarning(KRDP) << "Accepting connections failed:" << what << "(" << error << "), open file descriptors" << openFds
                        << "of soft limit" << qulonglong(softFileLimit()) << "; will retry with back-off";
    }
    ++d->acceptErrors;
    d->haveAcceptError = true;
    d->lastAcceptError = now;
    d->acceptResumed = false;
    if (!d->acceptRetry.isActive()) {
        d->acceptRetry.start(d->acceptBackoff);
        d->acceptBackoff = std::min(d->acceptBackoff * 2, kAcceptBackoffMax);
    }
}

Server::~Server()
{
    StallPhase stallPhase("~Server");
    stop();
    d->acceptRetry.stop();
    if (d->fdDir) {
        ::closedir(d->fdDir);
        d->fdDir = nullptr;
    }
    // Join every session first: each one hands its playback endpoint to the
    // background stop queue on close (AUD-D1). Then let those jobs restore
    // the host's default sink before the process can exit.
    // Moved out first: a closing session may still emit stateChanged and the
    // handler must not walk a vector that is being destroyed (E24).
    auto sessions = std::move(d->sessions);
    d->sessions.clear();
    sessions.clear();
    if (!PipeWireAudioPlayback::waitForPendingStops(10000)) {
        qCWarning(KRDP) << "PipeWire audio endpoints were still stopping at shutdown";
    }
}

bool Server::tlsFilesUsable(const std::filesystem::path &certificatePath, const std::filesystem::path &keyPath)
{
    auto *certificate = freerdp_certificate_new_from_file(certificatePath.string().data());
    if (!certificate) {
        qCCritical(KRDP) << "The TLS certificate" << QString::fromStdString(certificatePath.string()) << "cannot be read or is not a valid certificate";
        return false;
    }
    freerdp_certificate_free(certificate);

    auto *key = freerdp_key_new_from_file(keyPath.string().data());
    if (!key) {
        qCCritical(KRDP) << "The TLS key" << QString::fromStdString(keyPath.string()) << "cannot be read or is not a valid private key";
        return false;
    }
    freerdp_key_free(key);
    return true;
}

bool Server::start()
{
    if (!std::filesystem::exists(d->tlsCertificate) || !std::filesystem::exists(d->tlsCertificateKey)) {
        qCCritical(KRDP).nospace() << "A valid TLS certificate (" << QString::fromStdString(d->tlsCertificate.filename().string()) << ") and key ("
                                   << QString::fromStdString(d->tlsCertificateKey.filename().string()) << ") is required for the server to run!";
        return false;
    }
    // AUD-P3: existing is not enough. Every connection loads both files again
    // (RdpConnection::initialize()), so an unreadable or unparsable pair would
    // otherwise only show up as each client being dropped after TCP accept.
    if (!tlsFilesUsable(d->tlsCertificate, d->tlsCertificateKey)) {
        return false;
    }

    if (!listen(d->address, d->port)) {
        // NOTE: We cannot use QTcpServer methods to get the server address and port because it won't initialize them if listen fails.
        qCCritical(KRDP) << "Unable to listen for connections on" << d->address << d->port << errorString();
        return false;
    }

    // FreeRDP3 tries to use a global instance of the settings object when
    // initializing a new peer. However, it seems to fail at actually creating a
    // global default instance. So create one here and use that.
    d->settings = freerdp_settings_new(FREERDP_SETTINGS_SERVER_MODE);

    qCInfo(KRDP) << "Listening for connections on" << serverAddress() << serverPort();
    return true;
}

void Server::stop()
{
    d->dropAcceptWatch(); // before close(): the descriptor number may be reused
    d->watchedDescriptor = -1;
    close();

    if (d->settings) {
        freerdp_settings_free(d->settings);
        d->settings = nullptr;
    }
}

QHostAddress Server::address() const
{
    return d->address;
}

void Server::setAddress(const QHostAddress &newAddress)
{
    if (newAddress == d->address) {
        return;
    }

    d->address = newAddress;
}

quint16 Server::port() const
{
    return d->port;
}

void Server::setPort(quint16 newPort)
{
    if (newPort == d->port) {
        return;
    }

    d->port = newPort;
}

QList<User> KRdp::Server::users() const
{
    return d->users;
}

void KRdp::Server::setUsers(const QList<User> &users)
{
    d->users = users;
}

void KRdp::Server::addUser(const User &user)
{
    d->users.append(user);
}

bool KRdp::Server::matchesConfiguredUser(const QString &name, const QString &password) const
{
    if (d->mappedCredentials) return false; // Broker grants must carry their explicit desktop-owner scope.
    for (const auto &user : std::as_const(d->users)) {
        if (user.name.isEmpty() || user.password.isEmpty()) {
            continue;
        }
        if (user.name == name && user.password == password) {
            return true;
        }
    }
    return false;
}

std::chrono::milliseconds Server::handshakeTimeout() const
{
    return d->handshakeTimeout;
}

void Server::setHandshakeTimeout(std::chrono::milliseconds timeout)
{
    d->handshakeTimeout = timeout;
}

bool Server::usePAMAuthentication() const
{
    return d->usePamAuthentication;
}

void Server::setUsePAMAuthentication(bool usePAM)
{
    d->usePamAuthentication = usePAM;
}

bool Server::allowAnyPAMUser() const
{
    return d->allowAnyPamUser;
}

void Server::setAllowAnyPAMUser(bool allow)
{
    d->allowAnyPamUser = allow;
}

bool Server::setBrokerAuthenticationPolicy(bool usePAM, std::function<bool(quint32)> pamAdmission,
    std::function<std::optional<quint32>(const QString &, const QString &)> mappedCredentials)
{
    if (isListening() || !d->sessions.empty() || !pamAdmission || !mappedCredentials) return false;
    d->usePamAuthentication = usePAM;
    d->allowAnyPamUser = true;
    d->pamAdmission = std::move(pamAdmission);
    d->mappedCredentials = std::move(mappedCredentials);
    return true;
}

bool Server::acceptsPamIdentity(quint32 uid) const
{
    return !d->pamAdmission || d->pamAdmission(uid);
}

std::optional<quint32> Server::mappedCredentialIdentity(const QString &name, const QString &password) const
{
    if (!d->mappedCredentials) return {};
    const auto uid = d->mappedCredentials(name, password);
    return uid && *uid && *uid != quint32(-1) ? uid : std::nullopt;
}

std::filesystem::path Server::tlsCertificate() const
{
    return d->tlsCertificate;
}

void Server::setTlsCertificate(const std::filesystem::path &newTlsCertificate)
{
    if (newTlsCertificate == d->tlsCertificate) {
        return;
    }

    d->tlsCertificate = newTlsCertificate;
}

std::filesystem::path Server::tlsCertificateKey() const
{
    return d->tlsCertificateKey;
}

void Server::setTlsCertificateKey(const std::filesystem::path &newTlsCertificateKey)
{
    if (newTlsCertificateKey == d->tlsCertificateKey) {
        return;
    }

    d->tlsCertificateKey = newTlsCertificateKey;
}

QString Server::cameraLoopbackDevice() const
{
    return d->cameraLoopbackDevice;
}

void Server::setCameraLoopbackDevice(const QString &device)
{
    d->cameraLoopbackDevice = device.trimmed();
}

bool Server::standardClientMedia() const
{
    return d->standardClientMedia.load();
}

void Server::setStandardClientMedia(bool enabled)
{
    d->standardClientMedia.store(enabled);
}

void Server::incomingConnection(qintptr handle)
{
    auto session = std::make_unique<RdpConnection>(this, handle);
    auto sessionPtr = session.get();
    connect(sessionPtr, &RdpConnection::stateChanged, this, [this, sessionPtr](RdpConnection::State state) {
        if (state == RdpConnection::State::Closed) {
            auto itr = std::find_if(d->sessions.begin(), d->sessions.end(), [sessionPtr](auto &session) {
                return session.get() == sessionPtr;
            });
            if (itr == d->sessions.end()) {
                return; // already removed (re-entrant Closed)
            }
            // Out of the vector first: close() may re-enter this handler.
            auto dying = std::move(*itr);
            d->sessions.erase(itr);
            dying->close();
            dying.reset();
        }
    });
    d->sessions.push_back(std::move(session));
    Q_EMIT newConnectionCreated(sessionPtr);
}

rdp_settings *Server::rdpSettings() const
{
    return d->settings;
}

#include "moc_Server.cpp"
