// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Independent root service entry, never setuid or invoked with RDP-supplied argv.
#include "VirtualSessionServicePlan.h"
#include "VirtualSessionServiceScope.h"
#include "VirtualSessionServiceOwner.h"
#include "VirtualSessionLogin.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QDebug>
#include <QSocketNotifier>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <csignal>
#include <pwd.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/signalfd.h>
#include <unistd.h>

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
    for (char **entry = environ; entry && *entry; ++entry) {
        const QByteArray value(*entry);
        if (value != "PATH=/usr/bin:/bin" && value != "LANG=C.UTF-8") return refused("unclean environment");
    }
    sigset_t terminationMask;
    sigemptyset(&terminationMask); sigaddset(&terminationMask, SIGTERM); sigaddset(&terminationMask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &terminationMask, nullptr)
        || syscall(SYS_close_range, 3U, ~0U, 0U)) return refused("clean execution context");
    umask(0077);
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    for (const auto &name : {"session", "device-entry", "guardian", "pam-keeper", "launcher", "worker", "support", "render-pci"})
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
    const auto keeper = parser.value(QStringLiteral("pam-keeper"));
    const KRdp::VirtualSessionLaunchPlan::Configuration config{parser.value(QStringLiteral("launcher")),
        parser.value(QStringLiteral("worker")), parser.value(QStringLiteral("support")),
        parser.value(QStringLiteral("render-pci")).split(QLatin1Char(','), Qt::KeepEmptyParts), {1280, 720}};
    const auto plan = KRdp::VirtualSessionServicePlan::build(*record, boot,
        {record->uid, QString::fromLocal8Bit(account.pw_name), QString::fromLocal8Bit(account.pw_dir)}, config, device, guardian);
    if (!plan) return refused("boot or trusted launch policy");
    if (!executable(device) || !executable(guardian) || !executable(keeper) || !executable(config.launcher) || !executable(config.worker)
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
    const int signalFd = signalfd(-1, &terminationMask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signalFd < 0) return refused("signal descriptor");
    QFile signalFile;
    if (!signalFile.open(signalFd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        close(signalFd); return refused("signal ownership");
    }
    KRdp::VirtualSessionServiceOwner::Checks checks;
    checks.keeper = [uid = record->uid, launch = record->launch](pid_t pid, const QString &id) {
        const auto login = KRdp::VirtualSessionLogin::read(pid);
        if (!login || !login->matchesLaunch(uid, pid, id, QStringLiteral("/run/user/%1").arg(uid), launch)) return false;
        QFile cgroup(QStringLiteral("/proc/%1/cgroup").arg(pid));
        if (!cgroup.open(QIODevice::ReadOnly)) return false;
        const auto bytes = cgroup.read(65537);
        return cgroup.error() == QFileDevice::NoError && bytes.size() <= 65536
            && bytes.startsWith("0::/") && bytes.count('\n') == 1
            && bytes.endsWith((QLatin1Char('/') + login->scope + QLatin1Char('\n')).toUtf8());
    };
    checks.descendantsGone = [&scope] { return scope->descendantsGone(); };
    checks.signalDescendants = [&scope](int signal) { return scope->signalDescendants(signal); };
    checks.emergency = [&app, session = record->session] {
        qCritical("Virtual desktop extinction uncertain; requesting failed service teardown, PAM cleanup unproven");
        auto stop = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
            QStringLiteral("/org/freedesktop/systemd1"), QStringLiteral("org.freedesktop.systemd1.Manager"), QStringLiteral("StopUnit"));
        stop.setArguments({QStringLiteral("krdp-virtual-session@%1.service").arg(session), QStringLiteral("replace")});
        QDBusConnection::systemBus().asyncCall(stop, 3000);
        // Also bound bus failure: exiting the dedicated unit's main process
        // invokes its KillMode=mixed fallback. Bypass QProcess destructors,
        // which otherwise wait indefinitely. Keeper observes owner death.
        QTimer::singleShot(5000, &app, [] { _exit(1); });
    };
    KRdp::VirtualSessionServiceOwner owner(std::move(checks), [&app, record = *record](int result) {
        // Only the root owner's normal completion supplies this outcome; the
        // independent ExecStopPost reconciliation proof is still required.
        if (result == 0 && !KRdp::VirtualSessionJournal::recordOrderedExit(record))
            result = refused("durable ordered-exit evidence");
        app.exit(result);
    });
    QSocketNotifier notifier(signalFd, QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
        signalfd_siginfo signal{};
        while (read(signalFd, &signal, sizeof(signal)) == sizeof(signal)) owner.stop();
    });
    // Schedule startup after exec() begins, including failure completion.
    QTimer::singleShot(0, &app, [&] {
        if (!owner.start(*record, keeper, *plan, {})) app.exit(1);
    });
    return app.exec();
}
