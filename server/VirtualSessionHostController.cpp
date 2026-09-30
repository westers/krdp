// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionHostController.h"
#include "RenderAccess.h"
#include "WorkspaceFrameGeometry.h"
#include "RemoteMonitorGeometry.h"
#include <algorithm>
#include <QDebug>
#include <QFileInfo>
#include <QPointer>
#include <limits>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <unistd.h>

namespace KRdp
{
namespace {
bool terminalProof(VirtualSessionJournal &journal, const VirtualSessionJournal::Record &record, const QString &boot)
{
    // A readback of complete dismissal bytes does not prove a previous fsync
    // succeeded. Establish durability on every dismissal-based retirement path.
    return record.boot == boot && journal.reconciled(record) == std::optional<bool>(true)
        && (journal.orderedExit(record) == std::optional<bool>(true) || journal.durableDismissed(record));
}
}
VirtualSessionHostController::VirtualSessionHostController(Server *server, Prepare prepare, QObject *parent)
    : QObject(parent), m_prepare(std::move(prepare)),
      m_supervisor([this](quint32 uid, const auto &handle) { return this->prepare(uid, handle); }),
      m_control(m_supervisor, [this](quint64 client, const auto &) {
          const auto found = m_clients.find(client);
          if (found != m_clients.end()) found->second->revoke();
      })
{
    Q_ASSERT(server);
    connect(&m_reconcileTimer, &QTimer::timeout, this, &VirtualSessionHostController::reconcileCleanExits);
    m_supervisor.setUnavailableCallback([this](const auto &handle) {
        const QPointer<VirtualSessionHostController> alive(this);
        // Closing one desktop must not revoke any other user's transport.
        QList<quint64> affected;
        for (const auto &[id, client] : m_clients) {
            const auto attached = m_control.attachment(id);
            if (attached && attached->id == handle.id && attached->manager == handle.manager
                && attached->generation == handle.generation) affected.append(id);
        }
        for (const auto id : affected) {
            const auto found = m_clients.find(id);
            if (found != m_clients.end()) found->second->unavailable();
            if (!alive) return;
        }
        if (auto *endpoint = resolve(handle)) endpoint->close();
        // Never on the supervisor's callback stack: it may still use the runtime.
        if (m_recoveredPending.contains(handle.id))
            QTimer::singleShot(0, this, [this, id = handle.id] { retireFailedRecovery(id); });
    });
    // AUD-D4: a stock client took a desktop over from another connection of
    // the same user; that one is told with the standard error code and closed.
    m_control.setDisplacedHandler([this](quint64 client) {
        const auto found = m_clients.find(client);
        if (found != m_clients.end()) found->second->displaced();
    });
    connect(server, &Server::newConnectionCreated, this, &VirtualSessionHostController::addClient);
}

VirtualSessionHostController::~VirtualSessionHostController()
{
    m_reconcileTimer.stop();
    m_supervisor.setUnavailableCallback({});
    m_supervisor.setGuardianAvailableCallback({});
    // Release all transports while control and endpoints still exist. The
    // supervisor subsequently tears down leaders on explicit host shutdown.
    while (!m_clients.empty()) removeClient(m_clients.begin()->first);
    for (auto &[id, worker] : m_workers) {
        worker->endpoint->disconnect(this);
        worker->endpoint->close();
    }
}

std::optional<VirtualSessionSupervisor::Launch> VirtualSessionHostController::prepare(
    quint32 uid, const VirtualSessionRegistry::Handle &handle)
{
    if (!m_prepare || !uid) return {};
    const QPointer<VirtualSessionHostController> alive(this);
    const auto callback = m_prepare;
    const auto token = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
    const auto prepared = callback(uid, handle, token);
    if (!alive) return {};
    if (!prepared) return {};
    if (!prepareEndpoint(uid, handle, prepared->socketName, token)) return {};
    return prepared->process;
}

bool VirtualSessionHostController::adopt(const VirtualSessionGuardianClient::Identity &identity, const QString &workerSocket)
{
    const QFileInfo worker(workerSocket), guardian(identity.socket);
    if (m_workers.contains(identity.session) || worker.fileName() != QStringLiteral("worker.sock")
        || worker.absolutePath() != guardian.absolutePath()) return false;
    auto lease = VirtualSessionBrokerLease::acquire(identity.uid, workerSocket);
    if (!lease) return false;
    const auto handle = m_supervisor.adopt(identity);
    if (!handle) return false;
    for (const auto &summary : m_supervisor.list(identity.uid)) {
        if (summary.id == handle->id && summary.phase == VirtualSessionState::Phase::Failed) return false;
    }
    if (prepareEndpoint(identity.uid, *handle, workerSocket, identity.token, std::move(lease))) return true;
    m_supervisor.captureUnavailable(*handle);
    return false;
}

bool VirtualSessionHostController::recover(VirtualSessionJournal &journal, QString *error)
{
    const auto records = journal.records(error);
    if (!records) return false;
    QFile file(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot establish current boot identity");
        return false;
    }
    const auto boot = QString::fromLatin1(file.read(128)).trimmed();
    if (m_recoveryAttempted || m_nextClient || !m_workers.empty()) {
        if (error) *error = QStringLiteral("Recovery requires a fresh host and valid launch intents within admission limits");
        return false;
    }
    m_recoveredJournal = &journal;
    QSet<QString> completed;
    QVector<VirtualSessionJournal::Record> current;
    for (const auto &record : *records) {
        const bool done = terminalProof(journal, record, boot);
        // AUD-FIX F5: a desktop from an earlier boot is gone and can never be
        // adopted. Move its intent aside instead of listing it as Failed
        // forever (it could not even be dismissed: that needs this boot).
        if (!done && record.valid() && record.boot != boot && retireRecord(journal, record, QStringLiteral("it belongs to an earlier boot")))
            continue;
        if (done) completed.insert(record.session);
        current.append(record);
    }
    if (!recoverRecords(current, boot, error, completed)) return false;
    // "Most recently used" for stock clients (AUD-D4): nothing was used since
    // this broker started, so the journal's creation order stands in for it.
    for (const auto &record : current) {
        if (!completed.contains(record.session)) m_control.noteUsed(record.session);
    }
    m_recoveryBoot = boot;
    return true;
}

bool VirtualSessionHostController::enableIndependentCreates(VirtualSessionJournal &journal, StartService start,
    AdmitCreate admission)
{
    if (!m_recoveryAttempted || m_recoveredJournal != &journal || m_nextClient || m_journal
        || (!start && (getuid() || geteuid()))) return false;
    m_journal = &journal;
    m_admitCreate = std::move(admission);
    m_reconcileTimer.start(1000);
    m_commitIntent = [&journal](const auto &record) { return journal.insert(record); };
    m_startService = start ? std::move(start) : [this](const auto &unit, const auto &handle) { return startIndependentService(unit, handle); };
    m_control.setCreateHandler([this](quint32 uid) { return createIndependent(uid); });
    // Selected-screen create is the only create krdp-client offers (KRDPCTL v2
    // `capabilities.virtualSessions.selectedCreate`), so it is always on.
    m_control.setSelectedCreateHandler([this](quint32 uid, const auto &outputs) { return createIndependent(uid, outputs); });
    m_control.setInitialLayoutPreviewCapabilities({.maxOutputs = 16, .maxOutputDimension = 4096,
        .maxAtlasDimension = 8192});
    m_control.setDismissHandlers([this](quint32 uid, const QString &id) { return dismissalEligible(uid, id); },
        [this](quint32 uid, const QString &id) { return dismissFailure(uid, id); });
    m_supervisor.setGuardianAvailableCallback([this](const auto &handle) {
        const auto found = m_newIntents.find(handle.id);
        if (found == m_newIntents.end()) return;
        const auto &record = found->second;
        auto lease = VirtualSessionBrokerLease::acquire(record.uid, record.workerSocket());
        if (!lease || !prepareEndpoint(record.uid, handle, record.workerSocket(), record.token, std::move(lease)))
            m_supervisor.captureUnavailable(handle);
    });
    return true;
}

VirtualSessionControl::CreateResult VirtualSessionHostController::createIndependent(quint32 uid,
    const VirtualSessionControl::InitialOutputs &initialOutputs)
{
    if (!m_journal || !uid || m_creationBlocked) return {};
    const QPointer<VirtualSessionHostController> alive(this);
    const auto admission = m_admitCreate;
    const auto admitted = admission ? admission(uid) : CreateAdmission::Permitted;
    if (!alive) return {};
    if (admitted != CreateAdmission::Permitted)
        return VirtualSessionControl::CreateResult(VirtualSessionControl::CreateResult::Refusal::Maintenance);
    reconcileCleanExits();
    if (!alive) return {};
    if (m_creationBlocked) return {};
    const auto existing = m_journal->records();
    // Journal history has a separate bounded capacity; never publish an intent
    // that the next broker would refuse to enumerate.
    if (!existing) return {};
    if (existing->size() >= 256) return VirtualSessionControl::CreateResult(VirtualSessionControl::CreateResult::Refusal::Limit);
    QFile bootFile(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!bootFile.open(QIODevice::ReadOnly)) return {};
    const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
    VirtualSessionJournal::Record record{uid, uuid(), uuid(), uuid(), QString::fromLatin1(bootFile.read(128)).trimmed(),
        QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122()};
    record.initialOutputs = initialOutputs;
    if (!record.valid()) return {};
    // The per-user and host desktop limits (the registry's slot accounting).
    if (!m_supervisor.canAdopt(uid, record.session))
        return VirtualSessionControl::CreateResult(VirtualSessionControl::CreateResult::Refusal::Limit);
    const auto commit = m_commitIntent;
    const bool committed = commit(record);
    if (!alive) return {};
    if (!committed) {
        // A failed fsync may leave a published record outside live accounting.
        // Freeze creation until recovery/reconciliation reads authoritative state.
        m_creationBlocked = true;
        return {};
    }
    warnRenderAccess(uid);
    // From here every failure leaves a durable intent, never a second start.
    const auto handle = m_supervisor.adopt(record.identity(), true);
    if (!handle) return {};
    m_newIntents.emplace(record.session, record);
    const QString unit = QStringLiteral("farside-virtual-session@%1.service").arg(record.session);
    const auto start = m_startService;
    const bool started = start(unit, *handle);
    if (!alive) return {};
    if (!started) m_supervisor.captureUnavailable(*handle);
    return handle;
}

void VirtualSessionHostController::warnRenderAccess(quint32 uid)
{
    if (!uid || m_renderWarned.contains(uid)) return;
    m_renderWarned.insert(uid);
    const QString warning = RenderAccess::warningFor(uid,
        QStringLiteral("a virtual desktop still encodes in hardware (it gets a private copy of its granted node, owned by the "
                       "user), but the user's console and krdpserver sessions need the group"));
    if (!warning.isEmpty()) qWarning().noquote() << warning;
}

bool VirtualSessionHostController::startIndependentService(const QString &unit, const VirtualSessionRegistry::Handle &handle)
{
    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) return false;
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
        QStringLiteral("/org/freedesktop/systemd1"), QStringLiteral("org.freedesktop.systemd1.Manager"), QStringLiteral("StartUnit"));
    message.setArguments({unit, QStringLiteral("fail")});
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, handle](QDBusPendingCallWatcher *finished) {
        const QDBusPendingReply<QDBusObjectPath> reply = *finished;
        finished->deleteLater();
        // An accepted job is NOT a running desktop. Even error/timeout is not
        // permission to restart or terminate a service that may have started.
        if (reply.isError()) m_supervisor.captureUnavailable(handle);
    });
    return true;
}

