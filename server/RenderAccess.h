// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

#include <QFile>
#include <QList>
#include <QString>
#include <QStringList>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "RenderNodes.h"

namespace KRdp::RenderAccess
{
/**
 * AUD-FIX8 B3: a user's full group list - the primary group and every supplementary group,
 * including `render` and `video` - as initgroups() would install it. getgrouplist() may talk to
 * NSS (nscd, sssd): call it before fork(), never between fork and exec. nullopt: unresolvable.
 */
inline std::optional<std::vector<gid_t>> userGroups(const char *login, gid_t primary)
{
    if (!login || !*login) {
        return std::nullopt;
    }
    std::vector<gid_t> groups(64);
    int count = int(groups.size());
    if (getgrouplist(login, primary, groups.data(), &count) < 0) {
        if (count <= 0 || count > 65536) {
            return std::nullopt;
        }
        groups.resize(size_t(count));
        if (getgrouplist(login, primary, groups.data(), &count) < 0) {
            return std::nullopt;
        }
    }
    groups.resize(size_t(count));
    return groups;
}

struct Node {
    QString path;
    uid_t owner = 0;
    gid_t group = 0;
    mode_t mode = 0;
};

/// The render nodes of this host (stat()-based, RenderNodes::list()).
inline QList<Node> nodes(const QString &directory = QStringLiteral("/dev/dri"))
{
    QList<Node> result;
    for (const auto &path : RenderNodes::list(directory)) {
        struct stat st{};
        if (::stat(QFile::encodeName(path).constData(), &st) == 0) {
            result.append({path, st.st_uid, st.st_gid, mode_t(st.st_mode & 07777)});
        }
    }
    return result;
}

/// Read-write access by owner, group or other bits (a logind seat ACL is not counted: it only
/// exists while the user's session is on the seat).
inline bool canUse(const Node &node, uid_t uid, const std::vector<gid_t> &groups)
{
    if (uid == 0) return true;
    if (node.owner == uid) return (node.mode & 0600) == 0600;
    if (std::find(groups.begin(), groups.end(), node.group) != groups.end()) return (node.mode & 0060) == 0060;
    return (node.mode & 0006) == 0006;
}

inline QString groupName(gid_t gid)
{
    if (const group *entry = getgrgid(gid); entry && entry->gr_name) {
        return QString::fromLocal8Bit(entry->gr_name);
    }
    return QString::number(gid);
}

/**
 * A warning naming the fix when \a login cannot use a render node through its groups (empty when
 * it can use every one, or there is none). \a consequence says what that means on this broker.
 */
inline QString missingGroupWarning(const QString &login, uid_t uid, const std::vector<gid_t> &groups, const QList<Node> &nodes,
                                   const QString &consequence, const std::function<QString(gid_t)> &nameOf = groupName)
{
    QStringList missing;
    QStringList paths;
    for (const auto &node : nodes) {
        if (canUse(node, uid, groups)) continue;
        const QString name = nameOf(node.group);
        if (!missing.contains(name)) missing.append(name);
        paths.append(node.path);
    }
    if (missing.isEmpty()) {
        return {};
    }
    QStringList fixes;
    for (const auto &name : missing) {
        fixes.append(QStringLiteral("usermod -aG %1 %2").arg(name, login));
    }
    return QStringLiteral("User %1 is not in group %2 (owner of %3): %4. Fix: %5, then log the user out and back in (a running "
                          "desktop keeps its old groups).")
        .arg(login, missing.join(QStringLiteral("/")), paths.join(QStringLiteral(", ")), consequence, fixes.join(QStringLiteral("; ")));
}

/// missingGroupWarning() for an account, resolved by uid (NSS; not between fork and exec).
inline QString warningFor(uid_t uid, const QString &consequence, const QString &directory = QStringLiteral("/dev/dri"))
{
    const passwd *account = getpwuid(uid);
    if (!account || !account->pw_name) return {};
    const QString login = QString::fromLocal8Bit(account->pw_name);
    const auto groups = userGroups(account->pw_name, account->pw_gid);
    if (!groups) return {};
    return missingGroupWarning(login, uid, *groups, nodes(directory), consequence);
}
}
