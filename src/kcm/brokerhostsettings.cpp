// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerhostsettings.h"
#include "BrokerHostAdmin.h"
#include "BrokerHostRuntime.h"
#include "ServerCertificate.h"
#include <KLocalizedString>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <fcntl.h>
#include <memory>
#include <openssl/crypto.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
namespace Host = KRdp::BrokerHostSettings;
namespace Admin = KRdp::BrokerHostAdmin;
namespace Certificate = KRdp::ServerCertificate;
namespace {
void wipe(QByteArray &value) { if (!value.isEmpty()) OPENSSL_cleanse(value.data(), size_t(value.size())); value.clear(); }
struct PrivateBuffer {
    QByteArray bytes;
    explicit PrivateBuffer(QByteArray value) : bytes(std::move(value)) {}
    ~PrivateBuffer() { wipe(bytes); }
};
QVariantMap publicCertificate(const Certificate::Info &info)
{
    return {{u"fingerprint"_s, info.sha256Fingerprint}, {u"algorithm"_s, info.algorithm},
        {u"notBefore"_s, info.notBefore.toString(Qt::ISODate)}, {u"notAfter"_s, info.notAfter.toString(Qt::ISODate)}};
}
QByteArray readPem(const QUrl &url)
{
    // No QFile open on a FIFO/device: inspect a nonblocking FD first. Paths are
    // local caller-side inputs only, never sent to the privileged helper.
    if (!url.isLocalFile() || url.hasQuery() || url.hasFragment()) return {};
    const int fd = ::open(QFile::encodeName(url.toLocalFile()).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return {};
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); return {}; }
    struct stat before{}, after{};
    if (::fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size <= 0 || before.st_size > Admin::MaximumPemBytes) return {};
    auto bytes = file.read(Admin::MaximumPemBytes + 1);
    if (file.error() != QFileDevice::NoError || ::fstat(fd, &after) || bytes.size() != before.st_size
        || before.st_dev != after.st_dev || before.st_ino != after.st_ino || before.st_size != after.st_size
        || before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) wipe(bytes);
    return bytes;
}
bool validSnapshot(Host::Scope scope, const QJsonObject &value)
{
    const QStringList base{u"version"_s, u"scope"_s, u"revision"_s, u"values"_s, u"defaults"_s, u"effective"_s,
        u"runtimeVerified"_s, u"application"_s};
    auto allowed = base;
    allowed += scope == Host::Scope::VirtualSession ? QStringList{u"renderDevices"_s} : QStringList{u"tls"_s, u"cameraLoopback"_s};
    if (value.size() != allowed.size()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) if (!allowed.contains(it.key())) return false;
    if (!value[u"version"_s].isDouble() || value[u"version"_s].toDouble() != 1
        || value[u"scope"_s].toString() != Admin::scopeName(scope) || !value[u"revision"_s].isString()
        || !QRegularExpression(u"\\A[0-9a-f]{64}\\z"_s).match(value[u"revision"_s].toString()).hasMatch()
        || !value[u"runtimeVerified"_s].isBool() || value[u"runtimeVerified"_s].toBool()
        || value[u"application"_s].toString() != (scope == Host::Scope::VirtualSession ? u"new-desktops" : u"broker-restart")) return false;
    for (const auto &name : {u"values"_s, u"defaults"_s, u"effective"_s}) {
        if (!value[name].isObject()) return false;
        const auto map = value[name].toObject();
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (!it.value().isString()) return false;
            const auto normalized = Host::normalize(scope, it.key(), it.value().toString());
            if (!normalized || *normalized != it.value().toString()) return false;
        }
    }
    const auto defaults = Host::defaults(scope);
    if (value[u"defaults"_s].toObject().toVariantMap() != defaults) return false;
    auto effective = defaults;
    const auto overrides = value[u"values"_s].toObject();
    for (auto it = overrides.begin(); it != overrides.end(); ++it) effective[it.key()] = it.value().toString();
    if (value[u"effective"_s].toObject().toVariantMap() != effective) return false;
    if (scope != Host::Scope::VirtualSession) {
        if (effective[u"Certificate"_s] == effective[u"CertificateKey"_s] || !value[u"tls"_s].isObject() || !value[u"cameraLoopback"_s].isObject()) return false;
        const auto tls = value[u"tls"_s].toObject(), camera = value[u"cameraLoopback"_s].toObject();
        const QStringList tlsKeys{u"state"_s, u"administratorManaged"_s, u"fingerprint"_s, u"algorithm"_s, u"notBefore"_s, u"notAfter"_s};
        if (tls.size() != tlsKeys.size() || !tls[u"administratorManaged"_s].isBool()) return false;
        for (auto it = tls.begin(); it != tls.end(); ++it)
            if (!tlsKeys.contains(it.key()) || (it.key() != u"administratorManaged" && (!it.value().isString() || it.value().toString().size() > 256))) return false;
        if (!QStringList{u"valid"_s, u"expiring"_s, u"missing"_s, u"unsafe"_s, u"invalid"_s, u"expired"_s, u"not-yet-valid"_s}.contains(tls[u"state"_s].toString())) return false;
        if (camera.size() != 2 || !camera[u"supported"_s].isBool() || camera[u"supported"_s].toBool() != (scope == Host::Scope::Console)
            || !QStringList{u"disabled"_s, u"namespace-unavailable"_s, u"available"_s, u"unavailable"_s}.contains(camera[u"state"_s].toString())) return false;
    } else {
        if (!value[u"renderDevices"_s].isArray() || value[u"renderDevices"_s].toArray().size() > 128) return false;
        for (const auto &item : value[u"renderDevices"_s].toArray()) {
            if (!item.isObject()) return false;
            const auto device = item.toObject();
            if (device.size() != 3 || !device[u"pci"_s].isString() || device[u"pci"_s].toString().isEmpty() || device[u"pci"_s].toString().contains(u',')
                || !Host::normalize(scope, u"RenderPci"_s, device[u"pci"_s].toString())
                || !QRegularExpression(u"\\A/dev/dri/renderD[0-9]{1,6}\\z"_s).match(device[u"render"_s].toString()).hasMatch()
                || !QStringList{u"nvidia"_s, u"amdgpu"_s, u"i915"_s, u"xe"_s}.contains(device[u"driver"_s].toString())) return false;
        }
    }
    return true;
}
}

