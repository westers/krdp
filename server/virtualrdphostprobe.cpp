// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionHostController.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <filesystem>
#include <fcntl.h>
#include <cerrno>
#include <unistd.h>

using namespace KRdp;
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    const bool adopting = arguments.size() == 5 && arguments[1] == QStringLiteral("--adopt");
    const bool mixed = arguments.size() == 3 && arguments[1] == QStringLiteral("--multi-mixed");
    const bool multi = arguments.size() == 3 && (arguments[1] == QStringLiteral("--multi") || mixed);
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1)) return 1;
    const auto hostName = QByteArray(name).split('.').first();
    const bool buzzIntel = hostName == "buzz" && qgetenv("KRDP_BUZZ_INTEL_PRIVATE") == "1";
    if ((hostName != "sol" && !buzzIntel) || (buzzIntel && (!mixed || adopting))
        || !getuid() || getuid() != geteuid() || (arguments.size() != 2 && !adopting && !multi)) return 1;
    const auto script = adopting ? QString() : arguments.at(multi ? 2 : 1);
    if (!adopting && !QDir::isAbsolutePath(script)) return 1;
    QTemporaryDir runtime(QStringLiteral("/run/user/%1/krdp-virtual-host.XXXXXX").arg(getuid()));
    if (!runtime.isValid()) return 1;
    runtime.setAutoRemove(false);
    qInfo().noquote() << "PAM host probe evidence:" << runtime.path();
    const auto cert = runtime.filePath(QStringLiteral("probe.crt"));
    const auto key = runtime.filePath(QStringLiteral("probe.key"));
    QProcess tls;
    tls.setStandardOutputFile(QProcess::nullDevice());
    tls.setStandardErrorFile(runtime.filePath(QStringLiteral("tls.log")));
    tls.start(QStringLiteral("/usr/bin/openssl"), {QStringLiteral("req"), QStringLiteral("-x509"),
        QStringLiteral("-newkey"), QStringLiteral("rsa:2048"), QStringLiteral("-nodes"), QStringLiteral("-days"), QStringLiteral("1"),
        QStringLiteral("-subj"), buzzIntel ? QStringLiteral("/CN=buzz.local") : QStringLiteral("/CN=sol.local"),
        QStringLiteral("-keyout"), key, QStringLiteral("-out"), cert});
    if (!tls.waitForFinished(10000) || tls.exitCode()) return 1;
    Server server;
    server.setAddress(QHostAddress(buzzIntel ? QStringLiteral("127.0.0.1") : QStringLiteral("192.168.48.57")));
    server.setPort(multi ? 3396 : 3395);
    server.setTlsCertificate(std::filesystem::path(cert.toStdString()));
    server.setTlsCertificateKey(std::filesystem::path(key.toStdString()));
    server.setUsePAMAuthentication(true);
    server.setAllowAnyPAMUser(false); // probe may launch only its actual OS user
    VirtualSessionHostController host(&server, [&](quint32 uid, const auto &handle, const QByteArray &token)
        -> std::optional<VirtualSessionHostController::PreparedLaunch> {
        if (adopting || uid != getuid()) return {};
        QTemporaryDir desktop(QStringLiteral("/run/user/%1/krdp-headless.XXXXXX").arg(uid));
        if (!desktop.isValid()) return {};
        desktop.setAutoRemove(false);
        QFile secret(desktop.filePath(QStringLiteral("worker-token")));
        if (!secret.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            || !secret.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            || secret.write(token) != token.size()) return {};
        secret.close();
        QProcessEnvironment env;
        env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
        env.insert(QStringLiteral("HOME"), QDir::homePath());
        if (buzzIntel) {
            env.insert(QStringLiteral("KRDP_BUZZ_INTEL_PRIVATE"), QStringLiteral("1"));
            env.insert(QStringLiteral("LD_LIBRARY_PATH"), qEnvironmentVariable("LD_LIBRARY_PATH"));
        }
        qInfo().noquote() << "PAM-owned desktop" << handle.id << "uid" << uid << "runtime" << desktop.path();
        return VirtualSessionHostController::PreparedLaunch{desktop.filePath(QStringLiteral("worker.sock")),
            {QStringLiteral("/usr/bin/bash"), {script, buzzIntel ? QStringLiteral("--supervised-mixed-worker-intel")
                : mixed ? QStringLiteral("--supervised-mixed-worker-nvidia")
                : multi ? QStringLiteral("--supervised-multi-worker-nvidia")
                : QStringLiteral("--supervised-worker-nvidia"), desktop.path(), handle.id}, env, {}}};
    });
    if (adopting) {
        // Trusted local recovery inputs, not a protocol-supplied process/path.
        // Credentials arrive on stdin; do not read or print account passwords.
        QByteArray token(32, '\0');
        const int flags = fcntl(0, F_GETFL);
        if (flags < 0 || fcntl(0, F_SETFL, flags | O_NONBLOCK)) return 1;
        qsizetype offset = 0;
        while (offset < token.size()) {
            const auto count = ::read(0, token.data() + offset, token.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return 1;
            offset += count;
        }
        close(0);
        const auto desktop = arguments[4];
        if (!desktop.startsWith(QStringLiteral("/run/user/%1/krdp-virtual/").arg(getuid()))) return 1;
        if (!host.adopt({quint32(getuid()), arguments[2], arguments[3], desktop + QStringLiteral("/guardian.sock"), token},
                desktop + QStringLiteral("/worker.sock"))) return 1;
        qInfo().noquote() << "Adopting retained desktop" << arguments[2] << "without owning its process lifetime";
    }
    if (!server.start()) return 1;
    const int lifetimeMs = buzzIntel ? 360000 : 180000;
    qInfo() << "Isolated PAM virtual host listening on" << hostName << (multi ? 3396 : 3395)
            << "for" << lifetimeMs / 1000 << "seconds";
    QTimer::singleShot(lifetimeMs, &app, &QCoreApplication::quit);
    return app.exec();
}
