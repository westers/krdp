// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerhostsettings.h"
#include "BrokerHostAdmin.h"
#include "BrokerHostBatch.h"
#include "BrokerHostPublicSnapshot.h"
#include "BrokerHostRuntime.h"
#include "ServerCertificate.h"
#include "settingfielddefinition.h"
#include <KLocalizedString>
#include <QFile>
#include <QPointer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <functional>
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
// The unprivileged public snapshot omits the TLS path values (never published);
// the helper's full reply still carries them. Compare both forms path-agnostically.
QVariantMap withoutHiddenPaths(QVariantMap map)
{
    map.remove(u"Certificate"_s); map.remove(u"CertificateKey"_s);
    return map;
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
    if (withoutHiddenPaths(value[u"defaults"_s].toObject().toVariantMap()) != withoutHiddenPaths(defaults)) return false;
    auto effective = defaults;
    const auto overrides = value[u"values"_s].toObject();
    for (auto it = overrides.begin(); it != overrides.end(); ++it) effective[it.key()] = it.value().toString();
    if (withoutHiddenPaths(value[u"effective"_s].toObject().toVariantMap()) != withoutHiddenPaths(effective)) return false;
    const auto reported = value[u"effective"_s].toObject();
    if (scope != Host::Scope::VirtualSession) {
        if ((reported.contains(u"Certificate"_s) && reported[u"Certificate"_s] == reported[u"CertificateKey"_s]) || !value[u"tls"_s].isObject() || !value[u"cameraLoopback"_s].isObject()) return false;
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
QString BrokerHostSettings::fieldLabel(const QString &key) const
{
    for (const auto &definition : definitions()) {
        const auto row = definition.toMap();
        if (row.value(u"key"_s).toString() == key) return row.value(u"formLabel"_s).toString();
    }
    return key;
}
QString BrokerHostSettings::validationError() const
{
    if (!loaded()) return {};
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
        if (it.value().metaType().id() != QMetaType::QString || !Host::normalize(m_scope, it.key(), it.value().toString()))
            return i18nc("@info %1 setting name", "“%1” has an invalid value.", fieldLabel(it.key()));
    if (m_scope == Scope::VirtualSession) return {};
    auto effective = unitDefaults(); for (auto it = m_pending.begin(); it != m_pending.end(); ++it) effective[it.key()] = it.value();
    if (effective[u"Certificate"_s] == effective[u"CertificateKey"_s]) return i18nc("@info", "The certificate and private key must be different files.");
    if (m_tlsMode == u"keep") {
        // Compare with the saved overrides, not the effective paths: the public
        // snapshot does not carry them, so a kept path is simply absent here.
        const auto saved = m_snapshot[u"values"_s].toObject();
        for (const auto &key : {u"Certificate"_s, u"CertificateKey"_s})
            if (m_pending.contains(key) != saved.contains(key) || (m_pending.contains(key) && m_pending[key].toString() != saved[key].toString()))
                return i18nc("@info", "Choose how to change the certificate before editing its file paths.");
    }
    if (m_tlsMode == u"existing" && (!m_pending.contains(u"Certificate"_s) || !m_pending.contains(u"CertificateKey"_s)))
        return i18nc("@info", "Both a certificate file and a private key file are required.");
    if (m_tlsMode == u"import") {
        if (m_certificate.isEmpty() || m_key.isEmpty()) return i18nc("@info", "Choose a matching certificate and unencrypted private key to import.");
        const auto until = QDateTime::fromString(m_importMetadata[u"notAfter"_s].toString(), Qt::ISODate);
        if (until <= QDateTime::currentDateTimeUtc()) return i18nc("@info", "The selected certificate has expired.");
    }
    return {};
}
QString BrokerHostSettings::error() const { return !m_error.isEmpty() ? m_error : validationError(); }
bool BrokerHostSettings::canSave() const { return modified() && !busy() && !m_outcomeUnknown && validationError().isEmpty(); }
QObject *BrokerHostSettings::certificateDraft()
{
    if (m_draftOnly || m_scope == Scope::VirtualSession) return nullptr;
    if (!m_certificateDraft) {
        m_certificateDraft = new BrokerHostSettings(m_scope, m_program, m_arguments, m_timeoutMs, this);
        m_certificateDraft->m_draftOnly = true;
    }
    return m_certificateDraft;
}
bool BrokerHostSettings::canStageCertificate() const
{
    return m_draftOnly && loaded() && validationError().isEmpty();
}
bool BrokerHostSettings::beginCertificateEdit()
{
    if (!loaded() || busy() || m_outcomeUnknown || !certificateDraft()) return false;
    auto *draft = m_certificateDraft;
    draft->clearImport();
    draft->m_snapshot = m_snapshot;
    draft->m_pending.clear();
    for (const auto &key : {u"Certificate"_s, u"CertificateKey"_s})
        if (m_pending.contains(key)) draft->m_pending[key] = m_pending[key];
    draft->m_tlsMode = m_tlsMode;
    draft->m_certificate = m_certificate; draft->m_key = m_key;
    draft->m_importMetadata = m_importMetadata;
    draft->m_error.clear();
    Q_EMIT draft->changed();
    return true;
}
bool BrokerHostSettings::stageCertificateEdit()
{
    auto *draft = m_certificateDraft;
    if (busy() || m_outcomeUnknown || !draft || !draft->canStageCertificate()) return false;
    if (draft->m_snapshot[u"revision"_s] != m_snapshot[u"revision"_s])
        return draft->reject(i18nc("@info", "The host settings changed. Cancel, then reopen the certificate editor."));
    for (const auto &key : {u"Certificate"_s, u"CertificateKey"_s}) {
        m_pending.remove(key);
        if (draft->m_pending.contains(key)) m_pending[key] = draft->m_pending[key];
    }
    clearImport();
    m_tlsMode = draft->m_tlsMode;
    m_certificate = draft->m_certificate; m_key = draft->m_key;
    m_importMetadata = draft->m_importMetadata;
    cancelCertificateEdit();
    m_error.clear(); Q_EMIT changed();
    return true;
}
void BrokerHostSettings::cancelCertificateEdit()
{
    if (!m_certificateDraft) return;
    m_certificateDraft->clearImport();
    m_certificateDraft->m_snapshot = {}; m_certificateDraft->m_pending.clear();
    m_certificateDraft->m_tlsMode = u"keep"_s; m_certificateDraft->m_error.clear();
    Q_EMIT m_certificateDraft->changed();
}
bool BrokerHostSettings::setValue(const QString &key, const QString &value)
{
    if (!loaded() || busy() || !Host::keys(m_scope).contains(key) || value.size() > Host::MaximumValue || value.contains(QChar::Null)
        || value.contains(u'\n') || value.contains(u'\r')) return reject(i18nc("@info", "That setting cannot be changed here."));
    if ((key == u"Certificate" || key == u"CertificateKey") && m_tlsMode != u"existing") return false;
    if (key == u"CameraLoopbackDevice" && m_scope == Scope::Virtual && value != u"none")
        return reject(i18nc("@info", "Camera sharing is not available for Virtual desktops yet."));
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
        return reject(i18nc("@info", "The files could not be used. Choose readable PEM files with a current, matching certificate and an unencrypted private key."));
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
    // Ordinary defaults do not change certificates or a staged private import.
    for (const auto &key : m_pending.keys())
        if (key != u"Certificate" && key != u"CertificateKey") m_pending.remove(key);
    m_error.clear(); Q_EMIT changed();
}
bool BrokerHostSettings::representsDefaults() const
{
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
        if (it.key() != u"Certificate" && it.key() != u"CertificateKey") return false;
    return true;
}
void BrokerHostSettings::discard()
{
    if (!loaded() || busy()) return;
    m_pending = m_snapshot[u"values"_s].toObject().toVariantMap(); m_tlsMode = u"keep"_s;
    clearImport(); m_error.clear(); Q_EMIT changed();
}
bool BrokerHostSettings::reload() { return start({{u"version"_s, 1}, {u"operation"_s, u"read"_s}, {u"scope"_s, scope()}}, false); }
bool BrokerHostSettings::refresh()
{
    // Unprivileged: the root-written public-metadata snapshot, never a helper.
    if (busy() || m_draftOnly) return false;
    const auto override = qEnvironmentVariable("FARSIDE_PUBLIC_SETTINGS_DIR");
    const auto read = KRdp::BrokerHostPublicSnapshot::read(override.isEmpty() ? KRdp::BrokerHostPublicSnapshot::defaultDirectory() : override,
        m_scope, !override.isEmpty());
    if (!read.error.isEmpty()) return reject(read.error);
    if (!validSnapshot(m_scope, read.value)) return reject(i18nc("@info", "The published settings are not valid. Restart the service to publish them again."));
    adoptSnapshot(read.value, false);
    return true;
}
void BrokerHostSettings::adoptSnapshot(const QJsonObject &snapshot, bool saving)
{
    // A crash can occur after publication but before any reply. If the
    // subsequent explicit reload finds a new revision, retain the need to
    // apply it rather than clearing the unknown outcome without a notice.
    if (!saving && m_outcomeUnknown && loaded() && m_snapshot[u"revision"_s] != snapshot[u"revision"_s]) m_applicationRequired = true;
    m_snapshot = snapshot; m_pending = snapshot[u"values"_s].toObject().toVariantMap();
    clearImport(); m_tlsMode = u"keep"_s; m_outcomeUnknown = false; m_error.clear(); Q_EMIT changed();
}
bool BrokerHostSettings::inspectRuntime()
{
    if (!loaded() || busy() || m_scope == Scope::VirtualSession) return false;
    m_runtime = {}; m_runtimeCheckedAt.clear();
    return start({{u"version"_s, 1}, {u"operation"_s, u"inspect-runtime"_s}, {u"scope"_s, scope()}}, false);
}
QJsonObject BrokerHostSettings::saveRequest() const
{
    QJsonObject values;
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) values[it.key()] = *Host::normalize(m_scope, it.key(), it.value().toString());
    QJsonObject request{{u"version"_s, 1}, {u"operation"_s, u"save"_s}, {u"scope"_s, scope()},
        {u"revision"_s, m_snapshot[u"revision"_s]}, {u"values"_s, values}};
    if (m_scope != Scope::VirtualSession) {
        QJsonObject tls{{u"mode"_s, m_tlsMode}};
        if (m_tlsMode == u"import") { tls[u"certificatePem"_s] = QString::fromUtf8(m_certificate); tls[u"privateKeyPem"_s] = QString::fromUtf8(m_key); }
        request[u"tls"_s] = tls;
    }
    return request;
}
bool BrokerHostSettings::save()
{
    if (!canSave()) return false;
    return start(saveRequest(), true);
}
int BrokerHostSettings::saveTogether(const QList<BrokerHostSettings *> &requested)
{
    QList<QPointer<BrokerHostSettings>> models;
    QList<QJsonObject> requests;
    for (auto *model : requested) {
        if (!model || model->m_draftOnly || !model->canSave() || models.size() >= KRdp::BrokerHostBatch::MaximumRequests) continue;
        models.append(model); requests.append(model->saveRequest());
    }
    if (models.isEmpty()) return 0;
    // One dirty scope: the ordinary single-scope protocol.
    if (models.size() == 1) return models.first()->save() ? 1 : 0;
    auto *carrier = models.first().data();
    auto input = std::make_shared<PrivateBuffer>(QJsonDocument(KRdp::BrokerHostBatch::request(requests)).toJson(QJsonDocument::Compact));
    QStringList scopes;
    for (const auto &model : models) scopes.append(model->scope());
    for (const auto &model : models) { model->m_batch = true; model->m_error.clear(); }
    auto *process = new QProcess(carrier);
    process->setProgram(carrier->m_program); process->setArguments(carrier->m_arguments);
    auto output = std::make_shared<QByteArray>();
    auto failed = std::make_shared<bool>(false);
    // Every model leaves the batch exactly once, through settle().
    const auto settle = [models](const std::function<void(BrokerHostSettings *, int)> &each) {
        for (int i = 0; i < models.size(); ++i) if (models[i]) { models[i]->m_batch = false; each(models[i], i); }
    };
    auto *timer = new QTimer(process); timer->setSingleShot(true);
    const auto uncertain = [process, failed, settle] {
        if (*failed) return;
        *failed = true; process->kill();
        settle([](BrokerHostSettings *model, int) {
            model->m_outcomeUnknown = true;
            model->reject(i18nc("@info", "Administration did not finish reliably. Reset the page before saving again; a save that was already sent may have changed the stored settings."));
        });
    };
    connect(timer, &QTimer::timeout, process, uncertain);
    connect(process, &QProcess::started, process, [process, input] {
        process->write(input->bytes); process->closeWriteChannel(); wipe(input->bytes);
    });
    connect(process, &QProcess::readyReadStandardOutput, process, [process, output, uncertain] {
        output->append(process->readAllStandardOutput());
        if (output->size() > KRdp::BrokerHostBatch::MaximumInputBytes) { wipe(*output); uncertain(); }
    });
    connect(process, &QProcess::readyReadStandardError, process, [process] { auto bytes = process->readAllStandardError(); wipe(bytes); });
    connect(process, &QProcess::errorOccurred, process, [process, settle](QProcess::ProcessError code) {
        if (code != QProcess::FailedToStart) return;
        process->deleteLater();
        settle([](BrokerHostSettings *model, int) { model->reject(i18nc("@info", "Farside host administration could not start.")); });
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process,
        [process, output, failed, timer, settle, scopes](int code, QProcess::ExitStatus status) {
        timer->stop(); output->append(process->readAllStandardOutput()); process->deleteLater();
        if (*failed) { wipe(*output); settle([](BrokerHostSettings *model, int) { Q_EMIT model->changed(); }); return; }
        // Cancelled, denied or crashed: the same handling as a single-scope save, for every scope.
        std::optional<QList<KRdp::BrokerHostBatch::Entry>> entries;
        if (status == QProcess::NormalExit && code == 0) {
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(*output, &parse);
            if (parse.error == QJsonParseError::NoError && document.isObject()) entries = KRdp::BrokerHostBatch::parseReply(document.object(), scopes);
            if (!entries) { wipe(*output); settle([](BrokerHostSettings *model, int) { QByteArray bad("x"); model->finish(0, QProcess::NormalExit, bad, true, false); }); return; }
        }
        const QByteArray unwrapped = *output;
        wipe(*output);
        settle([&](BrokerHostSettings *model, int index) {
            if (!entries) { QByteArray copy = unwrapped; model->finish(code, status, copy, true, false); return; }
            QByteArray reply = QJsonDocument(entries->at(index).reply).toJson(QJsonDocument::Compact);
            model->finish(entries->at(index).status, QProcess::NormalExit, reply, true, false);
        });
    });
    for (const auto &model : models) Q_EMIT model->changed();
    timer->start(carrier->m_timeoutMs); process->start();
    return int(models.size());
}
bool BrokerHostSettings::start(QJsonObject request, bool saving)
{
    // The local certificate editor can validate/stage, never invoke helpers.
    if (busy() || m_draftOnly) return false;
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
        process->kill(); reject(i18nc("@info", "Administration did not finish reliably. Reset the page before saving again; a save that was already sent may have changed the stored settings."));
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
        finish(code, status, *output, saving, inspecting);
    });
    Q_EMIT changed(); timer->start(m_timeoutMs); process->start(); return true;
}

