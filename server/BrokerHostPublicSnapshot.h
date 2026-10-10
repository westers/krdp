// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "BrokerHostSettings.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// World-readable public-metadata snapshot of a host settings scope, written by
// a root process (the settings helper, at broker start and after each save) so
// the settings panel can open populated without any administrator prompt.
//
// Allow-list only: revision, non-path values/defaults/effective keys, TLS
// state/fingerprint/validity, camera bridge state and GPU render device ids.
// Never account names, aliases, verifiers, certificate/key paths or anything
// read from /etc/farside beyond these projected fields.
namespace KRdp::BrokerHostPublicSnapshot {
constexpr qsizetype MaximumBytes = 262144;
QString defaultDirectory(); // /var/lib/farside-public
QString snapshotFileName(BrokerHostSettings::Scope scope);
// Keys whose values are filesystem paths to TLS material: never published.
bool isHiddenKey(const QString &key);
QJsonObject sanitize(BrokerHostSettings::Scope scope, const QJsonObject &full);
// Atomic (temp file + rename), mode 0644, owned by the writing user (root in
// production). Creates the directory 0755 when missing.
bool write(const QString &directory, BrokerHostSettings::Scope scope, const QJsonObject &full, QString *error = nullptr);
struct ReadResult {
    QJsonObject value;
    QString error;
};
// Unprivileged read. Requires a regular, single-link file owned by root (or by
// the caller when allowCurrentUser, for tests and a private override) that no
// other user can write, inside a directory with the same ownership rules.
ReadResult read(const QString &directory, BrokerHostSettings::Scope scope, bool allowCurrentUser = false);

// OPT-062 S3: `videoEncoders` (Console and Virtual only): what the broker's last encoder probe found, so the
// settings page can say when a codec has no software encoder. Each entry is {codec, backend, hw, device?, name?}
// with bounded strings; anything else is dropped. At most MaximumEncoders entries.
constexpr int MaximumEncoders = 12;
QJsonArray sanitizeVideoEncoders(const QJsonArray &encoders);
// The broker (root) records its probe: reads the current public snapshot, replaces `videoEncoders` and writes it
// back atomically. False (nothing written) when there is no trusted snapshot yet; the next start tries again.
bool updateVideoEncoders(const QString &directory, BrokerHostSettings::Scope scope, const QJsonArray &encoders, bool allowCurrentUser = false, QString *error = nullptr);
}
