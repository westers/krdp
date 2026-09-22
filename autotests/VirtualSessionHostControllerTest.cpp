// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <QTemporaryDir>
#include "VirtualSessionHostController.h"

namespace KRdp
{
class VirtualSessionHostControllerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void recoveryReadsJournalAndBootWithoutChangingIntent()
    {
        QTemporaryDir directory;
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        QFile bootFile(QStringLiteral("/proc/sys/kernel/random/boot_id"));
        QVERIFY(bootFile.open(QIODevice::ReadOnly));
        const auto boot = QString::fromLatin1(bootFile.readAll()).trimmed();
        const VirtualSessionJournal::Record record{1000, uuid(), uuid(), uuid(), uuid(), QByteArray(32, 'x')};
        QVERIFY(record.boot != boot);
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr);
        QVERIFY(journal); QVERIFY(journal->insert(record));
        Server server;
        VirtualSessionHostController host(&server, {});
        QVERIFY(host.recover(*journal));
        QCOMPARE(host.m_supervisor.list(1000).size(), 1);
        QCOMPARE(host.m_supervisor.list(1000).first().phase, VirtualSessionState::Phase::Failed);
        QVERIFY(host.m_workers.empty());
        const auto saved = journal->records();
        QVERIFY(saved); QCOMPARE(saved->size(), 1);
        QCOMPARE(saved->first().incarnation, record.incarnation);
        QCOMPARE(saved->first().token, record.token);
        QVERIFY(!host.recover(*journal));
    }
    void recoveryRefusesAfterClientAdmission()
    {
        Server server;
        VirtualSessionHostController host(&server, {});
        auto connection = std::make_unique<RdpConnection>(&server, -1);
        Q_EMIT server.newConnectionCreated(connection.get());
        connection.reset();
        QVERIFY(!host.recoverRecords({}, QUuid::createUuid().toString(QUuid::WithoutBraces), nullptr));
    }
    void unavailableRecoveryNeverCreatesOrForgetsApps()
    {
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        const auto boot = uuid();
        Server server;
        int launched = 0;
        VirtualSessionHostController host(&server, [&](quint32, const auto &, const auto &) -> std::optional<VirtualSessionHostController::PreparedLaunch> {
            ++launched; return {};
        });
        const VirtualSessionJournal::Record oldBoot{quint32(getuid()), uuid(), uuid(), uuid(), uuid(), QByteArray(32, 'x')};
        const VirtualSessionJournal::Record missing{quint32(getuid()), uuid(), uuid(), uuid(), boot, QByteArray(32, 'y')};
        QVERIFY(!QFileInfo::exists(QFileInfo(missing.workerSocket()).absolutePath()));
        QString error;
        QVERIFY2(host.recoverRecords({oldBoot, missing}, boot, &error), qPrintable(error));
        QCOMPARE(launched, 0);
        QCOMPARE(host.m_supervisor.list(getuid()).size(), 2);
        QVERIFY(host.m_supervisor.list(getuid() + 1).isEmpty());
        for (const auto &summary : host.m_supervisor.list(getuid())) {
            QCOMPARE(summary.phase, VirtualSessionState::Phase::Failed);
            QVERIFY(!host.m_supervisor.attach(getuid(), summary.id, 1));
            QVERIFY(!host.m_supervisor.stop(getuid(), summary.id));
            QVERIFY(!host.m_supervisor.recreate(getuid(), summary.id));
            QVERIFY(!host.m_supervisor.forget(getuid(), summary.id));
        }
        QVERIFY(!host.recoverRecords({}, boot, &error));
        QVERIFY(!QFileInfo::exists(QFileInfo(missing.workerSocket()).absolutePath()));
        QCOMPARE(launched, 0);
    }
    void recoveryPreflightIsAllOrNothing()
    {
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        const auto boot = uuid();
        const VirtualSessionJournal::Record good{1000, uuid(), uuid(), uuid(), boot, QByteArray(32, 'x')};
        for (const auto &kind : {QStringLiteral("invalid"), QStringLiteral("duplicate"), QStringLiteral("alias"), QStringLiteral("capacity")}) {
            Server server;
            VirtualSessionHostController host(&server, {});
            QVector<VirtualSessionJournal::Record> records{good};
            auto second = good;
            if (kind == QStringLiteral("invalid")) second.token.clear();
            if (kind == QStringLiteral("alias")) { second.session = uuid(); second.incarnation = uuid(); }
            records.append(second);
            if (kind == QStringLiteral("capacity")) {
                records.clear();
                for (int i = 0; i < 5; ++i) records.append({1000, uuid(), uuid(), uuid(), boot, QByteArray(32, 'x')});
            }
            QVERIFY(!host.recoverRecords(records, boot, nullptr));
            QVERIFY(host.m_supervisor.list(1000).isEmpty());
            QVERIFY(host.m_workers.empty());
        }
    }
    void brokerLeaseReclaimsOnlyRefusedSocket()
    {
        if (!getuid()) QSKIP("Nonroot lease fixture");
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("worker.sock"));
        const auto encoded = QFile::encodeName(path);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        QVERIFY(encoded.size() < qsizetype(sizeof(address.sun_path)));
        std::memcpy(address.sun_path, encoded.constData(), size_t(encoded.size()) + 1);
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        QVERIFY(fd >= 0);
        QCOMPARE(bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
        QCOMPARE(listen(fd, 4), 0);
        QVERIFY(!VirtualSessionBrokerLease::acquire(getuid(), path));
        QVERIFY(QFileInfo::exists(path)); // active listener must never be removed
        close(fd); // dead broker leaves the bound socket inode behind
        auto lease = VirtualSessionBrokerLease::acquire(getuid(), path);
        QVERIFY(lease);
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!VirtualSessionBrokerLease::acquire(getuid(), path));
        lease.reset();
        QVERIFY(VirtualSessionBrokerLease::acquire(getuid(), path));
    }
    void brokerLeaseRejectsUnsafeArtifacts()
    {
        if (!getuid()) QSKIP("Nonroot lease fixture");
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("worker.sock"));
        QFile ordinary(path);
        QVERIFY(ordinary.open(QIODevice::WriteOnly));
        ordinary.close();
        QVERIFY(!VirtualSessionBrokerLease::acquire(getuid(), path));
        QVERIFY(ordinary.exists());
        QVERIFY(ordinary.remove());
        QVERIFY(QFile::link(directory.filePath(QStringLiteral("missing")), path));
        QVERIFY(!VirtualSessionBrokerLease::acquire(getuid(), path));
        QVERIFY(QFileInfo(path).isSymLink());
        QVERIFY(!VirtualSessionBrokerLease::acquire(getuid() + 1, path));
    }
    void hostMayBeDestroyedBeforeAnOpenConnection()
    {
        Server server;
        auto connection = std::make_unique<RdpConnection>(&server, -1);
        {
            VirtualSessionHostController host(&server, {});
            Q_EMIT server.newConnectionCreated(connection.get());
            QCOMPARE(host.m_clients.size(), size_t(1));
        }
        connection.reset(); // no stale host/adapter callbacks
    }
    void connectionDestructionDoesNotLaunchDesktop()
    {
        Server server;
        int preparations = 0;
        VirtualSessionHostController host(&server, [&](quint32, const auto &, const auto &) -> std::optional<VirtualSessionHostController::PreparedLaunch> {
            ++preparations;
            return {};
        });
        auto connection = std::make_unique<RdpConnection>(&server, -1);
        Q_EMIT server.newConnectionCreated(connection.get());
        QCOMPARE(host.m_clients.size(), size_t(1));
        connection.reset();
        QCOMPARE(host.m_clients.size(), size_t(0));
        QCOMPARE(preparations, 0);
    }
    void ownedEndpointAndGenerationResolution()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Server server;
        QByteArray delivered;
        VirtualSessionHostController host(&server, [&](quint32 uid, const auto &handle, const QByteArray &token)
                -> std::optional<VirtualSessionHostController::PreparedLaunch> {
            if (uid != 1000 || handle.id.isEmpty()) return {};
            delivered = token;
            return VirtualSessionHostController::PreparedLaunch{directory.filePath(QStringLiteral("worker.sock")),
                {QStringLiteral("/usr/bin/sleep"), {QStringLiteral("60")}, {}, {}}};
        });
        const auto handle = host.m_supervisor.create(1000);
        QVERIFY(handle);
        QCOMPARE(delivered.size(), 32);
        auto *endpoint = host.resolve(*handle);
        QVERIFY(endpoint);
        QCOMPARE(endpoint->target().adapter, ConsoleSeat::Adapter::VirtualUser);
        QCOMPARE(endpoint->target().uid, quint32(1000));
        QCOMPARE(endpoint->target().sessionId, handle->id);
        QVERIFY(!endpoint->ready());
        auto stale = *handle;
        ++stale.generation;
        QVERIFY(!host.resolve(stale));
        stale = *handle;
        stale.manager = QUuid::createUuid();
        QVERIFY(!host.resolve(stale));
        QVERIFY(host.m_supervisor.stop(1000, handle->id));
        QTRY_COMPARE(host.m_supervisor.list(1000).first().phase, VirtualSessionState::Phase::Absent);
    }
};
}
QTEST_GUILESS_MAIN(KRdp::VirtualSessionHostControllerTest)
#include "VirtualSessionHostControllerTest.moc"
