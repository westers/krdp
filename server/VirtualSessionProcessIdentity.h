// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QFile>
#include <optional>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <linux/magic.h>
namespace KRdp {
// Start ticks disambiguate a PID read back after a service crash. Callers must
// pin a pidfd BEFORE comparing this value if they intend to signal the process.
inline std::optional<quint64> virtualProcessStartTime(pid_t pid)
{
    if (pid <= 1) return {};
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.read(65537);
    const QByteArray prefix = QByteArray::number(pid) + " (";
    if (file.error() != QFileDevice::NoError || bytes.size() > 65536 || !bytes.endsWith('\n')
        || !bytes.startsWith(prefix)) return {};
    // comm can contain spaces, ')' and newlines; the final ')' closes it.
    const auto end = bytes.lastIndexOf(") ");
    if (end < 0) return {};
    const auto fields = bytes.mid(end + 2).split(' ');
    if (fields.size() < 20) return {};
    bool ok = false;
    const auto ticks = fields[19].toULongLong(&ok); // field22, state is field3
    return ok && ticks && QByteArray::number(ticks) == fields[19] ? std::optional<quint64>(ticks) : std::nullopt;
}
// Modern 64-bit pidfs exposes the kernel's boot-unique process identifier as
// the pidfd inode. Refuse old anonymous-inode pidfds and 32-bit truncation.
// Unlike clock ticks alone, this also separates same-tick PID reuse.
inline std::optional<quint64> virtualPidfdIdentity(int fd)
{
    struct stat st{}; struct statfs fs{};
    if (sizeof(void *) < 8 || sizeof(st.st_ino) < 8 || fstatfs(fd, &fs) || fs.f_type != PID_FS_MAGIC
        || fstat(fd, &st) || st.st_ino < 2) return {};
    return quint64(st.st_ino);
}
}