void BrokerHostSettings::finish(int code, QProcess::ExitStatus status, QByteArray &output, bool saving, bool inspecting)
{
    if (status != QProcess::NormalExit) { if (saving) m_outcomeUnknown = true; wipe(output);
        reject(i18nc("@info", "Administration stopped unexpectedly. Reset the page to see whether the stored settings changed.")); return; }
    if ((code == 126 || code == 127) && output.isEmpty()) {
        reject(code == 126 ? i18nc("@info", "Administrator authentication was cancelled. Your pending edits are kept.")
            : i18nc("@info", "Administrator authentication was not granted. Your pending edits are kept.")); return;
    }
    QJsonParseError parse;
    const auto document = output.size() <= Admin::MaximumRequestBytes ? QJsonDocument::fromJson(output, &parse) : QJsonDocument();
    wipe(output);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        if (saving) m_outcomeUnknown = true;
        reject(i18nc("@info", "Administration gave an unexpected reply. Reset the page to see whether the stored settings changed.")); return;
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
        reject(i18nc("@info", "Administration gave an unexpected reply. Reset the page to see whether the stored settings changed.")); return;
    }
    if (reply[u"saved"_s].isBool() && reply[u"saved"_s].toBool()) m_applicationRequired = true;
    if (code != 0 || reply.contains(u"error"_s)) {
        if (m_applicationRequired && saving && reply[u"saved"_s].toBool()) m_outcomeUnknown = true;
        // Structural categorization only: never echo arbitrary reply text.
        const auto reason = reply[u"error"_s].toString();
        reject(reason.contains(u"changed") ? i18nc("@info", "The stored settings changed. Reset the page before saving again. Your pending edits are kept.")
            : reply[u"saved"_s].toBool() ? i18nc("@info", "The settings were saved, but checking them afterwards failed. Reset the page before you use them.")
            : i18nc("@info", "Host administration refused the request. Check the fields, certificate files and devices. Your pending edits are kept.")); return;
    }
    if (inspecting) {
        if (!reply[u"runtime"_s].isObject() || !KRdp::BrokerHostRuntime::validPublic(m_scope, reply[u"runtime"_s].toObject())) {
            reject(i18nc("@info", "The service check gave an unexpected reply. Your pending edits are kept.")); return;
        }
        m_runtime = reply[u"runtime"_s].toObject(); m_runtimeCheckedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        Q_EMIT changed(); return;
    }
    const auto snapshot = reply[u"snapshot"_s].toObject();
    if (!validSnapshot(m_scope, snapshot) || (saving && (!reply[u"saved"_s].isBool() || !reply[u"saved"_s].toBool()
        || !reply[u"restartRequired"_s].isBool() || reply[u"restartRequired"_s].toBool() != (m_scope != Scope::VirtualSession)
        || !reply[u"newDesktopRequired"_s].isBool() || reply[u"newDesktopRequired"_s].toBool() != (m_scope == Scope::VirtualSession)))) {
        if (saving) m_outcomeUnknown = true;
        reject(i18nc("@info", "The host settings could not be read back. Reset the page before applying or saving settings.")); return;
    }
    adoptSnapshot(snapshot, saving);
}

