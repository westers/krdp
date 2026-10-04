// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerHostPublicSnapshot.h"
#include "BrokerHostAdmin.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
using Scope = KRdp::BrokerHostSettings::Scope;
namespace KRdp::BrokerHostPublicSnapshot {
namespace {
struct Fd {
    int value = -1;
    explicit Fd(int descriptor = -1) : value(descriptor) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
QJsonObject withoutHidden(const QJsonObject &map)
{
    QJsonObject result;
    for (auto it = map.begin(); it != map.end(); ++it) if (!isHiddenKey(it.key())) result.insert(it.key(), it.value());
    return result;
}
QJsonObject pick(const QJsonObject &source, const QStringList &keys)
{
    QJsonObject result;
    for (const auto &key : keys) if (source.contains(key)) result.insert(key, source[key]);
    return result;
}
bool trusted(const struct stat &info, bool allowCurrentUser)
{
    return (!info.st_uid || (allowCurrentUser && info.st_uid == ::geteuid())) && !(info.st_mode & 0022);
}
}

QString defaultDirectory() { return u"/var/lib/farside-public"_s; }
QString snapshotFileName(Scope scope) { return BrokerHostAdmin::scopeName(scope) + u".json"_s; }
bool isHiddenKey(const QString &key) { return key == u"Certificate" || key == u"CertificateKey"; }

QJsonObject sanitize(Scope scope, const QJsonObject &full)
{
    QJsonObject result = pick(full, {u"version"_s, u"scope"_s, u"revision"_s, u"runtimeVerified"_s, u"application"_s});
    for (const auto &name : {u"values"_s, u"defaults"_s, u"effective"_s}) result.insert(name, withoutHidden(full[name].toObject()));
    if (scope == Scope::VirtualSession) {
        QJsonArray devices;
        for (const auto &item : full[u"renderDevices"_s].toArray())
            devices.append(pick(item.toObject(), {u"pci"_s, u"driver"_s, u"render"_s}));
        result.insert(u"renderDevices"_s, devices);
    } else {
        result.insert(u"tls"_s, pick(full[u"tls"_s].toObject(),
            {u"state"_s, u"administratorManaged"_s, u"fingerprint"_s, u"algorithm"_s, u"notBefore"_s, u"notAfter"_s}));
        result.insert(u"cameraLoopback"_s, pick(full[u"cameraLoopback"_s].toObject(), {u"supported"_s, u"state"_s}));
    }
    return result;
}

bool write(const QString &directory, Scope scope, const QJsonObject &full, QString *error)
{
    const auto fail = [error](const QString &text) { if (error) *error = text; return false; };
    const auto dir = QFile::encodeName(directory);
    if (::mkdir(dir.constData(), 0755) && errno != EEXIST) return fail(u"snapshot directory cannot be created"_s);
    const Fd directoryFd(::open(dir.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (directoryFd.value < 0 || ::fstat(directoryFd.value, &info) || info.st_uid != ::geteuid() || (info.st_mode & 0022))
        return fail(u"snapshot directory is unsafe"_s);
    if ((info.st_mode & 0755) != 0755 && ::fchmod(directoryFd.value, 0755)) return fail(u"snapshot directory mode cannot be set"_s);
    QByteArray bytes = QJsonDocument(sanitize(scope, full)).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    if (bytes.size() > MaximumBytes) return fail(u"snapshot is oversized"_s);
    const auto name = snapshotFileName(scope).toUtf8();
    const QByteArray temporary = QByteArray(".") + name + ".tmp." + QByteArray::number(qlonglong(::getpid()));
    ::unlinkat(directoryFd.value, temporary.constData(), 0);
    {
        const Fd file(::openat(directoryFd.value, temporary.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
        if (file.value < 0 || ::fchmod(file.value, 0644)) return fail(u"snapshot cannot be created"_s);
        qsizetype written = 0;
        while (written < bytes.size()) {
            const auto count = ::write(file.value, bytes.constData() + written, size_t(bytes.size() - written));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { ::unlinkat(directoryFd.value, temporary.constData(), 0); return fail(u"snapshot cannot be written"_s); }
            written += count;
        }
        if (::fsync(file.value)) { ::unlinkat(directoryFd.value, temporary.constData(), 0); return fail(u"snapshot cannot be synced"_s); }
    }
    if (::renameat(directoryFd.value, temporary.constData(), directoryFd.value, name.constData())) {
        ::unlinkat(directoryFd.value, temporary.constData(), 0);
        return fail(u"snapshot cannot be published"_s);
    }
    ::fsync(directoryFd.value);
    return true;
}

ReadResult read(const QString &directory, Scope scope, bool allowCurrentUser)
{
    const auto unavailable = ReadResult{{}, u"Host settings snapshot is unavailable. Start or restart the service once so it can publish it."_s};
    const Fd directoryFd(::open(QFile::encodeName(directory).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (directoryFd.value < 0 || ::fstat(directoryFd.value, &info) || !trusted(info, allowCurrentUser)) return unavailable;
    const Fd file(::openat(directoryFd.value, snapshotFileName(scope).toUtf8().constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (file.value < 0 || ::fstat(file.value, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 || !trusted(info, allowCurrentUser)
        || info.st_size <= 0 || info.st_size > MaximumBytes) return unavailable;
    QByteArray bytes;
    char chunk[4096];
    while (bytes.size() <= MaximumBytes) {
        const auto count = ::read(file.value, chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return unavailable;
        if (!count) break;
        bytes.append(chunk, qsizetype(count));
    }
    const auto document = QJsonDocument::fromJson(bytes);
    if (bytes.size() > MaximumBytes || !document.isObject()) return unavailable;
    return {document.object(), {}};
}
}
