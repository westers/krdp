// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionRuntimeProfile.h"
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vector>
#include <utility>

namespace KRdp {
namespace {
constexpr qint64 ManifestLimit = 1024 * 1024, FileLimit = 1024LL * 1024 * 1024, TotalLimit = 4 * FileLimit;
constexpr int ObjectLimit = 4096, EntriesLimit = 32768, MapLimit = 4 * 1024 * 1024;
struct Fd {
    int fd = -1;
    explicit Fd(int value = -1) : fd(value) {}
    ~Fd() { if (fd >= 0) ::close(fd); }
    Fd(Fd &&o) noexcept : fd(std::exchange(o.fd, -1)) {}
    Fd &operator=(Fd &&o) noexcept { if (fd >= 0) ::close(fd); fd = std::exchange(o.fd, -1); return *this; }
    Fd(const Fd &) = delete;
};
bool refuse(QString *error, const QString &message) { if (error) *error = message; return false; }
bool component(const QString &s) {
    if (s.isEmpty() || s.size()>255 || s==QStringLiteral(".") || s==QStringLiteral("..")) return false;
    for (const auto c : s) {
        const auto n=c.unicode();
        if (!((n>='a' && n<='z') || (n>='A' && n<='Z') || (n>='0' && n<='9')
            || n=='_' || n=='+' || n=='@' || n=='.' || n==',' || n==':' || n=='-')) return false;
    }
    return true;
}
bool digits(const QString &s, bool hex) {
    if (s.isEmpty()) return false;
    for (const auto c : s) if (!((c>=QLatin1Char('0') && c<=QLatin1Char('9'))
        || (hex && c>=QLatin1Char('a') && c<=QLatin1Char('f')))) return false;
    return true;
}
bool path(const QString &s) {
    if (s == QStringLiteral("/")) return true;
    if (s.size() > 4096 || !s.startsWith(QLatin1Char('/'))) return false;
    const auto parts = s.mid(1).split(QLatin1Char('/'));
    return std::all_of(parts.begin(), parts.end(), component);
}
bool target(const QString &s) {
    if (s.isEmpty() || s.size() > 4096) return false;
    const auto parts = (s.startsWith(QLatin1Char('/')) ? s.mid(1) : s).split(QLatin1Char('/'));
    bool leading = !s.startsWith(QLatin1Char('/'));
    for (const auto &part : parts) {
        if (leading && part == QStringLiteral("..")) continue;
        leading = false;
        if (!component(part)) return false;
    }
    return true;
}
bool keys(const QJsonObject &o, std::initializer_list<const char *> fields) {
    QStringList names;
    for (auto field : fields) names.append(QString::fromLatin1(field));
    names.sort(); return o.keys() == names;
}
bool number(const QJsonValue &v, qint64 maximum) {
    return v.isDouble() && v.toDouble() >= 0 && v.toDouble() <= double(maximum) && std::floor(v.toDouble()) == v.toDouble();
}
QByteArray canonical(const QJsonValue &v) {
    if (v.isString()) {
        QByteArray s = v.toString().toUtf8(); s.replace("\\", "\\\\"); s.replace("\"", "\\\""); return '"' + s + '"';
    }
    if (v.isDouble()) return QByteArray::number(v.toInteger());
    QByteArray bytes;
    if (v.isArray()) {
        bytes = "["; for (const auto &item : v.toArray()) { if (bytes.size() > 1) bytes += ','; bytes += canonical(item); } return bytes + ']';
    }
    bytes = "{"; const auto o = v.toObject();
    for (const auto &key : o.keys()) { if (bytes.size() > 1) bytes += ','; bytes += canonical(key) + ':' + canonical(o[key]); }
    return bytes + '}';
}
bool identity(const struct stat &a, const struct stat &b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }
bool unchanged(const struct stat &a, const struct stat &b) {
    return identity(a,b) && a.st_mode == b.st_mode && a.st_uid == b.st_uid && a.st_gid == b.st_gid && a.st_nlink == b.st_nlink
        && a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
bool trustedDirectory(int fd, unsigned owner) {
    struct stat st{}; return fd >= 0 && !fstat(fd, &st) && S_ISDIR(st.st_mode) && st.st_uid == owner && !(st.st_mode & 0022);
}
bool readBounded(int fd, qint64 limit, QByteArray &data, bool regular = true) {
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) || (regular && (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > limit))) return false;
    data.clear(); char bytes[16384];
    while (data.size() <= limit) {
        const auto n = ::read(fd, bytes, std::min<qint64>(sizeof(bytes), limit + 1 - data.size()));
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (!n) return data.size() <= limit && (!regular || data.size() == st.st_size);
        data.append(bytes, n);
    }
    return false;
}
}
struct VirtualSessionRuntimeProfile::Data {
    struct Object { QString path; unsigned uid, gid, mode; qint64 size = 0; QByteArray hash; QMap<QString, QString> entries; };
    struct Link { QString target; unsigned uid, gid; };
    struct Process { QString executable; QSet<QString> required, allowed, data; };
    Fd root, manifest;
    struct stat rootStat{}, manifestStat{};
    QString rootPath, manifestPath, digest;
    unsigned owner = 0;
    pid_t pid = getpid();
    QByteArray bytes;
    std::vector<Object> files, directories;
    QMap<QString, Link> links;
    QMap<QString, Process> processes;
    QStringList absent;