void VirtualSessionHostController::reconcileCleanExits()
{
    // A nested event loop can deliver this timer during control dispatch.
    // Retry on the next tick rather than revoking a transport below its reply.
    if (!m_journal || m_control.dispatchActive()) return;
    const auto records = m_journal->records();
    if (!records) { m_creationBlocked = true; return; }
    for (const auto &record : *records) {
        if (!terminalProof(*m_journal, record, m_recoveryBoot)) continue;
        const QPointer<VirtualSessionHostController> alive(this);
        const bool retired = m_supervisor.forgetReconciled(record.identity());
        if (!alive) return;
        if (!retired) continue;
        // The supervisor first revokes/disconnects affected transports. Remove
        // endpoints only afterward; journal intents/claims stay immutable.
        m_workers.erase(record.session);
        m_newIntents.erase(record.session);
    }
}

bool VirtualSessionHostController::retireRecord(VirtualSessionJournal &journal, const VirtualSessionJournal::Record &record, const QString &reason)
{
    // A reserved row must go too; one that proved capture is not "unadoptable".
    bool reserved = false;
    for (const auto &summary : m_supervisor.list(record.uid)) {
        if (summary.id == record.session) reserved = true;
    }
    QString error;
    if (!journal.retire(record, &error)) {
        qWarning().noquote() << "Cannot retire virtual desktop record" << record.session << "(uid" << record.uid << "):" << error;
        return false;
    }
    if (reserved && !m_supervisor.retireUnadopted(record.identity())) {
        qWarning().noquote() << "Retired virtual desktop record" << record.session << "stays listed as failed until the broker restarts";
    }
    m_workers.erase(record.session);
    m_recoveredPending.erase(record.session);
    qWarning().noquote() << "Retired virtual desktop record" << record.session << "(uid" << record.uid << "):" << reason
                         << QStringLiteral("- moved aside as .retired-%1.json; it no longer holds a slot. The desktop's").arg(record.session)
                         << "profile and any process still running were left alone.";
    return true;
}

