// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "UserConfiguration.h"

#include <QFile>
#include <QScopeGuard>
#include <cerrno>
#include <fcntl.h>
#include <pwd.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace KRdp::UserConfiguration
{
std::optional<QByteArray> readFile(const QString &path, quint32 uid)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return std::nullopt;
    const auto closeFile = qScopeGuard([fd] { ::close(fd); });
    struct stat before{};
    if (::fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_uid != uid
        || before.st_size < 0 || before.st_size > MaximumBytes) return std::nullopt;
    QByteArray data(qsizetype(before.st_size), Qt::Uninitialized);
    qsizetype total = 0;
    while (total < data.size()) {
        const ssize_t got = ::read(fd, data.data() + total, size_t(data.size() - total));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return std::nullopt;
        total += got;
    }
    // Never parse a prefix or a mixture observed during an in-place save.
    struct stat after{};
    if (::fstat(fd, &after) || after.st_size != before.st_size || after.st_uid != uid
        || after.st_mtim.tv_sec != before.st_mtim.tv_sec || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec
        || after.st_ctim.tv_sec != before.st_ctim.tv_sec || after.st_ctim.tv_nsec != before.st_ctim.tv_nsec) return std::nullopt;
    return data;
}

namespace {
struct Account { gid_t gid; QString path; };
std::optional<Account> accountFor(quint32 uid)
{
    if (!uid) return std::nullopt;
    passwd account{};
    passwd *resolved = nullptr;
    std::vector<char> buffer(4096);
    int status = 0;
    while ((status = ::getpwuid_r(uid, &account, buffer.data(), buffer.size(), &resolved)) == ERANGE && buffer.size() < 1048576)
        buffer.resize(buffer.size() * 2);
    if (status || !resolved || account.pw_uid != uid || !account.pw_dir || account.pw_dir[0] != '/') return std::nullopt;
    return Account{account.pw_gid, QFile::decodeName(account.pw_dir) + QStringLiteral("/.config/farsideserverrc")};
}
}

std::optional<QString> userPath(quint32 uid)
{
    const auto account = accountFor(uid);
    return account ? std::optional(account->path) : std::nullopt;
}

std::optional<QByteArray> readUser(quint32 uid)
{
    const auto account = accountFor(uid);
    if (!account) return std::nullopt;
    const auto previousGid = ::setfsgid(account->gid);
    const auto previousUid = ::setfsuid(uid);
    const auto restoreIdentity = qScopeGuard([previousGid, previousUid] {
        ::setfsuid(previousUid);
        ::setfsgid(previousGid);
    });
    // These Linux calls return the old identity even when a switch failed.
    if (uid_t(::setfsuid(uid_t(-1))) != uid_t(uid) || gid_t(::setfsgid(gid_t(-1))) != account->gid) return std::nullopt;
    return readFile(account->path, uid);
}
}
