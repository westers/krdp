#include "FarsideMigration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class FarsideMigrationTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init()
    {
        // All locations are inside main's private QTemporaryDir; no session bus.
        for (const auto location : {QStandardPaths::ConfigLocation, QStandardPaths::GenericDataLocation,
                                    QStandardPaths::StateLocation}) {
            const QString root = QStandardPaths::writableLocation(location);
            QVERIFY(QDir(root).removeRecursively());
            QVERIFY(QDir().mkpath(root));
        }
    }

    void copiesUserFilesWithoutChangingOldData()
    {
        const QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        const QString state = QStandardPaths::writableLocation(QStandardPaths::StateLocation);
        QVERIFY(QDir().mkpath(config));
        QVERIFY(QDir().mkpath(data + QStringLiteral("/krdpserver")));
        QVERIFY(QDir().mkpath(state + QStringLiteral("/krdp")));
        auto write = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return false;
            return file.write(bytes) == bytes.size();
        };
        const QByteArray oldRc = QStringLiteral("[General]\nMonitorMode=multi\nUsers=steve\nCertificate=%1/krdpserver/krdp.crt\n"
                                                 "CertificateKey=%1/krdpserver/krdp.key\n")
                                     .arg(data)
                                     .toUtf8();
        const QByteArray oldCert("test-certificate-unchanged");
        QVERIFY(write(config + QStringLiteral("/krdpserverrc"), oldRc));
        QVERIFY(write(data + QStringLiteral("/krdpserver/krdp.crt"), oldCert));
        QVERIFY(write(data + QStringLiteral("/krdpserver/krdp.key"), QByteArray("test-key-unchanged")));
        QVERIFY(write(state + QStringLiteral("/krdp/output-restore.json"), QByteArray("{}")));

        FarsideMigration::copyUserFiles();
        QFile settings(config + QStringLiteral("/farsideserverrc"));
        QVERIFY(settings.open(QIODevice::ReadOnly));
        QVERIFY(settings.readAll().contains("MigratedFrom=krdpserverrc"));
        settings.close();
        const auto migratedConfig = KSharedConfig::openConfig(settings.fileName(), KConfig::SimpleConfig);
        KConfigGroup general(migratedConfig, QStringLiteral("General"));
        QCOMPARE(general.readEntry("Certificate"), data + QStringLiteral("/farside-server/server.crt"));
        QCOMPARE(general.readEntry("CertificateKey"), data + QStringLiteral("/farside-server/server.key"));
        QFile cert(data + QStringLiteral("/farside-server/server.crt"));
        QVERIFY(cert.open(QIODevice::ReadOnly));
        QCOMPARE(cert.readAll(), oldCert);
        QCOMPARE(QFileInfo(cert).permissions() & QFileDevice::WriteGroup, QFileDevice::Permissions());
        cert.close();
        QVERIFY(QFileInfo::exists(state + QStringLiteral("/farside/output-restore.json")));
        QVERIFY(write(config + QStringLiteral("/farsideserverrc"), QByteArray("new-choice")));
        FarsideMigration::copyUserFiles();
        QVERIFY(settings.open(QIODevice::ReadOnly));
        QCOMPARE(settings.readAll(), QByteArray("new-choice"));
        QFile original(config + QStringLiteral("/krdpserverrc"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), oldRc);
    }

    void recordsCredentialsAsPendingWithoutWalletBackend()
    {
        const QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        QFile original(config + QStringLiteral("/krdpserverrc"));
        const QByteArray bytes("[General]\nUsers=old-user,other-user,old-user\n");
        QVERIFY(original.open(QIODevice::WriteOnly));
        QCOMPARE(original.write(bytes), bytes.size());
        original.close();
        FarsideMigration::recordPendingCredentials();
        const QString statePath = QStandardPaths::writableLocation(QStandardPaths::StateLocation)
            + QStringLiteral("/farside/migration.json");
        QFile state(statePath);
        QVERIFY(state.open(QIODevice::ReadOnly));
        const QByteArray stateBytes = state.readAll();
        QCOMPARE(QJsonDocument::fromJson(stateBytes).object().value(QStringLiteral("pending")).toArray(),
                 (QJsonArray{QStringLiteral("old-user"), QStringLiteral("other-user")}));
        QCOMPARE(QFileInfo(state).permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                                                  | QFileDevice::ReadOther | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
        state.close();
        FarsideMigration::recordPendingCredentials();
        QVERIFY(state.open(QIODevice::ReadOnly));
        QCOMPARE(state.readAll(), stateBytes);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), bytes);
    }

    void preservesExistingMigrationState_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("pending-and-unknown-fields") << QByteArray("{\"pending\":[\"old-user\"],\"future\":true}\n");
        QTest::newRow("already-reconciled") << QByteArray("{\"pending\":[]}\n");
        QTest::newRow("uncertain-interrupted-state") << QByteArray("{\"pending\":");
    }

    void preservesExistingMigrationState()
    {
        QFETCH(QByteArray, bytes);
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::StateLocation) + QStringLiteral("/farside");
        QVERIFY(QDir().mkpath(dir));
        QFile state(dir + QStringLiteral("/migration.json"));
        QVERIFY(state.open(QIODevice::WriteOnly));
        QCOMPARE(state.write(bytes), bytes.size());
        state.close();
        const auto permissions = QFileInfo(state).permissions();
        FarsideMigration::recordPendingCredentials();
        FarsideMigration::recordPendingCredentials();
        QVERIFY(state.open(QIODevice::ReadOnly));
        QCOMPARE(state.readAll(), bytes);
        QCOMPARE(QFileInfo(state).permissions(), permissions);
    }

    void doesNotFollowPendingStateSymlink()
    {
        const QString root = QStandardPaths::writableLocation(QStandardPaths::StateLocation);
        const QString dir = root + QStringLiteral("/farside");
        QVERIFY(QDir().mkpath(dir));
        const QString missing = root + QStringLiteral("/absent-state-target");
        QVERIFY(QFile::link(missing, dir + QStringLiteral("/migration.json")));
        FarsideMigration::recordPendingCredentials();
        QVERIFY(!QFileInfo::exists(missing));
        QVERIFY(QFileInfo(dir + QStringLiteral("/migration.json")).isSymLink());
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    qputenv("XDG_CONFIG_HOME", (temporary.path() + QStringLiteral("/config")).toUtf8());
    qputenv("XDG_DATA_HOME", (temporary.path() + QStringLiteral("/data")).toUtf8());
    qputenv("XDG_STATE_HOME", (temporary.path() + QStringLiteral("/state")).toUtf8());
    QCoreApplication app(argc, argv);
    FarsideMigrationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "FarsideMigrationTest.moc"
