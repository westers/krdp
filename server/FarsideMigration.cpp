#include "FarsideMigration.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <qt6keychain/keychain.h>

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

struct Secret {
    bool found = false;
    QString value;
};
Secret readSecret(const QString &service, const QString &key)
{
    auto *job = new QKeychain::ReadPasswordJob(service);
    job->setAutoDelete(false);
    job->setKey(key);
    QEventLoop loop;
    bool done = false;
    QObject::connect(job, &QKeychain::Job::finished, &loop, [&] { done = true; loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    job->start();
    if (!done) loop.exec();
    Secret result;
    if (done && job->error() == QKeychain::NoError) {
        result.found = true;
        result.value = job->textData();
    }
    job->deleteLater();
    return result;
}
bool writeSecret(const QString &key, const QString &value)
{
    auto *job = new QKeychain::WritePasswordJob(QStringLiteral("Farside Server"));
    job->setAutoDelete(false);
    job->setKey(key);
    job->setTextData(value);
    QEventLoop loop;
    bool done = false;
    QObject::connect(job, &QKeychain::Job::finished, &loop, [&] { done = true; loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    job->start();
    if (!done) loop.exec();
    const bool ok = done && job->error() == QKeychain::NoError;
    job->deleteLater();
    return ok;
}
QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
}

void FarsideMigration::copyUserFiles()
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString state = QStandardPaths::writableLocation(QStandardPaths::StateLocation);
    const QString oldRc = config + QStringLiteral("/krdpserverrc");
    const QString newRc = config + QStringLiteral("/farsideserverrc");
    if (QFileInfo::exists(oldRc) && !QFileInfo::exists(newRc) && !QFileInfo(oldRc).isSymLink()) {
        if (copyIfAbsent(oldRc, newRc, true)) {
            QFile file(newRc);
            if (file.open(QIODevice::Append)) file.write("\n[FarsideMigration]\nMigratedFrom=krdpserverrc\n");
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
    copyIfAbsent(state + QStringLiteral("/krdp/output-restore.json"), state + QStringLiteral("/farside/output-restore.json"), true);
    copyIfAbsent(state + QStringLiteral("/krdp-serverstaterc"), state + QStringLiteral("/farside-serverstaterc"), true);
}

void FarsideMigration::migrateCredentialsAndPermission()
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString stateDir = QStandardPaths::writableLocation(QStandardPaths::StateLocation) + QStringLiteral("/farside");
    QDir().mkpath(stateDir);
    const QString statePath = stateDir + QStringLiteral("/migration.json");
    const QJsonObject oldState = readJson(statePath);
    QSet<QString> pending;
    for (const QJsonValue &entry : oldState.value(QStringLiteral("pending")).toArray()) pending.insert(entry.toString());
    if (!oldState.contains(QStringLiteral("pending"))) {
        KSharedConfig::Ptr oldConfig = KSharedConfig::openConfig(config + QStringLiteral("/krdpserverrc"), KConfig::SimpleConfig);
        for (const QString &user : oldConfig->group(QStringLiteral("General")).readEntry(QStringLiteral("Users"), QStringList{})) pending.insert(user);
    }
    pending.remove(QString());
    int copied = 0;
    const QStringList users = pending.values();
    for (const QString &user : users) {
        const Secret old = readSecret(QStringLiteral("KRDP"), user);
        if (!old.found || !writeSecret(user, old.value)) continue;
        const Secret fresh = readSecret(QStringLiteral("Farside Server"), user);
        if (fresh.found && fresh.value == old.value) {
            pending.remove(user);
            ++copied;
        }
    }
    QJsonArray pendingArray;
    for (const QString &user : pending) pendingArray.append(user);
    QSaveFile state(statePath);
    if (state.open(QIODevice::WriteOnly)) {
        state.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        state.write(QJsonDocument(QJsonObject{{QStringLiteral("pending"), pendingArray}}).toJson());
        state.commit();
    }
    qCInfo(FARSIDE_MIGRATION) << "Wallet migration:" << copied << "copied," << pending.size() << "pending";

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
