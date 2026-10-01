// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <functional>
#include <optional>

namespace KRdp { class Server; }
namespace KRdp::BrokerAuthentication
{
enum class Desktop { Console, Virtual };
enum class PamMode { Any, AllowList, Disabled };
constexpr qsizetype MaximumBytes = 65536;
constexpr int PasswordIterations = 600000;

/** Administratively granted alias; owner is never resolved from the alias.
 * A migrated legacy alias remains a grant to the original daemon owner's UID. */
struct Credential {
    quint32 ownerUid = 0;
    QByteArray salt;
    QByteArray digest;
};
struct Route {
    PamMode pam = PamMode::Disabled;
    QSet<quint32> pamAccounts;
    QHash<QString, Credential> credentials;
    bool allowsPam(quint32 uid) const;
    std::optional<quint32> authenticate(const QString &alias, const QString &password) const;
};
struct Policy {
    Route console, virtualDesktop;
    const Route &route(Desktop desktop) const { return desktop == Desktop::Console ? console : virtualDesktop; }
};
using AccountResolver = std::function<std::optional<quint32>(const QString &)>;
struct Result {
    std::optional<Policy> policy;
    QString error; // Structural reason only; never verifier, password or file contents.
    // Safe file reader's exact bytes, for revision-bound privileged editing.
    // Never expose this document to the unprivileged settings UI.
    QByteArray document = {};
};

Result parse(const QByteArray &contents, const AccountResolver &resolve);
std::optional<quint32> resolveAccount(const QString &account);
/** Optional absent default file preserves the deployed brokers' PAM policy.
 * An explicit path, malformed file, unsafe mode/owner/type, or failed read
 * fails startup. Loaded once before listening; edits require broker restart. */
Result readFile(const QString &path, bool required);
bool apply(Server &server, const Route &route);
std::optional<Credential> makeCredential(quint32 ownerUid, const QString &password);
}
