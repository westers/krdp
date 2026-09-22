// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionGuardian.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <cerrno>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

// Trusted service entrypoint, not an RDP command runner. The eventual launcher
// supplies argv after identity drop. No client-provided executable/environment.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("session"), QStringLiteral("Server-generated desktop UUID"), QStringLiteral("uuid")});
    parser.addOption({QStringLiteral("instance"), QStringLiteral("Fresh canonical guardian UUID recorded by the trusted launcher before spawn"), QStringLiteral("uuid")});
    parser.addOption({QStringLiteral("socket"), QStringLiteral("Fresh socket in an owned private runtime"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("launch-id"), QStringLiteral("Prepare and own persistent profile/runtime storage for this launch UUID; excludes --socket"), QStringLiteral("uuid")});
    parser.addOption({QStringLiteral("token-fd"), QStringLiteral("Descriptor with exactly 32 credential bytes available"), QStringLiteral("fd")});
    parser.addPositionalArgument(QStringLiteral("helper"), QStringLiteral("Trusted namespace leader and arguments (after --)"), QStringLiteral("helper [arguments...]") );
    parser.process(app);
    bool validFd = false;
    const int descriptor = parser.value(QStringLiteral("token-fd")).toInt(&validFd);
    const auto arguments = parser.positionalArguments();
    const auto incarnation = parser.value(QStringLiteral("instance"));
    if (!getuid() || getuid() != geteuid() || !validFd || descriptor < 0 || arguments.isEmpty()
        || (parser.isSet(QStringLiteral("instance")) && (QUuid(incarnation).isNull()
            || QUuid(incarnation).toString(QUuid::WithoutBraces) != incarnation))
        || parser.isSet(QStringLiteral("launch-id")) == parser.isSet(QStringLiteral("socket"))) return 1;
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
    bool started = false;
    if (parser.isSet(QStringLiteral("launch-id"))) {
        struct passwd account{};
        struct passwd *resolved = nullptr;
        QByteArray buffer(16384, '\0');
        int status = 0;
        while ((status = getpwuid_r(getuid(), &account, buffer.data(), buffer.size(), &resolved)) == ERANGE && buffer.size() < 1048576) {
            buffer.resize(buffer.size() * 2);
        }
        if (status || !resolved || account.pw_uid != getuid() || !account.pw_dir || !account.pw_name) return 1;
        auto storage = KRdp::VirtualSessionStorage::prepare(getuid(), QString::fromLocal8Bit(account.pw_dir),
            parser.value(QStringLiteral("session")), parser.value(QStringLiteral("launch-id")), token, &error);
        if (!storage) { qCritical().noquote() << error; return 1; }
        environment.insert(QStringLiteral("HOME"), QString::fromLocal8Bit(account.pw_dir));
        environment.insert(QStringLiteral("USER"), QString::fromLocal8Bit(account.pw_name));
        environment.insert(QStringLiteral("LOGNAME"), QString::fromLocal8Bit(account.pw_name));
        environment.insert(QStringLiteral("KRDP_VIRTUAL_RUNTIME"), storage->runtimeDirectory());
        environment.insert(QStringLiteral("KRDP_VIRTUAL_PROFILE"), storage->profileDirectory());
        started = guardian.startPrepared(getuid(), parser.value(QStringLiteral("session")), token, std::move(storage),
            {arguments.first(), arguments.mid(1), environment, {}}, &error, incarnation);
    } else {
        started = guardian.start(getuid(), parser.value(QStringLiteral("session")), token,
            parser.value(QStringLiteral("socket")), {arguments.first(), arguments.mid(1), environment, {}}, &error, incarnation);
    }
    if (!started) {
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
