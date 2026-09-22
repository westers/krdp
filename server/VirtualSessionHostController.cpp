// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionHostController.h"
#include <QDebug>
#include <QFileInfo>
#include <limits>

namespace KRdp
{
VirtualSessionHostController::VirtualSessionHostController(Server *server, Prepare prepare, QObject *parent)
    : QObject(parent), m_prepare(std::move(prepare)),
      m_supervisor([this](quint32 uid, const auto &handle) { return this->prepare(uid, handle); }),
      m_control(m_supervisor, [this](quint64 client, const auto &) {
          const auto found = m_clients.find(client);
          if (found != m_clients.end()) found->second->revoke();
      })
{
    Q_ASSERT(server);
    m_supervisor.setUnavailableCallback([this](const auto &handle) {
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
        }
        if (auto *endpoint = resolve(handle)) endpoint->close();
    });
    connect(server, &Server::newConnectionCreated, this, &VirtualSessionHostController::addClient);
}

VirtualSessionHostController::~VirtualSessionHostController()
{
    m_supervisor.setUnavailableCallback({});
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
    const auto token = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
    const auto prepared = m_prepare(uid, handle, token);
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
    return recoverRecords(*records, QString::fromLatin1(file.read(128)).trimmed(), error);
}

bool VirtualSessionHostController::recoverRecords(const QVector<VirtualSessionJournal::Record> &records,
    const QString &boot, QString *error)
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
    for (const auto &record : records) {
        const QString runtime = QString::number(record.uid) + QLatin1Char('/') + record.launch;
        if (!record.valid() || runtimes.contains(runtime) || incarnations.contains(record.incarnation)) return refuse();
        runtimes.insert(runtime);
        incarnations.insert(record.incarnation);
        identities.append({record.uid, record.session});
    }
    // Validate the whole set before socket activity or metadata mutation.
    if (!m_supervisor.canRecover(identities)) return refuse();
    m_recoveryAttempted = true;
    for (const auto &record : records) {
        if (record.boot == boot && adopt(record.identity(), record.workerSocket())) continue;
        bool reserved = false;
        for (const auto &summary : m_supervisor.list(record.uid)) {
            if (summary.id == record.session) { reserved = true; break; }
        }
        // Failed adoption may already have reserved a runtime. Missing runtime
        // or occupied lease must still leave a visible intent, never a new spawn.
        if (!reserved && !m_supervisor.rememberUnavailable(record.uid, record.session)) return refuse();
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
    connect(endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [entry](const auto &outputs) { entry->outputs = outputs; });
    connect(endpoint, &ConsoleWorkerEndpoint::workerStopped, this, [this, handle] { m_supervisor.captureUnavailable(handle); });
    connect(endpoint, &ConsoleWorkerEndpoint::frameReceived, this, [this, entry](const VideoFrame &frame) {
        if (!frame.isKeyFrame || frame.data.isEmpty() || frame.size.isEmpty() || entry->outputs.monitors.isEmpty()
            || frame.monitors.size() != entry->outputs.monitors.size()) return;
        for (qsizetype i = 0; i < frame.monitors.size(); ++i) {
            if (frame.monitors[i].geometry != entry->outputs.monitors[i].geometry) return;
        }
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

void VirtualSessionHostController::addClient(RdpConnection *connection)
{
    if (m_nextClient == std::numeric_limits<quint64>::max()) {
        connection->close();
        return;
    }
    const quint64 id = ++m_nextClient;
    m_clients.emplace(id, std::make_unique<VirtualSessionTransport>(id, connection, m_control,
        [this](const auto &handle) { return resolve(handle); }, m_sequence));
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
