// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionJournal.h"
#include "VirtualSessionProcessIdentity.h"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <cmath>
#include <limits>
#include <climits>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace KRdp {
namespace {
bool uuid(const QString &s) { return !QUuid(s).isNull() && QUuid(s).toString(QUuid::WithoutBraces) == s; }
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
bool safeFile(int fd, quint32 owner) {
    struct stat s{};
    return !fstat(fd, &s) && S_ISREG(s.st_mode) && s.st_uid == owner
        && (s.st_mode & 07777) == 0600 && s.st_nlink == 1 && s.st_size > 0 && s.st_size <= 4096;
}
}
bool VirtualSessionJournal::Record::valid() const {
    return uid && uid != std::numeric_limits<quint32>::max() && uuid(session) && uuid(launch)
        && uuid(incarnation) && uuid(boot) && token.size() == 32;
}
VirtualSessionGuardianClient::Identity VirtualSessionJournal::Record::identity() const {
    return {uid, session, incarnation, QStringLiteral("/run/user/%1/krdp-virtual/%2/guardian.sock").arg(uid).arg(launch), token};
}
QString VirtualSessionJournal::Record::workerSocket() const {
    return QStringLiteral("/run/user/%1/krdp-virtual/%2/worker.sock").arg(uid).arg(launch);
}
VirtualSessionJournal::~VirtualSessionJournal() { close(m_directory); }
std::unique_ptr<VirtualSessionJournal> VirtualSessionJournal::open(QString *error) {
    if (getuid() || geteuid()) { fail(error, QStringLiteral("Journal requires the root service")); return {}; }
    return openAt(QStringLiteral("/var/lib/krdp/virtual-sessions"), 0, error);
}
std::optional<VirtualSessionJournal::Record> VirtualSessionJournal::readLaunchIntent(const QString &session, QString *error) {
    if (getuid() || geteuid()) { fail(error, QStringLiteral("Launch intent requires the root service")); return {}; }
    auto journal = openAt(QStringLiteral("/var/lib/krdp/virtual-sessions"), 0, error, false);
    return journal ? journal->readRecord(session, error) : std::nullopt;
}
bool VirtualSessionJournal::claimLaunch(const Record &expected, QString *error) {
    if (getuid() || geteuid()) return fail(error, QStringLiteral("Launch claim requires the root service"));
    auto journal = openAt(QStringLiteral("/var/lib/krdp/virtual-sessions"), 0, error, false);
    return journal && journal->claimRecord(expected, error);
}
bool VirtualSessionJournal::recordKeeper(const Record &expected, QString *error) {
    if (getuid() || geteuid()) return fail(error, QStringLiteral("Keeper record requires root"));
    const auto birth = virtualProcessStartTime(getpid());
    const int fd = int(syscall(SYS_pidfd_open, getpid(), 0));
    const auto identity = virtualPidfdIdentity(fd);
    if (fd >= 0) close(fd);
    auto journal = openAt(QStringLiteral("/var/lib/krdp/virtual-sessions"), 0, error, false);
    return birth && identity && journal && journal->writeKeeper(expected, {getpid(), *birth, *identity}, error);
}
std::optional<VirtualSessionJournal::Keeper> VirtualSessionJournal::readKeeper(const Record &expected, bool *missing, QString *error) {
    if (missing) *missing = false;
    if (getuid() || geteuid()) { fail(error, QStringLiteral("Keeper record requires root")); return {}; }
    auto journal = openAt(QStringLiteral("/var/lib/krdp/virtual-sessions"), 0, error, false);
    return journal ? journal->readKeeperRecord(expected, missing, error) : std::nullopt;
}
bool VirtualSessionJournal::writeKeeper(const Record &expected, const Keeper &keeper, QString *error) {
    const auto current = readRecord(expected.session, error);
    if (!current || *current != expected || !hasClaim(expected) || keeper.pid <= 1 || !keeper.startTicks || keeper.pidInode < 2)
        return fail(error, QStringLiteral("Invalid keeper launch identity"));
    const QByteArray name = QByteArray(".keeper-") + expected.session.toLatin1();
    const QJsonObject object{{QStringLiteral("v"), 1}, {QStringLiteral("session"), expected.session},
        {QStringLiteral("launch"), expected.launch}, {QStringLiteral("boot"), expected.boot},
        {QStringLiteral("uid"), qint64(expected.uid)}, {QStringLiteral("pid"), qint64(keeper.pid)},
        {QStringLiteral("start"), QString::number(keeper.startTicks)}, {QStringLiteral("pidInode"), QString::number(keeper.pidInode)}};
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const int fd = openat(m_directory, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return fail(error, QStringLiteral("Keeper identity already recorded or unavailable"));
    ssize_t count;
    do { count = write(fd, data.constData(), data.size()); } while (count < 0 && errno == EINTR);
    const bool prepared = count == data.size() && !fchmod(fd, 0600) && safeFile(fd, m_owner) && !fsync(fd);
    close(fd);
    const bool durable = !fsync(m_directory);
    if (!prepared || !durable) return fail(error, QStringLiteral("Keeper identity uncertain; PAM must not open"));
    if (error) error->clear();
    return true;
}
std::optional<VirtualSessionJournal::Keeper> VirtualSessionJournal::readKeeperRecord(const Record &expected, bool *missing, QString *error) const {
    if (missing) *missing = false;
    const auto refuse = [error]() -> std::optional<Keeper> {
        fail(error, QStringLiteral("Unsafe or mismatched keeper identity")); return {};
    };
    const auto current = readRecord(expected.session, error);
    if (!current || *current != expected || !hasClaim(expected)) return refuse();
    const QByteArray name = QByteArray(".keeper-") + expected.session.toLatin1();
    const int fd = openat(m_directory, name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) return refuse();
        if (missing) *missing = true;
        if (error) error->clear();
        return {};
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { close(fd); return refuse(); }
    if (!safeFile(fd, m_owner)) return refuse();
    const auto bytes = file.read(4097);
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    const auto object = document.object();
    const auto pid = object.value(QStringLiteral("pid")).toDouble(-1);
    const auto start = object.value(QStringLiteral("start")).toString();
    bool ok = false; const auto ticks = start.toULongLong(&ok);
    const auto inode = object.value(QStringLiteral("pidInode")).toString();
    bool inodeOk = false; const auto identity = inode.toULongLong(&inodeOk);
    if (file.error() != QFileDevice::NoError || bytes.size() > 4096 || parse.error != QJsonParseError::NoError
        || !document.isObject() || object.size() != 8 || object.value(QStringLiteral("v")) != QJsonValue(1)
        || object.value(QStringLiteral("session")) != QJsonValue(expected.session)
        || object.value(QStringLiteral("launch")) != QJsonValue(expected.launch)
        || object.value(QStringLiteral("boot")) != QJsonValue(expected.boot)
        || object.value(QStringLiteral("uid")) != QJsonValue(qint64(expected.uid))
        || pid <= 1 || pid > INT_MAX || std::floor(pid) != pid
        || !ok || !ticks || QString::number(ticks) != start
        || !inodeOk || identity < 2 || QString::number(identity) != inode) return refuse();
    if (error) error->clear();
    return Keeper{pid_t(pid), ticks, identity};
}
bool VirtualSessionJournal::hasClaim(const Record &expected) const {
    if (!expected.valid()) return false;
    const QByteArray name = QByteArray(".claimed-") + expected.session.toLatin1();
    const int fd = openat(m_directory, name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { close(fd); return false; }
    if (!safeFile(fd, m_owner)) return false;
    const auto bytes = file.read(4097);
    return file.error() == QFileDevice::NoError && bytes == expected.launch.toLatin1() + '\n' + expected.incarnation.toLatin1();
}
bool VirtualSessionJournal::claimRecord(const Record &expected, QString *error) {
    const auto current = readRecord(expected.session, error);
    if (!current || *current != expected) return fail(error, QStringLiteral("Launch intent changed or is unavailable"));
    const QByteArray name = QByteArray(".claimed-") + expected.session.toLatin1();
    const int fd = openat(m_directory, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return fail(error, QStringLiteral("Launch already consumed or claim unavailable"));
    // Even an empty/partial marker blocks replay. Never unlink it on failure.
    const QByteArray marker = expected.launch.toLatin1() + '\n' + expected.incarnation.toLatin1();
    ssize_t count;
    do { count = write(fd, marker.constData(), marker.size()); } while (count < 0 && errno == EINTR);
    const bool prepared = count == marker.size() && !fchmod(fd, 0600) && safeFile(fd, m_owner) && !fsync(fd);
    close(fd);
    const bool durable = !fsync(m_directory);
    if (!prepared || !durable) return fail(error, QStringLiteral("Launch claim uncertain; explicit reconciliation required"));
    if (error) error->clear();
    return true;
}
std::unique_ptr<VirtualSessionJournal> VirtualSessionJournal::openAt(const QString &path, quint32 owner, QString *error, bool writable) {
    // Production ancestors are checked too; tests use an owned private temp root.
    if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path || path.contains(QChar::Null)) return {};
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const auto parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype i = 0; fd >= 0 && i < parts.size(); ++i) {
        const int next = openat(fd, QFile::encodeName(parts[i]).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        close(fd); fd = next;
        struct stat s{};
        const bool leaf = i == parts.size() - 1;
        if (fd >= 0 && (fstat(fd, &s) || (leaf && (s.st_uid != owner || (s.st_mode & 07777) != 0700))
            || (!leaf && owner == 0 && (s.st_uid != 0 || (s.st_mode & 0022))))) { close(fd); fd = -1; }
    }
    if (fd < 0 || parts.isEmpty()) {
        if (fd >= 0) close(fd);
        fail(error, QStringLiteral("Unsafe or unavailable journal directory")); return {};
    }
    if (writable && flock(fd, LOCK_EX | LOCK_NB)) {
        close(fd); fail(error, QStringLiteral("Launch journal already in use")); return {};
    }
    if (error) error->clear();
    return std::unique_ptr<VirtualSessionJournal>(new VirtualSessionJournal(fd, owner, writable));
}
bool VirtualSessionJournal::insert(const Record &r, QString *error) {
    if (!m_writable) return fail(error, QStringLiteral("Read-only launch intent lookup"));
    if (!r.valid()) return fail(error, QStringLiteral("Invalid launch record"));
    const auto existing = records(error);
    if (!existing) return false;
    if (existing->size() >= 256) return fail(error, QStringLiteral("Launch journal capacity reached"));
    const QJsonObject object{{QStringLiteral("v"), 1}, {QStringLiteral("uid"), qint64(r.uid)},
        {QStringLiteral("session"), r.session}, {QStringLiteral("launch"), r.launch},
        {QStringLiteral("instance"), r.incarnation}, {QStringLiteral("boot"), r.boot},
        {QStringLiteral("token"), QString::fromLatin1(r.token.toHex())}};
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const QByteArray temporary = QByteArray(".pending-") + QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    const QByteArray name = r.session.toLatin1() + ".json";
    const int fd = openat(m_directory, temporary.constData(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return fail(error, QStringLiteral("Cannot prepare launch record"));
    qsizetype offset = 0;
    bool ok = true;
    while (offset < data.size()) {
        const auto n = write(fd, data.constData() + offset, data.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        offset += n;
    }
    // An inherited restrictive umask must not publish an unreadable record.
    ok = ok && !fchmod(fd, 0600) && safeFile(fd, m_owner) && !fsync(fd);
    close(fd);
    // linkat is atomic and never replaces an existing intent (including symlinks).
    if (ok) ok = !linkat(m_directory, temporary.constData(), m_directory, name.constData(), 0);
    const bool cleaned = !unlinkat(m_directory, temporary.constData(), 0);
    const bool durable = !fsync(m_directory);
    if (!ok || !cleaned || !durable) return fail(error, QStringLiteral("Launch intent not durably committed; do not spawn or replace"));
    if (error) error->clear();
    return true;
}
std::optional<VirtualSessionJournal::Record> VirtualSessionJournal::readRecord(const QString &session, QString *error) const {
    const auto refuse = [error]() -> std::optional<Record> {
        fail(error, QStringLiteral("Unsafe, malformed or unreadable launch intent")); return {};
    };
    if (!uuid(session)) return refuse();
    const QByteArray name = session.toLatin1() + ".json";
    const int fd = openat(m_directory, name.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return refuse();
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { close(fd); return refuse(); }
    if (!safeFile(fd, m_owner)) return refuse();
    const auto data = file.read(4097);
    QJsonParseError parse{};
    const auto doc = QJsonDocument::fromJson(data, &parse);
    const auto o = doc.object();
    const double uid = o.value(QStringLiteral("uid")).toDouble(-1);
    if (file.error() != QFileDevice::NoError || data.size() > 4096 || parse.error != QJsonParseError::NoError
        || !doc.isObject() || o.size() != 7 || o.value(QStringLiteral("v")) != QJsonValue(1)
        || uid < 1 || uid >= std::numeric_limits<quint32>::max() || std::floor(uid) != uid) return refuse();
    const auto encoded = o.value(QStringLiteral("token")).toString().toLatin1();
    Record record{quint32(uid), o.value(QStringLiteral("session")).toString(), o.value(QStringLiteral("launch")).toString(),
        o.value(QStringLiteral("instance")).toString(), o.value(QStringLiteral("boot")).toString(), QByteArray::fromHex(encoded)};
    if (!record.valid() || record.token.toHex() != encoded || record.session != session) return refuse();
    if (error) error->clear();
    return record;
}
std::optional<QVector<VirtualSessionJournal::Record>> VirtualSessionJournal::records(QString *error) const {
    const auto refuse = [error]() -> std::optional<QVector<Record>> {
        fail(error, QStringLiteral("Unsafe, malformed or unreadable launch journal")); return {};
    };
    // A new open description avoids sharing readdir offsets between calls.
    const int scan = openat(m_directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan < 0) return refuse();
    DIR *directory = fdopendir(scan);
    if (!directory) { close(scan); return refuse(); }
    QVector<Record> result;
    bool ok = true;
    while (true) {
        errno = 0;
        const auto *entry = readdir(directory);
        if (!entry) { if (errno) ok = false; break; }
        const QByteArray name(entry->d_name);
        if (name == "." || name == ".." || name.startsWith(".pending-")) continue;
        // Consumption markers are not recovery identities. Their presence,
        // including interrupted creation, forbids a second launch but must not
        // hide unrelated surviving desktops from recovery.
        if (name.startsWith(".claimed-") && uuid(QString::fromLatin1(name.mid(9)))) continue;
        if (name.startsWith(".keeper-") && uuid(QString::fromLatin1(name.mid(8)))) continue;
        if (!name.endsWith(".json") || !uuid(QString::fromLatin1(name.chopped(5))) || result.size() >= 256) { ok = false; break; }
        const auto record = readRecord(QString::fromLatin1(name.chopped(5)), error);
        if (!record) { ok = false; break; }
        result.append(*record);
    }
    closedir(directory);
    if (!ok) return refuse();
    if (error) error->clear();
    return result;
}
}