std::optional<bool> VirtualSessionHostController::sessionUnitAlive(const QString &session)
{
    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) return std::nullopt;
    const QString systemd = QStringLiteral("org.freedesktop.systemd1");
    for (const QString &unitName : {QStringLiteral("farside-virtual-session@%1.service").arg(session),
                                    QStringLiteral("krdp-virtual-session@%1.service").arg(session)}) {
        auto get = QDBusMessage::createMethodCall(systemd, QStringLiteral("/org/freedesktop/systemd1"),
            QStringLiteral("org.freedesktop.systemd1.Manager"), QStringLiteral("GetUnit"));
        get.setArguments({unitName});
        const auto unit = bus.call(get, QDBus::Block, 3000);
        if (unit.type() == QDBusMessage::ErrorMessage) {
            if (unit.errorName() == QLatin1String("org.freedesktop.systemd1.NoSuchUnit")) continue;
            return std::nullopt;
        }
        const auto path = unit.arguments().value(0).value<QDBusObjectPath>().path();
        if (path.isEmpty()) return std::nullopt;
        auto property = QDBusMessage::createMethodCall(systemd, path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        property.setArguments({QStringLiteral("org.freedesktop.systemd1.Unit"), QStringLiteral("ActiveState")});
        const auto state = bus.call(property, QDBus::Block, 3000);
        if (state.type() == QDBusMessage::ErrorMessage) return std::nullopt;
        const QString active = state.arguments().value(0).value<QDBusVariant>().variant().toString();
        if (active.isEmpty()) return std::nullopt;
        if (active != QLatin1String("inactive") && active != QLatin1String("failed")) return true;
    }
    return false;
}

