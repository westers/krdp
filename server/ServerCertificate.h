// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QDateTime>
#include <QByteArray>
#include <QString>
#include <QStringList>

// AUD-K3: the per-user server owns its TLS certificate. With
// AutogenerateCertificates=true it keeps a self-signed ECDSA P-256 certificate
// under the data directory valid: it creates one when it is missing, cannot be
// parsed, does not match its key, has expired or expires within
// kRenewBeforeDays, and logs the SHA-256 fingerprint so a client's trust prompt
// can be checked against it. The KCM uses inspect() to show the same
// fingerprint and the expiry date. All decisions take "now" as a parameter so
// they can be tested with an injected clock.
namespace KRdp::ServerCertificate
{
constexpr int kValidityDays = 3650;
constexpr int kRenewBeforeDays = 30;

struct Paths {
    QString certificate;
    QString key;
};

// $XDG_DATA_HOME/krdpserver/krdp.{crt,key}: where the server keeps the
// certificate it manages when AutogenerateCertificates is on.
Paths defaultPaths();

struct Info {
    bool certificateExists = false;
    bool keyExists = false;
    bool certificateReadable = false;
    bool keyReadable = false;
    bool keyMatches = false;
    QDateTime notBefore;
    QDateTime notAfter;
    QString sha256Fingerprint; // "AB:CD:..." upper-case hex
    QString algorithm; // "ECDSA P-256", "RSA 2048", ...

    bool usable() const
    {
        return certificateReadable && keyReadable && keyMatches;
    }
};

// Reads the certificate and key without prompting for a passphrase. Never
// logs key material.
Info inspect(const Paths &paths);
// Bounded in-memory inspection for privileged FD reads and TLS imports. Never
// prompts for a passphrase; only public certificate metadata is returned.
Info inspectPem(const QByteArray &certificate, const QByteArray &key);

enum class Decision {
    UseExisting,
    GenerateMissing, // certificate or key file does not exist
    GenerateUnusable, // unreadable, unparseable or mismatched pair
    GenerateExpired,
    GenerateExpiringSoon,
};

Decision decide(const Info &info, const QDateTime &now);
inline bool needsGeneration(Decision decision)
{
    return decision != Decision::UseExisting;
}
QString describe(Decision decision);

// Writes a new key (mode 0600) and self-signed certificate valid from
// now - 1 h to now + validityDays, replacing both files atomically.
bool generate(const Paths &paths, const QString &commonName, const QDateTime &now, int validityDays, QString *error);

struct EnsureResult {
    bool ok = false;
    bool generated = false;
    Decision decision = Decision::UseExisting;
    Info info;
    QString error;
};

// inspect() + decide() + generate() when needed + inspect() again.
EnsureResult ensure(const Paths &paths, const QString &commonName, const QDateTime &now);

/**
 * AUD-FIX7: the certificate of a root broker (the console host and the virtual-desktop host),
 * at the paths its /etc/krdp/ .conf file names. ensure() - created when missing or unusable, renewed
 * when expired or within kRenewBeforeDays, a valid one kept - plus the file hygiene a root
 * service needs:
 * - both paths must be absolute;
 * - a missing parent directory is created 0755 (not with the service's umask, so the other
 *   /etc/krdp files stay readable) and owned by \a owner;
 * - AUD-FIX8: an existing parent directory owned by another user, or group/other-writable, could
 *   let that user replace the key. When it holds only this certificate and key it is repaired
 *   (owner \a owner, group root, mode 0755) and noted; a shared one is an error (no listening);
 * - an existing key is made owned by \a owner (the service's euid) and mode 0600, an existing
 *   certificate owned by \a owner and not group/other-writable; each repair is noted;
 * - a symlinked certificate or key is the administrator's: it is checked and noted but never
 *   replaced or changed (an unusable one is an error, an expiring one only a note).
 * New files are written by generate(): key 0600, certificate 0644, owned by the euid.
 */
struct SystemResult : EnsureResult {
    QStringList notes; ///< repairs and warnings, for the log
    bool administratorManaged = false; ///< a symlink: checked, never replaced
};
SystemResult ensureSystem(const Paths &paths, const QString &commonName, const QDateTime &now, uint owner);
}
