// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionControl.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>

namespace KRdp
{
namespace
{
QJsonObject reply(const QJsonObject &request, bool ok, const QString &message = {})
{
    QJsonObject result{{QStringLiteral("type"), QStringLiteral("virtual-session")}, {QStringLiteral("v"), 1},
                       {QStringLiteral("ok"), ok}, {QStringLiteral("id"), request.value(QStringLiteral("id"))}};
    if (!message.isEmpty()) result.insert(QStringLiteral("message"), message);
    return result;
}
QString phaseName(VirtualSessionState::Phase phase)
{
    using Phase = VirtualSessionState::Phase;
    switch (phase) {
    case Phase::Absent: return QStringLiteral("absent");
    case Phase::Starting: return QStringLiteral("starting");
    case Phase::Retained: return QStringLiteral("retained");
    case Phase::Attached: return QStringLiteral("attached");
    case Phase::Stopping: return QStringLiteral("stopping");
    case Phase::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}
bool validRequest(const QJsonObject &request)
{
    static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    if (request.value(QStringLiteral("type")) != QStringLiteral("virtual-session")
        || !request.value(QStringLiteral("v")).isDouble() || request.value(QStringLiteral("v")).toDouble() != 1
        || !token.match(request.value(QStringLiteral("id")).toString()).hasMatch()) return false;
    const auto action = request.value(QStringLiteral("action")).toString();
    const bool sessionRequired = action == QStringLiteral("attach") || action == QStringLiteral("stop");
    if (!sessionRequired && action != QStringLiteral("list") && action != QStringLiteral("create") && action != QStringLiteral("detach")) return false;
    if (request.size() != (sessionRequired ? 5 : 4)) return false;
    if (sessionRequired) {
        const auto id = request.value(QStringLiteral("session")).toString();
        const QUuid uuid(id);
        if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != id) return false;
    }
    return true;
}
}

QJsonObject VirtualSessionControl::request(std::optional<quint32> uid, quint64 client, const QJsonObject &record)
{
    if (!uid || !*uid || !client) return reply(record, false, QStringLiteral("PAM-authenticated nonroot identity required"));
    if (!validRequest(record)) return reply(record, false, QStringLiteral("invalid virtual-session request"));
    auto it = m_transports.find(client);
    if (it == m_transports.end()) {
        it = m_transports.insert(client, Transport{*uid, {}, {}});
    }
    auto &transport = it.value();
    if (transport.uid != *uid) return reply(record, false, QStringLiteral("transport identity changed"));
    for (const auto &old : transport.replies) {
        if (old.request.value(QStringLiteral("id")) == record.value(QStringLiteral("id"))) {
            return old.request == record ? old.response : reply(record, false, QStringLiteral("request id reused with different content"));
        }
    }
    // Do not evict accepted mutation IDs: a delayed retry could create another
    // desktop after eviction. A bounded connection must reconnect when full.
    if (transport.replies.size() >= 256) return reply(record, false, QStringLiteral("request limit reached; reconnect"));
    auto response = dispatch(*uid, client, transport, record);
    transport.replies.push_back({record, response});
    return response;
}

QJsonObject VirtualSessionControl::dispatch(quint32 uid, quint64 client, Transport &transport, const QJsonObject &record)
{
    const auto action = record.value(QStringLiteral("action")).toString();
    auto response = reply(record, true);
    if (action == QStringLiteral("list")) {
        QJsonArray sessions;
        for (const auto &entry : m_supervisor.list(uid)) {
            sessions.append(QJsonObject{{QStringLiteral("session"), entry.id}, {QStringLiteral("state"), phaseName(entry.phase)}});
        }
        response.insert(QStringLiteral("sessions"), sessions);
        return response;
    }
    if (action == QStringLiteral("create")) {
        const auto handle = m_supervisor.create(uid);
        if (!handle) return reply(record, false, QStringLiteral("session creation refused"));
        response.insert(QStringLiteral("session"), handle->id);
        // Accepted is not ready: failed launch/first-frame readiness is exposed
        // by subsequent list requests. No transport is automatically attached.
        for (const auto &entry : m_supervisor.list(uid)) {
            if (entry.id == handle->id) response.insert(QStringLiteral("state"), phaseName(entry.phase));
        }
        return response;
    }
    if (action == QStringLiteral("attach")) {
        if (transport.attached) return reply(record, false, QStringLiteral("detach current session first"));
        const auto handle = m_supervisor.attach(uid, record.value(QStringLiteral("session")).toString(), client);
        if (!handle) return reply(record, false, QStringLiteral("session unavailable"));
        transport.attached = handle;
        response.insert(QStringLiteral("session"), handle->id);
        response.insert(QStringLiteral("state"), QStringLiteral("attached"));
        return response;
    }
    if (action == QStringLiteral("detach")) {
        release(client, transport);
        return response;
    }
    const QString id = record.value(QStringLiteral("session")).toString();
    // Check ownership before releasing another transport or disclosing state.
    const auto owned = m_supervisor.list(uid);
    const auto found = std::find_if(owned.cbegin(), owned.cend(), [&](const auto &entry) { return entry.id == id; });
    if (found == owned.cend()) return reply(record, false, QStringLiteral("session unavailable"));
    for (auto i = m_transports.begin(); i != m_transports.end(); ++i) {
        if (i->uid == uid && i->attached && i->attached->id == id) release(i.key(), i.value());
    }
    if (!m_supervisor.stop(uid, id)) return reply(record, false, QStringLiteral("session stop refused"));
    response.insert(QStringLiteral("state"), QStringLiteral("stopping"));
    return response;
}

void VirtualSessionControl::release(quint64 client, Transport &transport)
{
    if (!transport.attached) return;
    const auto handle = *transport.attached;
    if (m_release) m_release(client, handle);
    m_supervisor.disconnect(handle, client);
    transport.attached.reset();
}

void VirtualSessionControl::disconnected(quint64 client)
{
    auto it = m_transports.find(client);
    if (it == m_transports.end()) return;
    release(client, it.value());
    m_transports.erase(it);
}

std::optional<VirtualSessionControl::Handle> VirtualSessionControl::attachment(quint64 client) const
{
    const auto it = m_transports.constFind(client);
    return it == m_transports.cend() ? std::nullopt : it->attached;
}
}