BrokerHostSettings::BrokerHostSettings(Scope scope, QObject *parent)
    : BrokerHostSettings(scope, u"/usr/bin/pkexec"_s,
        {u"--disable-internal-agent"_s, QString::fromUtf8(FARSIDE_HOST_SETTINGS_HELPER)}, 180000, parent) {}
BrokerHostSettings::BrokerHostSettings(Scope scope, const QString &program, const QStringList &arguments, int timeoutMs, QObject *parent)
    : QObject(parent), m_scope(scope), m_program(program), m_arguments(arguments), m_timeoutMs(qMax(1, timeoutMs)) {}
BrokerHostSettings::~BrokerHostSettings() { clearImport(); }
QString BrokerHostSettings::scope() const { return Admin::scopeName(m_scope); }
QVariantMap BrokerHostSettings::unitDefaults() const { return Host::defaults(m_scope); }
QVariantMap BrokerHostSettings::metadata() const
{
    auto result = m_snapshot; result.remove(u"values"_s); result.remove(u"revision"_s);
    return result.toVariantMap();
}
void BrokerHostSettings::clearImport() { wipe(m_certificate); wipe(m_key); m_importMetadata.clear(); }
bool BrokerHostSettings::modified() const { return loaded() && (m_pending != m_snapshot[u"values"_s].toObject().toVariantMap() || m_tlsMode != u"keep"); }
bool BrokerHostSettings::runtimeStale() const { return m_runtime.isEmpty() || !loaded()
    || m_runtime[u"state"_s].toString() == u"stale" || m_runtime[u"storedRevision"_s] != m_snapshot[u"revision"_s]; }