QString BrokerHostSettings::sectionTitle(const QString &section) const
{
    if (section == u"connection") return i18nc("@title:group", "Connection");
    if (section == u"picture") return i18nc("@title:group", "Picture and Sound");
    return {};
}

QVariantList BrokerHostSettings::definitions() const
{
    using namespace KRdp::SettingFields;
    const auto choice = [](const QString &value, const QString &text) { return QVariantMap{{u"value"_s, value}, {u"text"_s, text}}; };
    const QVariantList boolean{choice(u"true"_s, i18nc("@item:inlistbox", "On")), choice(u"false"_s, i18nc("@item:inlistbox", "Off"))};
    QVariantList result;
    const auto add = [&](const QString &key, const QString &group, const QString &label, const QString &help, QVariantList options, const Spec &spec) {
        if (!Host::keys(m_scope).contains(key)) return;
        if (!options.isEmpty()) options.prepend(choice({}, i18nc("@item:inlistbox", "Use default")));
        auto withScope = spec;
        withScope.inheritText = i18nc("@item:inlistbox", "Use default");
        result.append(makeFieldDefinition(key, group, label, help, options, withScope));
    };
    const auto listener = i18nc("@title:group", "Connection"), video = i18nc("@title:group", "Host Video Defaults"), media = i18nc("@title:group", "Audio and Devices");
    add(u"Address"_s, listener, i18nc("@label", "Listen address"), i18nc("@info", "The network address to accept connections on. Enter a numeric IPv4 or IPv6 address; 0.0.0.0 accepts connections on all IPv4 interfaces."), {},
        {.control = u"address"_s, .section = u"connection"_s, .formLabel = i18nc("@label", "Listen on"), .keepEmpty = true,
         .modes = {choice({}, i18nc("@item:inlistbox", "Default (all IPv4 interfaces)")), choice(u"0.0.0.0"_s, i18nc("@item:inlistbox", "All IPv4 interfaces")),
                   choice(u"::"_s, i18nc("@item:inlistbox", "All IPv6 interfaces")), choice(AddressCustom, i18nc("@item:inlistbox", "A specific address"))}});
    add(u"Port"_s, listener, i18nc("@label", "Listen port"), i18nc("@info", "A number from 1 to 65535. After the service restarts, clients must connect to the new port."), {},
        {.control = u"spin"_s, .section = u"connection"_s, .formLabel = i18nc("@label", "Port"), .min = 1, .max = 65535});
    add(u"Certificate"_s, listener, i18nc("@label", "Certificate file"), i18nc("@info", "The full path of an existing certificate file that the administrator manages. Choose “Use existing system paths” to edit both file paths."), {},
        {.control = u"path"_s, .section = u"certificate"_s, .keepEmpty = true});
    add(u"CertificateKey"_s, listener, i18nc("@label", "Private key file"), i18nc("@info", "The full path of the matching unencrypted private key, readable only by the administrator. The key itself is never shown."), {},
        {.control = u"path"_s, .section = u"certificate"_s, .keepEmpty = true});
    add(u"Quality"_s, video, i18nc("@label", "Video quality"), i18nc("@info", "From 0 to 100. Higher values look sharper and use more bandwidth. People can choose their own value in My Preferences."), {},
        {.control = u"slider"_s, .section = u"picture"_s, .formLabel = i18nc("@label", "Image quality"), .min = 0, .max = 100});
    add(u"AdaptiveQuality"_s, video, i18nc("@label", "Adapt quality to the connection"), i18nc("@info", "Lower the quality automatically when the connection cannot keep up."), boolean,
        {.section = u"picture"_s, .formLabel = i18nc("@label", "Adjust to connection")});
    add(u"SoftwareEncoding"_s, video, i18nc("@label", "Software encoding"), i18nc("@info", "The graphics hardware and the client still decide which codec is used. “Prefer hardware” falls back to software only as a last resort."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"never"_s, i18nc("@item:inlistbox", "Prefer hardware")), choice(u"prefer"_s, i18nc("@item:inlistbox", "Allow the best codec in software"))},
        {.section = u"encoding"_s, .advanced = true, .formLabel = i18nc("@label", "Encoding policy")});
    add(u"Av1Tiles"_s, video, i18nc("@label", "AV1 tiles"), i18nc("@info", "Automatic uses what the client can decode. The available encoders may limit the choice."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"1"_s, u"1"_s), choice(u"2"_s, u"2"_s), choice(u"4"_s, u"4"_s), choice(u"8"_s, u"8"_s), choice(u"16"_s, u"16"_s)},
        {.section = u"encoding"_s, .advanced = true});
    add(u"PreferAudioQuality"_s, media, i18nc("@label", "Prefer audio quality"), i18nc("@info", "When the network is busy, favor sound over video."), boolean,
        {.section = u"picture"_s, .formLabel = i18nc("@label", "When network is busy"),
         .optionText = {{u"true"_s, i18nc("@item:inlistbox", "Keep sound smooth")}, {u"false"_s, i18nc("@item:inlistbox", "Keep video sharp")}}});
    add(u"StandardClientMedia"_s, media, i18nc("@label", "Media for other RDP apps"), i18nc("@info", "Lets other remote desktop apps send sound and devices. Each app must still ask for it."), boolean,
        {.section = u"picture"_s, .formLabel = i18nc("@label", "Media for other RDP apps"),
         .optionText = {{u"true"_s, i18nc("@item:inlistbox", "Allow")}, {u"false"_s, i18nc("@item:inlistbox", "Block")}}});
    add(u"CameraLoopbackDevice"_s, media, i18nc("@label", "Camera device"), i18nc("@info", "The virtual camera that carries your camera into Console, for example /dev/video10 (a V4L2 loopback device). Enter “none” to turn camera sharing off. Console also needs permission to use the device. Virtual desktops cannot use it yet; camera delivery through PipeWire is separate."), {},
        {.section = u"devices"_s, .advanced = true, .formLabel = i18nc("@label", "Camera device"),
         .unavailable = m_scope == Scope::Virtual ? i18nc("@info", "Camera sharing is not available for Virtual desktops yet.") : QString()});
    add(u"RenderPci"_s, i18nc("@title:group", "New Virtual Desktops"), i18nc("@label", "Graphics devices for new desktops"), i18nc("@info", "The PCI addresses of the graphics devices new desktops may use, separated by commas, for example 0000:01:00.0. Leave empty to allow none. This only grants access; it does not choose the encoder. Existing desktops keep their current setting."), {},
        {.section = u"devices"_s, .advanced = true, .formLabel = i18nc("@label", "Graphics device IDs"), .keepEmpty = true});
    add(u"VaapiDriver"_s, m_scope == Scope::VirtualSession ? i18nc("@title:group", "New Virtual Desktops") : video,
        i18nc("@label", "Video acceleration driver"), i18nc("@info", "Chooses which VA-API driver is tried for hardware video encoding. It does not select NVIDIA encoding or a graphics device. For Virtual, changes apply to newly created desktops only."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"off"_s, i18nc("@item:inlistbox", "Disabled")),
         choice(u"radeonsi"_s, u"radeonsi"_s), choice(u"iHD"_s, u"iHD"_s), choice(u"i965"_s, u"i965"_s)},
        {.section = u"encoding"_s, .advanced = true});
    return result;
}