bool VirtualSessionHostController::stillRunning(const VirtualSessionJournal::Record &record) const
{
    const auto check = m_sessionAlive;
    // Unknown counts as running: retiring is only for a desktop that is provably gone.
    return !check || check(record.session) != std::optional<bool>(false);
}

void VirtualSessionHostController::retireFailedRecovery(const QString &session)
{
    const auto found = m_recoveredPending.find(session);
    if (found == m_recoveredPending.end() || !m_recoveredJournal) return;
    const auto record = found->second;
    bool failed = false;
    for (const auto &summary : m_supervisor.list(record.uid)) {
        if (summary.id == session) failed = summary.phase == VirtualSessionState::Phase::Failed;
    }
    if (!failed) return;
    // Only a runtime that never proved capture; a desktop that worked and
    // then crashed stays listed (and dismissible) as before.
    if (!m_supervisor.unadoptedFailure(record.identity())) {
        m_recoveredPending.erase(found);
        return;
    }
    // AUD-FIX8: 23b328a's broker retired Sol's cefbcbb6 and 4783de1e here while their units still
    // ran (their workers were refused, B1). A desktop that still runs is adopted again - its
    // desktop script starts a fresh worker as soon as the socket is back - never retired.
    if (stillRunning(record)) {
        int &attempts = m_readoptions[session];
        if (attempts < MaxReadoptions && m_supervisor.retireUnadopted(record.identity())) {
            ++attempts;
            m_workers.erase(session);
            if (adopt(record.identity(), record.workerSocket())) {
                qWarning().noquote() << "Virtual desktop" << session << "(uid" << record.uid << ") failed before it was captured,"
                                     << "but its session unit still runs: adopted again, attempt" << attempts << "of" << MaxReadoptions;
                return; // still pending: a second failure comes back here
            }
            m_supervisor.rememberUnavailable(record.identity());
        }
        m_recoveredPending.erase(session);
        qWarning().noquote() << "Kept virtual desktop record" << session << "(uid" << record.uid << "): it never delivered a picture"
                             << "to this broker, but its session unit still runs, so it was not retired. It is listed as failed;"
                             << "the next broker start adopts it again, or end it with"
                             << QStringLiteral("`systemctl stop farside-virtual-session@%1.service`.").arg(session);
        return;
    }
    retireRecord(*m_recoveredJournal, record, QStringLiteral("its recovered desktop failed before it was ever captured and its session unit is gone"));
    m_recoveredPending.erase(session);
}

