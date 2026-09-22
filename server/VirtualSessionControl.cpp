// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionControl.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QScopeGuard>
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
    const bool sessionRequired = action == QStringLiteral("attach") || action == QStringLiteral("stop") || action == QStringLiteral("dismiss");
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
    if (!m_supervisor) return reply(record, false, QStringLiteral("session supervisor unavailable"));
    // Commands are synchronous. A nested event loop or callback may reenter;
    // reject before allocating a transport or executing any mutation.
    if (dispatchActive()) return reply(record, false, QStringLiteral("virtual-session command in progress; retry"));
    auto it = m_transports.find(client);
    if (it == m_transports.end()) {
        it = m_transports.insert(client, std::make_shared<Transport>(Transport{*uid, {}, {}}));
    }
    const auto transport = it.value();
    if (transport->uid != *uid) return reply(record, false, QStringLiteral("transport identity changed"));
    for (const auto &old : transport->replies) {
        if (old.request.value(QStringLiteral("id")) == record.value(QStringLiteral("id"))) {
            if (old.pending) return reply(record, false, QStringLiteral("virtual-session command in progress; retry"));
            return old.request == record ? old.response : reply(record, false, QStringLiteral("request id reused with different content"));
        }
    }
    // Do not evict accepted mutation IDs: a delayed retry could create another
    // desktop after eviction. A bounded connection must reconnect when full.
    if (transport->replies.size() >= 256) return reply(record, false, QStringLiteral("request limit reached; reconnect"));
    const QPointer<VirtualSessionControl> alive(this);
    ++m_dispatchDepth;
    const auto leave = qScopeGuard([alive] { if (alive) --alive->m_dispatchDepth; });
    // Reserve before callbacks; the exact object survives removal/rehashing.
    transport->replies.push_back({record, {}, true});
    auto response = dispatch(*uid, client, transport, record);
    if (!alive || !m_supervisor || m_transports.value(client) != transport)
        return reply(record, false, QStringLiteral("transport closed during command; outcome uncertain"));
    transport->replies.back().response = response;
    transport->replies.back().pending = false;
    return response;
}

