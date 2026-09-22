// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Root-service entrypoint, NOT setuid and NOT an RDP command endpoint.
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QDebug>
#include <cerrno>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <pwd.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace {
struct Device { QByteArray path; dev_t number; };
bool snapshot(const QByteArray &path, std::vector<Device> &devices)
{
    struct stat info{};
    if (lstat(path.constData(), &info) || !S_ISCHR(info.st_mode)) return false;
    devices.push_back({path, info.st_rdev});
    return true;
}

bool snapshotDriverNode(const QByteArray &path, const QString &driver, uint minorNumber, std::vector<Device> &devices)
{
    QFile registrations(QStringLiteral("/proc/devices"));
    if (!registrations.open(QIODevice::ReadOnly)) return false;
    const auto characters = QString::fromUtf8(registrations.read(65536)).section(QStringLiteral("Block devices:"), 0, 0);
    const auto match = QRegularExpression(QStringLiteral("^\\s*([0-9]+)\\s+%1\\s*$").arg(QRegularExpression::escape(driver)),
        QRegularExpression::MultilineOption).match(characters);
    bool ok = false;
    const uint majorNumber = match.captured(1).toUInt(&ok);
    if (!match.hasMatch() || !ok) return false;
    struct stat info{};
    if (lstat(path.constData(), &info) || !S_ISCHR(info.st_mode)
        || major(info.st_rdev) != majorNumber || minor(info.st_rdev) != minorNumber) return false;
    devices.push_back({path, info.st_rdev});
    return true;
}