std::optional<VirtualSessionJournal::Record> VirtualSessionHostController::dismissalRecord(quint32 uid, const QString &id) const
{
    if (!uid || !m_journal) return {};
    const auto records = m_journal->records();
    if (!records) return {};
    for (const auto &record : *records) {
        if (record.session == id && record.uid == uid && record.boot == m_recoveryBoot
            && m_journal->reconciled(record) == std::optional<bool>(true)) return record;
    }
    return {};
}

bool VirtualSessionHostController::dismissalEligible(quint32 uid, const QString &id) const
{
    bool failed = false;
    for (const auto &entry : m_supervisor.list(uid))
        if (entry.id == id) failed = entry.phase == VirtualSessionState::Phase::Failed;
    if (!failed) return false;
    const auto record = dismissalRecord(uid, id);
    // Missing dismissal is eligible; malformed/uncertain evidence is not.
    return record && m_journal->dismissed(*record).has_value();
}

VirtualSessionControl::DismissResult VirtualSessionHostController::dismissFailure(quint32 uid, const QString &id)
{
    using Result = VirtualSessionControl::DismissResult;
    const auto record = dismissalRecord(uid, id); // Authenticate owner before disclosing any outcome.
    if (!record) return Result::Unavailable;
    const auto dismissed = m_journal->dismissed(*record);
    bool present = false;
    for (const auto &entry : m_supervisor.list(uid)) {
        if (entry.id != id) continue;
        present = true;
        if (entry.phase != VirtualSessionState::Phase::Failed) return Result::Unavailable;
    }
    // No live row authorizes only a retry of an existing exact acknowledgement.
    if (!present && dismissed != std::optional<bool>(true)) return Result::Unavailable;
    if (!dismissed || !m_journal->recordDismissed(*record)) return Result::Uncertain;
    // Do not revoke transports on the command's reply stack. The recurring
    // timer is also a fallback if a nested event loop delivers this too soon.
    QTimer::singleShot(0, this, &VirtualSessionHostController::reconcileCleanExits);
    return Result::Accepted;
}

