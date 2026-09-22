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