// Service installation, not the user's checkout. Reject writable ancestors and
// symlinks so no user can replace code before it is launched by this service.
bool trustedExecutable(const QString &path)
{
    if (!path.startsWith(QLatin1Char('/')) || path == QStringLiteral("/") || QDir::cleanPath(path) != path
        || QFileInfo(path).canonicalFilePath() != path) return false;
    QString current = path;
    bool executable = true;
    while (true) {
        struct stat info{};
        if (lstat(QFile::encodeName(current).constData(), &info) || info.st_uid != 0
            || (info.st_mode & 0022) || (executable ? !S_ISREG(info.st_mode) : !S_ISDIR(info.st_mode))) return false;
        if (executable && !(info.st_mode & 0111)) return false;
        if (current == QStringLiteral("/")) return true;
        current = QFileInfo(current).path();
        executable = false;
    }
}
int fail(const char *stage)
{
    // Do not print argv, environment or credential data.
    qCritical() << "Virtual device entry refused:" << stage;
    return 1;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("uid"), QStringLiteral("OS UID authenticated by trusted PAM broker"), QStringLiteral("uid")});
    parser.addOption({QStringLiteral("allow-render-pci"), QStringLiteral("Administrator-approved PCI BDF; repeat in priority order"), QStringLiteral("bdf")});
    parser.addPositionalArgument(QStringLiteral("guardian"), QStringLiteral("Installed guardian and arguments after --"), QStringLiteral("guardian [arguments...]") );
    parser.process(app);
    // Both IDs must be root: installing setuid must not make this a user API.
    if (getuid() != 0 || geteuid() != 0) return fail("requires an explicit root service invocation");
    bool validUid = false;
    const auto uidText = parser.value(QStringLiteral("uid"));
    const uint uid = uidText.toUInt(&validUid);
    const auto command = parser.positionalArguments();
    if (!validUid || !uid || uid == uint(-1) || QString::number(uid) != uidText
        || command.isEmpty() || !trustedExecutable(command.first())) return fail("identity or installed guardian path");
    struct passwd account{};
    struct passwd *resolved = nullptr;
    QByteArray buffer(16384, '\0');
    int result;
    while ((result = getpwuid_r(uid, &account, buffer.data(), buffer.size(), &resolved)) == ERANGE
        && buffer.size() < 1048576) buffer.resize(buffer.size() * 2);
    if (result || !resolved || account.pw_uid != uid || account.pw_gid == gid_t(-1)) return fail("OS account resolution");
    const gid_t gid = account.pw_gid;

    const auto allow = parser.values(QStringLiteral("allow-render-pci"));
    const QRegularExpression pci(QStringLiteral("^[0-9a-f]{4}:[0-9a-f]{2}:[01][0-9a-f]\\.[0-7]$"));
    QSet<QString> seen;
    if (allow.isEmpty() || allow.size() > 32) return fail("explicit bounded GPU policy required");
    for (const auto &bdf : allow) {
        if (!pci.match(bdf).hasMatch() || seen.contains(bdf)) return fail("invalid or duplicate PCI identity");
        seen.insert(bdf);
    }
    std::vector<Device> gpu;
    QString selected, render;
    for (const auto &bdf : allow) {
        const auto candidateRender = QFileInfo(QStringLiteral("/dev/dri/by-path/pci-%1-render").arg(bdf)).canonicalFilePath();
        if (!QRegularExpression(QStringLiteral("^/dev/dri/renderD[0-9]+$")).match(candidateRender).hasMatch()) continue;
        const auto device = QFileInfo(QStringLiteral("/sys/class/drm/%1/device").arg(QFileInfo(candidateRender).fileName())).canonicalFilePath();
        if (device.isEmpty() || QFileInfo(device).fileName() != bdf) continue;
        const auto driver = QFileInfo(QFileInfo(device + QStringLiteral("/driver")).canonicalFilePath()).fileName();
        std::vector<Device> nodes;
        if (!snapshot(QFile::encodeName(candidateRender), nodes)) continue;
        const auto number = nodes.front().number;
        const auto deviceNumber = QStringLiteral("%1:%2").arg(major(number)).arg(minor(number));
        QFile sysfsNumber(QStringLiteral("/sys/class/drm/%1/dev").arg(QFileInfo(candidateRender).fileName()));
        if (!sysfsNumber.open(QIODevice::ReadOnly) || sysfsNumber.read(128).trimmed() != deviceNumber.toLatin1()
            || QFileInfo(QStringLiteral("/sys/dev/char/%1/device").arg(deviceNumber)).canonicalFilePath() != device) continue;
        if (driver == QStringLiteral("nvidia")) {
            QFile information(QStringLiteral("/proc/driver/nvidia/gpus/%1/information").arg(bdf));
            if (!information.open(QIODevice::ReadOnly)) continue;
            const auto match = QRegularExpression(QStringLiteral("^Device Minor:\\s+([0-9]+)\\s*$"), QRegularExpression::MultilineOption)
                .match(QString::fromUtf8(information.read(65536)));
            bool minorOk = false;
            const auto minor = match.captured(1).toUInt(&minorOk);
            if (!match.hasMatch() || !minorOk || minor > 255
                || !snapshotDriverNode("/dev/nvidia" + QByteArray::number(minor), QStringLiteral("nvidia"), minor, nodes)
                || !snapshotDriverNode("/dev/nvidiactl", QStringLiteral("nvidiactl"), 255, nodes)
                || !snapshotDriverNode("/dev/nvidia-uvm", QStringLiteral("nvidia-uvm"), 0, nodes)) continue;
        } else if (driver != QStringLiteral("amdgpu") && driver != QStringLiteral("i915") && driver != QStringLiteral("xe")) continue;
        selected = bdf;
        render = candidateRender;
        gpu = std::move(nodes);
        break;
    }
    if (gpu.empty()) return fail("no approved GPU has a complete supported device set");
    std::vector<Device> basic;
    for (const auto *path : {"/dev/null", "/dev/zero", "/dev/full", "/dev/random", "/dev/urandom", "/dev/tty"}) {
        if (!snapshot(path, basic)) return fail("basic device snapshot");
    }
    // Prepare argv before privilege changes. Only FD0 is the token channel.
    std::vector<QByteArray> encoded;
    for (const auto &arg : command) encoded.push_back(QFile::encodeName(arg));
    std::vector<char *> arguments;
    for (auto &arg : encoded) arguments.push_back(arg.data());
    arguments.push_back(nullptr);
    char pathEnv[] = "PATH=/usr/bin:/bin", langEnv[] = "LANG=C.UTF-8";
    char *environment[] = {pathEnv, langEnv, nullptr};
    umask(0077);
    // No mount operation is allowed before both isolation gates succeed.
    if (unshare(CLONE_NEWNS) || mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr)) return fail("private mount namespace");
    if (mount("krdp-virtual-devices", "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=0755,size=1048576")) return fail("private device filesystem");
    if (mkdir("/dev/dri", 0755) || mkdir("/dev/dri/by-path", 0755)) return fail("private device directories");
    // umask must not make these root-owned directories inaccessible to the UID.
    if (chmod("/dev/dri", 0755) || chmod("/dev/dri/by-path", 0755)) return fail("private device traversal");
    for (const auto &node : basic) {
        if (mknod(node.path.constData(), S_IFCHR | 0666, node.number) || chmod(node.path.constData(), 0666)) return fail("private basic device creation");
    }
    for (const auto &node : gpu) {
        if (mknod(node.path.constData(), S_IFCHR | 0600, node.number)
            || chown(node.path.constData(), uid, gid)) return fail("private selected GPU device creation");
    }
    const auto link = QFile::encodeName(QStringLiteral("/dev/dri/by-path/pci-%1-render").arg(selected));
    const auto target = QFile::encodeName(QStringLiteral("../") + QFileInfo(render).fileName());
    if (symlink(target.constData(), link.constData()) || symlink("/proc/self/fd", "/dev/fd")
        || symlink("/proc/self/fd/0", "/dev/stdin") || symlink("/proc/self/fd/1", "/dev/stdout")
        || symlink("/proc/self/fd/2", "/dev/stderr")) return fail("private device links");
    // Drop all bounding capabilities supported by the running kernel, not just
    // those known by build-time headers. EINVAL marks the end of the set.
    for (int capability = 0; ; ++capability) {
        const int present = prctl(PR_CAPBSET_READ, capability, 0, 0, 0);
        if (present < 0) {
            if (errno == EINVAL) break;
            return fail("capability enumeration");
        }
        if (prctl(PR_CAPBSET_DROP, capability, 0, 0, 0)) return fail("bounding capability drop");
    }
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0)
        || prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) || setgroups(0, nullptr)
        || setresgid(gid, gid, gid) || setresuid(uid, uid, uid)
        || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return fail("privilege drop");
    __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3, 0};
    __user_cap_data_struct capabilities[2]{};
    if (syscall(SYS_capset, &header, capabilities)) return fail("capability clear");
    if (syscall(SYS_close_range, 3U, ~0U, 0U)) return fail("inherited descriptor closure");
    if (chdir("/")) return fail("working directory");
    execve(arguments[0], arguments.data(), environment);
    return fail("guardian exec");
}