bool VirtualSessionHostController::recoverRecords(const QVector<VirtualSessionJournal::Record> &records,
    const QString &boot, QString *error, const QSet<QString> &completed)
{
    const auto refuse = [error]() {
        if (error) *error = QStringLiteral("Recovery requires a fresh host and valid launch intents within admission limits");
        return false;
    };
    if (m_recoveryAttempted || m_nextClient || !m_workers.empty()
        || QUuid(boot).isNull() || QUuid(boot).toString(QUuid::WithoutBraces) != boot) return refuse();
    QList<QPair<quint32, QString>> identities;
    QSet<QString> runtimes;
    QSet<QString> incarnations;
    QSet<QString> sessions;
    for (const auto &record : records) {
        const QString runtime = QString::number(record.uid) + QLatin1Char('/') + record.launch;
        if (!record.valid() || sessions.contains(record.session) || runtimes.contains(runtime) || incarnations.contains(record.incarnation)) return refuse();
        sessions.insert(record.session);
        runtimes.insert(runtime);
        incarnations.insert(record.incarnation);
        if (!completed.contains(record.session)) identities.append({record.uid, record.session});
    }
    if (!(completed - sessions).isEmpty()) return refuse();
    // Validate the whole set before socket activity or metadata mutation.
    if (!m_supervisor.canRecover(identities)) return refuse();
    m_recoveryAttempted = true;
    for (const auto &record : records) {
        if (completed.contains(record.session)) continue;
        if (record.boot == boot && adopt(record.identity(), record.workerSocket())) {
            // Adoption is asynchronous: it can still fail before capture.
            m_recoveredPending.insert_or_assign(record.session, record);
            continue;
        }
        bool reserved = false;
        for (const auto &summary : m_supervisor.list(record.uid)) {
            if (summary.id == record.session) { reserved = true; break; }
        }
        // AUD-FIX F5: a record this broker cannot adopt (no runtime, occupied
        // lease, unusable endpoint) is moved aside rather than kept as a Failed
        // row. Never a new spawn either way. AUD-FIX8: unless its session unit
        // still runs - then it stays a (dismissible) Failed row and the next
        // broker start tries again.
        if (record.boot == boot && stillRunning(record)) {
            qWarning().noquote() << "Could not adopt virtual desktop" << record.session << "(uid" << record.uid << "),"
                                 << "but its session unit still runs: kept, listed as failed, not retired";
        } else if (m_recoveredJournal && retireRecord(*m_recoveredJournal, record, QStringLiteral("its desktop could not be adopted")))
            continue;
        // Retirement failed: keep a visible intent (Failed rows hold no slot).
        if (!reserved && !m_supervisor.rememberUnavailable(record.identity())) return refuse();
    }
    if (error) error->clear();
    return true;
}

