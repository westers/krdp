// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QStringList>
#include <QVariantMap>
#include <optional>

namespace KRdp::BrokerHostSettings {
// Pure typed documents, not authorization or a filesystem writer. Production
// administration must inspect/lock fixed root paths and verify TLS/devices.
enum class Scope { Console, Virtual, VirtualSession };
constexpr qsizetype MaximumBytes = 65536;
constexpr qsizetype MaximumValue = 4096;
QString fileName(Scope scope);
QStringList keys(Scope scope);
QVariantMap defaults(Scope scope);
QString environmentName(Scope scope, const QString &key);
std::optional<QString> normalize(Scope scope, const QString &key, const QString &value);
struct Snapshot {
    QVariantMap overrides;
    // Shipped unit defaults + this document, not installed/drop-in readback.
    QVariantMap effective;
    QString error;
};
// Only whitelisted fields reach public snapshots. Unknown administrator entries
// remain private, including their complete quoting and continuation syntax.
Snapshot parse(Scope scope, const QByteArray &contents);
struct EditResult { QByteArray contents; QString error; };
// Desired is the complete override map; absence inherits the shipped unit
// default. Caller cannot provide arbitrary environment names or raw fragments.
EditResult edit(Scope scope, const QByteArray &original, const QVariantMap &desired);
}
