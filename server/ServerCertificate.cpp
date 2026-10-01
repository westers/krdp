// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ServerCertificate.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>

#include <memory>

#include <sys/stat.h>
#include <unistd.h>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

using namespace Qt::StringLiterals;

namespace KRdp::ServerCertificate
{
namespace
{
constexpr qint64 kMaxPemBytes = 64 * 1024;

template<typename T, void (*Free)(T *)>
struct Deleter {
    void operator()(T *p) const
    {
        Free(p);
    }
};
using BioPtr = std::unique_ptr<BIO, Deleter<BIO, BIO_free_all>>;
using X509Ptr = std::unique_ptr<X509, Deleter<X509, X509_free>>;
using PKeyPtr = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using ExtPtr = std::unique_ptr<X509_EXTENSION, Deleter<X509_EXTENSION, X509_EXTENSION_free>>;

int noPassphrase(char *, int, int, void *)
{
    return -1;
}

QByteArray readBounded(const QString &path, bool *ok)
{
    *ok = false;
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > kMaxPemBytes) {
        return {};
    }
    const auto data = file.read(kMaxPemBytes + 1);
    *ok = data.size() <= kMaxPemBytes;
    return data;
}

QDateTime fromAsn1(const ASN1_TIME *time)
{
    struct tm tm = {};
    if (!time || ASN1_TIME_to_tm(time, &tm) != 1) {
        return {};
    }
    return QDateTime(QDate(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday), QTime(tm.tm_hour, tm.tm_min, tm.tm_sec), QTimeZone::UTC);
}

QString algorithmName(EVP_PKEY *key)
{
    const int bits = EVP_PKEY_get_bits(key);
    switch (EVP_PKEY_get_base_id(key)) {
    case EVP_PKEY_EC: {
        char group[64] = {};
        size_t length = 0;
        if (EVP_PKEY_get_utf8_string_param(key, "group", group, sizeof(group), &length) == 1) {
            const auto name = QString::fromLatin1(group, qsizetype(length));
            if (name == "prime256v1"_L1) {
                return u"ECDSA P-256"_s;
            }
            return u"ECDSA %1"_s.arg(name);
        }
        return u"ECDSA %1"_s.arg(bits);
    }
    case EVP_PKEY_RSA:
        return u"RSA %1"_s.arg(bits);
    default:
        return u"%1 %2"_s.arg(QString::fromLatin1(OBJ_nid2sn(EVP_PKEY_get_base_id(key)))).arg(bits);
    }
}

bool addExtension(X509 *cert, int nid, const char *value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
    ExtPtr extension(X509V3_EXT_conf_nid(nullptr, &ctx, nid, value));
    return extension && X509_add_ext(cert, extension.get(), -1) == 1;
}

QByteArray pemOf(bool (*write)(BIO *, void *), void *object)
{
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio || !write(bio.get(), object)) {
        return {};
    }
    char *data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    return QByteArray(data, qsizetype(length));
}

bool writeAtomically(const QString &path, const QByteArray &data, QFileDevice::Permissions permissions, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = u"cannot write %1: %2"_s.arg(path, file.errorString());
        return false;
    }
    // Before any byte of a private key reaches the disk.
    file.setPermissions(permissions);
    if (file.write(data) != data.size() || !file.commit()) {
        *error = u"cannot write %1: %2"_s.arg(path, file.errorString());
        return false;
    }
    return true;
}
}

Paths defaultPaths()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/farside-server"_s;
    return {dir + u"/server.crt"_s, dir + u"/server.key"_s};
}

Info inspect(const Paths &paths)
{
    bool ok = false;
    const auto certPem = readBounded(paths.certificate, &ok);
    const auto keyPem = readBounded(paths.key, &ok);
    auto info = inspectPem(certPem, keyPem);
    info.certificateExists = !paths.certificate.isEmpty() && QFileInfo::exists(paths.certificate);
    info.keyExists = !paths.key.isEmpty() && QFileInfo::exists(paths.key);
    return info;
}

