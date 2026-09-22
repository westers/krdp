// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionProvisioningReceipt.h"
#include "VirtualSessionRuntimeProfile.h"
#include <QFile>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace KRdp {
namespace {
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
bool fail(QString *error) {
    if (error) *error = QStringLiteral("Missing, unsafe, stale or non-durable first-provisioning receipt");
    return false;
}
bool same(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode
        && a.st_uid == b.st_uid && a.st_gid == b.st_gid && a.st_nlink == b.st_nlink
        && a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_sec == b.st_ctim.tv_sec
        && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
int directory(const QString &path, unsigned owner, bool fixture) {
    if (!path.startsWith(QLatin1Char('/'))) return -1;
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat st{};
    if (fd < 0) return -1;
    if (fstat(fd, &st) || st.st_uid || st.st_gid || (st.st_mode & 0022)) { ::close(fd); return -1; }
    const auto parts = path.mid(1).split(QLatin1Char('/'));
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const auto &part = parts[i];
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..") || part.contains(QChar::Null)) {
            ::close(fd); return -1;
        }
        const int next = openat(fd, QFile::encodeName(part).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(fd); fd = next;
        if (fd < 0) return -1;
        const bool last = i == parts.size() - 1;
        if (fstat(fd, &st) || !S_ISDIR(st.st_mode)
            || (last ? (st.st_uid != owner || st.st_gid != owner || (st.st_mode & 07777) != 0700)
                     : (!fixture && (st.st_uid || st.st_gid || (st.st_mode & 0022))))) {
            ::close(fd); return -1;
        }
    }
    return fd;
}
bool readExact(int fd, unsigned owner, QByteArray &bytes, struct stat &st) {
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != owner || st.st_gid != owner
        || (st.st_mode & 07777) != 0600 || st.st_nlink != 1 || st.st_size <= 0 || st.st_size > 512) return false;
    bytes.resize(st.st_size);
    qsizetype pos = 0;
    while (pos < bytes.size()) {
        const auto n = pread(fd, bytes.data() + pos, bytes.size() - pos, pos);
        if (n <= 0) return false;
        pos += n;
    }
    char tail;
    struct stat after{};
    return pread(fd, &tail, 1, pos) == 0 && !fstat(fd, &after) && same(st, after);
}
}
bool VirtualSessionProvisioningReceipt::validate(const VirtualSessionMaintenanceRecord &expected,
    const VirtualSessionRuntimeProfile &approvedProfile, QString *error) {
    uid_t real{}, effective{}, saved{};
    if (getresuid(&real, &effective, &saved) || real != 0 || effective != 0 || saved != 0) return fail(error);
    QFile boot(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!boot.open(QIODevice::ReadOnly)) return fail(error);
    const QByteArray bytes = boot.read(128);
    if (bytes.size() != 37 || !bytes.endsWith('\n')) return fail(error);
    return validateAt(QStringLiteral("/var/lib/krdp/maintenance"), 0, false,
        QString::fromLatin1(bytes.first(36)), approvedProfile.digest(), expected, error);
}
bool VirtualSessionProvisioningReceipt::validateAt(const QString &path, unsigned owner, bool fixture,
    const QString &currentBoot, const QString &profileDigest,
    const VirtualSessionMaintenanceRecord &expected, QString *error) {
    if (error) error->clear();
    if (!expected.valid() || expected.phase != VirtualSessionMaintenanceRecord::Phase::InitialBlocked
        || expected.boot != currentBoot || expected.profile != profileDigest) return fail(error);
    QByteArray canonical("KRDP-FIRST-PROVISIONING-1\n");
    for (const auto &field : {expected.installation, expected.transaction, expected.generation,
             expected.boot, expected.profile, expected.attestation}) canonical += field.toLatin1() + '\n';
    Fd dir(directory(path, owner, fixture));
    if (dir.value < 0) return fail(error);
    struct stat dirBefore{};
    if (fstat(dir.value, &dirBefore)) return fail(error);
    Fd file(openat(dir.value, "first-provisioning-receipt", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    QByteArray bytes;
    struct stat before{};
    if (!readExact(file.value, owner, bytes, before) || bytes != canonical
        || fsync(file.value) || fsync(dir.value)) return fail(error);
    // Rewalk names after syncing, then reread the pinned file. No repair or
    // deletion on any uncertainty. Exclusion remains the caller's obligation.
    Fd namedDir(directory(path, owner, fixture));
    struct stat dirAfter{}, named{};
    QByteArray afterBytes;
    struct stat after{};
    if (namedDir.value < 0 || fstat(namedDir.value, &dirAfter) || !same(dirBefore, dirAfter)
        || fstatat(namedDir.value, "first-provisioning-receipt", &named, AT_SYMLINK_NOFOLLOW)
        || !same(before, named) || !readExact(file.value, owner, afterBytes, after)
        || !same(before, after) || afterBytes != canonical) return fail(error);
    return true;
}
}