bool BrokerHostSettings::reject(const QString &error) { m_error = error; Q_EMIT changed(); return false; }
QString BrokerHostSettings::validationError() const
{
    if (!loaded()) return {};
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
        if (it.value().metaType().id() != QMetaType::QString || !Host::normalize(m_scope, it.key(), it.value().toString()))
            return i18nc("@info", "Invalid value for %1.", it.key());
    if (m_scope == Scope::VirtualSession) return {};
    auto effective = unitDefaults(); for (auto it = m_pending.begin(); it != m_pending.end(); ++it) effective[it.key()] = it.value();
    if (effective[u"Certificate"_s] == effective[u"CertificateKey"_s]) return i18nc("@info", "Certificate and private key paths must be different.");
    if (m_tlsMode == u"keep" && (effective[u"Certificate"_s] != m_snapshot[u"effective"_s].toObject()[u"Certificate"_s].toString()
        || effective[u"CertificateKey"_s] != m_snapshot[u"effective"_s].toObject()[u"CertificateKey"_s].toString()))
        return i18nc("@info", "Choose an explicit certificate operation before changing TLS paths.");
    if (m_tlsMode == u"existing" && (!m_pending.contains(u"Certificate"_s) || !m_pending.contains(u"CertificateKey"_s)))
        return i18nc("@info", "Both existing TLS paths are required.");
    if (m_tlsMode == u"import") {
        if (m_certificate.isEmpty() || m_key.isEmpty()) return i18nc("@info", "Select a matching certificate and unencrypted private key to import.");
        const auto until = QDateTime::fromString(m_importMetadata[u"notAfter"_s].toString(), Qt::ISODate);
        if (until <= QDateTime::currentDateTimeUtc()) return i18nc("@info", "The selected certificate has expired.");
    }
    return {};
}
QString BrokerHostSettings::error() const { return !m_error.isEmpty() ? m_error : validationError(); }
bool BrokerHostSettings::canSave() const { return modified() && !busy() && !m_outcomeUnknown && validationError().isEmpty(); }
bool BrokerHostSettings::setValue(const QString &key, const QString &value)
{
    if (!loaded() || busy() || !Host::keys(m_scope).contains(key) || value.size() > Host::MaximumValue || value.contains(QChar::Null)
        || value.contains(u'\n') || value.contains(u'\r')) return reject(i18nc("@info", "Invalid host field."));
    if ((key == u"Certificate" || key == u"CertificateKey") && m_tlsMode != u"existing") return false;
    if (key == u"CameraLoopbackDevice" && m_scope == Scope::Virtual && value != u"none")
        return reject(i18nc("@info", "Virtual camera loopback is unavailable in the current device namespace."));
    m_pending[key] = value; m_error.clear(); Q_EMIT changed(); return true;
}
bool BrokerHostSettings::inherit(const QString &key)
{
    if (!loaded() || busy() || !Host::keys(m_scope).contains(key)) return false;
    if (key == u"Certificate" || key == u"CertificateKey") return false;
    m_pending.remove(key); m_error.clear(); Q_EMIT changed(); return true;
}
bool BrokerHostSettings::chooseTls(const QString &mode)
{
    if (!loaded() || busy() || m_scope == Scope::VirtualSession
        || !QStringList{u"keep"_s, u"existing"_s, u"standard"_s, u"import"_s}.contains(mode)) return false;
    clearImport();
    m_tlsMode = mode;
    for (const auto &key : {u"Certificate"_s, u"CertificateKey"_s}) {
        if (mode == u"existing") m_pending[key] = m_snapshot[u"effective"_s].toObject()[key].toString();
        else if (mode == u"keep") {
            const auto saved = m_snapshot[u"values"_s].toObject();
            if (saved.contains(key)) m_pending[key] = saved[key].toString(); else m_pending.remove(key);
        } else m_pending.remove(key);
    }
    m_error.clear(); Q_EMIT changed(); return true;
}
bool BrokerHostSettings::importTls(const QUrl &certificate, const QUrl &key)
{
    if (!loaded() || busy() || m_tlsMode != u"import") return false;
    auto cert = readPem(certificate), privateKey = readPem(key);
    const auto info = Certificate::inspectPem(cert, privateKey);
    const auto now = QDateTime::currentDateTimeUtc();
    if (!info.usable() || !info.notBefore.isValid() || !info.notAfter.isValid() || info.notBefore > now || info.notAfter <= now) {
        wipe(cert); wipe(privateKey); clearImport();
        return reject(i18nc("@info", "Import requires readable, bounded PEM files containing a current matching certificate and unencrypted private key."));
    }
    clearImport(); m_certificate = std::move(cert); m_key = std::move(privateKey); m_importMetadata = publicCertificate(info);
    m_error.clear(); Q_EMIT changed(); return true;
}
void BrokerHostSettings::clearTlsImport()
{
    if (busy() || m_tlsMode != u"import") return;
    clearImport(); m_error.clear(); Q_EMIT changed();
}
void BrokerHostSettings::defaults()
{
    if (!loaded() || busy()) return;
    m_pending.clear(); clearImport(); m_tlsMode = m_scope == Scope::VirtualSession ? u"keep"_s : u"standard"_s;
    m_error.clear(); Q_EMIT changed();
}
void BrokerHostSettings::discard()
{
    if (!loaded() || busy()) return;
    m_pending = m_snapshot[u"values"_s].toObject().toVariantMap(); m_tlsMode = u"keep"_s;
    clearImport(); m_error.clear(); Q_EMIT changed();
}
bool BrokerHostSettings::reload() { return start({{u"version"_s, 1}, {u"operation"_s, u"read"_s}, {u"scope"_s, scope()}}, false); }
bool BrokerHostSettings::inspectRuntime()
{
    if (!loaded() || busy() || m_scope == Scope::VirtualSession) return false;
    m_runtime = {}; m_runtimeCheckedAt.clear();
    return start({{u"version"_s, 1}, {u"operation"_s, u"inspect-runtime"_s}, {u"scope"_s, scope()}}, false);
}
bool BrokerHostSettings::save()
{
    if (!canSave()) return false;
    QJsonObject values;
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) values[it.key()] = *Host::normalize(m_scope, it.key(), it.value().toString());
    QJsonObject request{{u"version"_s, 1}, {u"operation"_s, u"save"_s}, {u"scope"_s, scope()},
        {u"revision"_s, m_snapshot[u"revision"_s]}, {u"values"_s, values}};
    if (m_scope != Scope::VirtualSession) {
        QJsonObject tls{{u"mode"_s, m_tlsMode}};
        if (m_tlsMode == u"import") { tls[u"certificatePem"_s] = QString::fromUtf8(m_certificate); tls[u"privateKeyPem"_s] = QString::fromUtf8(m_key); }
        request[u"tls"_s] = tls;
    }
    return start(request, true);
}
bool BrokerHostSettings::start(QJsonObject request, bool saving)
{
    if (busy()) return false;
    auto input = std::make_shared<PrivateBuffer>(QJsonDocument(request).toJson(QJsonDocument::Compact));
    if (input->bytes.size() > Admin::MaximumRequestBytes) return reject(i18nc("@info", "Host settings request is too large."));
    auto *process = new QProcess(this); m_process = process; m_error.clear();
    const bool inspecting = request[u"operation"_s].toString() == u"inspect-runtime";
    process->setProgram(m_program); process->setArguments(m_arguments);
    auto output = std::make_shared<QByteArray>();
    auto failed = std::make_shared<bool>(false);
    auto *timer = new QTimer(process); timer->setSingleShot(true);
    const auto uncertain = [this, process, saving, failed] {
        if (m_process != process) return;
        *failed = true; if (saving) m_outcomeUnknown = true;
        process->kill(); reject(i18nc("@info", "Administration did not finish reliably. Reload before saving again; a submitted save may already have changed stored settings."));
    };
    connect(timer, &QTimer::timeout, this, uncertain);
    connect(process, &QProcess::started, this, [process, input] {
        process->write(input->bytes); process->closeWriteChannel(); wipe(input->bytes);
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [process, output, uncertain] {
        output->append(process->readAllStandardOutput());
        if (output->size() > Admin::MaximumRequestBytes) { wipe(*output); uncertain(); }
    });
    // Do not forward helper/pkexec diagnostics (which might echo input); bound
    // buffering by draining stderr as it arrives without a readable property.
    connect(process, &QProcess::readyReadStandardError, this, [process] { auto bytes = process->readAllStandardError(); wipe(bytes); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError code) {
        if (m_process != process || code != QProcess::FailedToStart) return;
        m_process = nullptr; process->deleteLater(); reject(i18nc("@info", "Farside host administration could not start."));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, process, output, failed, timer, saving, inspecting](int code, QProcess::ExitStatus status) {
        if (m_process != process) return;
        timer->stop(); m_process = nullptr; output->append(process->readAllStandardOutput()); process->deleteLater();
        if (*failed) { wipe(*output); Q_EMIT changed(); return; }
        if (status != QProcess::NormalExit) { if (saving) m_outcomeUnknown = true; wipe(*output);
            reject(i18nc("@info", "Administration stopped unexpectedly. Reload to determine whether stored settings changed.")); return; }
        if ((code == 126 || code == 127) && output->isEmpty()) {
            reject(code == 126 ? i18nc("@info", "Administrator authentication was cancelled. Pending edits are preserved.")
                : i18nc("@info", "Administrator authentication was not granted. Pending edits are preserved.")); return;
        }
        QJsonParseError parse;
        const auto document = output->size() <= Admin::MaximumRequestBytes ? QJsonDocument::fromJson(*output, &parse) : QJsonDocument();
        wipe(*output);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            if (saving) m_outcomeUnknown = true;
            reject(i18nc("@info", "Invalid administration reply. Reload to determine whether stored settings changed.")); return;
        }
        const auto reply = document.object();
        const QStringList allowed = reply.contains(u"error"_s) ? QStringList{u"error"_s, u"saved"_s}
            : saving ? QStringList{u"snapshot"_s, u"saved"_s, u"restartRequired"_s, u"newDesktopRequired"_s}
                : inspecting ? QStringList{u"runtime"_s} : QStringList{u"snapshot"_s};
        bool structure = reply.size() == (reply.contains(u"error"_s) ? reply.contains(u"saved"_s) ? 2 : 1 : allowed.size());
        for (auto it = reply.begin(); it != reply.end(); ++it) if (!allowed.contains(it.key())) structure = false;
        if (reply.contains(u"error"_s) && (!reply[u"error"_s].isString()
            || (reply.contains(u"saved"_s) && (!reply[u"saved"_s].isBool() || !reply[u"saved"_s].toBool())))) structure = false;
        if (!structure) {
            if (saving) m_outcomeUnknown = true;
            reject(i18nc("@info", "Invalid administration reply. Reload to determine whether stored settings changed.")); return;
        }
        if (reply[u"saved"_s].isBool() && reply[u"saved"_s].toBool()) m_applicationRequired = true;
        if (code != 0 || reply.contains(u"error"_s)) {
            if (m_applicationRequired && saving && reply[u"saved"_s].toBool()) m_outcomeUnknown = true;
            // Structural categorization only: never echo arbitrary reply text.
            const auto reason = reply[u"error"_s].toString();
            reject(reason.contains(u"changed") ? i18nc("@info", "Stored settings changed. Reload before saving; pending edits are preserved.")
                : reply[u"saved"_s].toBool() ? i18nc("@info", "Settings were saved but verification failed. Reload before applying them.")
                : i18nc("@info", "Host administration refused the request. Check fields, TLS material and device availability. Pending edits are preserved.")); return;
        }
        if (inspecting) {
            if (!reply[u"runtime"_s].isObject() || !KRdp::BrokerHostRuntime::validPublic(m_scope, reply[u"runtime"_s].toObject())) {
                reject(i18nc("@info", "Invalid running-host inspection reply. Pending edits are preserved.")); return;
            }
            m_runtime = reply[u"runtime"_s].toObject(); m_runtimeCheckedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            Q_EMIT changed(); return;
        }
        const auto snapshot = reply[u"snapshot"_s].toObject();
        if (!validSnapshot(m_scope, snapshot) || (saving && (!reply[u"saved"_s].isBool() || !reply[u"saved"_s].toBool()
            || !reply[u"restartRequired"_s].isBool() || reply[u"restartRequired"_s].toBool() != (m_scope != Scope::VirtualSession)
            || !reply[u"newDesktopRequired"_s].isBool() || reply[u"newDesktopRequired"_s].toBool() != (m_scope == Scope::VirtualSession)))) {
            if (saving) m_outcomeUnknown = true;
            reject(i18nc("@info", "Invalid host snapshot. Reload before applying or saving settings.")); return;
        }
        // A crash can occur after publication but before any reply. If the
        // subsequent explicit reload finds a new revision, retain the need to
        // apply it rather than clearing the unknown outcome without a notice.
        if (!saving && m_outcomeUnknown && loaded() && m_snapshot[u"revision"_s] != snapshot[u"revision"_s]) m_applicationRequired = true;
        m_snapshot = snapshot; m_pending = snapshot[u"values"_s].toObject().toVariantMap();
        clearImport(); m_tlsMode = u"keep"_s; m_outcomeUnknown = false; m_error.clear(); Q_EMIT changed();
    });
    Q_EMIT changed(); timer->start(m_timeoutMs); process->start(); return true;
}

