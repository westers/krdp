// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualMaintenanceCommand.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTest>
#include <unistd.h>

class VirtualMaintenanceCommandTest : public QObject {
    Q_OBJECT
    void run(const QStringList &arguments, int expected, const QByteArray &message) {
        QProcess command;
        command.start(QCoreApplication::applicationDirPath() + QStringLiteral("/krdp-virtual-maintenance"), arguments);
        QVERIFY(command.waitForFinished(3000));
        QCOMPARE(command.exitStatus(), QProcess::NormalExit); QCOMPARE(command.exitCode(), expected);
        QVERIFY(command.readAllStandardOutput().isEmpty()); QVERIFY(command.readAllStandardError().contains(message));
    }
private Q_SLOTS:
    void closedGrammar() {
        run({}, 64, "Usage:");
        for (const auto *word : {"clean", "bootstrap", "force-clean", "--help", "STATUS", "sh"})
            run({QString::fromLatin1(word)}, 64, "Usage:");
        run({QStringLiteral("status"), QStringLiteral("/tmp/alternate")}, 64, "Usage:");
        run({QStringLiteral("invalidate"), QStringLiteral("--force")}, 64, "Usage:");
    }
    void ordinaryUserCannotAccessHostState() {
        if (!getuid() || !geteuid()) QSKIP("Host-access refusal test requires ordinary user");
        run({QStringLiteral("status")}, 1, "explicit root caller required");
        run({QStringLiteral("invalidate")}, 1, "explicit root caller required");
    }
    void cleanStatusDoesNotClaimAdmission() {
        KRdp::VirtualSessionMaintenanceGuard::Diagnostic value;
        value.record.phase = KRdp::VirtualSessionMaintenanceRecord::Phase::Clean;
        value.currentBoot = true;
        const auto json = QJsonDocument::fromJson(KRdp::maintenanceStatusJson(value)).object();
        QCOMPARE(json[QStringLiteral("phase")].toString(), QStringLiteral("clean"));
        QCOMPARE(json[QStringLiteral("admission")].toString(), QStringLiteral("not-evaluated"));
        QVERIFY(json[QStringLiteral("currentBoot")].toBool());
    }
};
QTEST_GUILESS_MAIN(VirtualMaintenanceCommandTest)
#include "VirtualMaintenanceCommandTest.moc"
