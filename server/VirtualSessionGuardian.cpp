// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionGuardian.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QVariant>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace KRdp
{
VirtualSessionGuardian::VirtualSessionGuardian(QObject *parent) : QObject(parent)
{
    connect(&m_server, &QLocalServer::newConnection, this, &VirtualSessionGuardian::accept);
    connect(&m_process, &QProcess::started, this, [this] {
        if (m_phase == QStringLiteral("stopping")) m_process.terminate();
        else m_phase = QStringLiteral("running");
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](auto error) {
        if (error == QProcess::FailedToStart) m_phase = QStringLiteral("failed");
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_phase = m_phase == QStringLiteral("stopping") || (code == 0 && status == QProcess::NormalExit)
            ? QStringLiteral("exited") : QStringLiteral("failed");
    });
}

VirtualSessionGuardian::~VirtualSessionGuardian()
{
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(3000);
    }
    // Only guardian shutdown owns child teardown. Broker socket loss never does.
    for (auto *client : std::as_const(m_clients)) {
        client->disconnect(this);
        client->abort();
    }
    m_server.close();
}

bool VirtualSessionGuardian::start(quint32 uid, const QString &session, const QByteArray &token,
    const QString &socket, const VirtualSessionSupervisor::Launch &launch, QString *error)
{
    const auto refuse = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    const QFileInfo socketInfo(socket), directory(socketInfo.absolutePath());
    struct stat permissions{};
    if (m_started || !uid || uid != getuid() || uid != geteuid() || token.size() != 32
        || QUuid(session).isNull() || QUuid(session).toString(QUuid::WithoutBraces) != session
        || !QDir::isAbsolutePath(socket) || QDir::cleanPath(socket) != socket
        || socketInfo.exists() || socketInfo.isSymLink() || !directory.isDir()
        || directory.canonicalFilePath() != directory.absoluteFilePath() || directory.ownerId() != uid
        || ::stat(QFile::encodeName(directory.absoluteFilePath()).constData(), &permissions)
        || (permissions.st_mode & 0777) != 0700
        || !QDir::isAbsolutePath(launch.program) || launch.environment.inheritsFromParent()) {
        return refuse(QStringLiteral("Invalid guardian identity, private socket, or trusted launch"));
    }
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(socket)) return refuse(m_server.errorString());
    m_started = true;
    m_uid = uid;
    m_session = session;
    m_token = token;
    m_incarnation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_process.setProgram(launch.program);
    m_process.setArguments(launch.arguments);
    m_process.setProcessEnvironment(launch.environment);
    if (launch.childSetup) m_process.setChildProcessModifier(launch.childSetup);
    m_process.setStandardInputFile(QProcess::nullDevice());
    // The namespace launcher owns its private diagnostic files.
    m_process.setStandardOutputFile(QProcess::nullDevice());
    m_process.setStandardErrorFile(QProcess::nullDevice());
    m_phase = QStringLiteral("starting");
    m_process.start();
    if (error) error->clear();
    return true;
}

bool VirtualSessionGuardian::startPrepared(quint32 uid, const QString &session, const QByteArray &token,
    std::unique_ptr<VirtualSessionStorage> storage, const VirtualSessionSupervisor::Launch &launch, QString *error)
{
    if (m_started || !storage || storage->ownerUid() != uid || storage->sessionId() != session) {
        if (error) *error = QStringLiteral("Guardian requires fresh prepared storage");
        return false;
    }
    const auto socket = storage->runtimeDirectory() + QStringLiteral("/guardian.sock");
    // This object lives in the independent guardian, not the RDP broker. Neither
    // client socket loss nor the child's exec closes the parent's profile lock.
    m_storage = std::move(storage);
    if (start(uid, session, token, socket, launch, error)) return true;
    m_storage.reset();
    return false;
}

void VirtualSessionGuardian::accept()
{
    while (auto *socket = m_server.nextPendingConnection()) {
        struct ucred peer{};
        socklen_t size = sizeof(peer);
        if (m_clients.size() >= 8 || getsockopt(socket->socketDescriptor(), SOL_SOCKET, SO_PEERCRED, &peer, &size)
            || (peer.uid != 0 && peer.uid != m_uid)) {
            socket->abort(); socket->deleteLater(); continue;
        }
        socket->setParent(this);
        socket->setReadBufferSize(8193);
        m_clients.insert(socket);
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            m_clients.remove(socket); socket->deleteLater();
        });
        // Bounded unauthenticated peers; token possession is required even for root.
        QTimer::singleShot(5000, socket, [socket] {
            if (!socket->property("authenticated").toBool()) socket->abort();
        });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            if (socket->bytesAvailable() > 8192 || socket->bytesToWrite() > 8192) { socket->abort(); return; }
            while (socket->canReadLine()) {
                if (socket->bytesToWrite() > 8192) { socket->abort(); return; }
                const int count = socket->property("requests").toInt() + 1;
                socket->setProperty("requests", count);
                if (count > 64) { socket->abort(); return; }
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(socket->readLine(8193), &error);
                if (error.error != QJsonParseError::NoError || !document.isObject()) { socket->abort(); return; }
                const auto reply = request(document.object());
                socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                if (!reply.value(QStringLiteral("ok")).toBool()) {
                    socket->disconnectFromServer(); return;
                }
                socket->setProperty("authenticated", true);
            }
        });
    }
}

QJsonObject VirtualSessionGuardian::request(const QJsonObject &record)
{
    QJsonObject reply{{QStringLiteral("v"), 1}, {QStringLiteral("id"), record.value(QStringLiteral("id"))},
        {QStringLiteral("ok"), false}};
    static const QRegularExpression id(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    const auto operation = record.value(QStringLiteral("op")).toString();
    const bool stopping = operation == QStringLiteral("stop");
    const QByteArray encoded = record.value(QStringLiteral("token")).toString().toLatin1();
    const QByteArray token = QByteArray::fromHex(encoded);
    unsigned int difference = 0;
    if (token.size() == m_token.size()) {
        for (qsizetype i = 0; i < token.size(); ++i) difference |= static_cast<unsigned char>(token[i] ^ m_token[i]);
    } else difference = 1;
    if (record.size() != (stopping ? 6 : 5) || record.value(QStringLiteral("v")) != QJsonValue(1)
        || !id.match(record.value(QStringLiteral("id")).toString()).hasMatch()
        || record.value(QStringLiteral("session")) != m_session || encoded != token.toHex() || difference
        || (!stopping && operation != QStringLiteral("status"))
        || (stopping && record.value(QStringLiteral("instance")) != m_incarnation)) return reply;
    if (stopping) stop();
    reply.insert(QStringLiteral("ok"), true);
    reply.insert(QStringLiteral("session"), m_session);
    reply.insert(QStringLiteral("instance"), m_incarnation);
    reply.insert(QStringLiteral("state"), m_phase);
    reply.insert(QStringLiteral("processRunning"), m_process.state() == QProcess::Running);
    return reply;
}

void VirtualSessionGuardian::stop()
{
    if (m_phase == QStringLiteral("stopping") || m_process.state() == QProcess::NotRunning) return;
    m_phase = QStringLiteral("stopping");
    m_process.terminate();
    QTimer::singleShot(5000, &m_process, [this] {
        if (m_process.state() != QProcess::NotRunning) m_process.kill();
    });
}
}
