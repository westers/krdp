// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerHostSettings.h"
#include "VirtualSystemBus.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>

using namespace KRdp;
using namespace KRdp::VirtualSystemBus;
using namespace Qt::StringLiterals;

namespace
{
QMap<QString, QString> environmentOf(const QByteArray &bytes, QString *error)
{
    const auto parsed = BrokerHostSettings::privateEnvironment(bytes);
    *error = parsed.error;
    return parsed.assignments;
}
Settings off(Settings settings)
{
    settings.network = settings.power = settings.packages = settings.polkit = settings.bluetooth = settings.printing = false;
    settings.extra.clear();
    return settings;
}
}

class VirtualSystemBusTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void defaultsAreFilteredWithNetworkAndPowerOnly()
    {
        const auto settings = defaults();
        QCOMPARE(settings.mode, Mode::Filtered);
        QVERIFY(settings.network && settings.power);
        QVERIFY(!settings.packages && !settings.polkit && !settings.bluetooth && !settings.printing);
        QVERIFY(settings.extra.isEmpty());
        // The key table is the one source of the defaults the scope, the unit and the example file repeat.
        QMap<QString, QString> table;
        for (const auto &entry : keyTable()) table.insert(QLatin1String(entry.key), QLatin1String(entry.fallback));
        QCOMPARE(apply(Settings{}, table).value(), settings);
    }
    void fullIsNeverShippedAsDefault()
    {
        QCOMPARE(QLatin1String(keyTable().first().fallback), QLatin1String("filtered"));
        QFile unit(QStringLiteral(SOURCE_DIR "/server/krdp-virtual-session@.service.in"));
        QVERIFY(unit.open(QIODevice::ReadOnly));
        const auto text = QString::fromUtf8(unit.readAll());
        QVERIFY(text.contains(u"Environment=FARSIDE_VIRTUAL_SYSTEM_BUS=filtered\n"_s));
        QVERIFY(!text.contains(u"SYSTEM_BUS=full"_s));
        QVERIFY(text.contains(u"FARSIDE_VIRTUAL_SYSTEM_BUS_EXTRA=\n"_s));
        QFile example(QStringLiteral(SOURCE_DIR "/server/virtual-session.conf.example"));
        QVERIFY(example.open(QIODevice::ReadOnly));
        for (const auto &line : QString::fromUtf8(example.readAll()).split(u'\n'))
            if (!line.startsWith(u'#')) QVERIFY2(!line.contains(u"SYSTEM_BUS=full"_s), qPrintable(line));
        // Every environment name in the unit is one the table knows.
        for (const auto &entry : keyTable()) QVERIFY(text.contains(QStringLiteral("Environment=FARSIDE_VIRTUAL_") + QLatin1String(entry.suffix) + u"="_s));
    }
    void modes()
    {
        QCOMPARE(parseMode(u"none"_s), std::optional<Mode>(Mode::None));
        QCOMPARE(parseMode(u"filtered"_s), std::optional<Mode>(Mode::Filtered));
        QCOMPARE(parseMode(u"full"_s), std::optional<Mode>(Mode::Full));
        for (const auto &bad : {u""_s, u"Full"_s, u"FILTERED"_s, u"filtered "_s, u"on"_s, u"true"_s, u"none\nfull"_s}) QVERIFY(!parseMode(bad));
        for (const auto mode : {Mode::None, Mode::Filtered, Mode::Full}) QCOMPARE(parseMode(modeName(mode)), std::optional<Mode>(mode));
    }
    void noneProducesNoRulesAndNoPlan()
    {
        Settings settings;
        settings.mode = Mode::None;
        settings.polkit = settings.printing = true;
        const auto result = plan(settings, true, true);
        QCOMPARE(result.mode, Mode::None);
        QVERIFY(result.proxyRules.isEmpty() && !result.polkitAgent && !result.cupsSocket && result.note.isEmpty());
    }
    void networkRulesAreReadOnly()
    {
        auto settings = off(defaults());
        settings.network = true;
        const auto rules = proxyRules(settings);
        QCOMPARE(rules.first(), u"--see=org.freedesktop.NetworkManager"_s);
        QVERIFY(rules.contains(u"--call=org.freedesktop.NetworkManager=org.freedesktop.DBus.Properties.Get@/org/freedesktop/NetworkManager"_s));
        QVERIFY(rules.contains(u"--call=org.freedesktop.NetworkManager=org.freedesktop.DBus.Properties.GetAll@/org/freedesktop/NetworkManager/*"_s));
        QVERIFY(rules.contains(u"--call=org.freedesktop.NetworkManager=org.freedesktop.NetworkManager.Settings.ListConnections@/org/freedesktop/NetworkManager/*"_s));
        QVERIFY(rules.contains(u"--broadcast=org.freedesktop.NetworkManager=@/org/freedesktop/NetworkManager/*"_s));
        // No talk (which would allow every call) and none of the changing methods.
        const auto joined = rules.join(u'\n');
        QVERIFY(!joined.contains(u"--talk"_s));
        for (const auto &forbidden : {u"Properties.Set"_s, u"ActivateConnection"_s, u"AddConnection"_s, u"Deactivate"_s, u"Delete"_s, u"Update"_s, u"RequestScan"_s,
                                      u"CheckConnectivity"_s, u"Enable"_s, u"Sleep"_s, u"SetLogging"_s, u"=*"_s})
            QVERIFY2(!joined.contains(forbidden), qPrintable(forbidden));
    }
    void powerRulesAreReadOnly()
    {
        auto settings = off(defaults());
        settings.power = true;
        const auto rules = proxyRules(settings);
        const auto joined = rules.join(u'\n');
        QVERIFY(rules.contains(u"--see=org.freedesktop.UPower"_s) && rules.contains(u"--see=org.freedesktop.login1"_s));
        QVERIFY(rules.contains(u"--call=org.freedesktop.UPower=org.freedesktop.UPower.EnumerateDevices@/org/freedesktop/UPower"_s));
        QVERIFY(rules.contains(u"--call=org.freedesktop.login1=org.freedesktop.login1.Manager.CanSuspend@/org/freedesktop/login1/*"_s));
        QVERIFY(!joined.contains(u"--talk"_s));
        for (const auto &forbidden : {u"Inhibit"_s, u"Manager.PowerOff@"_s, u"Manager.Reboot@"_s, u"Manager.Suspend@"_s, u"Manager.Hibernate@"_s, u"Terminate"_s, u"Lock"_s, u"Activate"_s, u"Properties.Set"_s, u"=*"_s})
            QVERIFY2(!joined.contains(forbidden), qPrintable(forbidden));
    }
    void eachToggleAddsExactlyItsNames()
    {
        const auto only = [](auto set) {
            auto settings = off(defaults());
            set(settings);
            return proxyRules(settings);
        };
        QCOMPARE(only([](Settings &s) { s.packages = true; }), QStringList({u"--talk=org.freedesktop.PackageKit"_s, u"--talk=org.freedesktop.fwupd"_s}));
        QCOMPARE(only([](Settings &s) { s.polkit = true; }), QStringList({u"--talk=org.freedesktop.PolicyKit1"_s}));
        QCOMPARE(only([](Settings &s) { s.bluetooth = true; }), QStringList({u"--talk=org.bluez"_s}));
        QCOMPARE(only([](Settings &s) { s.printing = true; }), QStringList({u"--talk=org.opensuse.CupsPkHelper.Mechanism"_s}));
        QCOMPARE(only([](Settings &s) { s.extra = {u"org.example.Service"_s, u"org.example.Other.*"_s}; }),
                 QStringList({u"--talk=org.example.Service"_s, u"--talk=org.example.Other.*"_s}));
        QVERIFY(proxyRules(off(defaults())).isEmpty());
    }
    void everyGeneratedRuleIsInTheLauncherGrammar()
    {
        Settings everything;
        everything.packages = everything.polkit = everything.bluetooth = everything.printing = true;
        everything.extra = {u"org.example.A-b_c.D"_s, u"io.github.thing.*"_s};
        const auto rules = proxyRules(everything);
        QVERIFY(rules.size() > 30 && rules.size() < MaximumRules);
        for (const auto &rule : rules) QVERIFY2(validProxyArgument(rule), qPrintable(rule));
        QCOMPARE(rules, proxyRules(everything)); // stable order
        // The grammar admits no option the builder does not make.
        for (const auto &bad : {u"--filter"_s, u"--own=org.x.Y"_s, u"--fd=3"_s, u"--talk="_s, u"--talk=org.x y"_s, u"--talk=a;b"_s, u"--call=org.x.Y="_s,
                                u"--call=org.x.Y"_s, u"--talk=org.x\n--own=y"_s, u"unix:path=/x"_s, u"--talk=a$(b)"_s, u"--see=org.x.Y\n"_s, u" --see=org.x.Y"_s})
            QVERIFY2(!validProxyArgument(bad), qPrintable(bad));
    }
    void extraNamesAreStrict()
    {
        QCOMPARE(normalizeExtra(u""_s), std::optional<QString>(u""_s));
        QCOMPARE(normalizeExtra(u"  "_s), std::optional<QString>(u""_s));
        QCOMPARE(normalizeExtra(u"org.a.B, org.c.D ,org.a.B"_s), std::optional<QString>(u"org.a.B,org.c.D"_s));
        QCOMPARE(normalizeExtra(u"org.example.*"_s), std::optional<QString>(u"org.example.*"_s));
        const QStringList bad{u"org"_s, u"org.*"_s, u"*"_s, u".org.a"_s, u"org..a"_s, u"org.a."_s, u"org.1a"_s, u"org.a b"_s, u"org.a;ls"_s, u"org.a=b"_s,
                              u"org.a,"_s, u",org.a"_s, u"org.a,,org.b"_s, u"org.a\nb"_s, u"org.a\tb"_s, u"--talk=org.a"_s, u"org.a/b"_s, u"org.a$b"_s,
                              u"org.a.*.b"_s, u"org.a*"_s, u"org.é.a"_s, u"org.a`id`"_s, u"org.a'b"_s, u"org.a\"b"_s, u"org.a\\b"_s, u"org.a\0b"_s,
                              u"org.freedesktop.DBus"_s, u"org.freedesktop.DBus.*"_s, u"org.freedesktop.DBus.Debug.Stats"_s, u"org.freedesktop.systemd1"_s,
                              u"org.freedesktop.systemd1.*"_s, u"org.freedesktop.machine1"_s, u"org.freedesktop.*"_s, u"org.freedesktop.system.*"_s + u",org.freedesktop.systemd1"_s};
        for (const auto &name : bad) QVERIFY2(!normalizeExtra(name), qPrintable(name));
        QStringList many;
        for (int i = 0; i < MaximumExtraNames; ++i) many.append(QStringLiteral("org.example.N%1").arg(i));
        QVERIFY(normalizeExtra(many.join(u','))); // exactly the maximum
        many.append(u"org.example.One.More"_s);
        QVERIFY(!normalizeExtra(many.join(u','))); // one over
        QVERIFY(!normalizeExtra(u"org."_s + QString(MaximumNameBytes, u'a'))); // over the length bound
        QVERIFY(normalizeExtra(u"org."_s + QString(MaximumNameBytes - 4, u'a')));
        QVERIFY(normalizeExtra(u"org.freedesktop.UPower"_s)); // an ordinary freedesktop name stays allowed
    }
    void scopeValuesUseTheSameGrammar()
    {
        using BrokerHostSettings::Scope;
        using BrokerHostSettings::normalize;
        for (const auto &mode : {u"none"_s, u"filtered"_s, u"full"_s}) QCOMPARE(normalize(Scope::VirtualSession, u"VirtualSystemBus"_s, mode), std::optional<QString>(mode));
        for (const auto &bad : {u""_s, u"Full"_s, u"on"_s, u"filtered\nVirtualSystemBus=full"_s}) QVERIFY(!normalize(Scope::VirtualSession, u"VirtualSystemBus"_s, bad));
        for (const auto &key : {u"SystemBusNetwork"_s, u"SystemBusPower"_s, u"SystemBusPackages"_s, u"SystemBusPolkit"_s, u"SystemBusBluetooth"_s, u"SystemBusPrinting"_s}) {
            QCOMPARE(normalize(Scope::VirtualSession, key, u"true"_s), std::optional<QString>(u"true"_s));
            QCOMPARE(normalize(Scope::VirtualSession, key, u"false"_s), std::optional<QString>(u"false"_s));
            for (const auto &bad : {u"TRUE"_s, u"1"_s, u"yes"_s, u""_s, u"true "_s}) QVERIFY2(!normalize(Scope::VirtualSession, key, bad), qPrintable(key + bad));
            QVERIFY(!normalize(Scope::Virtual, key, u"true"_s)); // not a key of the other scopes
            QVERIFY(!normalize(Scope::Console, key, u"true"_s));
        }
        QCOMPARE(normalize(Scope::VirtualSession, u"SystemBusExtra"_s, u"org.a.B, org.c.D"_s), std::optional<QString>(u"org.a.B,org.c.D"_s));
        QVERIFY(!normalize(Scope::VirtualSession, u"SystemBusExtra"_s, u"org.freedesktop.systemd1"_s));
    }
    void planDegradesWithoutProxyOrSocket()
    {
        Settings filtered;
        filtered.polkit = filtered.printing = true;
        auto result = plan(filtered, true, true);
        QCOMPARE(result.mode, Mode::Filtered);
        QVERIFY(!result.proxyRules.isEmpty() && result.polkitAgent && result.cupsSocket && result.note.isEmpty());
        result = plan(filtered, false, true);
        QCOMPARE(result.mode, Mode::None);
        QVERIFY(result.proxyRules.isEmpty() && !result.polkitAgent && !result.cupsSocket);
        QVERIFY(result.note.contains(u"xdg-dbus-proxy is not installed"_s));
        result = plan(filtered, true, false);
        QCOMPARE(result.mode, Mode::None);
        QVERIFY(result.note.contains(u"does not exist"_s));
        // Full needs neither the proxy nor rules, only the socket.
        Settings full;
        full.mode = Mode::Full;
        result = plan(full, false, true);
        QCOMPARE(result.mode, Mode::Full);
        QVERIFY(result.proxyRules.isEmpty());
        QCOMPARE(plan(full, true, false).mode, Mode::None);
    }
    void applyRefusesAnyInvalidValue()
    {
        QString error;
        QVERIFY(!apply(defaults(), {{u"VirtualSystemBus"_s, u"full"_s}, {u"SystemBusExtra"_s, u"org.freedesktop.DBus"_s}}, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!apply(defaults(), {{u"Nonsense"_s, u"true"_s}}));
        const auto ok = apply(defaults(), {{u"VirtualSystemBus"_s, u"full"_s}, {u"SystemBusNetwork"_s, u"false"_s}}, &error);
        QVERIFY(ok && ok->mode == Mode::Full && !ok->network && ok->power && error.isEmpty());
    }
    void perUserOverrideTakesPrecedenceKeyByKey()
    {
        Settings global;
        global.packages = true;
        const auto read = [](const QByteArray &bytes) { FileRead file; file.exists = true; file.bytes = bytes; return file; };
        const auto result = resolveUser(global, u"alice"_s, read("FARSIDE_VIRTUAL_SYSTEM_BUS_NETWORK=false\nFARSIDE_VIRTUAL_SYSTEM_BUS_POLKIT=true\n"), environmentOf);
        QCOMPARE(result.state, UserOverride::State::Applied);
        QVERIFY(!result.settings.network); // overridden
        QVERIFY(result.settings.polkit); // overridden
        QVERIFY(result.settings.power && result.settings.packages); // falls back to the global values
        QCOMPARE(result.settings.mode, Mode::Filtered);
        const auto none = resolveUser(global, u"alice"_s, read("FARSIDE_VIRTUAL_SYSTEM_BUS=none\n"), environmentOf);
        QCOMPARE(none.settings.mode, Mode::None);
        // An override may also lower the mode from full, and an empty file changes nothing.
        Settings wide = global;
        wide.mode = Mode::Full;
        QCOMPARE(resolveUser(wide, u"alice"_s, read("FARSIDE_VIRTUAL_SYSTEM_BUS=filtered\n"), environmentOf).settings.mode, Mode::Filtered);
        QCOMPARE(resolveUser(wide, u"alice"_s, read(""), environmentOf).settings, wide);
    }
    void absentOrUnaddressableOverrideLeavesGlobalValues()
    {
        const Settings global = defaults();
        QCOMPARE(resolveUser(global, u"alice"_s, FileRead{}, environmentOf).state, UserOverride::State::Absent);
        QCOMPARE(resolveUser(global, u"alice"_s, FileRead{}, environmentOf).settings, global);
        FileRead file;
        file.exists = true;
        file.bytes = "FARSIDE_VIRTUAL_SYSTEM_BUS=none\n";
        for (const auto &login : {u"Alice"_s, u"../etc"_s, u"a b"_s, u""_s, u"root/x"_s, QString(33, u'a')})
            QCOMPARE(resolveUser(global, login, file, environmentOf).settings, global); // no lookup for an odd login
    }
    void brokenOverrideFailsClosedToNone()
    {
        Settings global;
        global.mode = Mode::Full;
        const auto expectClosed = [&](const QByteArray &bytes, const QString &error = {}) {
            FileRead file;
            file.exists = true;
            file.bytes = bytes;
            file.error = error;
            const auto result = resolveUser(global, u"alice"_s, file, environmentOf);
            QCOMPARE(result.state, UserOverride::State::Invalid);
            QCOMPARE(result.settings.mode, Mode::None);
            QVERIFY(!result.error.isEmpty());
        };
        expectClosed("FARSIDE_VIRTUAL_SYSTEM_BUS=full\nFARSIDE_VIRTUAL_SYSTEM_BUS_EXTRA=org.freedesktop.systemd1\n");
        expectClosed("FARSIDE_VIRTUAL_SYSTEM_BUS=Full\n");
        expectClosed("FARSIDE_VIRTUAL_RENDER_PCI=0000:01:00.0\n"); // any other key
        expectClosed("PATH=/tmp\n");
        expectClosed("FARSIDE_VIRTUAL_SYSTEM_BUS='unterminated\n");
        expectClosed("FARSIDE_VIRTUAL_SYSTEM_BUS=none\n", u"override file is not trusted"_s);
    }
    void overrideFileMustBeTrusted()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto dir = directory.path();
        const uid_t me = geteuid();
        const auto write = [&](const QString &name, const QByteArray &bytes, mode_t mode) {
            QFile file(dir + u'/' + name);
            QVERIFY(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size());
            file.close();
            QVERIFY(!chmod(QFile::encodeName(file.fileName()).constData(), mode));
        };
        QVERIFY(!chmod(QFile::encodeName(dir).constData(), 0700));
        QVERIFY(!readTrustedFile(dir, u"nobody.conf"_s, me).exists); // absent
        write(u"alice.conf"_s, "FARSIDE_VIRTUAL_SYSTEM_BUS=none\n", 0600);
        auto read = readTrustedFile(dir, u"alice.conf"_s, me);
        QVERIFY(read.exists && read.error.isEmpty());
        QCOMPARE(read.bytes, QByteArray("FARSIDE_VIRTUAL_SYSTEM_BUS=none\n"));
        QVERIFY(!readTrustedFile(dir, u"alice.conf"_s, me + 1).error.isEmpty()); // wrong owner
        write(u"bob.conf"_s, "x=1\n", 0666);
        QVERIFY(!readTrustedFile(dir, u"bob.conf"_s, me).error.isEmpty()); // writable by others
        write(u"carl.conf"_s, "x=1\n", 0620);
        QVERIFY(!readTrustedFile(dir, u"carl.conf"_s, me).error.isEmpty()); // writable by the group
        write(u"huge.conf"_s, QByteArray(70000, 'a'), 0600);
        QVERIFY(!readTrustedFile(dir, u"huge.conf"_s, me).error.isEmpty());
        QVERIFY(QFile::link(dir + u"/alice.conf"_s, dir + u"/link.conf"_s));
        QVERIFY(!readTrustedFile(dir, u"link.conf"_s, me).error.isEmpty()); // a symlink is never followed
        QVERIFY(!::link(QFile::encodeName(dir + u"/alice.conf"_s).constData(), QFile::encodeName(dir + u"/hard.conf"_s).constData()));
        QVERIFY(!readTrustedFile(dir, u"hard.conf"_s, me).error.isEmpty()); // a second link to the same inode
        QVERIFY(!chmod(QFile::encodeName(dir).constData(), 0770));
        QVERIFY(!readTrustedFile(dir, u"huge.conf"_s, me).error.isEmpty()); // a group-writable directory
        QVERIFY(!chmod(QFile::encodeName(dir).constData(), 0700));
        QVERIFY(!readTrustedFile(dir + u"/absent"_s, u"alice.conf"_s, me).exists); // no directory, no overrides
    }
};

QTEST_APPLESS_MAIN(VirtualSystemBusTest)
#include "VirtualSystemBusTest.moc"