QVariantList BrokerHostSettings::definitions() const
{
    const auto choice = [](const QString &value, const QString &text) { return QVariantMap{{u"value"_s, value}, {u"text"_s, text}}; };
    const QVariantList boolean{choice(u"true"_s, i18nc("@item:inlistbox", "On")), choice(u"false"_s, i18nc("@item:inlistbox", "Off"))};
    QVariantList result;
    const auto add = [&](const QString &key, const QString &group, const QString &label, const QString &help, QVariantList options = {}) {
        if (!Host::keys(m_scope).contains(key)) return;
        if (!options.isEmpty()) options.prepend(choice({}, i18nc("@item:inlistbox", "Use unit default")));
        result.append(QVariantMap{{u"key"_s, key}, {u"group"_s, group}, {u"label"_s, label}, {u"help"_s, help}, {u"choices"_s, options}});
    };
    const auto listener = i18nc("@title:group", "Connection"), video = i18nc("@title:group", "Host Video Defaults"), media = i18nc("@title:group", "Audio and Devices");
    add(u"Address"_s, listener, i18nc("@label", "Listen address"), i18nc("@info", "Numeric IPv4 or IPv6 address. 0.0.0.0 listens on all IPv4 interfaces."));
    add(u"Port"_s, listener, i18nc("@label", "Listen port"), i18nc("@info", "1–65535. Changing the port requires clients to use the new port after a broker restart."));
    add(u"Certificate"_s, listener, i18nc("@label", "Existing certificate path"), i18nc("@info", "An existing absolute path managed by root. Select Use existing paths to edit both TLS paths."));
    add(u"CertificateKey"_s, listener, i18nc("@label", "Existing private key path"), i18nc("@info", "An existing absolute path to a matching, unencrypted, private root key. Key material is never displayed."));
    add(u"Quality"_s, video, i18nc("@label", "Video quality"), i18nc("@info", "0–100. Users can override this default; higher values use more bandwidth."));
    add(u"AdaptiveQuality"_s, video, i18nc("@label", "Adapt quality to the connection"), i18nc("@info", "Adjust quality to measured link capacity."), boolean);
    add(u"SoftwareEncoding"_s, video, i18nc("@label", "Software encoding"), i18nc("@info", "Hardware and client capabilities still determine the codec. Hardware preference allows software AVC as a last resort."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"never"_s, i18nc("@item:inlistbox", "Prefer hardware")), choice(u"prefer"_s, i18nc("@item:inlistbox", "Allow the best codec in software"))});
    add(u"Av1Tiles"_s, video, i18nc("@label", "AV1 tiles"), i18nc("@info", "Automatic considers client decode support. Available encoders may limit tile choice."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"1"_s, u"1"_s), choice(u"2"_s, u"2"_s), choice(u"4"_s, u"4"_s), choice(u"8"_s, u"8"_s), choice(u"16"_s, u"16"_s)});
    add(u"PreferAudioQuality"_s, media, i18nc("@label", "Prefer audio quality"), i18nc("@info", "Prioritize audio quality when media is enabled."), boolean);
    add(u"StandardClientMedia"_s, media, i18nc("@label", "Allow standard client media"), i18nc("@info", "Host permission for standard media channels; channel consent is still required."), boolean);
    add(u"CameraLoopbackDevice"_s, media, i18nc("@label", "Camera loopback device"), i18nc("@info", "none or an existing V4L2 loopback /dev/videoN. Console workers also need OS permission. Virtual loopback is currently unavailable; normal PipeWire camera delivery is separate."));
    add(u"RenderPci"_s, i18nc("@title:group", "New Virtual Desktops"), i18nc("@label", "Granted GPU PCI identities"), i18nc("@info", "Comma-separated identities such as 0000:01:00.0. Empty grants no GPU. This is a namespace allowlist, not an encoder selector. Existing desktops keep their current grants."));
    add(u"VaapiDriver"_s, m_scope == Scope::VirtualSession ? i18nc("@title:group", "New Virtual Desktops") : video,
        i18nc("@label", "VA-API driver policy"), i18nc("@info", "Controls VA-API probing, not NVIDIA encoding or per-stream GPU selection. Virtual changes apply only to newly created desktops."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"off"_s, i18nc("@item:inlistbox", "Disabled")),
         choice(u"radeonsi"_s, u"radeonsi"_s), choice(u"iHD"_s, u"iHD"_s), choice(u"i965"_s, u"i965"_s)});
    return result;
}
