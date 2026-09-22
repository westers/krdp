// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <unistd.h>

class VirtualDeviceEntryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void nonrootCannotEnterOrExecute()
    {
        if (!getuid() || !geteuid()) QSKIP("This negative test must run as an ordinary user");
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto marker = temporary.filePath(QStringLiteral("must-not-exist"));
        const auto mountState = [] {
            QFile file(QStringLiteral("/proc/self/mountinfo"));
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        const auto before = mountState();
        QVERIFY(!before.isEmpty());
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(QStringLiteral(KRDP_DEVICE_ENTRY), {QStringLiteral("--uid"), QString::number(getuid()),
            QStringLiteral("--allow-render-pci"), QStringLiteral("0000:09:00.0"), QStringLiteral("--"),
            QStringLiteral("/usr/bin/touch"), marker});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(process.readAll().contains("requires an explicit root service invocation"));
        QVERIFY(!QFile::exists(marker));
        QCOMPARE(mountState(), before);
    }
};
QTEST_GUILESS_MAIN(VirtualDeviceEntryTest)
#include "VirtualDeviceEntryTest.moc"