Info inspectPem(const QByteArray &certPem, const QByteArray &keyPem)
{
    Info info;
    info.certificateExists = !certPem.isEmpty();
    info.keyExists = !keyPem.isEmpty();
    X509Ptr cert;
    if (!certPem.isEmpty() && certPem.size() <= kMaxPemBytes) {
        BioPtr bio(BIO_new_mem_buf(certPem.constData(), int(certPem.size())));
        cert.reset(bio ? PEM_read_bio_X509(bio.get(), nullptr, noPassphrase, nullptr) : nullptr);
    }
    if (cert) {
        info.certificateReadable = true;
        info.notBefore = fromAsn1(X509_get0_notBefore(cert.get()));
        info.notAfter = fromAsn1(X509_get0_notAfter(cert.get()));
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (X509_digest(cert.get(), EVP_sha256(), digest, &length) == 1) {
            info.sha256Fingerprint = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(digest), int(length)).toHex(':').toUpper());
        }
        if (EVP_PKEY *publicKey = X509_get0_pubkey(cert.get())) {
            info.algorithm = algorithmName(publicKey);
        }
    }

    PKeyPtr key;
    if (!keyPem.isEmpty() && keyPem.size() <= kMaxPemBytes) {
        BioPtr bio(BIO_new_mem_buf(keyPem.constData(), int(keyPem.size())));
        key.reset(bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, noPassphrase, nullptr) : nullptr);
    }
    info.keyReadable = bool(key);
    info.keyMatches = cert && key && X509_check_private_key(cert.get(), key.get()) == 1;
    return info;
}

Decision decide(const Info &info, const QDateTime &now)
{
    if (!info.certificateExists || !info.keyExists) {
        return Decision::GenerateMissing;
    }
    if (!info.usable() || !info.notAfter.isValid()) {
        return Decision::GenerateUnusable;
    }
    if (info.notAfter <= now) {
        return Decision::GenerateExpired;
    }
    if (info.notAfter <= now.addDays(kRenewBeforeDays)) {
        return Decision::GenerateExpiringSoon;
    }
    return Decision::UseExisting;
}

QString describe(Decision decision)
{
    switch (decision) {
    case Decision::UseExisting:
        return u"valid"_s;
    case Decision::GenerateMissing:
        return u"missing"_s;
    case Decision::GenerateUnusable:
        return u"unreadable or not matching its key"_s;
    case Decision::GenerateExpired:
        return u"expired"_s;
    case Decision::GenerateExpiringSoon:
        return u"expires within %1 days"_s.arg(kRenewBeforeDays);
    }
    return {};
}