bool VirtualSessionHostController::prepareEndpoint(quint32 uid, const VirtualSessionRegistry::Handle &handle,
    const QString &socket, const QByteArray &token, std::unique_ptr<VirtualSessionBrokerLease> lease)
{
    auto worker = std::make_unique<Worker>();
    worker->handle = handle;
    worker->lease = std::move(lease);
    worker->endpoint = std::make_unique<ConsoleWorkerEndpoint>();
    QString error;
    if (!worker->endpoint->listen(socket,
            {ConsoleSeat::Adapter::VirtualUser, handle.id, uid}, token, &error)) {
        qWarning().noquote() << "Virtual worker endpoint:" << error;
        return false;
    }
    auto *endpoint = worker->endpoint.get();
    auto *entry = worker.get();
    connect(endpoint, &ConsoleWorkerEndpoint::workerReady, this, [endpoint](const auto &) { endpoint->requestKeyFrame(); });
    connect(endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [entry](const auto &outputs) {
        if (entry->outputs != outputs) {
            entry->verifiedKeyframes.clear();
            entry->wireLayout.clear();
            entry->multiPublished = false;
        }
        entry->outputs = outputs;
    });
    connect(endpoint, &ConsoleWorkerEndpoint::workerStopped, this, [this, handle] { m_supervisor.captureUnavailable(handle); });
    connect(endpoint, &ConsoleWorkerEndpoint::frameReceived, this, [this, entry](const VideoFrame &frame) {
        if (!frame.isKeyFrame || frame.data.isEmpty() || frame.size.isEmpty() || entry->outputs.monitors.isEmpty()
            || frame.monitors.size() != entry->outputs.monitors.size()) return;
        if (entry->outputs.monitors.size() > 1) {
            const qsizetype count = entry->outputs.monitors.size();
            if (count > 16 || frame.monitorIndex < 0 || frame.monitorIndex >= count
                || frame.monitors[frame.monitorIndex].geometry.size() != frame.size) return;
            QSet<QString> names;
            QVector<RemoteMonitorGeometry::Output> projection;
            QVector<RemoteTopologyCatalog::Output> observed;
            int primaries = 0;
            for (qsizetype i = 0; i < count; ++i) {
                const auto &output = entry->outputs.monitors[i];
                const auto &wire = frame.monitors[i];
                if (output.name.isEmpty() || names.contains(output.name) || output.geometry.isEmpty()
                    || output.geometry.left() < 0 || output.geometry.top() < 0
                    || wire.geometry.size().isEmpty() || wire.geometry.width() > 4096 || wire.geometry.height() > 4096
                    || !WorkspaceFrameGeometry::matches(wire.geometry.size(), output.geometry.size(), output.scale)
                    || wire.primary != output.primary) return;
                names.insert(output.name);
                if (output.primary) ++primaries;
                projection.append({output.geometry.topLeft(), wire.geometry.size(), output.scale, output.primary});
                observed.append({.backendKey = output.name, .name = output.name,
                    .nativePixels = wire.geometry.size(),
                    .logicalGeometry = output.geometry.translated(entry->outputs.compositorOrigin),
                    .scale = output.scale, .enabled = true, .primary = output.primary,
                    .physical = false, .owner = entry->handle.id});
            }
            if (primaries != 1 || RemoteMonitorGeometry::projectToWire(projection) != frame.monitors) return;
            QRect atlasBounds;
            for (qsizetype i = 0; i < count; ++i) {
                const auto &wire = frame.monitors[i].geometry;
                if (wire.left() < 0 || wire.top() < 0) return;
                atlasBounds |= wire;
                for (qsizetype j = i + 1; j < count; ++j) {
                    if (wire.intersects(frame.monitors[j].geometry)
                        || observed[i].logicalGeometry.intersects(observed[j].logicalGeometry)) return;
                }
            }
            if (atlasBounds.width() > 8192 || atlasBounds.height() > 8192) return;
            if (entry->wireLayout != frame.monitors) {
                entry->wireLayout = frame.monitors;
                entry->verifiedKeyframes.clear();
                entry->multiPublished = false;
            }
            entry->verifiedKeyframes.insert(frame.monitorIndex);
            if (!entry->multiPublished && entry->verifiedKeyframes.size() == count && entry->topology.observe(observed)) {
                entry->multiPublished = true;
                m_supervisor.captureReady(entry->handle);
                // The first captured keyframes were needed to prove readiness;
                // ask for a fresh set after the RDP surface layout is enabled.
                entry->endpoint->requestKeyFrame();
            }
            return;
        }
        // The encoded RDP monitor is in pixels; Outputs deliberately remains
        // in KWin logical coordinates for virtual input and resize readback.
        const auto &monitor = entry->outputs.monitors.first();
        if (frame.monitors.size() != 1 || frame.monitors.first().geometry != QRect(QPoint(0, 0), frame.size)
            || monitor.geometry.topLeft() != QPoint(0, 0)
            || !WorkspaceFrameGeometry::matches(frame.size, monitor.geometry.size(), monitor.scale)) return;
        const RemoteTopologyCatalog::Output observed{
            .backendKey = monitor.name,
            .name = monitor.name,
            .nativePixels = frame.size,
            .logicalGeometry = monitor.geometry.translated(entry->outputs.compositorOrigin),
            .scale = monitor.scale,
            .enabled = true,
            .primary = monitor.primary,
            .physical = false,
            .owner = entry->handle.id,
        };
        // The historical single-output capture path remains distinct from
        // the independently encoded multi-output path above.
        if (!entry->topology.observe({observed})) return;
        entry->wireLayout = frame.monitors;
        entry->multiPublished = false;
        m_supervisor.captureReady(entry->handle);
    });
    m_workers.insert_or_assign(handle.id, std::move(worker));
    return true;
}

