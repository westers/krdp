// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Bounded Sol-only real-RDP authentication gate. Uses the production Virtual
// controller/transport with an empty private registry and no desktop launcher.
// Never opens the deployed journal, adopts a desktop or connects a session bus.
#include "BrokerAuthentication.h"
#include "VirtualSessionHostController.h"
#include <Server.h>
#include <RdpConnection.h>
#include <QCoreApplication>
#include <QTimer>
#include <QHostInfo>
#include <unistd.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (QHostInfo::localHostName().section(QLatin1Char('.'), 0, 0) != QLatin1String("sol")
        || getuid() || geteuid() || qEnvironmentVariable("FARSIDE_TEST_AUTH_PROBE") != QLatin1String("1")
        || app.arguments().size() != 4) return 1;
    const auto args = app.arguments();
    const auto policy = KRdp::BrokerAuthentication::readFile(args[1], true);
    if (!policy.policy) return 1;
    KRdp::Server server;
    server.setAddress(QHostAddress(QStringLiteral("192.168.48.57")));
    server.setPort(3398);
    server.setTlsCertificate(args[2].toStdString());
    server.setTlsCertificateKey(args[3].toStdString());
    server.setStandardClientMedia(false);
    if (!KRdp::BrokerAuthentication::apply(server, policy.policy->virtualDesktop)) return 1;
    KRdp::VirtualSessionHostController host(&server, [](quint32, const auto &, const auto &)
        -> std::optional<KRdp::VirtualSessionHostController::PreparedLaunch> { return {}; });
    QObject::connect(&server, &KRdp::Server::newConnectionCreated, &app, [](KRdp::RdpConnection *connection) {
        QObject::connect(connection, &KRdp::RdpConnection::controlRecordReceived, connection, [connection](const QJsonObject &record) {
            qInfo() << "Virtual auth probe request" << record.value(QStringLiteral("type")).toString()
                << "owner UID" << connection->authenticatedUserUid().value_or(0)
                << "PAM UID" << connection->authenticatedPamUid().value_or(0);
        });
    });
    if (!server.start()) return 1;
    QTimer::singleShot(120000, &app, &QCoreApplication::quit);
    return app.exec();
}