QJsonObject VirtualSessionControl::dispatch(quint32 uid, quint64 client, const std::shared_ptr<Transport> &transport, const QJsonObject &record)
{
    const QPointer<VirtualSessionControl> alive(this);
    const auto supervisor = m_supervisor;
    const auto valid = [alive, supervisor, client, transport] {
        return alive && supervisor && alive->m_transports.value(client) == transport;
    };
    const auto uncertain = [&record] { return reply(record, false, QStringLiteral("transport closed during command; outcome uncertain")); };
    const auto action = record.value(QStringLiteral("action")).toString();
    auto response = reply(record, true);
    if (action == QStringLiteral("list")) {
        QJsonArray sessions;
        for (const auto &entry : supervisor->list(uid)) {
            QJsonObject row{{QStringLiteral("session"), entry.id}, {QStringLiteral("state"), phaseName(entry.phase)}};
            if (m_dismissible && m_dismiss) {
                const auto eligible = m_dismissible;
                const bool permitted = entry.phase == VirtualSessionState::Phase::Failed && eligible(uid, entry.id);
                if (!valid()) return uncertain();
                row.insert(QStringLiteral("dismissible"), permitted);
            }
            sessions.append(row);
        }
        response.insert(QStringLiteral("sessions"), sessions);
        return response;
    }
    if (action == QStringLiteral("create")) {
        const auto create = m_create; // Callback storage can be destroyed by itself.
        const CreateResult handle = create ? create(uid) : CreateResult(supervisor->create(uid));
        if (!valid()) return uncertain();
        if (!handle) return reply(record, false, handle.refusal == CreateResult::Refusal::Maintenance
            ? QStringLiteral("session creation unavailable during maintenance") : QStringLiteral("session creation refused"));
        response.insert(QStringLiteral("session"), handle->id);
        // Accepted is not ready: failed launch/first-frame readiness is exposed
        // by subsequent list requests. No transport is automatically attached.
        for (const auto &entry : supervisor->list(uid)) {
            if (entry.id == handle->id) response.insert(QStringLiteral("state"), phaseName(entry.phase));
        }
        return response;
    }
    if (action == QStringLiteral("attach")) {
        if (transport->attached) return reply(record, false, QStringLiteral("detach current session first"));
        const auto handle = supervisor->attach(uid, record.value(QStringLiteral("session")).toString(), client);
        if (!handle) return reply(record, false, QStringLiteral("session unavailable"));
        transport->attached = handle;
        response.insert(QStringLiteral("session"), handle->id);
        response.insert(QStringLiteral("state"), QStringLiteral("attached"));
        return response;
    }
    if (action == QStringLiteral("detach")) {
        release(client, transport);
        if (!valid()) return uncertain();
        return response;
    }
    const QString id = record.value(QStringLiteral("session")).toString();
    if (action == QStringLiteral("dismiss")) {
        // Host resolves immutable owner identity first, including historical
        // retries whose live row is already retired. Never route through Stop.
        const auto dismiss = m_dismiss;
        const auto result = dismiss ? dismiss(uid, id) : DismissResult::Unavailable;
        if (!valid()) return uncertain();
        if (result == DismissResult::Unavailable) return reply(record, false, QStringLiteral("session unavailable"));
        if (result == DismissResult::Uncertain)
            return reply(record, false, QStringLiteral("dismissal outcome uncertain; refresh before retrying with a new request id"));
        response.insert(QStringLiteral("session"), id);
        response.insert(QStringLiteral("state"), QStringLiteral("dismissed"));
        return response;
    }
    // Check ownership before releasing another transport or disclosing state.
    const auto owned = supervisor->list(uid);
    const auto found = std::find_if(owned.cbegin(), owned.cend(), [&](const auto &entry) { return entry.id == id; });
    if (found == owned.cend()) return reply(record, false, QStringLiteral("session unavailable"));
    QList<QPair<quint64, std::shared_ptr<Transport>>> affected;
    for (auto i = m_transports.cbegin(); i != m_transports.cend(); ++i) {
        if ((*i)->uid == uid && (*i)->attached && (*i)->attached->id == id) affected.append({i.key(), i.value()});
    }
    for (const auto &[target, value] : affected) {
        if (m_transports.value(target) == value) release(target, value);
        if (!valid()) return uncertain();
    }
    const bool stopped = supervisor->stop(uid, id);
    if (!valid()) return uncertain();
    if (!stopped) return reply(record, false, QStringLiteral("session stop refused"));
    response.insert(QStringLiteral("state"), QStringLiteral("stopping"));
    return response;
}

void VirtualSessionControl::release(quint64 client, const std::shared_ptr<Transport> &transport, std::function<void()> revoke)
{
    const auto handle = transport ? transport->attached : std::nullopt;
    if (!handle && !revoke) return;
    // Reentrant disconnect must not revoke twice, or erase a successor record.
    if (transport) transport->attached.reset();
    const auto callback = m_release;
    const auto supervisor = m_supervisor;
    const QPointer<VirtualSessionControl> alive(this);
    ++m_dispatchDepth;
    const auto leave = qScopeGuard([alive] { if (alive) --alive->m_dispatchDepth; });
    if (revoke) revoke();
    if (alive && callback && handle) callback(client, *handle);
    // Revocation precedes registry release. This remains safe if Control died
    // but the independently-owned supervisor is still alive.
    if (supervisor && handle) supervisor->disconnect(*handle, client);
}

void VirtualSessionControl::disconnected(quint64 client)
{
    disconnected(client, {});
}

void VirtualSessionControl::disconnected(quint64 client, std::function<void()> revoke)
{
    const auto transport = m_transports.take(client);
    release(client, transport, std::move(revoke));
}

std::optional<VirtualSessionControl::Handle> VirtualSessionControl::attachment(quint64 client) const
{
    const auto it = m_transports.constFind(client);
    return it == m_transports.cend() ? std::nullopt : (*it)->attached;
}
}
