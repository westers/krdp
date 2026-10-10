// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerHostSettings.h"
#include <QFile>
#include <QJsonDocument>
#include <QTest>
using namespace Qt::StringLiterals;
using namespace KRdp::BrokerHostSettings;
class BrokerHostSettingsTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void completeFieldsAndScopedDefaults()
    {
        // OPT-062 S3 added SoftwareAvc, SoftwareHevc and SoftwareAv1 to Console and Virtual (a deliberate update of this pin).
        QCOMPARE(keys(Scope::Console).size(), 15);
        QCOMPARE(keys(Scope::Virtual).size(), 14);
        QCOMPARE(keys(Scope::VirtualSession).size(), 2);
        for (const auto scope : {Scope::Console, Scope::Virtual, Scope::VirtualSession}) {
            const auto values = defaults(scope);
            const auto result = edit(scope, "# keep\nUNKNOWN=fixture-only-secret\n", values);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QVERIFY(result.contents.startsWith("# keep\nUNKNOWN=fixture-only-secret\n"));
            const auto snapshot = parse(scope, result.contents);
            QCOMPARE(snapshot.overrides, values);
            QCOMPARE(snapshot.effective, values);
            QVERIFY(!QJsonDocument::fromVariant(snapshot.effective).toJson().contains("fixture-only-secret"));
            const auto inherited = edit(scope, result.contents, {});
            QVERIFY(inherited.error.isEmpty());
            QCOMPARE(inherited.contents, QByteArray("# keep\nUNKNOWN=fixture-only-secret\n"));
            QCOMPARE(parse(scope, inherited.contents).effective, values);
        }
        QCOMPARE(defaults(Scope::Console)[u"Port"_s].toString(), u"3391"_s);
        QCOMPARE(defaults(Scope::Virtual)[u"Port"_s].toString(), u"3395"_s);
        QCOMPARE(defaults(Scope::VirtualSession)[u"RenderPci"_s].toString(), QString());
        QVERIFY(!keys(Scope::Virtual).contains(u"VaapiDriver"_s));
        QVERIFY(!normalize(Scope::Virtual, u"RenderPci"_s, u"0000:01:00.0"_s));
        QVERIFY(!parse(Scope(42), {}).error.isEmpty());
        QVERIFY(fileName(Scope(42)).isEmpty());
    }
    void preservesUnknownSyntaxAndRemovesFullDuplicateStatements()
    {
        const QByteArray preserved("# comment ending in slash\\\r\n"
            "UNKNOWN='multiple\r\nlines = retained'\r\n"
            "FARSIDE_CONSOLE_WORKER=\"/root/custom worker\"\r\n"
            "FARSIDE_VIRTUAL_PORT=4999\r\n"
            "  ; stay\r\nIGNORED WITHOUT EQUALS\r\n");
        const QByteArray first("FARSIDE_CONSOLE_PORT=not-a-number\r\n");
        const QByteArray last("  FARSIDE_CONSOLE_PORT = 04\\\n321\r\n");
        const auto original = preserved + first + last;
        const auto snapshot = parse(Scope::Console, original);
        QVERIFY2(snapshot.error.isEmpty(), qPrintable(snapshot.error));
        QCOMPARE(snapshot.overrides, QVariantMap({{u"Port"_s, u"4321"_s}}));
        QCOMPARE(edit(Scope::Console, original, snapshot.overrides).contents, original);
        const auto changed = edit(Scope::Console, original, {{u"Port"_s, u"5432"_s}});
        QVERIFY2(changed.error.isEmpty(), qPrintable(changed.error));
        QCOMPARE(changed.contents, preserved + "FARSIDE_CONSOLE_PORT=\"5432\"\r\n");
        QCOMPARE(edit(Scope::Console, changed.contents, {}).contents, preserved);
    }
    void quotingContinuationAndLiteralCharacters()
    {
        const QByteArray raw("FARSIDE_CONSOLE_CERTIFICATE = \"/root/TLS file \\$HOME \\` \\\" \\\\ \\q.pem\"\n"
            "FARSIDE_CONSOLE_CERTIFICATE_KEY='/root/key \\ $HOME.pem'\n"
            "FARSIDE_CONSOLE_PORT=12\\\n34   \n"
            "FARSIDE_CONSOLE_ADDRESS='::1'\n"
            "FARSIDE_CONSOLE_QUALITY=0'1'\n");
        // In an unquoted value interior quotes are literal, not shell quotes.
        QVERIFY(!parse(Scope::Console, raw).error.isEmpty());
        auto valid = raw; valid.replace("QUALITY=0'1'", "QUALITY='0'\"1\"");
        const auto snapshot = parse(Scope::Console, valid);
        QVERIFY2(snapshot.error.isEmpty(), qPrintable(snapshot.error));
        QCOMPARE(snapshot.overrides[u"Port"_s].toString(), u"1234"_s);
        QCOMPARE(snapshot.overrides[u"Quality"_s].toString(), u"1"_s);
        QCOMPARE(snapshot.overrides[u"Certificate"_s].toString(), u"/root/TLS file $HOME ` \" \\ \\q.pem"_s);
        QCOMPARE(snapshot.overrides[u"CertificateKey"_s].toString(), u"/root/key \\ $HOME.pem"_s);
        auto desired = snapshot.overrides;
        desired[u"Certificate"_s] = u"/root/TLS space quote' dollar$ slash\\ tick` double\".pem"_s;
        desired[u"Port"_s] = u"6543"_s;
        const auto edited = edit(Scope::Console, valid, desired);
        QVERIFY2(edited.error.isEmpty(), qPrintable(edited.error));
        QCOMPARE(parse(Scope::Console, edited.contents).overrides, desired);
        QCOMPARE(edit(Scope::Console, "# no newline", {{u"Port"_s, u"4321"_s}}).contents,
            QByteArray("# no newline\nFARSIDE_CONSOLE_PORT=\"4321\"\n"));
    }
    void boundedDocumentsAndInvalidDrafts()
    {
        for (const auto &contents : {QByteArray("OTHER='unfinished"), QByteArray("OTHER=value\\"), QByteArray("OTHER=\"unfinished\\"),
            QByteArray("OTHER=x\0y", 9), QByteArray("OTHER=\xff", 7), QByteArray(MaximumBytes + 1, '#')}) {
            QVERIFY(!parse(Scope::Console, contents).error.isEmpty());
            QVERIFY(!edit(Scope::Console, contents, {}).error.isEmpty());
        }
        for (const auto &value : {QByteArray::fromHex("efbbbf"), QByteArray::fromHex("efb790"), QByteArray::fromHex("f09fbfbf")})
            QVERIFY(!parse(Scope::Console, QByteArray("OTHER=") + value).error.isEmpty());
        QVERIFY(!parse(Scope::Console, QByteArray::fromHex("efbbbf") + "# initial BOM\n").error.isEmpty());
        QVERIFY(!parse(Scope::Console, "FARSIDE_CONSOLE_ADAPTIVE_QUALITY=TRUE\n").error.isEmpty());
        const QByteArray original("# keep\nFARSIDE_CONSOLE_PORT=4321\n");
        for (const auto &desired : QList<QVariantMap>{{{u"Port"_s, u"0"_s}}, {{u"Port"_s, u"65536"_s}},
                {{u"Quality"_s, u"101"_s}}, {{u"Quality"_s, u"-1"_s}}, {{u"Port"_s, 1234}},
                {{u"Quality"_s, u"80\nINJECTION=value"_s}}, {{u"INJECTION\nfixture-secret"_s, u"x"_s}},
                {{u"Certificate"_s, u"relative.pem"_s}}, {{u"CertificateKey"_s, u"/root/../etc/file"_s}},
                {{u"Address"_s, u"example.com"_s}}, {{u"Address"_s, u" ::1"_s}},
                {{u"SoftwareEncoding"_s, u"invented"_s}}, {{u"Av1Tiles"_s, u"3"_s}},
                {{u"SoftwareHevc"_s, u"last-resort"_s}}, {{u"SoftwareAvc"_s, u"never"_s}}, {{u"SoftwareAv1"_s, u"sometimes"_s}},
                {{u"SoftwareAv1"_s, u"never\nINJECTION=1"_s}},
                {{u"CameraLoopbackDevice"_s, u"/etc/file"_s}}, {{u"Certificate"_s, u"/etc/farside/console.key"_s}},
                {{u"Certificate"_s, QString(u"/"_s + QString(MaximumValue, u'x'))}}, {{u"AdaptiveQuality"_s, u"1"_s}}}) {
            const auto result = edit(Scope::Console, original, desired);
            QVERIFY2(!result.error.isEmpty(), QJsonDocument::fromVariant(desired).toJson().constData());
            QVERIFY(result.contents.isEmpty());
            QVERIFY(!result.error.contains(u"fixture-secret"_s));
            QCOMPARE(original, QByteArray("# keep\nFARSIDE_CONSOLE_PORT=4321\n"));
        }
    }
    // OPT-062 S3: the host's software ceiling per codec.
    void softwareCeilingsPerCodec()
    {
        for (const auto scope : {Scope::Console, Scope::Virtual}) {
            for (const auto &key : {u"SoftwareAvc"_s, u"SoftwareHevc"_s, u"SoftwareAv1"_s}) {
                QVERIFY(keys(scope).contains(key));
                QCOMPARE(defaults(scope)[key].toString(), u"auto"_s);
                QCOMPARE(normalize(scope, key, u""_s), std::optional<QString>(u"auto"_s));
                QCOMPARE(normalize(scope, key, u" AUTO "_s), std::optional<QString>(u"auto"_s));
                QCOMPARE(normalize(scope, key, u"Allowed"_s), std::optional<QString>(u"allowed"_s));
            }
            QCOMPARE(normalize(scope, u"SoftwareAvc"_s, u"last-resort"_s), std::optional<QString>(u"last-resort"_s));
            QCOMPARE(normalize(scope, u"SoftwareHevc"_s, u"NEVER"_s), std::optional<QString>(u"never"_s));
            QCOMPARE(normalize(scope, u"SoftwareAv1"_s, u"never"_s), std::optional<QString>(u"never"_s));
            // H.264 can only be a last resort, never "never"; HEVC and AV1 have no last resort.
            QVERIFY(!normalize(scope, u"SoftwareAvc"_s, u"never"_s));
            QVERIFY(!normalize(scope, u"SoftwareHevc"_s, u"last-resort"_s));
            QVERIFY(!normalize(scope, u"SoftwareAv1"_s, u"prefer"_s));
        }
        QVERIFY(!keys(Scope::VirtualSession).contains(u"SoftwareAv1"_s)); // a host setting, not a desktop's
        QCOMPARE(environmentName(Scope::Console, u"SoftwareHevc"_s), u"FARSIDE_CONSOLE_SOFTWARE_HEVC"_s);
        QCOMPARE(environmentName(Scope::Virtual, u"SoftwareAv1"_s), u"FARSIDE_VIRTUAL_SOFTWARE_AV1"_s);
        QCOMPARE(environmentName(Scope::Virtual, u"SoftwareAvc"_s), u"FARSIDE_VIRTUAL_SOFTWARE_AVC"_s);
        // Edit and parse round trip, other lines untouched; an old file (no such lines) means auto.
        const auto edited = edit(Scope::Console, "# keep\nFARSIDE_CONSOLE_PORT=4321\n", {{u"SoftwareAv1"_s, u"never"_s}, {u"SoftwareAvc"_s, u"last-resort"_s}});
        QVERIFY2(edited.error.isEmpty(), qPrintable(edited.error));
        QVERIFY(edited.contents.contains("FARSIDE_CONSOLE_SOFTWARE_AV1=\"never\"") || edited.contents.contains("FARSIDE_CONSOLE_SOFTWARE_AV1=never"));
        const auto snapshot = parse(Scope::Console, edited.contents);
        QCOMPARE(snapshot.effective[u"SoftwareAv1"_s].toString(), u"never"_s);
        QCOMPARE(snapshot.effective[u"SoftwareAvc"_s].toString(), u"last-resort"_s);
        QCOMPARE(snapshot.effective[u"SoftwareHevc"_s].toString(), u"auto"_s);
        QVERIFY(edited.contents.startsWith("# keep\n"));
        QVERIFY(!parse(Scope::Console, "FARSIDE_CONSOLE_SOFTWARE_HEVC=last-resort\n").error.isEmpty());
        QCOMPARE(parse(Scope::Console, "# nothing here\n").effective[u"SoftwareHevc"_s].toString(), u"auto"_s);
    }
    void typedPoliciesAndPciGrants()
    {
        QCOMPARE(normalize(Scope::Console, u"VaapiDriver"_s, u"IHD"_s), std::optional<QString>(u"iHD"_s));
        QCOMPARE(normalize(Scope::Console, u"SoftwareEncoding"_s, u"PREFER"_s), std::optional<QString>(u"prefer"_s));
        QCOMPARE(parse(Scope::Console, "FARSIDE_CONSOLE_SOFTWARE_ENCODING=\n").overrides[u"SoftwareEncoding"_s].toString(), u"auto"_s);
        QCOMPARE(normalize(Scope::Console, u"AdaptiveQuality"_s, u"TRUE"_s), std::optional<QString>(u"true"_s));
        QCOMPARE(normalize(Scope::Console, u"Av1Tiles"_s, u"8"_s), std::optional<QString>(u"8"_s));
        QCOMPARE(normalize(Scope::Console, u"CameraLoopbackDevice"_s, u"/dev/v4l/by-id/loopback"_s), std::optional<QString>(u"/dev/v4l/by-id/loopback"_s));
        QCOMPARE(normalize(Scope::VirtualSession, u"RenderPci"_s, u"0000:01:00.0,0000:c5:00.0"_s), std::optional<QString>(u"0000:01:00.0,0000:c5:00.0"_s));
        for (const auto &value : {u"renderD128"_s, u"0000:20:20.0"_s, u"0000:01:00.8"_s,
                u"0000:01:00.0,0000:01:00.0"_s, u"0000:01:00.0, 0000:c5:00.0"_s, u"0000:C5:00.0"_s})
            QVERIFY(!normalize(Scope::VirtualSession, u"RenderPci"_s, value));
        const auto snapshot = parse(Scope::VirtualSession,
            "FARSIDE_VIRTUAL_RENDER_PCI=0000:01:00.0\nFARSIDE_VIRTUAL_VAAPI_DRIVER=off\nFARSIDE_CONSOLE_PORT=bad\n");
        QVERIFY(snapshot.error.isEmpty());
        QCOMPARE(snapshot.overrides.size(), 2);
        QCOMPARE(snapshot.effective[u"VaapiDriver"_s].toString(), u"off"_s);
    }
    void defaultsMatchShippedUnits()
    {
        for (const auto scope : {Scope::Console, Scope::Virtual, Scope::VirtualSession}) {
            const QString unit = scope == Scope::Console ? u"krdp-console-host.service.in"_s
                : scope == Scope::Virtual ? u"krdp-virtual-host.service.in"_s : u"krdp-virtual-session@.service.in"_s;
            QFile file(QStringLiteral(HOST_SETTINGS_SOURCE) + u"/server/"_s + unit);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto bytes = file.readAll();
            const auto values = defaults(scope);
            for (auto it = values.cbegin(); it != values.cend(); ++it) {
                const auto name = environmentName(scope, it.key()).toUtf8();
                const QByteArray definition = "Environment=" + name + '=' + it.value().toString().toUtf8() + '\n';
                const QByteArray argument = "${" + name + '}';
                QVERIFY2(bytes.contains(definition), qPrintable(it.key()));
                QVERIFY2(bytes.contains(argument), qPrintable(it.key()));
            }
        }
    }
};
QTEST_GUILESS_MAIN(BrokerHostSettingsTest)
#include "BrokerHostSettingsTest.moc"
