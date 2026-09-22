// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Independent service child, not an authentication endpoint or setuid helper.
#include "VirtualSessionJournal.h"
#include "VirtualSessionLogin.h"
#include "VirtualSessionPam.h"
#include "VirtualSessionOwnerWatch.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <atomic>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace {
struct Fd {
    int value;
    ~Fd() { if (value >= 0) close(value); }
};
bool trustedPolicy()
{
    // Refuse a missing service instead of allowing PAM's fallback to "other".
    // The supported root-installed policy and its includes are an install gate.
    QString path = QStringLiteral("/etc/pam.d/krdp-virtual-session");
    if (QFileInfo(path).canonicalFilePath() != path) return false;
    bool file = true;
    while (true) {
        struct stat st{};
        if (lstat(QFile::encodeName(path).constData(), &st) || st.st_uid || (st.st_mode & 0022)
            || (file ? !S_ISREG(st.st_mode) : !S_ISDIR(st.st_mode))) return false;
        if (path == QStringLiteral("/")) return true;
        path = QFileInfo(path).path(); file = false;
    }
}
bool privateRuntime(quint32 uid)
{
    Fd root{open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (root.value < 0) return false;
    const auto directory = [](int fd, quint32 owner, bool privateOnly) {
        struct stat st{};
        return fd >= 0 && !fstat(fd, &st) && S_ISDIR(st.st_mode) && st.st_uid == owner
            && (privateOnly ? (st.st_mode & 07777) == 0700 : !(st.st_mode & 0022));
    };
    if (!directory(root.value, 0, false)) return false;
    Fd run{openat(root.value, "run", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!directory(run.value, 0, false)) return false;
    Fd users{openat(run.value, "user", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!directory(users.value, 0, false)) return false;
    Fd user{openat(users.value, QByteArray::number(uid).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    return directory(user.value, uid, true);
}
int refused(const char *stage) { qCritical("Virtual PAM keeper refused: %s", stage); return 1; }
}
int main(int argc, char **argv)
{
    // Environment comes solely from the trusted service parent's env-i launch.
    // Enforce that contract before Qt/PAM can consult alternate bus/display data.
    for (char **entry = environ; entry && *entry; ++entry) {
        const QByteArray value(*entry);
        if (value != "PATH=/usr/bin:/bin" && value != "LANG=C.UTF-8") return refused("unclean environment");
    }
    QCoreApplication app(argc, argv);
    QCommandLineParser parser; parser.addHelpOption();
    for (const auto *name : {"session", "launch", "parent"})
        parser.addOption({QString::fromLatin1(name), QStringLiteral("Trusted service owner parameter"), QStringLiteral("value")});
    parser.process(app);
    if (getuid() || geteuid()) return refused("explicit root service required");
    if (!parser.positionalArguments().isEmpty()) return refused("arguments");
    bool validParent = false;
    const auto parent = parser.value(QStringLiteral("parent")).toInt(&validParent);
    if (!validParent || parent <= 1 || getppid() != parent) return refused("parent identity");
    Fd parentFd{int(syscall(SYS_pidfd_open, parent, 0))};
    if (parentFd.value < 0 || getppid() != parent) return refused("parent pidfd");
    // Catch closed/broken owner channels instead of dying under default SIGPIPE.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) return refused("pipe handling");
    KRdp::VirtualSessionOwnerWatch watch(parentFd.value);
    struct stat input{}, output{};
    if (fstat(0, &input) || fstat(1, &output) || !S_ISFIFO(input.st_mode) || !S_ISFIFO(output.st_mode))
        return refused("private owner pipes required");
    const auto record = KRdp::VirtualSessionJournal::readLaunchIntent(parser.value(QStringLiteral("session")));
    if (!record || record->launch != parser.value(QStringLiteral("launch"))) return refused("launch intent");
    QFile boot(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!boot.open(QIODevice::ReadOnly) || QString::fromLatin1(boot.read(128)).trimmed() != record->boot)
        return refused("boot identity");
    struct passwd account{}, *resolved = nullptr;
    QByteArray buffer(16384, '\0'); int result;
    while ((result = getpwuid_r(record->uid, &account, buffer.data(), buffer.size(), &resolved)) == ERANGE
        && buffer.size() < 1048576) buffer.resize(buffer.size() * 2);
    if (result || !resolved || account.pw_uid != record->uid || !account.pw_name || !trustedPolicy())
        return refused("OS account or installed PAM policy");
    // Crash recovery must be able to pin this exact keeper even if PAM never
    // returns or Ready never reaches the parent. Failure forbids opening PAM.
    if (!KRdp::VirtualSessionJournal::recordKeeper(*record)) return refused("durable keeper identity");
    auto pam = KRdp::VirtualSessionPam::open(record->uid, QByteArray(account.pw_name), record->launch, [&watch] { watch.closing(); });
    if (!pam) return refused("PAM session");
    // Every failure after open still closes PAM before watch/descriptors die.
    const auto login = KRdp::VirtualSessionLogin::read(getpid());
    QFile cgroup(QStringLiteral("/proc/self/cgroup"));
    if (getuid() || geteuid() || !login || !login->matchesLaunch(record->uid, getpid(), pam->sessionId(), pam->runtimeDirectory(), record->launch)
        || !privateRuntime(record->uid) || !cgroup.open(QIODevice::ReadOnly)) return refused("login ownership/runtime");
    const auto membership = cgroup.read(65537);
    if (membership.size() > 65536 || !membership.startsWith("0::/")
        || membership.count('\n') != 1 || !membership.endsWith((QLatin1Char('/') + login->scope + QLatin1Char('\n')).toUtf8()))
        return refused("session scope membership");
    const QByteArray ready = QJsonDocument(QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("type"), QStringLiteral("ready")},
        {QStringLiteral("session"), record->session}, {QStringLiteral("launch"), record->launch},
        {QStringLiteral("login"), login->id}, {QStringLiteral("uid"), qint64(record->uid)},
        {QStringLiteral("leader"), qint64(getpid())}}).toJson(QJsonDocument::Compact) + '\n';
    // Small atomic pipe record; never emit a token, password or PAM environment.
    if (ready.size() > 1024 || write(1, ready.constData(), ready.size()) != ready.size()) return refused("ready delivery");
    watch.retained();
    // One private lifetime command, not a reusable RPC or RDP-facing endpoint.
    QByteArray command;
    bool stop = false;
    while (!stop) {
        char bytes[16]; const auto count = read(0, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        command.append(bytes, count);
        if (command.size() > 5 || !QByteArray("stop\n").startsWith(command)) break;
        stop = command == "stop\n";
    }
    const bool closed = pam->close();
    if (closed && !KRdp::VirtualSessionJournal::recordKeeperClosed(*record)) return refused("durable PAM close record");
    return stop && closed ? 0 : 1;
}
