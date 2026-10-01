#include "FarsideMigration.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSet>
#include <QStandardPaths>

#include <utility>

Q_LOGGING_CATEGORY(FARSIDE_MIGRATION, "farside.server.migration")

namespace
{
bool copyIfAbsent(const QString &source, const QString &target, bool privateMode = false)
{
    const QFileInfo old(source);
    if (!old.isFile() || old.isSymLink()) return false;
    if (QFileInfo::exists(target) || QFileInfo(target).isSymLink()) return true;
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) return false;
    if (!QFile::copy(source, target)) return false;
    if (privateMode) QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}


}

void FarsideMigration::copyUserFiles()
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString state = QStandardPaths::writableLocation(QStandardPaths::StateLocation);
    const QString oldRc = config + QStringLiteral("/krdpserverrc");
    const QString newRc = config + QStringLiteral("/farsideserverrc");
    bool copiedRc = false;
    if (QFileInfo::exists(oldRc) && !QFileInfo::exists(newRc) && !QFileInfo(oldRc).isSymLink()) {
        if (copyIfAbsent(oldRc, newRc, true)) {
            copiedRc = true;
        } else {
            qCWarning(FARSIDE_MIGRATION) << "Could not copy the old server settings";
        }
    }
    for (const QString &extension : {QStringLiteral("crt"), QStringLiteral("key")}) {
        const QString source = data + QStringLiteral("/krdpserver/krdp.") + extension;
        const QString target = data + QStringLiteral("/farside-server/server.") + extension;
        if (QFileInfo::exists(source) && !copyIfAbsent(source, target, true)) {
            qCWarning(FARSIDE_MIGRATION) << "Could not copy server certificate material";
        }
    }
    if (copiedRc) {
        // The old managed certificate paths can be present even when automatic
        // certificates are enabled. Point only those exact legacy values at
        // the copied files; preserve any custom certificate paths.
        const auto migratedConfig = KSharedConfig::openConfig(newRc, KConfig::SimpleConfig);
        KConfigGroup general(migratedConfig, QStringLiteral("General"));
        for (const auto &entry : {std::pair{QStringLiteral("Certificate"), QStringLiteral("crt")},
                                  std::pair{QStringLiteral("CertificateKey"), QStringLiteral("key")}}) {
            const QString source = data + QStringLiteral("/krdpserver/krdp.") + entry.second;
            const QString target = data + QStringLiteral("/farside-server/server.") + entry.second;
            if (QFileInfo::exists(target) && general.readEntry(entry.first) == source) general.writeEntry(entry.first, target);
        }
        migratedConfig->sync();
        QFile file(newRc);
        if (file.open(QIODevice::Append)) file.write("\n[FarsideMigration]\nMigratedFrom=krdpserverrc\n");
    }
    copyIfAbsent(state + QStringLiteral("/krdp/output-restore.json"), state + QStringLiteral("/farside/output-restore.json"), true);
    copyIfAbsent(state + QStringLiteral("/krdp-serverstaterc"), state + QStringLiteral("/farside-serverstaterc"), true);
}

void FarsideMigration::recordPendingCredentials()
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString stateDir = QStandardPaths::writableLocation(QStandardPaths::StateLocation) + QStringLiteral("/farside");
    const QString statePath = stateDir + QStringLiteral("/migration.json");
    // No atomic create-only operation is exposed by QtKeychain. Do not open the
    // wallet or copy passwords on startup; legacy reads in main keep old logins
    // usable until explicit reconciliation. Preserve all existing pending state.
    if (QFileInfo::exists(statePath) || QFileInfo(statePath).isSymLink()) return;
    if (!QDir().mkpath(stateDir)) return;
    const auto oldConfig = KSharedConfig::openConfig(config + QStringLiteral("/krdpserverrc"), KConfig::SimpleConfig);
    QSet<QString> pending;
    for (const QString &user : oldConfig->group(QStringLiteral("General")).readEntry(QStringLiteral("Users"), QStringList{}))
        if (!user.isEmpty()) pending.insert(user);
    QStringList users = pending.values();
    users.sort();
    QJsonArray pendingArray;
    for (const QString &user : users) pendingArray.append(user);
    QFile state(statePath);
    if (state.open(QIODevice::WriteOnly | QIODevice::NewOnly,
                   QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("pending"), pendingArray}}).toJson();
        if (state.write(bytes) != bytes.size())
            qCWarning(FARSIDE_MIGRATION) << "Could not record pending password migration";
    }
    qCInfo(FARSIDE_MIGRATION) << "Password migration:" << pending.size() << "pending; automatic copying disabled";
}

void FarsideMigration::migratePermission()
{
    QDBusInterface store(QStringLiteral("org.freedesktop.impl.portal.PermissionStore"),
                         QStringLiteral("/org/freedesktop/impl/portal/PermissionStore"),
                         QStringLiteral("org.freedesktop.impl.portal.PermissionStore"), QDBusConnection::sessionBus());
    const QDBusReply<QStringList> oldPermission = store.call(QStringLiteral("GetPermission"), QStringLiteral("kde-authorized"),
                                                               QStringLiteral("remote-desktop"), QStringLiteral("org.kde.krdpserver"));
    if (oldPermission.isValid() && oldPermission.value().contains(QStringLiteral("yes"))) {
        const QDBusReply<QStringList> current = store.call(QStringLiteral("GetPermission"), QStringLiteral("kde-authorized"),
                                                             QStringLiteral("remote-desktop"), QStringLiteral("io.github.westers.farside.server"));
        if (!current.isValid() || !current.value().contains(QStringLiteral("yes"))) {
            const QDBusReply<void> answer = store.call(QStringLiteral("SetPermission"), QStringLiteral("kde-authorized"), true,
                                                       QStringLiteral("remote-desktop"), QStringLiteral("io.github.westers.farside.server"),
                                                       QStringList{QStringLiteral("yes")});
            if (!answer.isValid()) qCWarning(FARSIDE_MIGRATION) << "Could not copy the remote-desktop portal permission";
        }
    }
}
