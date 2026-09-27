// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

#include <dlfcn.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <freerdp/peer.h>

#include "RdpConnection.h"
#include "Server.h"

using namespace KRdp;

// Interposes libfreerdp's freerdp_peer_context_free() (the executable's
// definition wins over the library's for libKRdp's calls) to count how often
// RdpConnection frees a peer context before forwarding to the real one.
static std::atomic<int> s_contextFrees = 0;

extern "C" void freerdp_peer_context_free(freerdp_peer *client)
{
    using Fn = void (*)(freerdp_peer *);
    static const auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "freerdp_peer_context_free"));
    if (client && client->context) {
        ++s_contextFrees;
    }
    real(client);
}

// AUD-P3: Server::start() refuses a certificate/key pair that does not parse,
// and a connection whose initialize() fails closes its socket and reaches
// Closed (which is what makes Server drop it) instead of leaking both.
class RdpConnectionInitTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;

    QString writeFile(const QString &name, const QByteArray &content)
    {
        const QString path = m_dir.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) {
            return {};
        }
        return path;
    }

    // A real self-signed pair, or an empty pair when openssl is unavailable.
    std::pair<QString, QString> generatePair()
    {
        const QString openssl = QStandardPaths::findExecutable(QStringLiteral("openssl"));
        if (openssl.isEmpty()) {
            return {};
        }
        const QString cert = m_dir.filePath(QStringLiteral("good.crt"));
        const QString key = m_dir.filePath(QStringLiteral("good.key"));
        QProcess process;
        process.start(openssl,
                      {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"), QStringLiteral("rsa:2048"), QStringLiteral("-nodes"),
                       QStringLiteral("-days"), QStringLiteral("1"), QStringLiteral("-subj"), QStringLiteral("/CN=krdp-test"), QStringLiteral("-keyout"), key,
                       QStringLiteral("-out"), cert});
        if (!process.waitForFinished(30000) || process.exitCode() != 0) {
            return {};
        }
        return {cert, key};
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
    }

    void garbageCertificateIsRefused()
    {
        const QString cert = writeFile(QStringLiteral("garbage.crt"), "not a certificate\n");
        const QString key = writeFile(QStringLiteral("garbage.key"), "not a key\n");
        QVERIFY(!Server::tlsFilesUsable(cert.toStdString(), key.toStdString()));

        Server server;
        server.setPort(0);
        server.setTlsCertificate(cert.toStdString());
        server.setTlsCertificateKey(key.toStdString());
        QVERIFY(!server.start());
        QVERIFY(!server.isListening());
    }

    void unreadableCertificateMakesStartFail()
    {
        if (::geteuid() == 0) {
            QSKIP("root reads a mode-000 file");
        }
        const auto [goodCert, goodKey] = generatePair();
        if (goodCert.isEmpty()) {
            QSKIP("openssl is needed to generate a valid pair");
        }
        QVERIFY(Server::tlsFilesUsable(goodCert.toStdString(), goodKey.toStdString()));

        const QString unreadable = m_dir.filePath(QStringLiteral("unreadable.crt"));
        QVERIFY(QFile::copy(goodCert, unreadable));
        QVERIFY(QFile::setPermissions(unreadable, QFileDevice::Permissions()));

        Server server;
        server.setPort(0);
        server.setTlsCertificate(unreadable.toStdString());
        server.setTlsCertificateKey(goodKey.toStdString());
        QVERIFY(!server.start());
        QVERIFY(!server.isListening());

        // An unreadable key is refused the same way.
        const QString unreadableKey = m_dir.filePath(QStringLiteral("unreadable.key"));
        QVERIFY(QFile::copy(goodKey, unreadableKey));
        QVERIFY(QFile::setPermissions(unreadableKey, QFileDevice::Permissions()));
        QVERIFY(!Server::tlsFilesUsable(goodCert.toStdString(), unreadableKey.toStdString()));

        // And the good pair starts.
        Server good;
        good.setPort(0);
        good.setTlsCertificate(goodCert.toStdString());
        good.setTlsCertificateKey(goodKey.toStdString());
        QVERIFY(good.start());
        good.stop();
    }

    void initFailureClosesTheConnection()
    {
        int fds[2];
        QCOMPARE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

        Server server;
        // Missing files: initialize() fails loading the certificate.
        server.setTlsCertificate(m_dir.filePath(QStringLiteral("missing.crt")).toStdString());
        server.setTlsCertificateKey(m_dir.filePath(QStringLiteral("missing.key")).toStdString());

        s_contextFrees = 0;
        auto connection = std::make_unique<RdpConnection>(&server, fds[0]);
        QSignalSpy states(connection.get(), &RdpConnection::stateChanged);
        QTRY_COMPARE_WITH_TIMEOUT(connection->state(), RdpConnection::State::Closed, 5000);
        QVERIFY(states.size() >= 2);
        QCOMPARE(states.first().first().value<RdpConnection::State>(), RdpConnection::State::Starting);
        QCOMPARE(states.last().first().value<RdpConnection::State>(), RdpConnection::State::Closed);

        // The server end is closed: the client end reads EOF.
        pollfd pfd{.fd = fds[1], .events = POLLIN, .revents = 0};
        QCOMPARE(::poll(&pfd, 1, 2000), 1);
        char byte = 0;
        QCOMPARE(::read(fds[1], &byte, 1), ssize_t(0));
        ::close(fds[1]);

        // Closing again, with a reason, on a connection without a peer is safe.
        connection->close(RdpConnection::CloseReason::VideoInitFailed);
        connection.reset();
        // The failure path freed the peer context, and the destructor did
        // not free it again.
        QCOMPARE(s_contextFrees.load(), 1);
    }

    // AUD-INT: a connection that initialized and then lost its client frees
    // the peer context (transport, rdp, channel manager) exactly once, when
    // it is destroyed. Before, ~RdpConnection only freed the freerdp_peer
    // struct and leaked the context of every normally closed connection.
    void normalDisconnectFreesThePeerContextOnce()
    {
        const auto [cert, key] = generatePair();
        if (cert.isEmpty()) {
            QSKIP("openssl is needed to generate a valid pair");
        }
        int fds[2];
        QCOMPARE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

        Server server;
        server.setTlsCertificate(cert.toStdString());
        server.setTlsCertificateKey(key.toStdString());

        s_contextFrees = 0;
        auto connection = std::make_unique<RdpConnection>(&server, fds[0]);
        // initialize() succeeded: the session thread runs and waits for the
        // client's first PDU.
        QTRY_COMPARE_WITH_TIMEOUT(connection->state(), RdpConnection::State::Running, 5000);

        // The client goes away; the session thread ends the connection.
        ::close(fds[1]);
        QTRY_COMPARE_WITH_TIMEOUT(connection->state(), RdpConnection::State::Closed, 10000);
        QCOMPARE(s_contextFrees.load(), 0);

        connection.reset();
        QCOMPARE(s_contextFrees.load(), 1);
    }
};

QTEST_GUILESS_MAIN(RdpConnectionInitTest)

#include "RdpConnectionInitTest.moc"
