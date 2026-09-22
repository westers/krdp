// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionGuardian.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

// Trusted service entrypoint, not an RDP command runner. The eventual launcher
// supplies argv after identity drop. No client-provided executable/environment.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("session"), QStringLiteral("Server-generated desktop UUID"), QStringLiteral("uuid")});
    parser.addOption({QStringLiteral("socket"), QStringLiteral("Fresh socket in an owned private runtime"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("token-fd"), QStringLiteral("Descriptor with exactly 32 credential bytes available"), QStringLiteral("fd")});
    parser.addPositionalArgument(QStringLiteral("helper"), QStringLiteral("Trusted namespace leader and arguments (after --)"), QStringLiteral("helper [arguments...]") );
    parser.process(app);
    bool validFd = false;
    const int descriptor = parser.value(QStringLiteral("token-fd")).toInt(&validFd);
    const auto arguments = parser.positionalArguments();
    if (!getuid() || getuid() != geteuid() || !validFd || descriptor < 0 || arguments.isEmpty()) return 1;
    const int flags = fcntl(descriptor, F_GETFL);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK)) return 1;
    QByteArray token(32, '\0');
    qsizetype offset = 0;
    while (offset < token.size()) {
        const auto count = ::read(descriptor, token.data() + offset, token.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ::close(descriptor); return 1; }
        offset += count;
    }
    ::close(descriptor);
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    KRdp::VirtualSessionGuardian guardian;
    QString error;
    if (!guardian.start(getuid(), parser.value(QStringLiteral("session")), token,
            parser.value(QStringLiteral("socket")), {arguments.first(), arguments.mid(1), environment, {}}, &error)) {
        qCritical().noquote() << error;
        return 1;
    }
    // No desktop lifetime timeout. Leave a brief terminal-status window so
    // brokers can observe explicit stop, then let the owning service finish.
    QTimer terminal;
    terminal.setInterval(100);
    QObject::connect(&terminal, &QTimer::timeout, &app, [&] {
        if (guardian.phase() != QStringLiteral("exited") && guardian.phase() != QStringLiteral("failed")) return;
        terminal.stop();
        const int result = guardian.phase() == QStringLiteral("failed") ? 1 : 0;
        QTimer::singleShot(1000, &app, [&, result] { app.exit(result); });
    });
    terminal.start();
    return app.exec();
}