bool generate(const Paths &paths, const QString &commonName, const QDateTime &now, int validityDays, QString *error)
{
    QString scratch;
    if (!error) {
        error = &scratch;
    }
    if (paths.certificate.isEmpty() || paths.key.isEmpty()) {
        *error = u"no certificate path"_s;
        return false;
    }
    for (const auto &path : {paths.certificate, paths.key}) {
        const QString dir = QFileInfo(path).absolutePath();
        if (!QDir().mkpath(dir)) {
            *error = u"cannot create %1"_s.arg(dir);
            return false;
        }
    }

    PKeyPtr key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
    X509Ptr cert(X509_new());
    if (!key || !cert) {
        *error = u"OpenSSL could not create an ECDSA key"_s;
        return false;
    }

    unsigned char serialBytes[16];
    if (RAND_bytes(serialBytes, sizeof(serialBytes)) != 1) {
        *error = u"OpenSSL could not create a serial number"_s;
        return false;
    }
    serialBytes[0] &= 0x7f; // positive
    std::unique_ptr<BIGNUM, Deleter<BIGNUM, BN_free>> serialNumber(BN_bin2bn(serialBytes, sizeof(serialBytes), nullptr));
    const qint64 notBefore = now.toSecsSinceEpoch() - 3600;
    const qint64 notAfter = now.addDays(validityDays).toSecsSinceEpoch();
    const QByteArray cn = (commonName.isEmpty() ? u"Farside Server"_s : commonName).left(64).toUtf8();

    X509_NAME *name = X509_get_subject_name(cert.get());
    const bool built = serialNumber && X509_set_version(cert.get(), X509_VERSION_3) == 1
        && BN_to_ASN1_INTEGER(serialNumber.get(), X509_get_serialNumber(cert.get())) != nullptr
        && ASN1_TIME_set(X509_getm_notBefore(cert.get()), time_t(notBefore)) != nullptr
        && ASN1_TIME_set(X509_getm_notAfter(cert.get()), time_t(notAfter)) != nullptr
        && X509_NAME_add_entry_by_txt(name, "O", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>("KRDP"), -1, -1, 0) == 1
        && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(cn.constData()), int(cn.size()), -1, 0) == 1
        && X509_set_issuer_name(cert.get(), name) == 1 && X509_set_pubkey(cert.get(), key.get()) == 1
        && addExtension(cert.get(), NID_basic_constraints, "critical,CA:FALSE")
        && addExtension(cert.get(), NID_key_usage, "critical,digitalSignature,keyAgreement")
        && addExtension(cert.get(), NID_ext_key_usage, "serverAuth")
        && addExtension(cert.get(), NID_subject_key_identifier, "hash")
        && X509_sign(cert.get(), key.get(), EVP_sha256()) > 0;
    if (!built) {
        *error = u"OpenSSL could not build the certificate"_s;
        return false;
    }

    const auto keyPem = pemOf(
        [](BIO *bio, void *object) {
            return PEM_write_bio_PrivateKey(bio, static_cast<EVP_PKEY *>(object), nullptr, nullptr, 0, nullptr, nullptr) == 1;
        },
        key.get());
    const auto certPem = pemOf(
        [](BIO *bio, void *object) {
            return PEM_write_bio_X509(bio, static_cast<X509 *>(object)) == 1;
        },
        cert.get());
    if (keyPem.isEmpty() || certPem.isEmpty()) {
        *error = u"OpenSSL could not encode the certificate"_s;
        return false;
    }
    // Key first: a new certificate next to an old key would be a mismatched
    // pair, which decide() treats as unusable and replaces on the next run.
    return writeAtomically(paths.key, keyPem, QFileDevice::ReadOwner | QFileDevice::WriteOwner, error)
        && writeAtomically(paths.certificate, certPem, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther, error);
}

EnsureResult ensure(const Paths &paths, const QString &commonName, const QDateTime &now)
{
    EnsureResult result;
    result.info = inspect(paths);
    result.decision = decide(result.info, now);
    if (!needsGeneration(result.decision)) {
        result.ok = true;
        return result;
    }
    if (!generate(paths, commonName, now, kValidityDays, &result.error)) {
        return result;
    }
    result.generated = true;
    result.info = inspect(paths);
    result.ok = result.info.usable();
    if (!result.ok) {
        result.error = u"the generated certificate could not be read back"_s;
    }
    return result;
}

