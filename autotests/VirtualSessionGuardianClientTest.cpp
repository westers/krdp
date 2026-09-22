// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QLocalServer>
#include <QJsonDocument>
#include <QJsonObject>
#include <unistd.h>
#include "VirtualSessionGuardian.h"
#include "VirtualSessionGuardianClient.h"
using namespace KRdp;
using namespace Qt::StringLiterals;

class VirtualSessionGuardianClientTest : public QObject
{
    Q_OBJECT
    const QString session = u"86b953ee-23fc-478b-9f11-3db6dca081f6"_s;
    const QByteArray token = QByteArray(32, 'g');
    VirtualSessionSupervisor::Launch child() const {
        QProcessEnvironment env;
        env.insert(u"PATH"_s, u"/usr/bin:/bin"_s);
        return {u"/usr/bin/sleep"_s, {u"30"_s}, env, {}};
    }
private Q_SLOTS:
    void reconnectDoesNotOwnDesktop()
    {
        if (!getuid()) QSKIP("Guardian requires nonroot");
        QTemporaryDir temporary;
        const auto socket = temporary.filePath(u"guardian.sock"_s);
        VirtualSessionGuardian guardian;
        QVERIFY(guardian.start(getuid(), session, token, socket, child()));
        QTRY_COMPARE(guardian.phase(), u"running"_s);
        const auto pid = guardian.processId();
        VirtualSessionGuardianClient::Identity identity{quint32(getuid()), session, guardian.incarnation(), socket, token};
        for (int i = 0; i < 2; ++i) {
            VirtualSessionGuardianClient client;
            QSignalSpy received(&client, &VirtualSessionGuardianClient::received);
            QSignalSpy failed(&client, &VirtualSessionGuardianClient::failed);
            QVERIFY(client.request(identity, VirtualSessionGuardianClient::Operation::Status));
            QVERIFY(!client.request(identity, VirtualSessionGuardianClient::Operation::Stop));
            QTRY_COMPARE(received.size(), 1);
            QCOMPARE(received[0][0].toString(), u"running"_s);
            QVERIFY(received[0][1].toBool());
            QVERIFY(failed.isEmpty());
        }
        QCOMPARE(guardian.processId(), pid);
        QCOMPARE(guardian.phase(), u"running"_s);
        VirtualSessionGuardianClient stop;
        QSignalSpy stopped(&stop, &VirtualSessionGuardianClient::received);
        QVERIFY(stop.request(identity, VirtualSessionGuardianClient::Operation::Stop));
        QTRY_COMPARE(stopped.size(), 1);
        QTRY_COMPARE(guardian.phase(), u"exited"_s);
    }
    void refusesWrongCredentialsAndIncarnation()
    {
        if (!getuid()) QSKIP("Guardian requires nonroot");
        QTemporaryDir temporary;
        const auto socket = temporary.filePath(u"guardian.sock"_s);
        VirtualSessionGuardian guardian;
        QVERIFY(guardian.start(getuid(), session, token, socket, child()));
        QTRY_COMPARE(guardian.phase(), u"running"_s);
        VirtualSessionGuardianClient client;
        QSignalSpy failed(&client, &VirtualSessionGuardianClient::failed);
        QSignalSpy received(&client, &VirtualSessionGuardianClient::received);
        VirtualSessionGuardianClient::Identity identity{quint32(getuid()), session, guardian.incarnation(), socket, QByteArray(32, 'x')};
        QVERIFY(client.request(identity, VirtualSessionGuardianClient::Operation::Status));
        QTRY_COMPARE(failed.size(), 1);
        identity.token = token;
        identity.incarnation = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(client.request(identity, VirtualSessionGuardianClient::Operation::Status));
        QTRY_COMPARE(failed.size(), 2);
        QVERIFY(client.request(identity, VirtualSessionGuardianClient::Operation::Stop));
        QTRY_COMPARE(failed.size(), 3);
        QVERIFY(received.isEmpty());
        QCOMPARE(guardian.phase(), u"running"_s);
        identity.uid++;
        QVERIFY(!client.request(identity, VirtualSessionGuardianClient::Operation::Status));
    }
    void boundedSilentPeer()
    {
        if (!getuid()) QSKIP("Nonroot test");
        QTemporaryDir temporary;
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        const auto socket = temporary.filePath(u"guardian.sock"_s);
        QVERIFY(server.listen(socket));
        VirtualSessionGuardianClient client(nullptr, 50);
        QSignalSpy failed(&client, &VirtualSessionGuardianClient::failed);
        QVERIFY(client.request({quint32(getuid()), session, session, socket, token}, VirtualSessionGuardianClient::Operation::Status));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!client.busy());
        QVERIFY(failed[0][0].toString().contains(u"timed out"_s));
    }
    void rejectsPeerReply_data()
    {
        QTest::addColumn<int>("mutation");
        QTest::newRow("wrong-correlation") << 0;
        QTest::newRow("inconsistent-liveness") << 1;
        QTest::newRow("oversized") << 2;
        QTest::newRow("extra-field") << 3;
    }
    void rejectsPeerReply()
    {
        if (!getuid()) QSKIP("Nonroot test");
        QFETCH(int, mutation);
        QTemporaryDir temporary;
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        const auto socket = temporary.filePath(u"guardian.sock"_s);
        QVERIFY(server.listen(socket));
        connect(&server, &QLocalServer::newConnection, this, [&] {
            auto *peer = server.nextPendingConnection();
            connect(peer, &QLocalSocket::readyRead, peer, [&, peer] {
                if (!peer->canReadLine()) return;
                const auto request = QJsonDocument::fromJson(peer->readLine()).object();
                QJsonObject reply{{u"v"_s, 1}, {u"id"_s, request.value(u"id"_s)}, {u"ok"_s, true},
                    {u"session"_s, session}, {u"instance"_s, session}, {u"state"_s, u"running"_s}, {u"processRunning"_s, true}};
                if (mutation == 0) reply[u"id"_s] = u"wrong"_s;
                if (mutation == 1) reply[u"processRunning"_s] = false;
                if (mutation == 3) reply[u"extra"_s] = true;
                peer->write(mutation == 2 ? QByteArray(8193, 'x') : QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
            });
        });
        VirtualSessionGuardianClient client;
        QSignalSpy failed(&client, &VirtualSessionGuardianClient::failed);
        QSignalSpy received(&client, &VirtualSessionGuardianClient::received);
        QVERIFY(client.request({quint32(getuid()), session, session, socket, token}, VirtualSessionGuardianClient::Operation::Status));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(received.isEmpty());
        QVERIFY(!failed[0][0].toString().contains(QString::fromLatin1(token.toHex())));
    }
};
QTEST_GUILESS_MAIN(VirtualSessionGuardianClientTest)
#include "VirtualSessionGuardianClientTest.moc"
