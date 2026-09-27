// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualStockClient.h"
#include "VirtualInitialLayout.h"
#include "VirtualResize.h"

#include <QFile>
#include <freerdp/error.h>

#include <algorithm>
#include <fcntl.h>
#include <pwd.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace KRdp::VirtualStockClient
{
namespace
{
constexpr qint64 MaxConfigBytes = 256 * 1024;

int evenWithin(int value, int minimum, int maximum)
{
    value = std::clamp(value, minimum, maximum);
    return value - (value % 2);
}
}

std::optional<Policy> parsePolicy(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed == QLatin1String("attach-or-create")) return Policy::AttachOrCreate;
    if (trimmed == QLatin1String("refuse")) return Policy::Refuse;
    return std::nullopt;
}

Policy policyFromConfig(const QByteArray &contents)
{
    // The subset of KConfig syntax a KCM-written krdpserverrc uses: [Group]
    // headers and key=value lines; a key may carry [$...] or [locale] suffixes.
    std::optional<Policy> found;
    bool general = false;
    for (const QByteArray &raw : contents.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('[')) {
            general = line == "[General]";
            continue;
        }
        if (!general) continue;
        const qsizetype equals = line.indexOf('=');
        if (equals <= 0) continue;
        QByteArray key = line.left(equals).trimmed();
        if (const qsizetype bracket = key.indexOf('['); bracket > 0) key = key.left(bracket).trimmed();
        if (key != "VirtualStockClientPolicy") continue;
        // KConfig: the last assignment in the file wins.
        found = parsePolicy(QString::fromUtf8(line.mid(equals + 1)));
    }
    return found.value_or(Policy::AttachOrCreate);
}

Policy readPolicyFile(const QString &path, quint32 uid)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return Policy::AttachOrCreate;
    struct stat info{};
    QByteArray contents;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == uid && info.st_size >= 0 && info.st_size <= MaxConfigBytes) {
        contents.resize(qsizetype(info.st_size));
        qsizetype total = 0;
        while (total < contents.size()) {
            const ssize_t got = ::read(fd, contents.data() + total, size_t(contents.size() - total));
            if (got <= 0) break;
            total += got;
        }
        contents.truncate(total);
    }
    ::close(fd);
    return policyFromConfig(contents);
}

Policy readUserPolicy(quint32 uid)
{
    if (!uid) return Policy::AttachOrCreate;
    passwd account{};
    passwd *resolved = nullptr;
    std::vector<char> buffer(4096);
    int status = 0;
    while ((status = getpwuid_r(uid, &account, buffer.data(), buffer.size(), &resolved)) == ERANGE && buffer.size() < 1048576) {
        buffer.resize(buffer.size() * 2);
    }
    if (status || !resolved || !account.pw_dir || account.pw_dir[0] != '/') return Policy::AttachOrCreate;
    const QString path = QFile::decodeName(account.pw_dir) + QStringLiteral("/.config/krdpserverrc");
    // Open with the user's own file-system identity (Linux, this thread only):
    // the broker never reads, through this path, a file the user cannot.
    const auto previousUid = setfsuid(uid);
    const auto previousGid = setfsgid(account.pw_gid);
    const Policy policy = readPolicyFile(path, uid);
    setfsgid(previousGid);
    setfsuid(previousUid);
    return policy;
}

quint32 errorInfo(Refusal refusal)
{
    switch (refusal) {
    case Refusal::Policy:
        return ERRINFO_SERVER_DENIED_CONNECTION; // 0x07
    case Refusal::NoFreeSlot:
        return ERRINFO_CB_DESTINATION_POOL_NOT_FREE; // 0x408: no available endpoint in the pool
    case Refusal::StartFailed:
        return ERRINFO_CB_SESSION_ONLINE_VM_SESSMON_FAILED; // 0x412: the endpoint failed while starting
    case Refusal::StartTimeout:
        return ERRINFO_CB_SESSION_ONLINE_VM_BOOT_TIMEOUT; // 0x411: the endpoint took too long to start
    case Refusal::Displaced:
        return ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION; // 0x05
    }
    return ERRINFO_SERVER_DENIED_CONNECTION;
}

QVector<VirtualSessionJournal::Record::InitialOutput> initialOutputs(const ClientDisplay::Info &info, const Limits &limits,
                                                                    const QSize &fallback)
{
    const int maxDimension = std::max(320, std::min(limits.maxOutputDimension, ClientDisplay::MaxDimension));
    const RemoteTopologyDraft::Capabilities caps{.addVirtual = true, .maxOutputs = std::max(1, limits.maxOutputs),
        .maxOutputDimension = maxDimension, .maxAtlasDimension = limits.maxAtlasDimension};
    const auto commit = [](const VirtualInitialLayout::Plan &plan) {
        QVector<VirtualSessionJournal::Record::InitialOutput> outputs;
        for (const auto &entry : plan.outputs) {
            const auto &output = entry.output;
            outputs.append({output.logicalGeometry.topLeft(), output.nativePixels, output.scale, output.primary});
        }
        return outputs;
    };
    const auto sanitised = ClientDisplay::sanitize(info, fallback);
    if (sanitised.monitors.size() >= 2 && sanitised.monitors.size() <= limits.maxOutputs) {
        QVector<VirtualInitialLayout::Screen> screens;
        for (qsizetype i = 0; i < sanitised.monitors.size(); ++i) {
            const auto &monitor = sanitised.monitors[i];
            // Scale 1: standard monitor data has no scale the broker could trust.
            screens.append({QStringLiteral("monitor-%1").arg(i), monitor.geometry.topLeft(),
                QSize(evenWithin(monitor.geometry.width(), 320, maxDimension), evenWithin(monitor.geometry.height(), 200, maxDimension)),
                1.0, monitor.primary});
        }
        const auto plan = VirtualInitialLayout::plan(screens, QStringLiteral("stock-client"), caps);
        if (plan.valid()) return commit(plan);
    }
    // One output: the core desktop size (or the union of a usable monitor
    // list that has more outputs than allowed), clamped rather than replaced,
    // so a 5120x1440 client still gets its aspect; the fallback only when the
    // client gave no size at all.
    QSize size = sanitised.monitors.isEmpty() ? info.desktopSize : sanitised.desktopSize;
    if (size.width() <= 0 || size.height() <= 0) size = fallback;
    const VirtualInitialLayout::Screen screen{QStringLiteral("desktop"), QPoint(0, 0),
        QSize(evenWithin(size.width(), 320, maxDimension), evenWithin(size.height(), 200, maxDimension)), 1.0, true};
    const auto plan = VirtualInitialLayout::plan({screen}, QStringLiteral("stock-client"), caps);
    if (plan.valid()) return commit(plan);
    return {{QPoint(0, 0), screen.pixels, 1.0, true}};
}

std::optional<QSize> displayControlSize(const QList<VideoMonitor> &monitors)
{
    if (monitors.size() != 1) return std::nullopt;
    const QSize requested = monitors.first().geometry.size();
    if (requested.width() <= 0 || requested.height() <= 0) return std::nullopt;
    const QSize size(evenWithin(requested.width(), 320, 4096), evenWithin(requested.height(), 200, 4096));
    if (!VirtualResize::validRequest(size, 1.0)) return std::nullopt;
    return size;
}
}