SystemResult ensureSystem(const Paths &paths, const QString &commonName, const QDateTime &now, uint owner)
{
    SystemResult result;
    if (paths.certificate.isEmpty() || paths.key.isEmpty() || !QDir::isAbsolutePath(paths.certificate) || !QDir::isAbsolutePath(paths.key)) {
        result.error = u"the certificate and key paths must be absolute"_s;
        return result;
    }
    bool symlinked = false;
    for (const auto &path : {paths.certificate, paths.key}) {
        if (QFileInfo(path).isSymLink()) {
            symlinked = true;
            result.notes << u"%1 is a symlink: managed by the administrator, never replaced"_s.arg(path);
        }
    }
    if (symlinked) {
        result.administratorManaged = true;
        result.info = inspect(paths);
        result.decision = decide(result.info, now);
        result.ok = result.info.usable();
        if (!result.ok) {
            result.error = u"the symlinked certificate or key is %1"_s.arg(describe(result.decision));
        } else if (needsGeneration(result.decision)) {
            result.notes << u"the symlinked certificate %1; replace it"_s.arg(describe(result.decision));
        }
        return result;
    }

    // AUD-FIX8: the directory holding the key decides who can replace it. It must belong to
    // root (or the service's euid) and be writable by nobody else. Sol's /opt/krdp-console/cert
    // was the greeter user's (sddm), from before the host ran as root.
    const QStringList ours{QFileInfo(paths.certificate).fileName(), QFileInfo(paths.key).fileName()};
    for (const auto &path : {paths.certificate, paths.key}) {
        const QString dir = QFileInfo(path).absolutePath();
        const QByteArray dirName = QFile::encodeName(dir);
        struct stat st{};
        if (::lstat(dirName.constData(), &st) != 0) {
            if (!QDir().mkpath(dir) || ::chmod(dirName.constData(), 0755) != 0) {
                result.error = u"cannot create %1 (0755)"_s.arg(dir);
                return result;
            }
            if (::lstat(dirName.constData(), &st) == 0 && st.st_uid != owner && ::chown(dirName.constData(), owner, gid_t(-1)) != 0) {
                result.error = u"cannot make %1 owned by uid %2"_s.arg(dir).arg(owner);
                return result;
            }
            result.notes << u"created %1 (0755)"_s.arg(dir);
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            result.error = u"%1 is not a directory"_s.arg(dir);
            return result;
        }
        const mode_t mode = st.st_mode & 07777;
        const bool foreignOwner = st.st_uid != owner && st.st_uid != 0;
        if (!foreignOwner && !(mode & (S_IWGRP | S_IWOTH))) {
            continue; // safe
        }
        // Only a directory that holds nothing but this certificate and key (and their atomic-write
        // temporaries) is ours to repair; a shared one (a sticky /tmp, a home directory) is refused.
        bool dedicated = !(mode & S_ISVTX);
        const auto entries = QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const auto &entry : entries) {
            if (!std::any_of(ours.begin(), ours.end(), [&entry](const QString &name) { return entry == name || entry.startsWith(name + u'.'); })) {
                dedicated = false;
            }
        }
        const QString what = u"%1 (uid %2, mode %3)"_s.arg(dir).arg(st.st_uid).arg(QString::number(mode, 8));
        if (!dedicated) {
            result.error = u"%1 can be written by others than root, who could replace the key, and it holds other files: make it "
                           u"root-owned and not group/other-writable (chown root:root, chmod 0755), or move the certificate"_s.arg(what);
            return result;
        }
        if ((foreignOwner && ::chown(dirName.constData(), owner, 0) != 0) || ::chmod(dirName.constData(), 0755) != 0) {
            result.error = u"%1 can be written by others than root, who could replace the key, and could not be repaired"_s.arg(what);
            return result;
        }
        result.notes << u"%1 could be written by others than root, who could replace the key; now uid %2, mode 755"_s.arg(what).arg(owner);
    }

    // Existing files: owner and mode. Never touches what they contain.
    const auto repair = [&](const QString &path, mode_t forbidden, mode_t wanted) -> bool {
        const QByteArray name = QFile::encodeName(path);
        struct stat st{};
        if (::lstat(name.constData(), &st) != 0) {
            return true; // missing: generate() writes it with the right mode
        }
        if (!S_ISREG(st.st_mode)) {
            result.error = u"%1 is not a regular file"_s.arg(path);
            return false;
        }
        if (st.st_uid != owner) {
            if (::chown(name.constData(), owner, gid_t(-1)) == 0) {
                result.notes << u"%1 was owned by uid %2; now by uid %3"_s.arg(path).arg(st.st_uid).arg(owner);
            } else {
                result.notes << u"%1 is owned by uid %2, not %3, and could not be changed"_s.arg(path).arg(st.st_uid).arg(owner);
            }
        }
        const mode_t mode = st.st_mode & 07777;
        if (mode & forbidden) {
            const mode_t fixed = wanted ? wanted : (mode & ~forbidden);
            if (::chmod(name.constData(), fixed) == 0) {
                result.notes << u"%1 had mode %2; now %3"_s.arg(path, QString::number(mode, 8), QString::number(fixed, 8));
            } else {
                result.error = u"%1 has mode %2 and cannot be made private"_s.arg(path, QString::number(mode, 8));
                return false;
            }
        }
        return true;
    };
    if (!repair(paths.key, 077 | S_ISUID | S_ISGID | S_ISVTX | S_IXUSR, 0600) || !repair(paths.certificate, S_IWGRP | S_IWOTH | S_ISUID | S_ISGID | S_ISVTX, 0)) {
        return result;
    }

    const auto ensured = ensure(paths, commonName, now);
    static_cast<EnsureResult &>(result) = ensured;
    return result;
}
}
