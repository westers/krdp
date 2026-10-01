// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerauthenticationsettings.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
using namespace Qt::StringLiterals;
class BrokerAuthenticationSettingsTest : public QObject
{
    Q_OBJECT
    static void mode(const QTemporaryDir &directory, const QByteArray &value)
    {
        QFile file(directory.filePath(u"mode"_s)); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(value), value.size());
    }
private Q_SLOTS:
    void loadEditSaveUsesStdinAndScrubsReadablePolicy()
    {
        QTemporaryDir directory;
        BrokerAuthenticationSettings settings(u"/usr/bin/python3"_s, {QString::fromUtf8(AUTH_FIXTURE), directory.path()}, nullptr);
        QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy()); QVERIFY(settings.loaded()); QVERIFY(!settings.modified());
        QVERIFY(settings.setPam(u"console"_s, u"any"_s, {}));
        QVERIFY(settings.setAlias(u"console"_s, u"guest"_s, u"westers"_s, u"fixture-only-password"_s));
        QVERIFY(settings.modified());
        const auto publicPolicy = QJsonDocument::fromVariant(settings.policy()).toJson(); QVERIFY(!publicPolicy.contains("password"));
        QCOMPARE(settings.policy().value(u"virtual"_s).toMap().value(u"pam"_s).toMap().value(u"mode"_s).toString(), u"disabled"_s);
        QVERIFY(settings.save()); QVERIFY(!settings.save()); QVERIFY(!settings.setPam(u"virtual"_s, u"any"_s, {}));
        QTRY_VERIFY(!settings.busy()); QVERIFY2(settings.error().isEmpty(), qPrintable(settings.error()));
        QVERIFY(!settings.modified()); QVERIFY(settings.lastSaveRequiresRestart());
        QCOMPARE(settings.policy().value(u"revision"_s).toString(), QString(64, u'b'));
        QFile file(directory.filePath(u"saved"_s)); QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(file.readAll()).object().value(u"passwordSeenOnStdin"_s).toBool());
    }
    void cancellationAndDeniedAuthorizationKeepEdits_data()
    {
        QTest::addColumn<QByteArray>("failure"); QTest::newRow("cancel") << QByteArray("cancel"); QTest::newRow("denied") << QByteArray("denied");
    }
    void cancellationAndDeniedAuthorizationKeepEdits()
    {
        QFETCH(QByteArray, failure); QTemporaryDir directory;
        BrokerAuthenticationSettings settings(u"/usr/bin/python3"_s, {QString::fromUtf8(AUTH_FIXTURE), directory.path()}, nullptr);
        QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy()); QVERIFY(settings.loaded());
        QVERIFY(settings.setPam(u"console"_s, u"any"_s, {})); const auto before = settings.policy();
        mode(directory, failure); QVERIFY(settings.save()); QTRY_VERIFY(!settings.busy());
        QCOMPARE(settings.policy(), before); QVERIFY(settings.modified()); QVERIFY(!settings.lastSaveRequiresRestart());
        QVERIFY(settings.error().contains(u"unchanged"_s)); QVERIFY(!QFile::exists(directory.filePath(u"saved"_s)));
        mode(directory, "success"); QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy()); QVERIFY(!settings.modified());
    }
    void brokenRepliesKeepCurrentSnapshot_data()
    {
        QTest::addColumn<QByteArray>("failure"); QTest::newRow("malformed") << QByteArray("malformed"); QTest::newRow("oversized") << QByteArray("oversized");
    }
    void brokenRepliesKeepCurrentSnapshot()
    {
        QFETCH(QByteArray, failure); QTemporaryDir directory;
        BrokerAuthenticationSettings settings(u"/usr/bin/python3"_s, {QString::fromUtf8(AUTH_FIXTURE), directory.path()}, nullptr);
        QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy()); const auto before = settings.policy();
        mode(directory, failure); QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy()); QCOMPARE(settings.policy(), before); QVERIFY(!settings.error().isEmpty());
    }
    void invalidEditsCannotIntroduceNewOwnerWithoutPassword()
    {
        QTemporaryDir directory;
        BrokerAuthenticationSettings settings(u"/usr/bin/python3"_s, {QString::fromUtf8(AUTH_FIXTURE), directory.path()}, nullptr);
        QVERIFY(!settings.setPam(u"console"_s, u"any"_s, {}));
        QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy());
        QVERIFY(!settings.setAlias(u"console"_s, u"guest"_s, u"westers"_s, {}));
        QVERIFY(!settings.setPam(u"virtual"_s, u"any"_s, {u"westers"_s})); QVERIFY(!settings.modified());
        QVERIFY(settings.setAlias(u"console"_s, u"guest"_s, u"westers"_s, u"fixture-only-password"_s));
        const auto before = settings.policy(); QVERIFY(!settings.setAlias(u"console"_s, u"guest"_s, u"nobody"_s, {})); QCOMPARE(settings.policy(), before);
        QVERIFY(settings.removeAlias(u"console"_s, u"guest"_s)); QVERIFY(!settings.modified());
    }
    void missingHelperFailsWithoutDirtyingPolicy()
    {
        BrokerAuthenticationSettings settings(u"/nonexistent/farside-test-helper"_s, {}, nullptr);
        QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy()); QVERIFY(!settings.loaded()); QVERIFY(!settings.modified()); QVERIFY(!settings.error().isEmpty());
    }
};
QTEST_GUILESS_MAIN(BrokerAuthenticationSettingsTest)
#include "BrokerAuthenticationSettingsTest.moc"