ConsoleWorkerEndpoint *VirtualSessionHostController::resolve(const VirtualSessionRegistry::Handle &handle)
{
    const auto found = m_workers.find(handle.id);
    if (found == m_workers.end()) return nullptr;
    const auto &worker = *found->second;
    if (worker.handle.manager != handle.manager || worker.handle.generation != handle.generation) return nullptr;
    return worker.endpoint.get();
}

std::optional<RemoteTopologyCatalog::Snapshot> VirtualSessionHostController::topologyFor(
    const VirtualSessionRegistry::Handle &handle) const
{
    const auto found = m_workers.find(handle.id);
    if (found == m_workers.end()) return {};
    const auto &worker = *found->second;
    if (worker.handle.manager != handle.manager || worker.handle.generation != handle.generation
        || !worker.endpoint->ready() || !worker.topology.observed()
        || worker.outputs.monitors.isEmpty() || worker.outputs.monitors.size() != worker.topology.snapshot().outputs.size()
        || worker.wireLayout.size() != worker.outputs.monitors.size()
        || (worker.outputs.monitors.size() > 1 && !worker.multiPublished)) return {};
    auto snapshot = worker.topology.snapshot();
    QVector<RemoteTopologyCatalog::Entry> ordered;
    for (qsizetype i = 0; i < worker.outputs.monitors.size(); ++i) {
        const auto &reported = worker.outputs.monitors[i];
        const auto foundOutput = std::find_if(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [&reported](const auto &entry) {
            return entry.output.name == reported.name;
        });
        if (foundOutput == snapshot.outputs.cend()) return {};
        const auto &observed = foundOutput->output;
        if (observed.logicalGeometry != reported.geometry.translated(worker.outputs.compositorOrigin) || observed.scale != reported.scale
            || observed.primary != reported.primary || observed.nativePixels != worker.wireLayout[i].geometry.size()
            || observed.owner != handle.id) return {};
        ordered.append(*foundOutput);
    }
    snapshot.outputs = std::move(ordered); // Same order as RDP surface indexes.
    return snapshot;
}

void VirtualSessionHostController::addClient(RdpConnection *connection)
{
    if (m_nextClient == std::numeric_limits<quint64>::max()) {
        connection->close();
        return;
    }
    const quint64 id = ++m_nextClient;
    auto transport = std::make_unique<VirtualSessionTransport>(id, connection, m_control,
        [this](const auto &handle) { return resolve(handle); }, m_sequence);
    transport->setTopologyResolver([this](const auto &handle) { return topologyFor(handle); });
    transport->setStockClientPolicy(m_stockPolicy);
    if (m_videoHost) transport->setVideoCodecHost(*m_videoHost);
    transport->setVideoQualityPolicy(m_qualityCap, m_adaptiveQuality);
    m_clients.emplace(id, std::move(transport));
    connect(connection, &RdpConnection::stateChanged, this, [this, id](RdpConnection::State state) {
        if (state == RdpConnection::State::Closed) removeClient(id);
    }, Qt::QueuedConnection);
    connect(connection, &QObject::destroyed, this, [this, id] { removeClient(id); });
}

void VirtualSessionHostController::removeClient(quint64 id)
{
    // Remove from lookup first: transport destruction calls the dispatcher's
    // release hook, which must not re-enter this same object's destruction.
    auto node = m_clients.extract(id);
}
}
