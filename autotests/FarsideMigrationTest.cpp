#include "FarsideMigration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class FarsideMigrationTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
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
