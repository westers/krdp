// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionStorage.h"
#include "VirtualSessionLaunchPlan.h"
#include <QFile>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace KRdp
{
namespace
{
struct Fd {
    int value = -1;
    explicit Fd(int value = -1) : value(value) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
bool directory(int fd, quint32 uid, bool privateOnly)
{
    struct stat info{};
    return fd >= 0 && !fstat(fd, &info) && S_ISDIR(info.st_mode) && info.st_uid == uid
        && (privateOnly ? (info.st_mode & 0777) == 0700 : !(info.st_mode & 0022));
}
int openPath(const QString &path)
{
    if (!VirtualSessionLaunchPlan::absoluteCleanPath(path)) return -1;
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (const auto &part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (fd < 0) return -1;
        const int next = openat(fd, QFile::encodeName(part).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        ::close(fd); fd = next;
        struct stat info{};
        if (fd >= 0 && (fstat(fd, &info) || (info.st_uid != 0 && info.st_uid != getuid())
            || ((info.st_mode & 0022) && !(info.st_uid == 0 && (info.st_mode & S_ISVTX))))) {
            ::close(fd); return -1;
        }
    }
    return fd;
}
int child(int parent, const char *name, quint32 uid, bool privateOnly, bool fresh = false)
{
    if (mkdirat(parent, name, 0700) && (errno != EEXIST || fresh)) return -1;
    const int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory(fd, uid, privateOnly)) return fd;
    if (fd >= 0) ::close(fd);
    return -1;
}
bool uuid(const QString &value)
{
    return !QUuid(value).isNull() && QUuid(value).toString(QUuid::WithoutBraces) == value;
}
}

VirtualSessionStorage::~VirtualSessionStorage()
{
    if (m_lock >= 0) ::close(m_lock);
}

std::unique_ptr<VirtualSessionStorage> VirtualSessionStorage::prepare(quint32 uid, const QString &home,
    const QString &session, const QString &launch, const QByteArray &token, QString *error)
{
    return prepareAt(uid, home, QStringLiteral("/run/user/%1").arg(uid), session, launch, token, error);
}

std::unique_ptr<VirtualSessionStorage> VirtualSessionStorage::prepareAt(quint32 uid, const QString &home,
    const QString &runtimeBase, const QString &session, const QString &launch, const QByteArray &token, QString *error)
{
    const auto refuse = [error](const QString &why) -> std::unique_ptr<VirtualSessionStorage> {
        if (error) *error = why;
        return {};
    };
    if (!uid || uid != getuid() || uid != geteuid() || !uuid(session) || !uuid(launch) || token.size() != 32) {
        return refuse(QStringLiteral("Storage preparation requires the authenticated nonroot identity, UUIDs and a 32-byte token"));
    }
    Fd homeFd(openPath(home)), runtimeFd(openPath(runtimeBase));
    if (!directory(homeFd.value, uid, false) || !directory(runtimeFd.value, uid, true)) {
        return refuse(QStringLiteral("Home/runtime roots must be owned directories without symlink components; runtime must be private"));
    }
    Fd local(child(homeFd.value, ".local", uid, false));
    Fd share(child(local.value, "share", uid, false));
    Fd profiles(child(share.value, "krdp-virtual", uid, true));
    Fd sessions(child(profiles.value, "sessions", uid, true));
    Fd profile(child(sessions.value, session.toLatin1().constData(), uid, true));
    if (profile.value < 0) return refuse(QStringLiteral("Cannot create or validate private persistent profile"));

    auto result = std::unique_ptr<VirtualSessionStorage>(new VirtualSessionStorage);
    result->m_lock = openat(profile.value, "profile.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat info{};
    if (result->m_lock < 0 || fstat(result->m_lock, &info) || !S_ISREG(info.st_mode)
        || info.st_uid != uid || (info.st_mode & 0777) != 0600 || info.st_nlink != 1
        || flock(result->m_lock, LOCK_EX | LOCK_NB)) {
        return refuse(QStringLiteral("Profile is already in use or its lock file is unsafe"));
    }
    for (const auto *name : {"config", "cache", "state", "data"}) {
        Fd subdir(child(profile.value, name, uid, true));
        if (subdir.value < 0) return refuse(QStringLiteral("Unsafe persistent profile subdirectory"));
    }
    Fd runtimes(child(runtimeFd.value, "krdp-virtual", uid, true));
    Fd runtime(child(runtimes.value, launch.toLatin1().constData(), uid, true, true));
    if (runtime.value < 0) return refuse(QStringLiteral("Runtime must be a new private directory; adoption requires separate validation"));
    Fd secret(openat(runtime.value, "worker-token", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (secret.value < 0) return refuse(QStringLiteral("Cannot create fresh worker credential"));
    qsizetype written = 0;
    while (written < token.size()) {
        const auto count = ::write(secret.value, token.constData() + written, token.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return refuse(QStringLiteral("Cannot write worker credential"));
        written += count;
    }
    if (fsync(secret.value)) return refuse(QStringLiteral("Cannot flush worker credential"));
    result->m_runtimePath = runtimeBase + QStringLiteral("/krdp-virtual/") + launch;
    result->m_profilePath = home + QStringLiteral("/.local/share/krdp-virtual/sessions/") + session;
    result->m_sessionId = session;
    result->m_ownerUid = uid;
    if (error) error->clear();
    return result;
}
}
