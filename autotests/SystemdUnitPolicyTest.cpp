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
