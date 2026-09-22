// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionGuardianClient.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <algorithm>
#include <sys/socket.h>
#include <sys/stat.h>

namespace KRdp {
namespace {
bool canonicalUuid(const QString &id)
{
    return !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id;
}
}
VirtualSessionGuardianClient::VirtualSessionGuardianClient(QObject *parent, int timeoutMs)
    : QObject(parent)
{
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(std::clamp(timeoutMs, 50, 10000));
    m_socket.setReadBufferSize(8193);
    connect(&m_deadline, &QTimer::timeout, this, [this] { fail(QStringLiteral("Guardian request timed out")); });
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        if (!m_busy) return;
        struct ucred peer{};
        socklen_t size = sizeof(peer);
        if (getsockopt(m_socket.socketDescriptor(), SOL_SOCKET, SO_PEERCRED, &peer, &size)
            || size != sizeof(peer) || peer.uid != m_identity.uid) {
            fail(QStringLiteral("Guardian peer identity mismatch"));
            return;
        }
        QJsonObject record{{QStringLiteral("v"), 1}, {QStringLiteral("id"), m_requestId},
            {QStringLiteral("op"), m_operation == Operation::Stop ? QStringLiteral("stop") : QStringLiteral("status")},
            {QStringLiteral("session"), m_identity.session},
            {QStringLiteral("token"), QString::fromLatin1(m_identity.token.toHex())}};
        if (m_operation == Operation::Stop) record.insert(QStringLiteral("instance"), m_identity.incarnation);
        const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
        if (m_socket.write(bytes) != bytes.size()) fail(QStringLiteral("Guardian request write failed"));
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &VirtualSessionGuardianClient::read);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](auto) { fail(QStringLiteral("Guardian transport error")); });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] { fail(QStringLiteral("Guardian disconnected before replying")); });
}

VirtualSessionGuardianClient::~VirtualSessionGuardianClient()
{
    m_busy = false;
    m_socket.disconnect(this);
    m_socket.abort(); // never Stop, signal a PID, or own guardian QProcess
}

bool VirtualSessionGuardianClient::request(const Identity &identity, Operation operation)
{
    if (m_busy || !identity.uid || !canonicalUuid(identity.session) || !canonicalUuid(identity.incarnation)
        || identity.token.size() != 32 || !QDir::isAbsolutePath(identity.socket)
        || QDir::cleanPath(identity.socket) != identity.socket || identity.socket.contains(QChar::Null)
        || (operation != Operation::Status && operation != Operation::Stop)) return false;
    const QFileInfo info(identity.socket), directory(info.absolutePath());
    struct stat parent{}, socket{};
    if (directory.canonicalFilePath() != directory.absoluteFilePath()
        || lstat(QFile::encodeName(directory.absoluteFilePath()).constData(), &parent)
        || !S_ISDIR(parent.st_mode) || parent.st_uid != identity.uid || (parent.st_mode & 0777) != 0700
        || lstat(QFile::encodeName(identity.socket).constData(), &socket) || !S_ISSOCK(socket.st_mode)
        || socket.st_uid != identity.uid || (socket.st_mode & 0077)) return false;
    m_identity = identity;
    m_operation = operation;
    m_input.clear();
    m_requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_busy = true;
    m_deadline.start();
    m_socket.connectToServer(identity.socket);
    return true;
}

void VirtualSessionGuardianClient::fail(const QString &message)
{
    if (!m_busy) return;
    m_busy = false;
    m_deadline.stop();
    m_identity.token.clear();
    m_socket.abort();
    Q_EMIT failed(message); // fixed diagnostics, never peer-controlled secrets
}

void VirtualSessionGuardianClient::read()
{
    if (!m_busy) return;
    m_input += m_socket.readAll();
    if (m_input.size() > 8192) { fail(QStringLiteral("Guardian reply exceeds limit")); return; }
    const auto newline = m_input.indexOf('\n');
    if (newline < 0) return;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(m_input.first(newline), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || !m_input.mid(newline + 1).trimmed().isEmpty()) { fail(QStringLiteral("Malformed guardian reply")); return; }
    const auto reply = document.object();
    const auto phase = reply.value(QStringLiteral("state")).toString();
    const bool running = reply.value(QStringLiteral("processRunning")).toBool();
    const bool validPhase = phase == QStringLiteral("starting") || phase == QStringLiteral("running")
        || phase == QStringLiteral("stopping") || phase == QStringLiteral("exited") || phase == QStringLiteral("failed");
    if (reply.size() != 7 || reply.value(QStringLiteral("v")) != QJsonValue(1)
        || reply.value(QStringLiteral("id")) != m_requestId || reply.value(QStringLiteral("ok")) != QJsonValue(true)
        || reply.value(QStringLiteral("session")) != m_identity.session
        || reply.value(QStringLiteral("instance")) != m_identity.incarnation
        || !reply.value(QStringLiteral("processRunning")).isBool() || !validPhase
        || (phase == QStringLiteral("running") && !running)
        || ((phase == QStringLiteral("exited") || phase == QStringLiteral("failed")) && running)
        || (m_operation == Operation::Stop && phase != QStringLiteral("stopping")
            && phase != QStringLiteral("exited") && phase != QStringLiteral("failed"))) {
        fail(QStringLiteral("Guardian reply identity or state mismatch"));
        return;
    }
    m_busy = false;
    m_deadline.stop();
    m_identity.token.clear();
    m_socket.abort();
    Q_EMIT received(phase, running);
}
}
