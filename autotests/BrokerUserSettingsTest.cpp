// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerUserSettings.h"
#include "UserConfiguration.h"
#include <QFile>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
#include <sys/stat.h>
#include <unistd.h>

using namespace KRdp;
class BrokerUserSettingsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void validatedTransaction()
    {
        const auto result = BrokerUserSettings::parse("[General]\nQuality=91\nAdaptiveQuality=0\nPreferAudioQuality=true\n"
            "SoftwareEncoding=prefer\nAv1Tiles=8\nCodec=avc444\nAvc444MotionGapMs=50\nAvc444RestMs=75\n"
            "Avc444MaxGapMs=1000\nMonitorMode=specific\nMonitorIndex=2\nVirtualMonitorPolicy=extend\n"
            "VirtualMonitorLayout=physical\nVirtualMonitorFallbackSize=2560x1440\nWakeDisplayOnConnect=false\n"
            "StandardClientMedia=false\nVirtualStockClientPolicy=refuse\n");
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        const auto &p = result.preferences;
        QCOMPARE(p.quality, std::optional<quint8>(91));
        QCOMPARE(p.adaptiveQuality, std::optional(false));
        QCOMPARE(p.preferAudioQuality, std::optional(true));
        QCOMPARE(p.softwareEncoding, std::optional(CodecPolicy::SoftwareEncoding::Prefer));
        QCOMPARE(p.av1Tiles, std::optional(8));
        QCOMPARE(p.codec, std::optional(CodecPreference::Avc444));
        QCOMPARE(p.chroma, std::optional(ChromaPolicy{50, 75, 1000}));
        QCOMPARE(p.monitorMode, std::optional(QStringLiteral("specific")));
        QCOMPARE(p.monitorIndex, std::optional(2));
        QCOMPARE(p.virtualMonitorFallbackSize, std::optional(QSize(2560, 1440)));
        QCOMPARE(p.standardClientMedia, std::optional(false));
    }

    void virtualMonitorPolicyOffIsTheConsoleScreensPermission()
    {
        for (const char *value : {"replace", "extend", "off"})
            QCOMPARE(BrokerUserSettings::parse(QByteArray("[General]\nVirtualMonitorPolicy=") + value + "\n").preferences.virtualMonitorPolicy,
                     std::optional(QString::fromLatin1(value)));
        QVERIFY(!BrokerUserSettings::parse("[General]\nVirtualMonitorPolicy=always\n").error.isEmpty());
        QVERIFY(BrokerUserSettings::parse("[General]\nMonitorMode=virtual\n").preferences.virtualMonitorPolicy == std::nullopt);
    }

    void invalidTransaction_data()
    {
        QTest::addColumn<QByteArray>("field");
        for (const auto *value : {"Quality=-1", "Quality=101", "Quality=9999999999999999999", "Quality=+20", "AdaptiveQuality=maybe",
                "SoftwareEncoding=nvidia", "Av1Tiles=3", "Codec=hevc", "MonitorMode=bogus", "MonitorIndex=-1",
                "VirtualMonitorLayout=bogus", "VirtualMonitorFallbackSize=0x0",
                "VirtualMonitorFallbackSize=1921x1080", "WakeDisplayOnConnect=maybe", "StandardClientMedia=2",
                "VirtualStockClientPolicy=any-user", "VirtualMonitorPolicy=always", "Avc444MotionGapMs=1000\nAvc444RestMs=50", "Avc444RestMs=5001"})
            QTest::newRow(value) << QByteArray(value);
    }
    void invalidTransaction()
    {
        QFETCH(QByteArray, field);
        const auto result = BrokerUserSettings::parse("[General]\nQuality=20\nPreferAudioQuality=true\n" + field + '\n');
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!result.preferences.quality);
        QVERIFY(!result.preferences.preferAudioQuality);
    }

    void configurationAuthorityAndKconfigMarkers()
    {
        const auto empty = BrokerUserSettings::parse("[General]\nListenPort=1234\nSystemUserEnabled=true\nUsers=other\n"
            "Certificate=/root/key\nCameraLoopbackDevice=/dev/video99\nVaapiDriverMode=nvidia\n");
        QVERIFY(empty.error.isEmpty());
        QVERIFY(!empty.preferences.quality);
        const auto result = BrokerUserSettings::parse("[Other]\nQuality=5\n[General]\nQuality=31\nQuality[$i]=42\n"
            "Quality[fr]=90\nPreferAudioQuality[$e]=$ROOT_SETTING\n[Other]\nQuality=100\n");
        QVERIFY(result.error.isEmpty());
        QCOMPARE(result.preferences.quality, std::optional<quint8>(42));
        QVERIFY(!result.preferences.preferAudioQuality);
        QVERIFY(!BrokerUserSettings::parse(QByteArray(UserConfiguration::MaximumBytes + 1, ' ')).error.isEmpty());
        QVERIFY(!BrokerUserSettings::parse(QByteArray("[General]\nQuality=42\0junk", 26)).error.isEmpty());
    }

    void boundedFileRead()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("preferences"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray config("[General]\nQuality=42\n");
        QCOMPARE(file.write(config), config.size());
        file.close();
        QCOMPARE(UserConfiguration::readFile(path, getuid()), std::optional(config));
        QVERIFY(!UserConfiguration::readFile(path, getuid() + 1));
        QVERIFY(!UserConfiguration::readFile(dir.path(), getuid()));
        const auto link = dir.filePath(QStringLiteral("link"));
        QVERIFY(QFile::link(path, link));
        QVERIFY(!UserConfiguration::readFile(link, getuid()));
        const auto fifo = dir.filePath(QStringLiteral("fifo"));
        QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!UserConfiguration::readFile(fifo, getuid()));
        QVERIFY(timer.elapsed() < 1000);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(UserConfiguration::MaximumBytes + 1));
        file.close();
        QVERIFY(!UserConfiguration::readFile(path, getuid()));
        QVERIFY(!UserConfiguration::readUser(0));
        QVERIFY(!UserConfiguration::readUser(quint32(-1)));
    }
};
QTEST_GUILESS_MAIN(BrokerUserSettingsTest)
#include "BrokerUserSettingsTest.moc"
