// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>

#include <QFile>
#include <QString>
#include <QStringList>

#include <dirent.h>
#include <sys/stat.h>

namespace KRdp::RenderNodes
{
/**
 * AUD-FIX8 B3: the DRM render nodes ("renderD*" character devices) in \a directory, sorted by
 * name, as absolute paths.
 *
 * Decided by stat(), never by the directory entry's type. A virtual desktop runs in a bwrap
 * sandbox whose /dev is a fresh tmpfs with the selected render node bind-mounted onto an empty
 * regular file: readdir() reports that entry as DT_REG while stat() sees the character device.
 * QDir's QDir::System filter trusts the entry type, so the worker's encoder probe found no render
 * node at all and reported no hardware encoder on ace and cray (KPipeWire, which stat()s through
 * libdrm, still encoded in hardware).
 */
inline QStringList list(const QString &directory = QStringLiteral("/dev/dri"))
{
    QStringList nodes;
    const QByteArray path = QFile::encodeName(directory);
    DIR *dir = ::opendir(path.constData());
    if (!dir) {
        return nodes;
    }
    while (const dirent *entry = ::readdir(dir)) {
        const QByteArray name(entry->d_name);
        if (!name.startsWith("renderD") || name.size() == 7) {
            continue;
        }
        struct stat st{};
        const QByteArray full = path + '/' + name;
        if (::stat(full.constData(), &st) == 0 && S_ISCHR(st.st_mode)) {
            nodes.append(QFile::decodeName(full));
        }
    }
    ::closedir(dir);
    std::sort(nodes.begin(), nodes.end());
    return nodes;
}

/**
 * list() with \a preferred first when it is one of the nodes: the virtual-desktop launcher
 * exports the render node it granted as KRDP_RENDER_NODE, and the probe should try that one.
 */
inline QStringList ordered(const QString &preferred, const QString &directory = QStringLiteral("/dev/dri"))
{
    QStringList nodes = list(directory);
    const auto found = nodes.indexOf(preferred);
    if (found > 0) {
        nodes.move(found, 0);
    }
    return nodes;
}

/// The node the virtual-desktop launcher granted (KRDP_RENDER_NODE), empty when unset.
inline QString preferredFromEnvironment()
{
    return qEnvironmentVariable("FARSIDE_RENDER_NODE");
}
}
