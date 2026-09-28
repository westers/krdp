// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionControl.h"
#include "VirtualInitialLayout.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QUuid>
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
    if (action == QStringLiteral("preview-create"))
        return request.size() == 5 && request.value(QStringLiteral("screens")).isArray();
    if (action == QStringLiteral("create") && request.size() == 7) {
        const QString tokenId = request.value(QStringLiteral("token")).toString();
        const QUuid uuid(tokenId);
        return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == tokenId
            && token.match(request.value(QStringLiteral("preview")).toString()).hasMatch()
            && request.value(QStringLiteral("screens")).isArray();
    }
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
        it = m_transports.insert(client, std::make_shared<Transport>(Transport{*uid, {}, {}, {}}));
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
    if (action == QStringLiteral("preview-create")) {
        // A normal create never infers this proposal. The token is scoped to
        // this authenticated transport and consumed before any create callback.
        transport->initialPreview.reset();
        if (m_initialCaps.maxOutputs <= 0)
            return reply(record, false, QStringLiteral("selected-screen creation unavailable"));
        const auto raw = record.value(QStringLiteral("screens")).toArray();
        const auto screens = VirtualInitialLayout::parseScreens(raw);
        if (!screens) return reply(record, false, QStringLiteral("invalid selected-screen proposal"));
        const RemoteTopologyDraft::Capabilities caps{.addVirtual = true, .maxOutputs = m_initialCaps.maxOutputs,
            .maxOutputDimension = m_initialCaps.maxOutputDimension, .maxAtlasDimension = m_initialCaps.maxAtlasDimension};
        auto proposed = VirtualInitialLayout::plan(*screens, QStringLiteral("initial-preview"), caps);
        if (!proposed.valid()) return reply(record, false, proposed.error);
        QJsonArray outputs;
        InitialOutputs committed;
        for (const auto &entry : proposed.outputs) {
            const auto &output = entry.output;
            committed.append({output.logicalGeometry.topLeft(), output.nativePixels, output.scale, output.primary});
            outputs.append(QJsonObject{{QStringLiteral("id"), entry.id},
                {QStringLiteral("logical"), QJsonObject{{QStringLiteral("x"), output.logicalGeometry.x()},
                    {QStringLiteral("y"), output.logicalGeometry.y()},
                    {QStringLiteral("width"), output.logicalGeometry.width()},
                    {QStringLiteral("height"), output.logicalGeometry.height()}}},
                {QStringLiteral("pixels"), QJsonObject{{QStringLiteral("width"), output.nativePixels.width()},
                    {QStringLiteral("height"), output.nativePixels.height()}}},
                {QStringLiteral("scale"), output.scale}, {QStringLiteral("primary"), output.primary}});
        }
        Transport::InitialPreview preview{QUuid::createUuid().toString(QUuid::WithoutBraces),
            record.value(QStringLiteral("id")).toString(), raw, outputs, committed, m_initialRevision, {}};
        preview.age.start();
        response.insert(QStringLiteral("token"), preview.token);
        response.insert(QStringLiteral("outputs"), outputs);
        transport->initialPreview = std::move(preview);
        return response;
    }
    if (action == QStringLiteral("list")) {
        if (m_initialCaps.maxOutputs > 0 && m_selectedCreate) {
            response.insert(QStringLiteral("initialLayout"), QJsonObject{{QStringLiteral("maxOutputs"), m_initialCaps.maxOutputs},
                {QStringLiteral("maxOutputDimension"), m_initialCaps.maxOutputDimension},
                {QStringLiteral("maxAtlasDimension"), m_initialCaps.maxAtlasDimension}});
        }
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
        CreateResult handle;
        if (record.contains(QStringLiteral("token"))) {
            const auto pending = std::move(transport->initialPreview);
            transport->initialPreview.reset(); // One use even if callback reenters or fails.
            if (!pending || !m_selectedCreate || m_initialCaps.maxOutputs <= 0
                || pending->revision != m_initialRevision || !pending->age.isValid() || pending->age.elapsed() > 30000
                || pending->token != record.value(QStringLiteral("token")).toString()
                || pending->requestId != record.value(QStringLiteral("preview")).toString()
                || pending->screens != record.value(QStringLiteral("screens")).toArray())
                return reply(record, false, QStringLiteral("selected-layout preview expired or changed"));
            const auto create = m_selectedCreate;
            handle = create(uid, pending->committed);
        } else {
            transport->initialPreview.reset(); // Legacy create cannot consume a selected-screen proposal.
            const auto create = m_create; // Callback storage can be destroyed by itself.
            handle = create ? create(uid) : CreateResult(supervisor->create(uid));
        }
        if (!valid()) return uncertain();
        if (!handle) return reply(record, false, handle.refusal == CreateResult::Refusal::Maintenance
            ? QStringLiteral("session creation unavailable during maintenance") : QStringLiteral("session creation refused"));
        noteUsed(handle->id);
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
        const QString session = record.value(QStringLiteral("session")).toString();
        // AUD-FIX2 F4: a desktop the same user's other connection holds is taken over, as for a
        // stock client: that connection is told it was opened from another device (a
        // `session-end` record, then ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION).
        const bool heldByOwnConnection = std::any_of(m_transports.cbegin(), m_transports.cend(), [&](const auto &other) {
            return other != transport && other->uid == uid && other->attached && other->attached->id == session;
        });
        if (heldByOwnConnection) {
            const auto result = takeOver(uid, client, transport, session);
            if (!valid()) return uncertain();
            if (result.kind != StockResult::Kind::Attached || !transport->attached) return reply(record, false, QStringLiteral("session unavailable"));
            response.insert(QStringLiteral("session"), session);
            response.insert(QStringLiteral("state"), QStringLiteral("attached"));
            return response;
        }
        const auto handle = supervisor->attach(uid, session, client);
        if (!handle) return reply(record, false, QStringLiteral("session unavailable"));
        transport->attached = handle;
        noteUsed(handle->id);
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
    if (handle) noteUsed(handle->id);
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

std::shared_ptr<VirtualSessionControl::Transport> VirtualSessionControl::stockTransport(std::optional<quint32> uid, quint64 client)
{
    if (!uid || !*uid || !client || !m_supervisor || dispatchActive()) return {};
    auto it = m_transports.find(client);
    if (it == m_transports.end()) it = m_transports.insert(client, std::make_shared<Transport>(Transport{*uid, {}, {}, {}}));
    return it.value()->uid == *uid ? it.value() : nullptr;
}

VirtualSessionControl::StockResult VirtualSessionControl::takeOver(quint32 uid, quint64 client,
    const std::shared_ptr<Transport> &transport, const QString &session)
{
    const QPointer<VirtualSessionControl> alive(this);
    const auto supervisor = m_supervisor;
    const auto valid = [alive, supervisor, client, transport] {
        return alive && supervisor && alive->m_transports.value(client) == transport;
    };
    StockResult failed;
    // The owner's other connections to this desktop give it up first (the
    // same revocation as the owner's own `stop`), then the attach can succeed.
    QList<QPair<quint64, std::shared_ptr<Transport>>> displaced;
    for (auto i = m_transports.cbegin(); i != m_transports.cend(); ++i) {
        if (i.key() != client && (*i)->uid == uid && (*i)->attached && (*i)->attached->id == session) displaced.append({i.key(), i.value()});
    }
    for (const auto &[target, value] : displaced) {
        if (m_transports.value(target) == value) release(target, value);
        if (!valid()) return failed;
    }
    const auto handle = supervisor->attach(uid, session, client);
    if (!valid()) return failed;
    if (handle) {
        transport->attached = handle;
        noteUsed(handle->id);
    }
    const auto notify = m_displaced;
    for (const auto &[target, value] : displaced) {
        if (notify) notify(target);
        if (!valid()) return failed;
    }
    if (!handle || !transport->attached || transport->attached->id != handle->id
        || transport->attached->generation != handle->generation || transport->attached->manager != handle->manager) return failed;
    return {StockResult::Kind::Attached, StockResult::Reason::Failed, session, handle};
}

VirtualSessionControl::StockResult VirtualSessionControl::attachStockClient(std::optional<quint32> uid, quint64 client,
    const InitialOutputs &outputs)
{
    const auto transport = stockTransport(uid, client);
    if (!transport) return {};
    if (transport->attached) return {StockResult::Kind::Attached, StockResult::Reason::Failed, transport->attached->id, transport->attached};
    const QPointer<VirtualSessionControl> alive(this);
    const auto supervisor = m_supervisor;
    ++m_dispatchDepth;
    const auto leave = qScopeGuard([alive] { if (alive) --alive->m_dispatchDepth; });
    using Phase = VirtualSessionState::Phase;
    std::optional<VirtualSessionRegistry::Summary> best;
    quint64 bestUse = 0;
    for (const auto &entry : supervisor->list(*uid)) {
        if (entry.phase != Phase::Retained && entry.phase != Phase::Attached && entry.phase != Phase::Starting) continue;
        const quint64 use = m_recency.value(entry.id, 0);
        if (!best || use > bestUse || (use == bestUse && entry.id > best->id)) {
            best = entry;
            bestUse = use;
        }
    }
    if (best) {
        if (best->phase == Phase::Starting) return {StockResult::Kind::Starting, StockResult::Reason::Failed, best->id, {}};
        return takeOver(*uid, client, transport, best->id);
    }
    CreateResult created;
    if (m_selectedCreate && m_initialCaps.maxOutputs > 0 && !outputs.isEmpty() && outputs.size() <= m_initialCaps.maxOutputs) {
        const auto create = m_selectedCreate; // Callback storage can be destroyed by itself.
        created = create(*uid, outputs);
    } else {
        const auto create = m_create;
        created = create ? create(*uid) : CreateResult(supervisor->create(*uid));
    }
    if (!alive || !supervisor || m_transports.value(client) != transport) return {};
    if (!created) {
        StockResult refused;
        if (created.refusal == CreateResult::Refusal::Limit) refused.reason = StockResult::Reason::Limit;
        else if (created.refusal == CreateResult::Refusal::Maintenance) refused.reason = StockResult::Reason::Maintenance;
        return refused;
    }
    noteUsed(created->id);
    return {StockResult::Kind::Starting, StockResult::Reason::Failed, created->id, {}};
}

VirtualSessionControl::StockResult VirtualSessionControl::attachStarted(std::optional<quint32> uid, quint64 client, const QString &session)
{
    const auto transport = stockTransport(uid, client);
    if (!transport) return {};
    if (transport->attached) return {StockResult::Kind::Attached, StockResult::Reason::Failed, transport->attached->id, transport->attached};
    const QPointer<VirtualSessionControl> alive(this);
    ++m_dispatchDepth;
    const auto leave = qScopeGuard([alive] { if (alive) --alive->m_dispatchDepth; });
    using Phase = VirtualSessionState::Phase;
    for (const auto &entry : m_supervisor->list(*uid)) {
        if (entry.id != session) continue;
        if (entry.phase == Phase::Starting) return {StockResult::Kind::Starting, StockResult::Reason::Failed, session, {}};
        if (entry.phase == Phase::Retained || entry.phase == Phase::Attached) return takeOver(*uid, client, transport, session);
        break;
    }
    return {};
}

std::optional<VirtualSessionControl::Handle> VirtualSessionControl::attachment(quint64 client) const
{
    const auto it = m_transports.constFind(client);
    return it == m_transports.cend() ? std::nullopt : (*it)->attached;
}
}
