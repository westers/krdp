// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QDateTime>
#include <QDebug>
#include <QSysInfo>

#include <unistd.h>

#include "ServerCertificate.h"

namespace KRdp
{
/**
 * AUD-FIX7: keep a root broker's certificate valid (ServerCertificate::ensureSystem(), CN = this
 * host's name) and log what it did and the SHA-256 fingerprint a client's trust prompt shows.
 * The hosts call it at startup (false: do not listen) and every 12 hours; connections read the
 * files per connection, so a renewal needs no restart.
 */
inline bool ensureHostCertificate(const ServerCertificate::Paths &paths, const char *service)
{
    using namespace ServerCertificate;
    const auto result = ensureSystem(paths, QSysInfo::machineHostName(), QDateTime::currentDateTimeUtc(), ::geteuid());
    for (const auto &note : result.notes) {
        qWarning().noquote() << service << "TLS certificate:" << note;
    }
    if (!result.ok) {
        qCritical().noquote() << service << "has no usable TLS certificate at" << paths.certificate << "and" << paths.key << "("
                              << describe(result.decision) << "):" << result.error;
        return false;
    }
    if (result.generated) {
        qInfo().noquote() << service << "generated a new self-signed TLS certificate (the old one:" << describe(result.decision) << "):"
                          << result.info.algorithm << "valid until" << result.info.notAfter.toString(Qt::ISODate) << "SHA-256"
                          << result.info.sha256Fingerprint << "at" << paths.certificate;
    } else {
        qInfo().noquote() << service << "TLS certificate" << paths.certificate << result.info.algorithm << "valid until"
                          << result.info.notAfter.toString(Qt::ISODate) << "SHA-256" << result.info.sha256Fingerprint
                          << (result.administratorManaged ? "(symlink, managed by the administrator)" : "");
    }
    return true;
}
}
