// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Independent root service entry, never setuid or invoked with RDP-supplied argv.
#include "VirtualSessionServicePlan.h"
#include "VirtualSessionServiceScope.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QDebug>
#include <pwd.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace {
bool trusted(const QString &path, bool directory = false)
{
    if (!KRdp::VirtualSessionLaunchPlan::absoluteCleanPath(path)
        || QFileInfo(path).canonicalFilePath() != path) return false;
    QString current = path;
    bool first = true;
    while (true) {
        struct stat s{};
        if (lstat(QFile::encodeName(current).constData(), &s) || s.st_uid != 0 || (s.st_mode & 0022)
            || ((first && !directory) ? !S_ISREG(s.st_mode) : !S_ISDIR(s.st_mode))) return false;
        if (current == QStringLiteral("/")) return true;
        current = QFileInfo(current).path(); first = false;
    }
}
bool executable(const QString &path) { return trusted(path) && QFileInfo(path).isExecutable(); }
int refused(const char *stage) { qCritical() << "Virtual session entry refused:" << stage; return 1; }
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    for (const auto &name : {"session", "device-entry", "guardian", "launcher", "worker", "support", "render-pci"})
        parser.addOption({QString::fromLatin1(name), QStringLiteral("Trusted service launch parameter"), QStringLiteral("value")});
    parser.process(app);
    if (getuid() || geteuid()) return refused("explicit root service required");
    if (!parser.positionalArguments().isEmpty()) return refused("unexpected arguments");
    const auto record = KRdp::VirtualSessionJournal::readLaunchIntent(parser.value(QStringLiteral("session")));
    if (!record) return refused("committed launch intent");
    // This launcher must already belong to its dedicated system unit, never
    // an administrator's shell, the broker unit, or a physical login scope.
    auto scope = KRdp::VirtualSessionServiceScope::open(record->session);
    if (!scope) return refused("dedicated nondelegated service scope");
    QFile bootFile(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!bootFile.open(QIODevice::ReadOnly)) return refused("boot identity");
    const auto boot = QString::fromLatin1(bootFile.read(128)).trimmed();
    struct passwd account{};
    struct passwd *resolved = nullptr;
    QByteArray buffer(16384, '\0');
    int status = 0;
    while ((status = getpwuid_r(record->uid, &account, buffer.data(), buffer.size(), &resolved)) == ERANGE
        && buffer.size() < 1048576) buffer.resize(buffer.size() * 2);
    if (status || !resolved || account.pw_uid != record->uid || !account.pw_name || !account.pw_dir)
        return refused("OS account");
    const auto device = parser.value(QStringLiteral("device-entry"));
    const auto guardian = parser.value(QStringLiteral("guardian"));
    const KRdp::VirtualSessionLaunchPlan::Configuration config{parser.value(QStringLiteral("launcher")),
        parser.value(QStringLiteral("worker")), parser.value(QStringLiteral("support")),
        parser.value(QStringLiteral("render-pci")).split(QLatin1Char(','), Qt::KeepEmptyParts), {1280, 720}};
    const auto plan = KRdp::VirtualSessionServicePlan::build(*record, boot,
        {record->uid, QString::fromLocal8Bit(account.pw_name), QString::fromLocal8Bit(account.pw_dir)}, config, device, guardian);
    if (!plan) return refused("boot or trusted launch policy");
    if (!executable(device) || !executable(guardian) || !executable(config.launcher) || !executable(config.worker)
        || !trusted(config.supportDirectory, true)
        || !trusted(config.supportDirectory + QStringLiteral("/virtual-session-bus.conf"))
        || !trusted(config.supportDirectory + QStringLiteral("/virtual-session-pipewire.conf"))
        || !trusted(config.supportDirectory + QStringLiteral("/defaults"), true)
        || !trusted(QFileInfo(config.launcher).path() + QStringLiteral("/virtual-session-desktop.sh")))
        return refused("installed code ownership");
    QDirIterator support(config.supportDirectory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    int entries = 0;
    while (support.hasNext()) {
        const auto path = support.next();
        if (++entries > 256 || !trusted(path, QFileInfo(path).isDir())) return refused("support tree ownership");
    }
    struct stat runtime{};
    if (!lstat(QFile::encodeName(plan->runtime).constData(), &runtime) || errno != ENOENT)
        return refused("runtime already exists or cannot be inspected");
    if (!KRdp::VirtualSessionJournal::claimLaunch(*record)) return refused("one-use launch claim");
    // No credential file or argv. The child consumes exactly32bytes from stdin.
    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC)) return refused("credential pipe");
    ssize_t written;
    do { written = write(pipeFds[1], plan->credential.constData(), plan->credential.size()); } while (written < 0 && errno == EINTR);
    close(pipeFds[1]);
    if (written != 32 || dup2(pipeFds[0], STDIN_FILENO) < 0) { close(pipeFds[0]); return refused("credential delivery"); }
    if (pipeFds[0] != STDIN_FILENO) close(pipeFds[0]);
    if (fcntl(STDIN_FILENO, F_SETFD, 0)) return refused("credential descriptor");
    std::vector<QByteArray> encoded{QFile::encodeName(plan->program)};
    for (const auto &argument : plan->arguments) encoded.push_back(argument.toUtf8());
    std::vector<char *> arguments;
    for (auto &argument : encoded) arguments.push_back(argument.data());
    arguments.push_back(nullptr);
    char path[] = "PATH=/usr/bin:/bin", lang[] = "LANG=C.UTF-8";
    char *environment[] = {path, lang, nullptr};
    umask(0077);
    // This entry still execs the guardian chain; only admission uses the scope
    // handle until the persistent parent is integrated. Close it before the
    // bulk inherited-FD close so failed exec cannot double-close a reused FD.
    scope.reset();
    if (chdir("/") || syscall(SYS_close_range, 3U, ~0U, 0U)) return refused("clean execution context");
    execve(arguments[0], arguments.data(), environment);
    return refused("device entry exec");
}