    // Resolves only manifest-declared links. Each restart is fd-relative to the
    // pinned root, and all directories traversed remain trusted. Never realpath.
    Fd open(const QString &name, bool directory, bool followFinal = true) const {
        QString current = name;
        for (int hops = 0; hops <= 40; ++hops) {
            Fd dir(openat(root.fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (!trustedDirectory(dir.fd,owner)) return Fd();
            if (current == QStringLiteral("/")) return directory ? std::move(dir) : Fd();
            const auto parts = current.mid(1).split(QLatin1Char('/')); QString prefix;
            bool restart = false;
            for (int i = 0; i < parts.size(); ++i) {
                const QString parent = prefix; prefix += QLatin1Char('/') + parts[i];
                const QByteArray part = parts[i].toLatin1(); struct stat st{};
                if (fstatat(dir.fd, part.constData(), &st, AT_SYMLINK_NOFOLLOW)) return Fd();
                const bool last = i == parts.size() - 1;
                if (S_ISLNK(st.st_mode) && (!last || followFinal)) {
                    const auto link = links.constFind(prefix);
                    if (link == links.cend() || st.st_uid != link->uid || st.st_gid != link->gid) return Fd();
                    char buffer[4097]; auto n = readlinkat(dir.fd, part.constData(), buffer, sizeof(buffer));
                    if (n <= 0 || n > 4096 || QByteArray(buffer,n) != link->target.toLatin1()) return Fd();
                    QString destination = link->target.startsWith(QLatin1Char('/')) ? link->target : parent + QLatin1Char('/') + link->target;
                    int depth = 0;
                    for (const auto &part : destination.mid(1).split(QLatin1Char('/'))) {
                        if (part == QStringLiteral("..")) { if (--depth < 0) return Fd(); }
                        else if (!part.isEmpty()) ++depth;
                    }
                    // Lexical normalization is applied to the declared target
                    // only; no undeclared filesystem link may be traversed.
                    destination = QDir::cleanPath(destination);
                    if (!path(destination)) return Fd();
                    for (int j = i + 1; j < parts.size(); ++j) destination += QLatin1Char('/') + parts[j];
                    current = destination; restart = true; break;
                }
                const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | ((!last || directory) ? O_DIRECTORY : 0);
                Fd next(openat(dir.fd, part.constData(), flags));
                if (next.fd < 0 || ((!last || directory) && !trustedDirectory(next.fd,owner))) return Fd();
                if (last) return next;
                dir = std::move(next);
            }
            if (!restart) return Fd();
        }
        return Fd();
    }
    bool associated() const {
        if (pid != getpid()) return false;
        Fd now(::open(QFile::encodeName(rootPath).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        struct stat st{};
        if (!trustedDirectory(now.fd,owner) || fstat(now.fd, &st) || !identity(st, rootStat)) return false;
        auto mf = open(manifestPath, false); QByteArray actual;
        return mf.fd >= 0 && !fstat(mf.fd, &st) && unchanged(st, manifestStat) && readBounded(mf.fd, ManifestLimit, actual) && actual == bytes;
    }
};
VirtualSessionRuntimeProfile::VirtualSessionRuntimeProfile(std::unique_ptr<Data> data) : d(std::move(data)) {}
VirtualSessionRuntimeProfile::~VirtualSessionRuntimeProfile() = default;
QString VirtualSessionRuntimeProfile::digest() const { return d->digest; }
QString VirtualSessionRuntimeProfile::approvedExecutable(const QString &fixedRole) const {
    const auto role=d->processes.constFind(fixedRole);
    return role==d->processes.cend() ? QString() : role->executable;
}
std::optional<QByteArray> VirtualSessionRuntimeProfile::approvedWriterPolicy(QString *error) const {
    const auto bad = [&]() -> std::optional<QByteArray> {
        refuse(error, QStringLiteral("Approved writer policy missing, changed or unsafe"));
        return {};
    };
    if (error) error->clear();
    if (!d->associated()) return bad();
    const QString name = QStringLiteral("/etc/krdp/virtual-writer-policy.json");
    const auto expected = std::find_if(d->files.begin(), d->files.end(), [&](const auto &f) { return f.path == name; });
    if (expected == d->files.end() || expected->size <= 0 || expected->size > ManifestLimit) return bad();
    auto file = d->open(name, false);
    struct stat before{}, after{}, named{};
    if (file.fd < 0 || fstat(file.fd, &before) || !S_ISREG(before.st_mode) || before.st_nlink != 1
        || before.st_uid != expected->uid || before.st_gid != expected->gid
        || (before.st_mode & 07777) != expected->mode || before.st_size != expected->size) return bad();
    QByteArray bytes;
    if (!readBounded(file.fd, ManifestLimit, bytes)
        || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != expected->hash
        || fstat(file.fd, &after) || !unchanged(before, after)) return bad();
    auto current = d->open(name, false);
    if (current.fd < 0 || fstat(current.fd, &named) || !unchanged(before, named) || !d->associated()) return bad();
    return bytes;
}

std::unique_ptr<VirtualSessionRuntimeProfile> VirtualSessionRuntimeProfile::loadApproved(QString *error) {
    if (getuid() || geteuid()) { refuse(error, QStringLiteral("Root runtime-profile caller required")); return {}; }
    return loadAt(QStringLiteral("/"), QStringLiteral("/etc/krdp/virtual-runtime-profile.json"), 0, error);
}
std::unique_ptr<VirtualSessionRuntimeProfile> VirtualSessionRuntimeProfile::loadAt(const QString &root, const QString &manifest, unsigned owner, QString *error) {
    const auto bad = [&]() -> std::unique_ptr<VirtualSessionRuntimeProfile> { refuse(error, QStringLiteral("Runtime profile missing, unsafe, noncanonical or malformed")); return {}; };
    if (!path(manifest) || root.isEmpty() || QDir::cleanPath(root) != root || !root.startsWith(QLatin1Char('/')) || root.contains(QChar::Null)) return bad();
    auto data = std::make_unique<Data>(); data->owner = owner; data->rootPath = root; data->manifestPath = manifest;
    data->root = Fd(::open(QFile::encodeName(root).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!trustedDirectory(data->root.fd,owner) || fstat(data->root.fd, &data->rootStat)) return bad();
    data->manifest = data->open(manifest, false);
    auto &ms = data->manifestStat;
    if (data->manifest.fd < 0 || fstat(data->manifest.fd, &ms) || !S_ISREG(ms.st_mode) || ms.st_uid != owner || ms.st_gid != owner
        || ms.st_nlink != 1 || (ms.st_mode & 07133) || !readBounded(data->manifest.fd, ManifestLimit, data->bytes)) return bad();
    QJsonParseError parse{}; const auto doc = QJsonDocument::fromJson(data->bytes, &parse); const auto o = doc.object();
    if (parse.error != QJsonParseError::NoError || !doc.isObject() || !keys(o, {"v","files","directories","symlinks","absent","processes"})
        || !number(o[QStringLiteral("v")],1) || o[QStringLiteral("v")].toInteger() != 1) return bad();
    for (const auto &name : {"files","directories","symlinks","absent","processes"}) if (!o[QString::fromLatin1(name)].isArray()) return bad();
    QSet<QString> seen; int objects = 0, entries = 0; qint64 total = 0;
    for (const auto &kind : {"files","directories","symlinks","absent"}) {
        QString previous;
        for (const auto &value : o[QString::fromLatin1(kind)].toArray()) {
            if (++objects > ObjectLimit) return bad();
            const auto r = value.toObject(); const QString p = QByteArray(kind) == "absent" ? value.toString() : r[QStringLiteral("path")].toString();
            if (!path(p) || (!previous.isEmpty() && p <= previous) || seen.contains(p)) return bad();
            previous = p; seen.insert(p);
            if (QByteArray(kind) == "absent") { if (!value.isString() || p == QStringLiteral("/")) return bad(); data->absent.append(p); continue; }
            if (!value.isObject() || !number(r[QStringLiteral("uid")], UINT32_MAX - 1) || !number(r[QStringLiteral("gid")], UINT32_MAX - 1)) return bad();
            const unsigned uid = r[QStringLiteral("uid")].toInteger(), gid = r[QStringLiteral("gid")].toInteger();
            // Approved loading objects must not be user-controlled.
            if (uid != owner || gid != owner) return bad();
            if (QByteArray(kind) == "symlinks") {
                if (!keys(r, {"path","target","uid","gid"}) || p == QStringLiteral("/") || !target(r[QStringLiteral("target")].toString())) return bad();
                data->links.insert(p, {r[QStringLiteral("target")].toString(),uid,gid}); continue;
            }
            if (!number(r[QStringLiteral("mode")],07777) || (r[QStringLiteral("mode")].toInteger() & 0022)) return bad();
            Data::Object object{p,uid,gid,unsigned(r[QStringLiteral("mode")].toInteger()),0,{}, {}};
            if (QByteArray(kind) == "files") {
                if (!keys(r, {"path","sha256","size","mode","uid","gid"}) || p == QStringLiteral("/") || !number(r[QStringLiteral("size")],FileLimit)) return bad();
                object.size = r[QStringLiteral("size")].toInteger(); total += object.size;
                const auto hash = r[QStringLiteral("sha256")].toString();
                if (total > TotalLimit || hash.size()!=64 || !digits(hash,true)) return bad();
                object.hash = hash.toLatin1(); data->files.push_back(std::move(object));
            } else {
                if (!keys(r, {"path","entries","mode","uid","gid"}) || !r[QStringLiteral("entries")].isArray()) return bad();
                QString prev;
                for (const auto &e : r[QStringLiteral("entries")].toArray()) {
                    const auto entry = e.toObject(); const auto name = entry[QStringLiteral("name")].toString(), type = entry[QStringLiteral("type")].toString();
                    if (++entries > EntriesLimit || !e.isObject() || !keys(entry,{"name","type"}) || !component(name)
                        || (!prev.isEmpty() && name <= prev) || (type != QStringLiteral("file") && type != QStringLiteral("directory") && type != QStringLiteral("symlink"))) return bad();
                    prev = name; object.entries.insert(name,type);
                }
                data->directories.push_back(std::move(object));
            }
        }
    }
    if (data->files.empty()) return bad();
    // Reject contradictory exact-directory declarations at parse time.
    QMap<QString,QString> declared;
    for (const auto &f : data->files) declared.insert(f.path,QStringLiteral("file"));
    for (const auto &dir : data->directories) declared.insert(dir.path,QStringLiteral("directory"));
    for (auto it=data->links.cbegin(); it!=data->links.cend(); ++it) declared.insert(it.key(),QStringLiteral("symlink"));
    for (const auto &dir : data->directories) {
        const QString base = dir.path == QStringLiteral("/") ? QString() : dir.path;
        for (auto it=declared.cbegin(); it!=declared.cend(); ++it) {
            if (!it.key().startsWith(base+QLatin1Char('/')) || it.key()==dir.path) continue;
            const QString child=it.key().mid(base.size()+1);
            if (!child.contains(QLatin1Char('/')) && dir.entries.value(child)!=it.value()) return bad();
        }
        for (const auto &absent : data->absent) {
            if (!absent.startsWith(base+QLatin1Char('/'))) continue;
            const QString child=absent.mid(base.size()+1);
            if (!child.contains(QLatin1Char('/')) && dir.entries.contains(child)) return bad();
        }
    }
    for (const auto &f : data->files) for (const auto &p : seen) if (p.startsWith(f.path+QLatin1Char('/'))) return bad();
    QSet<QString> filePaths;
    for (const auto &file : data->files) filePaths.insert(file.path);
    QString previousRole;
    for (const auto &value : o[QStringLiteral("processes")].toArray()) {
        const auto r = value.toObject(); const auto name = r[QStringLiteral("name")].toString();
        if (++objects > ObjectLimit || !value.isObject() || !keys(r,{"name","executable","required","allowed","data"})
            || !component(name) || (!previousRole.isEmpty() && name <= previousRole) || !filePaths.contains(r[QStringLiteral("executable")].toString())) return bad();
        previousRole = name; Data::Process process; process.executable = r[QStringLiteral("executable")].toString();
        for (const auto &field : {"required","allowed","data"}) {
            if (!r[QString::fromLatin1(field)].isArray()) return bad();
            auto &set = QByteArray(field) == "required" ? process.required : QByteArray(field) == "allowed" ? process.allowed : process.data;
            QString prev;
            for (const auto &entry : r[QString::fromLatin1(field)].toArray()) {
                const auto p = entry.toString();
                if (++entries > EntriesLimit || !entry.isString() || !filePaths.contains(p) || (!prev.isEmpty() && p <= prev)) return bad();
                prev = p; set.insert(p);
            }
        }
        if (!process.required.contains(process.executable) || !process.allowed.contains(process.required)
            || process.allowed.intersects(process.data)) return bad();
        data->processes.insert(name,std::move(process));
    }
    for (const auto &absent : data->absent) for (const auto &p : seen) if (p.startsWith(absent + QLatin1Char('/'))) return bad();
    if (canonical(o) + '\n' != data->bytes) return bad();
    // Domain separation, including its newline, is part of the V1 digest.
    const QByteArray domainBytes = QByteArray("KRDP-RUNTIME-PROFILE-V1\n") + data->bytes;
    data->digest = QString::fromLatin1(QCryptographicHash::hash(domainBytes, QCryptographicHash::Sha256).toHex());
    struct stat after{}; if (fstat(data->manifest.fd,&after) || !unchanged(ms,after)) return bad();
    return std::unique_ptr<VirtualSessionRuntimeProfile>(new VirtualSessionRuntimeProfile(std::move(data)));
}

bool VirtualSessionRuntimeProfile::validateFilesystem(QString *error) const { return validate({}, nullptr, {}, error); }
bool VirtualSessionRuntimeProfile::validateCurrentProcess(const QString &role, QString *error) const {
    if (role.isEmpty()) return refuse(error,QStringLiteral("Explicit runtime process role required"));
    return validate(role, nullptr, {}, error);
}
bool VirtualSessionRuntimeProfile::validate(const QString &role, const QByteArray *fixtureMaps, const QString &fixtureExe, QString *error) const {
    const auto bad = [&](const QString &p = {}) { return refuse(error, QStringLiteral("Runtime profile mismatch or uncertainty: ") + p); };
    QDeadlineTimer deadline(10000);
    if (!d->associated()) return bad(QStringLiteral("manifest/root identity"));
    struct Pinned { const Data::Object *object; Fd fd; struct stat st; };
    std::vector<Pinned> pins;
    std::vector<Pinned> directoryPins;
    QMap<QString,struct stat> linkStats;
    bool firstTopology = true;
    const auto metadata = [](const Data::Object &o, const struct stat &st) { return st.st_uid == o.uid && st.st_gid == o.gid && (st.st_mode & 07777) == o.mode; };
    for (const auto &o : d->files) {
        if (deadline.hasExpired()) return bad(QStringLiteral("deadline"));
        auto fd = d->open(o.path,false); struct stat st{};
        if (fd.fd < 0 || fstat(fd.fd,&st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || !metadata(o,st) || st.st_size != o.size) return bad(o.path);
        QCryptographicHash hash(QCryptographicHash::Sha256); char buffer[65536]; qint64 count = 0;
        while (true) {
            if (deadline.hasExpired()) return bad(QStringLiteral("deadline"));
            auto n = ::read(fd.fd,buffer,sizeof(buffer));
            if (n < 0) { if (errno == EINTR) continue; return bad(o.path); }
            if (!n) break;
            count += n; if (count > o.size) return bad(o.path); hash.addData(QByteArrayView(buffer,n));
        }
        struct stat after{};
        if (count != o.size || hash.result().toHex() != o.hash || fstat(fd.fd,&after) || !unchanged(st,after)) return bad(o.path);
        pins.push_back({&o,std::move(fd),st});
    }
    const auto topology = [&]() {
        for (auto it = d->links.cbegin(); it != d->links.cend(); ++it) {
            if (deadline.hasExpired()) return false;
            const QString parent = it.key().left(it.key().lastIndexOf(QLatin1Char('/'))); const auto leaf = it.key().mid(it.key().lastIndexOf(QLatin1Char('/'))+1).toLatin1();
            auto dir = d->open(parent.isEmpty() ? QStringLiteral("/") : parent,true); struct stat st{}; char bytes[4097];
            if (dir.fd < 0 || fstatat(dir.fd,leaf.constData(),&st,AT_SYMLINK_NOFOLLOW) || !S_ISLNK(st.st_mode) || st.st_uid != it->uid || st.st_gid != it->gid) return false;
            if (firstTopology) linkStats.insert(it.key(),st);
            else if (!unchanged(st,linkStats.value(it.key()))) return false;
            auto n = readlinkat(dir.fd,leaf.constData(),bytes,sizeof(bytes));
            if (n <= 0 || n > 4096 || QByteArray(bytes,n) != it->target.toLatin1()) return false;
            // Also require a resolvable, approved chain; dangling/looped links refuse.
            auto resolved = d->open(it.key(),true);
            if (resolved.fd < 0) resolved = d->open(it.key(),false);
            if (resolved.fd < 0) return false;
            struct stat resolvedStat{};
            if (fstat(resolved.fd,&resolvedStat)) return false;
            if (!S_ISDIR(resolvedStat.st_mode) && !std::any_of(pins.begin(),pins.end(),[&](const auto &pin){return identity(pin.st,resolvedStat);})) return false;
        }
        for (const auto &o : d->directories) {
            if (deadline.hasExpired()) return false;
            auto fd = d->open(o.path,true); struct stat st{};
            if (fd.fd < 0 || fstat(fd.fd,&st) || !metadata(o,st)) return false;
            if (firstTopology) {
                Fd pinned(openat(fd.fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
                if (pinned.fd<0) return false;
                directoryPins.push_back({&o,std::move(pinned),st});
            } else {
                const auto pin=std::find_if(directoryPins.begin(),directoryPins.end(),[&](const auto &p){return p.object==&o;});
                struct stat pinned{};
                if (pin==directoryPins.end() || !unchanged(st,pin->st) || fstat(pin->fd.fd,&pinned) || !unchanged(pinned,pin->st)) return false;
            }
            DIR *stream = fdopendir(fd.fd); if (!stream) return false;
            fd.fd=-1;
            QMap<QString,QString> entries; bool ok = true; errno = 0;
            while (auto *entry = readdir(stream)) {
                const QByteArray name(entry->d_name);
                if (name == "." || name == "..") { errno = 0; continue; }
                struct stat item{}; const QString text = QString::fromLatin1(name);
                if (deadline.hasExpired() || entries.size() >= EntriesLimit || !component(text) || fstatat(dirfd(stream),entry->d_name,&item,AT_SYMLINK_NOFOLLOW)) { ok = false; break; }
                const QString kind = S_ISREG(item.st_mode) ? QStringLiteral("file") : S_ISDIR(item.st_mode) ? QStringLiteral("directory") : S_ISLNK(item.st_mode) ? QStringLiteral("symlink") : QString();
                entries.insert(text,kind); errno = 0;
            }
            ok = ok && !errno && entries == o.entries; closedir(stream); if (!ok) return false;
        }
        for (const auto &p : d->absent) {
            if (deadline.hasExpired()) return false;
            const QString parent = p.left(p.lastIndexOf(QLatin1Char('/'))); const auto leaf = p.mid(p.lastIndexOf(QLatin1Char('/'))+1).toLatin1();
            auto fd = d->open(parent.isEmpty() ? QStringLiteral("/") : parent,true); struct stat st{};
            if (fd.fd < 0 || !fstatat(fd.fd,leaf.constData(),&st,AT_SYMLINK_NOFOLLOW) || errno != ENOENT) return false;
        }
        firstTopology=false;
        return true;
    };
    if (!topology()) return bad(QStringLiteral("directory/link/absence topology"));
    if (!role.isEmpty() || fixtureMaps) {
        const auto roleIt = d->processes.constFind(role);
        if (roleIt == d->processes.cend()) return bad(QStringLiteral("unknown process role"));
        const auto &process = *roleIt;
        QSet<QString> mapped;
        QByteArray maps;
        if (fixtureMaps) maps = *fixtureMaps;
        else { Fd fd(::open("/proc/self/maps",O_RDONLY|O_CLOEXEC)); if (!readBounded(fd.fd,MapLimit,maps,false)) return bad(QStringLiteral("maps read")); }
        if (maps.isEmpty() || maps.size() > MapLimit || !maps.endsWith('\n')) return bad(QStringLiteral("maps bounds"));
        const auto lines = maps.split('\n'); if (lines.size() > EntriesLimit) return bad(QStringLiteral("maps count"));
        // No QRegularExpression here or in manifest validation: Qt's PCRE JIT
        // creates anonymous executable mappings which this policy must refuse.
        quint64 end = 0; bool fileSeen = false;
        for (int i = 0; i < lines.size()-1; ++i) {
            if (deadline.hasExpired()) return bad(QStringLiteral("deadline"));
            const auto text=QString::fromLatin1(lines[i]);
            QString fields[5]; qsizetype pos=0;
            for (auto &field : fields) {
                const auto begin=pos;
                while (pos<text.size() && text[pos]!=QLatin1Char(' ')) ++pos;
                field=text.mid(begin,pos-begin);
                if (field.isEmpty()) return bad(QStringLiteral("maps syntax"));
                while (pos<text.size() && text[pos]==QLatin1Char(' ')) ++pos;
            }
            const QString name=text.mid(pos); // Pathname bytes are identity, not padding.
            const auto range=fields[0].split(QLatin1Char('-')), device=fields[3].split(QLatin1Char(':'));
            const auto &permissions=fields[1];
            if (range.size()!=2 || device.size()!=2 || !digits(range[0],true) || !digits(range[1],true)
                || !digits(fields[2],true) || !digits(device[0],true) || !digits(device[1],true) || !digits(fields[4],false)
                || permissions.size()!=4 || (permissions[0]!=QLatin1Char('r') && permissions[0]!=QLatin1Char('-'))
                || (permissions[1]!=QLatin1Char('w') && permissions[1]!=QLatin1Char('-'))
                || (permissions[2]!=QLatin1Char('x') && permissions[2]!=QLatin1Char('-'))
                || (permissions[3]!=QLatin1Char('p') && permissions[3]!=QLatin1Char('s'))) return bad(QStringLiteral("maps syntax"));
            bool ok[6]; const quint64 start=range[0].toULongLong(&ok[0],16), last=range[1].toULongLong(&ok[1],16);
            fields[2].toULongLong(&ok[2],16); const auto maj=device[0].toULongLong(&ok[3],16), min=device[1].toULongLong(&ok[4],16), ino=fields[4].toULongLong(&ok[5]);
            if (!std::all_of(std::begin(ok),std::end(ok),[](bool v){return v;}) || start < end || start >= last) return bad(QStringLiteral("maps range"));
            end = last;
            if (!ino) {
                if (maj || min || (!name.isEmpty() && name != QStringLiteral("[heap]") && name != QStringLiteral("[stack]") && name != QStringLiteral("[vvar]") && name != QStringLiteral("[vvar_vclock]") && name != QStringLiteral("[vdso]") && name != QStringLiteral("[vsyscall]"))
                    || (permissions[2]==QLatin1Char('x') && name != QStringLiteral("[vdso]") && name != QStringLiteral("[vsyscall]"))) return bad(QStringLiteral("unapproved anonymous mapping"));
                continue;
            }
            if (name.endsWith(QStringLiteral(" (deleted)"))) return bad(QStringLiteral("deleted mapping"));
            QString logical = name;
            if (d->rootPath != QStringLiteral("/")) {
                if (!name.startsWith(d->rootPath + QLatin1Char('/'))) return bad(QStringLiteral("mapping outside fixture root"));
                logical = name.mid(d->rootPath.size());
            }
            if (!path(logical)) return bad(QStringLiteral("mapping pathname"));
            auto actual = d->open(logical,false); struct stat st{};
            if (actual.fd < 0 || fstat(actual.fd,&st) || quint64(major(st.st_dev)) != maj || quint64(minor(st.st_dev)) != min || quint64(st.st_ino) != ino) return bad(QStringLiteral("mapped inode differs from pathname"));
            char magic[4]; const bool elf = pread(actual.fd,magic,sizeof(magic),0) == 4 && QByteArray(magic,4) == QByteArray("\177ELF",4);
            if (!elf && permissions[2]==QLatin1Char('x')) return bad(QStringLiteral("executable non-ELF mapping"));
            bool approved = false;
            for (const auto &pin : pins) if (identity(pin.st,st) && (elf ? process.allowed.contains(pin.object->path) : process.data.contains(pin.object->path))) {
                approved = true; mapped.insert(pin.object->path);
            }
            if (!approved) return bad(QStringLiteral("mapping not approved for process role"));
            fileSeen = true;
        }
        if (!fileSeen) return bad(QStringLiteral("no mapped implementation"));
        if (!mapped.contains(process.required)) return bad(QStringLiteral("required role mapping absent"));
        Fd exe = fixtureMaps ? d->open(fixtureExe,false) : Fd(::open("/proc/self/exe",O_RDONLY|O_CLOEXEC)); struct stat st{};
        if (exe.fd < 0 || fstat(exe.fd,&st) || !std::any_of(pins.begin(),pins.end(),[&](const auto &pin){return pin.object->path == process.executable && identity(pin.st,st);})) return bad(QStringLiteral("role executable identity"));
    }
    for (const auto &pin : pins) {
        struct stat st{}, named{}; auto fd = d->open(pin.object->path,false);
        if (deadline.hasExpired() || fstat(pin.fd.fd,&st) || !unchanged(pin.st,st) || fd.fd < 0 || fstat(fd.fd,&named) || !unchanged(pin.st,named)) return bad(pin.object->path);
    }
    if (!topology() || !d->associated() || deadline.hasExpired()) return bad(QStringLiteral("final revalidation"));
    return true;
}
}
