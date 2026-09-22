// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionGuardianClient.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    for (const auto &name : {QStringLiteral("uid"), QStringLiteral("session"), QStringLiteral("instance"), QStringLiteral("socket"), QStringLiteral("token-fd")})
        parser.addOption({name, QStringLiteral("Trusted guardian identity/credential input"), QStringLiteral("value")});
    parser.addOption({QStringLiteral("stop"), QStringLiteral("Explicitly stop the matching guardian's desktop; default is read-only status")});
    parser.process(app);
    bool uidOk = false, fdOk = false;
    const uint uid = parser.value(QStringLiteral("uid")).toUInt(&uidOk);
    const int fd = parser.value(QStringLiteral("token-fd")).toInt(&fdOk);
    if (!uidOk || !uid || !fdOk || fd < 0) return 1;
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) return 1;
    QByteArray token(32, '\0');
    qsizetype offset = 0;
    while (offset < token.size()) {
        const auto count = ::read(fd, token.data() + offset, token.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); return 1; }
        offset += count;
    }
    close(fd);
    KRdp::VirtualSessionGuardianClient client;
    QObject::connect(&client, &KRdp::VirtualSessionGuardianClient::failed, &app, [&](const QString &error) {
        qCritical().noquote() << error;
        app.exit(1);
    });
    QObject::connect(&client, &KRdp::VirtualSessionGuardianClient::received, &app, [&](const QString &phase, bool running) {
        qInfo().noquote() << QJsonDocument(QJsonObject{{QStringLiteral("phase"), phase}, {QStringLiteral("processRunning"), running}}).toJson(QJsonDocument::Compact);
        app.quit();
    });
    if (!client.request({uid, parser.value(QStringLiteral("session")), parser.value(QStringLiteral("instance")),
            parser.value(QStringLiteral("socket")), token}, parser.isSet(QStringLiteral("stop"))
            ? KRdp::VirtualSessionGuardianClient::Operation::Stop : KRdp::VirtualSessionGuardianClient::Operation::Status)) return 1;
    return app.exec();
}
