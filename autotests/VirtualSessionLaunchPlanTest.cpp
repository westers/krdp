// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "VirtualSessionLaunchPlan.h"
using namespace KRdp;
using namespace Qt::StringLiterals;

class VirtualSessionLaunchPlanTest : public QObject
{
    Q_OBJECT
    const QString id = u"a512ca25-048d-42f6-a2ed-a2bb2da22f22"_s;
    const VirtualSessionLaunchPlan::Account account{1000, u"user"_s, u"/home/user"_s};
    const VirtualSessionLaunchPlan::Configuration config{u"/usr/share/farside/virtual-session/launch-virtual-session.sh"_s,
        u"/usr/bin/farside-console-worker"_s, u"/usr/share/farside/virtual-session/support"_s,
        {u"0000:09:00.0"_s, u"0000:c5:00.0"_s}, {1280, 720}};
private Q_SLOTS:
    void recordedLaunchIdentityIsNotRegenerated()
    {
        const auto launch = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto first = VirtualSessionLaunchPlan::build(1000, account, id, config, nullptr, launch);
        const auto second = VirtualSessionLaunchPlan::build(1000, account, id, config, nullptr, launch);
        QVERIFY(first); QVERIFY(second);
        QCOMPARE(first->runtimeDirectory, QStringLiteral("/run/user/1000/farside-virtual/") + launch);
        QCOMPARE(first->runtimeDirectory, second->runtimeDirectory);
        QCOMPARE(first->arguments, second->arguments);
        QVERIFY(first->arguments.contains(first->runtimeDirectory));
        for (const auto &bad : {QStringLiteral("../old"), QStringLiteral("00000000-0000-0000-0000-000000000000"),
                QStringLiteral("{12345678-1234-1234-1234-123456789abc}")})
            QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, config, nullptr, bad));
    }
    void persistentProfileSeparateFromRuntime()
    {
        const auto plan = VirtualSessionLaunchPlan::build(1000, account, id, config);
        QVERIFY(plan);
        QVERIFY(plan->runtimeDirectory.startsWith(u"/run/user/1000/farside-virtual/"_s));
        QCOMPARE(plan->profileDirectory, u"/home/user/.farside-virtual/sessions/"_s + id);
        QVERIFY(!plan->environment.inheritsFromParent());
        QCOMPARE(plan->environment.keys().size(), 5);
        QVERIFY(!plan->environment.contains(u"DISPLAY"_s));
        QVERIFY(!plan->environment.contains(u"DBUS_SESSION_BUS_ADDRESS"_s));
        QVERIFY(!plan->environment.contains(u"PULSE_SERVER"_s));
        QVERIFY(!plan->arguments.contains(u"timeout"_s));
        QCOMPARE(plan->arguments.count(u"--allow-render-pci"_s), 2);
        const auto second = VirtualSessionLaunchPlan::build(1000, account, id, config);
        QCOMPARE(second->profileDirectory, plan->profileDirectory);
        QVERIFY(second->runtimeDirectory != plan->runtimeDirectory);
        QVERIFY(second->socketPath != plan->socketPath);
    }
    void refusesUntrustedIdentityAndPaths()
    {
        QVERIFY(!VirtualSessionLaunchPlan::build(0, account, id, config));
        QVERIFY(!VirtualSessionLaunchPlan::build(1001, account, id, config));
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, u"../physical"_s, config));
        auto changed = config;
        changed.launcher = u"relative-helper"_s;
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
        changed = config; changed.supportDirectory = u"/usr/share/../krdp"_s;
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
    }
    // OPT-075: golden launcher arguments for the system bus, and refusal of anything outside the grammar.
    void noSystemBusEmitsNoArguments()
    {
        const auto plan = VirtualSessionLaunchPlan::build(1000, account, id, config);
        QVERIFY(plan);
        for (const auto &argument : plan->arguments) QVERIFY2(!argument.contains(u"system-bus"_s) && !argument.contains(u"polkit"_s) && !argument.contains(u"cups"_s), qPrintable(argument));
        // The pre-OPT-075 golden is unchanged: five fixed pairs, two GPUs, nothing else.
        QCOMPARE(plan->arguments.size(), 16 + 4);
    }
    void filteredSystemBusArgumentsGolden()
    {
        auto changed = config;
        changed.systemBus = VirtualSystemBus::plan(VirtualSystemBus::defaults(), true, true);
        QCOMPARE(changed.systemBus.mode, VirtualSystemBus::Mode::Filtered);
        const auto launch = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto plan = VirtualSessionLaunchPlan::build(1000, account, id, changed, nullptr, launch);
        QVERIFY(plan);
        const auto base = VirtualSessionLaunchPlan::build(1000, account, id, config, nullptr, launch);
        QCOMPARE(plan->arguments.mid(0, base->arguments.size()), base->arguments); // appended, nothing reordered
        const auto added = plan->arguments.mid(base->arguments.size());
        QCOMPARE(added.mid(0, 2), QStringList({u"--system-bus"_s, u"filtered"_s}));
        QCOMPARE(added.size(), 2 + 2 * changed.systemBus.proxyRules.size());
        for (int i = 0; i < changed.systemBus.proxyRules.size(); ++i) {
            QCOMPARE(added[2 + 2 * i], u"--system-bus-rule"_s);
            QCOMPARE(added[3 + 2 * i], changed.systemBus.proxyRules[i]);
        }
        QVERIFY(plan->environment.keys().size() == 5); // the bus never travels in the environment
        QVERIFY(!plan->environment.contains(u"DBUS_SYSTEM_BUS_ADDRESS"_s));
        // polkit and CUPS flags are pairs too, because the launcher reads options two at a time.
        changed.systemBus.polkitAgent = changed.systemBus.cupsSocket = true;
        const auto flags = VirtualSessionLaunchPlan::build(1000, account, id, changed)->arguments;
        QVERIFY(flags.contains(u"--polkit-agent"_s) && flags[flags.indexOf(u"--polkit-agent"_s) + 1] == u"1"_s);
        QVERIFY(flags.contains(u"--cups-socket"_s) && flags[flags.indexOf(u"--cups-socket"_s) + 1] == u"1"_s);
    }
    void fullSystemBusHasNoRules()
    {
        auto changed = config;
        VirtualSystemBus::Settings settings;
        settings.mode = VirtualSystemBus::Mode::Full;
        changed.systemBus = VirtualSystemBus::plan(settings, true, true);
        const auto plan = VirtualSessionLaunchPlan::build(1000, account, id, changed);
        QVERIFY(plan);
        QVERIFY(plan->arguments.contains(u"--system-bus"_s));
        QVERIFY(!plan->arguments.contains(u"--system-bus-rule"_s));
        changed.systemBus.proxyRules = {u"--talk=org.bluez"_s};
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed)); // rules belong to filtered only
    }
    void systemBusRefusesAnythingOutsideTheGrammar()
    {
        auto changed = config;
        changed.systemBus = VirtualSystemBus::plan(VirtualSystemBus::defaults(), true, true);
        for (const auto &bad : {u"--filter"_s, u"--own=org.example.X"_s, u"--talk=org.x y"_s, u"--talk=org.x\n--own=y"_s, u"--fd=3"_s, u"--call=org.x.Y="_s}) {
            auto attempt = changed;
            attempt.systemBus.proxyRules.append(bad);
            QVERIFY2(!VirtualSessionLaunchPlan::build(1000, account, id, attempt), qPrintable(bad));
        }
        auto many = changed;
        while (many.systemBus.proxyRules.size() <= VirtualSystemBus::MaximumRules) many.systemBus.proxyRules.append(u"--talk=org.example.X"_s);
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, many));
        auto agent = config;
        agent.systemBus.polkitAgent = true; // a polkit agent without a bus is a contradiction
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, agent));
    }
    void gpuAccessMustBeExplicitAndStable()
    {
        auto changed = config;
        changed.allowedRenderPci.clear();
        QString error;
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed, &error));
        QVERIFY(error.contains(u"CPU-only"_s));
        changed.allowedRenderPci = {u"/dev/dri/renderD128"_s};
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
        changed.allowedRenderPci = {u"0000:09:00.0"_s, u"0000:09:00.0"_s};
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
        changed = config; changed.initialSize = QSize(4098, 720);
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
        changed.initialSize = QSize(1279, 720);
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
    }
};
QTEST_GUILESS_MAIN(VirtualSessionLaunchPlanTest)
#include "VirtualSessionLaunchPlanTest.moc"
