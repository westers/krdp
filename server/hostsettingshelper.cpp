// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Fixed-purpose privileged stdin protocol. No caller-supplied write path/argv.
#include "BrokerHostAdmin.h"
#include "BrokerHostRuntimeReader.h"
#include "ServerCertificate.h"
#include "VirtualGpuDevices.h"
#include "VirtualSessionLaunchPlan.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopeGuard>
#include <QUuid>
#include <openssl/crypto.h>
#include <memory>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace KRdp;
using Scope = BrokerHostSettings::Scope;
namespace {
struct Fd {
    int value = -1;
    explicit Fd(int descriptor = -1) : value(descriptor) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
bool safeDirectory(int fd)
{
    struct stat info{};
    return fd >= 0 && !::fstat(fd, &info) && S_ISDIR(info.st_mode) && !info.st_uid && !(info.st_mode & 0022);
}
int openDirectory(int parent, const char *name)
{
    const int fd = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!safeDirectory(fd)) { if (fd >= 0) ::close(fd); return -1; }
    return fd;
}
int policyDirectory()
{
    const Fd root(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!safeDirectory(root.value)) return -1;
    const Fd etc(openDirectory(root.value, "etc"));
    return openDirectory(etc.value, "farside");
}
bool directoryStillCurrent(int fd)
{
    const Fd current(policyDirectory());
    struct stat previous{}, fresh{};
    return safeDirectory(fd) && current.value >= 0 && !::fstat(fd, &previous) && !::fstat(current.value, &fresh)
        && previous.st_dev == fresh.st_dev && previous.st_ino == fresh.st_ino;
}
bool sameFile(const struct stat &a, const struct stat &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
struct Read { QByteArray bytes; bool exists = false; QString error; };
Read readRegular(int fd, qsizetype maximum, bool privateKey)
{
    Read result;
    result.exists = true;
    struct stat before{}, after{};
    if (fd < 0 || ::fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_uid || before.st_nlink != 1
        || (before.st_mode & (privateKey ? 0077 : 0022)) || before.st_size < 0 || before.st_size > maximum) {
        result.error = u"unsafe or oversized file"_s; return result;
    }
    char chunk[4096];
    while (result.bytes.size() <= maximum) {
        const auto count = ::read(fd, chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result.error = u"file cannot be read"_s; break; }
        if (!count) break;
        result.bytes.append(chunk, qsizetype(count));
    }
    OPENSSL_cleanse(chunk, sizeof(chunk));
    if (result.bytes.size() > maximum || ::fstat(fd, &after) || !sameFile(before, after)
        || result.bytes.size() != before.st_size) result.error = u"file changed or is oversized"_s;
    return result;
}
Read readSettings(int directory, Scope scope)
{
    const auto name = BrokerHostSettings::fileName(scope).toUtf8();
    struct stat entry{}, opened{}, after{};
    if (::fstatat(directory, name.constData(), &entry, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT ? Read{} : Read{{}, false, u"host settings cannot be inspected"_s};
    if (!S_ISREG(entry.st_mode)) return {{}, true, u"host settings must be a regular file"_s};
    const Fd fd(::openat(directory, name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    auto result = readRegular(fd.value, BrokerHostSettings::MaximumBytes, false);
    if (!result.error.isEmpty()) return result;
    if (::fstat(fd.value, &opened) || !sameFile(entry, opened)
        || ::fstatat(directory, name.constData(), &after, AT_SYMLINK_NOFOLLOW) || !sameFile(opened, after))
        result.error = u"host settings changed while reading"_s;
    return result;
}
bool trustedParents(const QString &path)
{
    // Check both the administrator's lexical chain and its resolved chain.
    // Root-managed symlinks are supported; user-writable ancestors are not.
    for (const auto &start : {QFileInfo(path).path(), QFileInfo(QFileInfo(path).canonicalFilePath()).path()}) {
        auto current = start;
        if (!QDir::isAbsolutePath(current)) return false;
        while (true) {
            struct stat info{};
            if (::lstat(QFile::encodeName(current).constData(), &info) || info.st_uid
                || (!S_ISLNK(info.st_mode) && (!S_ISDIR(info.st_mode) || (info.st_mode & 0022)))) return false;
            if (current == u"/") break;
            const auto parent = QFileInfo(current).path();
            if (parent == current) return false;
            current = parent;
        }
    }
    return true;
}
Read readTlsFile(const QString &path, bool key)
{
    struct stat entry{}, opened{}, current{};
    if (!VirtualSessionLaunchPlan::absoluteCleanPath(path)) return {{}, false, u"invalid TLS path"_s};
    if (::lstat(QFile::encodeName(path).constData(), &entry))
        return errno == ENOENT ? Read{} : Read{{}, false, u"TLS file cannot be inspected"_s};
    if (entry.st_uid || !trustedParents(path)) return {{}, true, u"unsafe TLS ownership or parent"_s};
    const auto canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty()) return {{}, true, u"TLS target is unavailable"_s};
    const Fd fd(::open(QFile::encodeName(canonical).constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    auto result = readRegular(fd.value, BrokerHostAdmin::MaximumPemBytes, key);
    if (!result.error.isEmpty()) return result;
    if (::fstat(fd.value, &opened) || ::stat(QFile::encodeName(path).constData(), &current) || !sameFile(opened, current))
        result.error = u"TLS target changed while reading"_s;
    return result;
}
struct Tls {
    ServerCertificate::Info info;
    QString state;
    QByteArray digest; // private, never exported: includes key bytes
    bool administratorManaged = false;
};
Tls inspectTls(const QVariantMap &values)
{
    const auto certificate = values[u"Certificate"_s].toString(), key = values[u"CertificateKey"_s].toString();
    auto cert = readTlsFile(certificate, false), privateKey = readTlsFile(key, true);
    const auto clear = qScopeGuard([&] { if (!privateKey.bytes.isEmpty()) OPENSSL_cleanse(privateKey.bytes.data(), size_t(privateKey.bytes.size())); });
    Tls result;
    result.administratorManaged = QFileInfo(certificate).isSymLink() || QFileInfo(key).isSymLink();
    if (!cert.error.isEmpty() || !privateKey.error.isEmpty()) { result.state = u"unsafe"_s; return result; }
    if (!cert.exists || !privateKey.exists) { result.state = u"missing"_s; return result; }
    result.info = ServerCertificate::inspectPem(cert.bytes, privateKey.bytes);
    const auto now = QDateTime::currentDateTimeUtc();
    if (!result.info.usable()) result.state = u"invalid"_s;
    else if (!result.info.notBefore.isValid() || result.info.notBefore > now) result.state = u"not-yet-valid"_s;
    else if (!result.info.notAfter.isValid() || result.info.notAfter <= now) result.state = u"expired"_s;
    else if (result.info.notAfter <= now.addDays(ServerCertificate::kRenewBeforeDays)) result.state = u"expiring"_s;
    else result.state = u"valid"_s;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(cert.bytes); hash.addData("\0"_ba); hash.addData(privateKey.bytes);
    result.digest = hash.result();
    return result;
}
bool cameraLoopback(const QString &path)
{
    const auto canonical = QFileInfo(path).canonicalFilePath();
    if (!canonical.startsWith(u"/dev/video") || !QRegularExpression(u"^/dev/video[0-9]+$"_s).match(canonical).hasMatch()
        || !trustedParents(path)) return false;
    const Fd fd(::open(QFile::encodeName(canonical).constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    v4l2_capability capabilities{};
    return fd.value >= 0 && !::fstat(fd.value, &info) && !info.st_uid && S_ISCHR(info.st_mode) && major(info.st_rdev) == 81
        && !::ioctl(fd.value, VIDIOC_QUERYCAP, &capabilities)
        && QByteArray(reinterpret_cast<const char *>(capabilities.driver), int(sizeof(capabilities.driver))).split('\0').front() == "v4l2 loopback";
}
QJsonObject publicView(Scope scope, const Read &current)
{
    auto result = BrokerHostAdmin::view(scope, current.exists, current.bytes).value;
    const auto effective = result[u"effective"_s].toObject().toVariantMap();
    if (scope != Scope::VirtualSession) {
        const auto tls = inspectTls(effective);
        result.insert(u"tls"_s, QJsonObject{{u"state"_s, tls.state}, {u"administratorManaged"_s, tls.administratorManaged},
            {u"fingerprint"_s, tls.info.sha256Fingerprint}, {u"algorithm"_s, tls.info.algorithm},
            {u"notBefore"_s, tls.info.notBefore.toString(Qt::ISODate)}, {u"notAfter"_s, tls.info.notAfter.toString(Qt::ISODate)}});
        const auto path = effective[u"CameraLoopbackDevice"_s].toString();
        result.insert(u"cameraLoopback"_s, QJsonObject{{u"supported"_s, scope == Scope::Console},
            {u"state"_s, path == u"none" ? u"disabled"_s : scope == Scope::Virtual ? u"namespace-unavailable"_s
                : cameraLoopback(path) ? u"available"_s : u"unavailable"_s}});
    } else {
        QJsonArray devices;
        const QDir directory(u"/dev/dri/by-path"_s);
        int inspected = 0;
        for (const auto &name : directory.entryList({u"pci-*-render"_s}, QDir::Files | QDir::System)) {
            if (++inspected > 128) break;
            const auto pci = name.mid(4, name.size() - 4 - 7);
            if (const auto device = VirtualGpuDevices::resolve(pci))
                devices.append(QJsonObject{{u"pci"_s, device->pci}, {u"driver"_s, device->driver}, {u"render"_s, device->render}});
        }
        result.insert(u"renderDevices"_s, devices);
    }
    // Never call a file save applied runtime state. Installed unit/drop-in and
    // process settings readback is a separate remaining service/UI gate.
    result.insert(u"runtimeVerified"_s, false);
    result.insert(u"application"_s, scope == Scope::VirtualSession ? u"new-desktops"_s : u"broker-restart"_s);
    return result;
}
bool writeNew(int directory, const char *name, const QByteArray &contents, mode_t mode)
{
    const Fd fd(::openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    if (fd.value < 0 || ::fchmod(fd.value, mode)) return false;
    qsizetype written = 0;
    while (written < contents.size()) {
        const auto count = ::write(fd.value, contents.constData() + written, size_t(contents.size() - written));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        written += count;
    }
    return !::fsync(fd.value);
}
struct Generation {
    Fd imports, route, directory;
    QByteArray name;
    QString prefix;
    bool committed = false;
    Generation(int policy, Scope scope)
        : imports(createDirectory(policy, "tls-imports")),
          route(createDirectory(imports.value, BrokerHostAdmin::scopeName(scope).toUtf8().constData()))
    {
        name = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
        if (route.value < 0 || ::mkdirat(route.value, name.constData(), 0700)) return;
        directory.value = openDirectory(route.value, name.constData());
        prefix = u"/etc/farside/tls-imports/%1/%2"_s.arg(BrokerHostAdmin::scopeName(scope), QString::fromLatin1(name));
    }
    static int createDirectory(int parent, const char *name)
    {
        if (parent < 0 || (::mkdirat(parent, name, 0700) && errno != EEXIST)) return -1;
        return openDirectory(parent, name);
    }
    ~Generation()
    {
        if (committed || directory.value < 0) return;
        for (const auto *name : {"certificate.crt", "private.key", "material.crt", "material.key"}) ::unlinkat(directory.value, name, 0);
        ::unlinkat(route.value, name.constData(), AT_REMOVEDIR);
    }
    bool write(const QByteArray &certificate, const QByteArray &key)
    {
        return directory.value >= 0 && writeNew(directory.value, "material.crt", certificate, 0644)
            && writeNew(directory.value, "material.key", key, 0600)
            && !::symlinkat("material.crt", directory.value, "certificate.crt")
            && !::symlinkat("material.key", directory.value, "private.key")
            && !::fsync(directory.value) && !::fsync(route.value) && !::fsync(imports.value);
    }
};
int reply(const QJsonObject &value, int status)
{
    QFile output;
    const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
    return output.open(stdout, QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.flush() ? status : 1;
}
int fail(const QString &error) { return reply({{u"error"_s, error}}, 1); }
}
int main(int argc, char **argv)
{
    if (::getuid() || ::geteuid()) return fail(u"administrator authorization is required"_s);
    if (argc != 1) return fail(u"invalid helper invocation"_s);
    rlimit limit{0, 0};
    if (::setrlimit(RLIMIT_CORE, &limit) || ::prctl(PR_SET_DUMPABLE, 0)) return fail(u"private execution context unavailable"_s);
    ::umask(0077);
    QCoreApplication app(argc, argv);
    QByteArray input;
    const auto clear = qScopeGuard([&] { if (!input.isEmpty()) OPENSSL_cleanse(input.data(), size_t(input.size())); });
    QElapsedTimer deadline; deadline.start();
    while (true) {
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const auto remaining = 10000 - deadline.elapsed();
        if (remaining <= 0) return fail(u"host settings request timed out"_s);
        const auto polled = ::poll(&descriptor, 1, int(remaining));
        if (polled < 0 && errno == EINTR) continue;
        if (polled <= 0) return fail(u"host settings request timed out"_s);
        char chunk[4096]; const auto count = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return fail(u"host settings request cannot be read"_s);
        if (!count) break;
        input.append(chunk, qsizetype(count)); OPENSSL_cleanse(chunk, sizeof(chunk));
        if (input.size() > BrokerHostAdmin::MaximumRequestBytes) return fail(u"host settings request is oversized"_s);
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(input, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return fail(u"invalid host settings request"_s);
    const auto request = document.object();
    const auto scope = BrokerHostAdmin::scope(request[u"scope"_s].toString());
    const auto operation = request[u"operation"_s].toString();
    if (!scope || !request[u"scope"_s].isString() || !request[u"version"_s].isDouble() || request[u"version"_s].toDouble() != 1
        || (operation != u"read" && operation != u"save" && operation != u"inspect-runtime")
        || (operation != u"save" && request.size() != 3)
        || (operation == u"inspect-runtime" && *scope == Scope::VirtualSession)) return fail(u"invalid host settings request"_s);
    const Fd directory(policyDirectory());
    if (directory.value < 0) return fail(u"Farside settings directory is unavailable or unsafe"_s);
    if (operation == u"inspect-runtime") {
        // Read-only inspection must not create a policy lock or touch TLS. The
        // complete stored file/directory is rechecked around the independent
        // fixed-unit/process reader instead.
        const auto current = readSettings(directory.value, *scope);
        if (!current.error.isEmpty()) return fail(current.error);
        const auto parsed = BrokerHostSettings::parse(*scope, current.bytes);
        if (!parsed.error.isEmpty()) return fail(parsed.error);
        const auto revision = BrokerHostAdmin::revision(*scope, current.exists, current.bytes);
        auto runtime = BrokerHostRuntime::inspect(*scope, parsed.effective, revision, QDBusConnection::systemBus());
        const auto after = readSettings(directory.value, *scope);
        if (!directoryStillCurrent(directory.value) || !after.error.isEmpty() || current.exists != after.exists || current.bytes != after.bytes)
            runtime = BrokerHostRuntime::summarize(*scope, {}, {}, {}, parsed.effective, revision,
                BrokerHostRuntime::installedContract(*scope), u"stale"_s);
        return reply({{u"runtime"_s, runtime}}, 0);
    }
    const QByteArray lockName = ".host-settings-" + BrokerHostAdmin::scopeName(*scope).toUtf8() + ".lock";
    // flock does not need a writable descriptor on the local policy filesystem.
    // This also allows a read on an existing safe lock during a read-only boot.
    const Fd lock(::openat(directory.value, lockName.constData(), O_RDONLY | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600));
    struct stat lockInfo{};
    if (lock.value < 0 || ::fstat(lock.value, &lockInfo) || !S_ISREG(lockInfo.st_mode) || lockInfo.st_uid || lockInfo.st_nlink != 1
        || (lockInfo.st_mode & 0077) || ::flock(lock.value, LOCK_EX | LOCK_NB)) return fail(u"host settings are busy or the lock is unsafe"_s);
    const auto current = readSettings(directory.value, *scope);
    if (!current.error.isEmpty()) return fail(current.error);
    const auto initial = BrokerHostAdmin::view(*scope, current.exists, current.bytes);
    if (!initial.error.isEmpty()) return fail(initial.error);
    if (operation == u"read") return reply({{u"snapshot"_s, publicView(*scope, current)}}, 0);
    auto update = BrokerHostAdmin::prepare(*scope, current.exists, current.bytes, request);
    const auto clearKey = qScopeGuard([&] { if (!update.privateKeyPem.isEmpty()) OPENSSL_cleanse(update.privateKeyPem.data(), size_t(update.privateKeyPem.size())); });
    if (!update.error.isEmpty()) return fail(update.error);
    const auto before = BrokerHostSettings::parse(*scope, current.bytes).effective;
    if (*scope == Scope::VirtualSession && update.effective[u"RenderPci"_s] != before[u"RenderPci"_s]) {
        const auto grants = update.effective[u"RenderPci"_s].toString();
        if (!grants.isEmpty()) for (const auto &pci : grants.split(u',')) if (!VirtualGpuDevices::resolve(pci))
            return fail(u"a selected PCI device has no complete supported GPU device set"_s);
    }
    if (*scope != Scope::VirtualSession && update.effective[u"CameraLoopbackDevice"_s] != before[u"CameraLoopbackDevice"_s]) {
        const auto camera = update.effective[u"CameraLoopbackDevice"_s].toString();
        if (camera != u"none" && (*scope == Scope::Virtual || !cameraLoopback(camera)))
            return fail(u"camera loopback is unavailable in this scope or is not a V4L2 loopback device"_s);
    }
    std::unique_ptr<Generation> generation;
    if (update.tls == BrokerHostAdmin::TlsMode::Import) {
        generation = std::make_unique<Generation>(directory.value, *scope);
        if (!generation->write(update.certificatePem, update.privateKeyPem)) return fail(u"TLS import could not be staged privately"_s);
        auto values = BrokerHostSettings::parse(*scope, update.document).overrides;
        values[u"Certificate"_s] = QString(generation->prefix + u"/certificate.crt"_s);
        values[u"CertificateKey"_s] = QString(generation->prefix + u"/private.key"_s);
        const auto edited = BrokerHostSettings::edit(*scope, update.document, values);
        if (!edited.error.isEmpty()) return fail(edited.error);
        update.document = edited.contents;
        update.effective = BrokerHostSettings::parse(*scope, update.document).effective;
    }
    Tls selectedTls;
    if (update.tls != BrokerHostAdmin::TlsMode::Keep) {
        selectedTls = inspectTls(update.effective);
        const bool standardMissing = update.tls == BrokerHostAdmin::TlsMode::Standard && selectedTls.state == u"missing";
        const bool standardRenewable = update.tls == BrokerHostAdmin::TlsMode::Standard && !selectedTls.administratorManaged
            && (selectedTls.state == u"invalid" || selectedTls.state == u"expired");
        if (!standardMissing && !standardRenewable && selectedTls.state != u"valid" && selectedTls.state != u"expiring")
            return fail(u"TLS paths require safe current matching certificate and key files"_s);
    }
    const auto fresh = readSettings(directory.value, *scope);
    if (!fresh.error.isEmpty() || fresh.exists != current.exists || fresh.bytes != current.bytes || !directoryStillCurrent(directory.value))
        return fail(u"host settings changed; reload before saving"_s);
    if (update.tls != BrokerHostAdmin::TlsMode::Keep) {
        const auto freshTls = inspectTls(update.effective);
        if (freshTls.state != selectedTls.state || freshTls.digest != selectedTls.digest || freshTls.administratorManaged != selectedTls.administratorManaged)
            return fail(u"TLS files changed; reload before saving"_s);
    }
    {
        QSaveFile file(u"/proc/self/fd/%1/%2"_s.arg(directory.value).arg(BrokerHostSettings::fileName(*scope)));
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
            || file.write(update.document) != update.document.size() || !file.flush() || ::fsync(file.handle()) || !file.commit())
            return fail(u"host settings could not be saved atomically"_s);
    }
    if (generation) generation->committed = true;
    const bool durable = !::fsync(directory.value);
    const auto saved = readSettings(directory.value, *scope);
    const auto verified = BrokerHostAdmin::view(*scope, saved.exists, saved.bytes);
    if (!durable || !saved.exists || !saved.error.isEmpty() || !verified.error.isEmpty() || saved.bytes != update.document || !directoryStillCurrent(directory.value))
        return reply({{u"saved"_s, true}, {u"error"_s, u"host settings saved but verification failed; reload before restarting"_s}}, 1);
    if (update.tls != BrokerHostAdmin::TlsMode::Keep) {
        const auto verifiedTls = inspectTls(update.effective);
        if (verifiedTls.state != selectedTls.state || verifiedTls.digest != selectedTls.digest
            || verifiedTls.administratorManaged != selectedTls.administratorManaged)
            return reply({{u"saved"_s, true}, {u"error"_s, u"host settings saved but TLS verification failed; reload before restarting"_s}}, 1);
    }
    return reply({{u"saved"_s, true}, {u"restartRequired"_s, *scope != Scope::VirtualSession},
        {u"newDesktopRequired"_s, *scope == Scope::VirtualSession}, {u"snapshot"_s, publicView(*scope, saved)}}, 0);
}
