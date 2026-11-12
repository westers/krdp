// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX F2: the configured systemd units restart a crashed server, and a
// start limit stops a crash loop. On Sol krdpserver exited 255 on every
// disconnect and Restart=on-abnormal left it down (a non-zero exit is a
// failure, not "abnormal").

#include <QFile>
#include <QMultiHash>
#include <QTest>

namespace
{
// "Section/Key" -> values, in file order.
QMultiHash<QString, QString> parseUnit(const QString &path)
{
    QMultiHash<QString, QString> result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return result;
    }
    QString section;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';'))) {
            continue;
        }
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2);
            continue;
        }
        const auto eq = line.indexOf(QLatin1Char('='));
        if (eq > 0) {
            result.insert(section + QLatin1Char('/') + line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
        }
    }
    return result;
}

QString unitPath(const char *name)
{
    return QStringLiteral(KRDP_UNIT_DIR "/") + QString::fromLatin1(name);
}
}

class SystemdUnitPolicyTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void restartsOnFailureWithALimit_data()
    {
        QTest::addColumn<QString>("unit");
        QTest::newRow("user krdpserver") << unitPath("app-io.github.westers.farside.server.service");
        QTest::newRow("console host") << unitPath("krdp-console-host.service");
        QTest::newRow("virtual host") << unitPath("krdp-virtual-host.service");
    }

    void restartsOnFailureWithALimit()
    {
        QFETCH(QString, unit);
        const auto keys = parseUnit(unit);
        QVERIFY2(!keys.isEmpty(), qPrintable(unit));

        // on-failure covers a non-zero exit (255) as well as signals.
        const QString restart = keys.value(QStringLiteral("Service/Restart"));
        QVERIFY2(restart == QLatin1String("on-failure") || restart == QLatin1String("always"), qPrintable(restart));

        bool ok = false;
        const int restartSec = keys.value(QStringLiteral("Service/RestartSec")).toInt(&ok);
        QVERIFY(ok);
        QVERIFY(restartSec >= 1);

        // The start limit belongs in [Unit]; systemd ignores it in [Service] on current versions.
        const int interval = keys.value(QStringLiteral("Unit/StartLimitIntervalSec")).toInt(&ok);
        QVERIFY(ok);
        const int burst = keys.value(QStringLiteral("Unit/StartLimitBurst")).toInt(&ok);
        QVERIFY(ok);
        QVERIFY(burst >= 2);
        // The limit must be reachable: burst restarts, spaced by RestartSec, fit in the interval.
        QVERIFY2(burst * restartSec < interval, "restart loop never hits the start limit");
        QVERIFY(!keys.contains(QStringLiteral("Service/StartLimitIntervalSec")));
        QVERIFY(!keys.contains(QStringLiteral("Service/StartLimitBurst")));
    }

    // OPT-055 K6.1: the user server and the console host are watched; the virtual host is not yet
    // (its unit runs under `env -i`, which strips NOTIFY_SOCKET; wave 2 X4).
    void watchdogOnUserServerAndConsoleHostOnly_data()
    {
        QTest::addColumn<QString>("unit");
        QTest::addColumn<bool>("watched");
        QTest::newRow("user farside-server") << unitPath("app-io.github.westers.farside.server.service") << true;
        QTest::newRow("console host") << unitPath("krdp-console-host.service") << true;
        QTest::newRow("virtual host") << unitPath("krdp-virtual-host.service") << false;
    }

    void watchdogOnUserServerAndConsoleHostOnly()
    {
        QFETCH(QString, unit);
        QFETCH(bool, watched);
        const auto keys = parseUnit(unit);
        QVERIFY2(!keys.isEmpty(), qPrintable(unit));
        QCOMPARE(keys.contains(QStringLiteral("Service/WatchdogSec")), watched);
        if (!watched) {
            QVERIFY(!keys.contains(QStringLiteral("Service/NotifyAccess")));
            QVERIFY(!keys.contains(QStringLiteral("Service/TimeoutAbortSec")));
            return;
        }
        QCOMPARE(keys.values(QStringLiteral("Service/WatchdogSec")), QStringList{QStringLiteral("90s")});
        QCOMPARE(keys.values(QStringLiteral("Service/NotifyAccess")), QStringList{QStringLiteral("main")});
        QCOMPARE(keys.values(QStringLiteral("Service/TimeoutAbortSec")), QStringList{QStringLiteral("30s")});
        // The ping is sent by the daemon's own main thread, so the unit must be Type=exec (no READY=1).
        QCOMPARE(keys.value(QStringLiteral("Service/Type")), QStringLiteral("exec"));
    }

    // REVIEW-S2: sandboxing that is safe for BOTH brokers. Each directive is justified in
    // docs/unit-hardening.md; the "never" tests below say why the other directives are absent.
    void brokersCarrySeccompAndPrivilegeHardening_data()
    {
        QTest::addColumn<QString>("unit");
        QTest::newRow("console host") << unitPath("krdp-console-host.service");
        QTest::newRow("virtual host") << unitPath("krdp-virtual-host.service");
    }

    void brokersCarrySeccompAndPrivilegeHardening()
    {
        QFETCH(QString, unit);
        const auto keys = parseUnit(unit);
        QVERIFY2(!keys.isEmpty(), qPrintable(unit));
        const auto only = [&keys](const char *key) { return keys.values(QStringLiteral("Service/") + QLatin1String(key)); };
        // The broker is root, so a setuid-root exec gains nothing it lacks; the console worker it starts
        // execs only a non-setuid kscreen-doctor (consoleworker.cpp), so inheriting the flag is harmless.
        QCOMPARE(only("NoNewPrivileges"), QStringList{QStringLiteral("yes")});
        QCOMPARE(only("LockPersonality"), QStringList{QStringLiteral("yes")});
        // PipeWire/rtkit take realtime through the rtkit daemon, never a direct sched_setscheduler of ours.
        QCOMPARE(only("RestrictRealtime"), QStringList{QStringLiteral("yes")});
        QCOMPARE(only("RestrictSUIDSGID"), QStringList{QStringLiteral("yes")});
        // No unshare/clone(CLONE_NEW*)/setns in a broker or the console worker (bwrap lives in the separate session unit).
        QCOMPARE(only("RestrictNamespaces"), QStringList{QStringLiteral("yes")});
        QCOMPARE(only("SystemCallArchitectures"), QStringList{QStringLiteral("native")});
        // AF_UNIX: D-Bus, worker and guardian sockets. AF_INET/AF_INET6: RDP. AF_NETLINK: libudev monitor, getifaddrs, libaudit.
        QCOMPARE(only("RestrictAddressFamilies"), QStringList{QStringLiteral("AF_UNIX AF_INET AF_INET6 AF_NETLINK")});
    }

    // CapabilityBoundingSet is a deny-list of capabilities nothing in the broker (or FreeRDP, Qt, PAM, libva, libudev in it) uses.
    // An allow-list could not be proven from the code (PAM stacks and GPU drivers are site specific), so none is shipped.
    void brokersDropOnlyCapabilitiesNobodyUses_data()
    {
        brokersCarrySeccompAndPrivilegeHardening_data();
    }

    void brokersDropOnlyCapabilitiesNobodyUses()
    {
        QFETCH(QString, unit);
        const auto keys = parseUnit(unit);
        const auto sets = keys.values(QStringLiteral("Service/CapabilityBoundingSet"));
        QCOMPARE(sets.size(), 1);
        QVERIFY2(sets.constFirst().startsWith(QLatin1Char('~')), "must be a deny-list (leading ~)");
        const auto dropped = sets.constFirst().mid(1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QStringList expected{QStringLiteral("CAP_SYS_MODULE"), QStringLiteral("CAP_SYS_RAWIO"), QStringLiteral("CAP_SYS_BOOT"),
                                   QStringLiteral("CAP_SYS_TIME"), QStringLiteral("CAP_SYS_PACCT"), QStringLiteral("CAP_SYS_TTY_CONFIG"),
                                   QStringLiteral("CAP_LINUX_IMMUTABLE"), QStringLiteral("CAP_MKNOD"), QStringLiteral("CAP_WAKE_ALARM"),
                                   QStringLiteral("CAP_BLOCK_SUSPEND"), QStringLiteral("CAP_SYSLOG"), QStringLiteral("CAP_BPF"),
                                   QStringLiteral("CAP_PERFMON"), QStringLiteral("CAP_MAC_ADMIN"), QStringLiteral("CAP_MAC_OVERRIDE")};
        QCOMPARE(dropped, expected);
        // What the brokers do use (setuid/setgid/setfsuid, chown of worker dirs and certificates, reading other users'
        // /proc/<pid>/environ and root-only files, signalling workers, PAM audit, GPU and network setup) must stay.
        for (const char *needed : {"CAP_SETUID", "CAP_SETGID", "CAP_CHOWN", "CAP_FOWNER", "CAP_DAC_OVERRIDE", "CAP_DAC_READ_SEARCH", "CAP_KILL",
                                   "CAP_SYS_PTRACE", "CAP_SYS_ADMIN", "CAP_AUDIT_WRITE", "CAP_AUDIT_CONTROL", "CAP_NET_BIND_SERVICE",
                                   "CAP_NET_ADMIN", "CAP_NET_RAW", "CAP_SYS_NICE", "CAP_SYS_RESOURCE", "CAP_IPC_LOCK", "CAP_SETPCAP", "CAP_SETFCAP",
                                   "CAP_SYS_CHROOT"}) {
            QVERIFY2(!dropped.contains(QLatin1String(needed)), needed);
        }
    }

    // The virtual broker starts no child process of its own (desktops are separate systemd units it starts over D-Bus),
    // so it can take the mount-namespace directives. Writes: /etc/farside (certificate; QSaveFile in the same directory),
    // StateDirectory and /var/lib/farside-public (both outside /usr, /boot and /etc).
    void virtualBrokerHasFilesystemAndCoreProtection()
    {
        const auto keys = parseUnit(unitPath("krdp-virtual-host.service"));
        QVERIFY(!keys.isEmpty());
        const auto only = [&keys](const char *key) { return keys.values(QStringLiteral("Service/") + QLatin1String(key)); };
        QCOMPARE(only("ProtectSystem"), QStringList{QStringLiteral("full")});
        QCOMPARE(only("ReadWritePaths"), QStringList{QStringLiteral("-/etc/farside")});
        QCOMPARE(only("ProtectKernelTunables"), QStringList{QStringLiteral("yes")});
        QCOMPARE(only("ProtectKernelModules"), QStringList{QStringLiteral("yes")});
        QCOMPARE(only("ProtectKernelLogs"), QStringList{QStringLiteral("yes")});
        QCOMPARE(only("ProtectControlGroups"), QStringList{QStringLiteral("yes")});
        QCOMPARE(only("PrivateTmp"), QStringList{QStringLiteral("yes")});
        // The broker and its ExecStartPre helper dump no core; there is no worker below it that would inherit the limit.
        QCOMPARE(only("LimitCORE"), QStringList{QStringLiteral("0")});
        // Already present before this review, kept.
        QCOMPARE(only("UMask"), QStringList{QStringLiteral("0077")});
    }

    // The console broker's cgroup also holds the capture worker, which runs as the logged-in user (or the greeter user) and
    // inherits everything below: seccomp filters, the bounding set, rlimits, umask, the mount namespace and the device policy.
    // These are therefore left out on purpose (docs/unit-hardening.md has the evidence for each).
    void consoleBrokerLeavesWhatItsWorkerInheritsAlone()
    {
        const auto keys = parseUnit(unitPath("krdp-console-host.service"));
        QVERIFY(!keys.isEmpty());
        for (const char *absent : {
                 "LimitCORE", // the worker keeps its own cores (Buzz worker abort analysis); the broker uses PR_SET_DUMPABLE instead
                 "UMask", // inherited by the worker that creates the user's staged files
                 "PrivateTmp", // host-to-client file copy reads whatever file URIs the user copied, including /tmp
                 "ProtectSystem", "ProtectHome", "ReadWritePaths", "ReadOnlyPaths", "InaccessiblePaths", "TemporaryFileSystem", // worker shares the mount namespace
                 "ProtectKernelTunables", "ProtectKernelModules", "ProtectKernelLogs", "ProtectControlGroups", // mount namespace; covered by capability deny-list
                 "ProtectClock", // implies DeviceAllow and with it a device cgroup policy: would risk /dev/dri and /dev/input
                 "ProtectHostname", // frozen hostname copy for broker and worker; no code calls sethostname
                 "PrivateDevices", "DevicePolicy", "DeviceAllow", // render nodes (VAAPI/CUDA), evdev, v4l2 loopback
                 "MemoryDenyWriteExecute", // GPU drivers (Mesa/LLVM, NVIDIA) map writable+executable memory
                 "SystemCallFilter", // no strace-based inventory yet
                 "ProtectProc", "ProcSubset", // the launcher reads other users' /proc/<pid>/environ and cgroup
             }) {
            QVERIFY2(!keys.contains(QStringLiteral("Service/") + QLatin1String(absent)), absent);
        }
    }

    // The virtual broker also needs the GPU (encoder probe), so the device-policy and W^X directives stay out there too.
    void neitherBrokerRestrictsDevicesOrMemoryExecution()
    {
        for (const char *name : {"krdp-console-host.service", "krdp-virtual-host.service"}) {
            const auto keys = parseUnit(unitPath(name));
            QVERIFY2(!keys.isEmpty(), name);
            for (const char *absent : {"PrivateDevices", "DevicePolicy", "DeviceAllow", "ProtectClock", "ProtectHome", "MemoryDenyWriteExecute", "SystemCallFilter",
                                       "ProtectProc", "ProcSubset"}) {
                QVERIFY2(!keys.contains(QStringLiteral("Service/") + QLatin1String(absent)), qPrintable(QLatin1String(name) + QLatin1Char(' ') + QLatin1String(absent)));
            }
        }
    }

    // The retained desktop unit runs a whole Plasma session (bwrap, unshare, PAM, setuid helpers, a browser): only a generous
    // task cap, which a fork bomb reaches long before the pid space and a normal desktop never does.
    void retainedDesktopHasOnlyAGenerousTaskCap()
    {
        const auto keys = parseUnit(unitPath("krdp-virtual-session@.service"));
        QVERIFY(!keys.isEmpty());
        bool ok = false;
        const int tasks = keys.value(QStringLiteral("Service/TasksMax")).toInt(&ok);
        QVERIFY(ok);
        QVERIFY2(tasks >= 65536, "a browser-heavy desktop has tens of thousands of threads");
        for (const char *absent : {"MemoryMax", "MemoryHigh", "MemorySwapMax", "CPUQuota", "NoNewPrivileges", "RestrictNamespaces", "RestrictSUIDSGID", "RestrictAddressFamilies",
                                   "CapabilityBoundingSet", "ProtectSystem", "ProtectHome", "PrivateTmp", "PrivateDevices", "SystemCallFilter", "LockPersonality",
                                   "RestrictRealtime", "MemoryDenyWriteExecute", "ProtectKernelTunables", "ProtectControlGroups", "LimitCORE"}) {
            QVERIFY2(!keys.contains(QStringLiteral("Service/") + QLatin1String(absent)), absent);
        }
    }

    // A retained desktop is adopted by its broker, never restarted by systemd.
    void retainedDesktopIsNotRestarted()
    {
        const auto keys = parseUnit(unitPath("krdp-virtual-session@.service"));
        QVERIFY(!keys.isEmpty());
        QCOMPARE(keys.value(QStringLiteral("Service/Restart")), QStringLiteral("no"));
    }
};

QTEST_GUILESS_MAIN(SystemdUnitPolicyTest)

#include "SystemdUnitPolicyTest.moc"
