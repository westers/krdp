// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-D4: the pure parts of the stock-client path on the virtual broker:
// VirtualStockClientPolicy, the standard error codes, the first layout from
// standard monitor data, and the MS-RDPEDISP resize size.

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <freerdp/error.h>
#include <sys/stat.h>
#include <unistd.h>

#include "VirtualStockClient.h"

using namespace KRdp;
using namespace Qt::StringLiterals;
using Policy = VirtualStockClient::Policy;

class VirtualStockClientTest : public QObject
{
    Q_OBJECT
    static ClientDisplay::Info display(QSize desktop, QVector<VideoMonitor> monitors = {})
    {
        return {desktop, monitors};
    }

private Q_SLOTS:
    void policyValues()
    {
        QCOMPARE(VirtualStockClient::parsePolicy(u"attach-or-create"_s), std::optional(Policy::AttachOrCreate));
        QCOMPARE(VirtualStockClient::parsePolicy(u" refuse "_s), std::optional(Policy::Refuse));
        QVERIFY(!VirtualStockClient::parsePolicy(u"Refuse"_s));
        QVERIFY(!VirtualStockClient::parsePolicy(QString()));
    }

    void policyFromConfig()
    {
        QCOMPARE(VirtualStockClient::policyFromConfig({}), Policy::AttachOrCreate);
        QCOMPARE(VirtualStockClient::policyFromConfig("[General]\nListenPort=3389\nVirtualStockClientPolicy=refuse\n"), Policy::Refuse);
        QCOMPARE(VirtualStockClient::policyFromConfig("[General]\nVirtualStockClientPolicy[$i]=refuse\n"), Policy::Refuse);
        // Only the [General] group counts, and an unknown value is the default.
        QCOMPARE(VirtualStockClient::policyFromConfig("[Other]\nVirtualStockClientPolicy=refuse\n"), Policy::AttachOrCreate);
        QCOMPARE(VirtualStockClient::policyFromConfig("[General]\nVirtualStockClientPolicy=maybe\n"), Policy::AttachOrCreate);
        // The last assignment wins, as in KConfig.
        QCOMPARE(VirtualStockClient::policyFromConfig("[General]\nVirtualStockClientPolicy=refuse\nVirtualStockClientPolicy=attach-or-create\n"),
                 Policy::AttachOrCreate);
    }

    void policyFileIsReadSafely()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(u"krdpserverrc"_s);
        QCOMPARE(VirtualStockClient::readPolicyFile(path, getuid()), Policy::AttachOrCreate); // missing
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[General]\nVirtualStockClientPolicy=refuse\n");
        file.close();
        QCOMPARE(VirtualStockClient::readPolicyFile(path, getuid()), Policy::Refuse);
        // Owned by someone else: ignored.
        QCOMPARE(VirtualStockClient::readPolicyFile(path, getuid() + 1), Policy::AttachOrCreate);
        // A symlink as the final component is never followed.
        const QString link = dir.filePath(u"link"_s);
        QVERIFY(QFile::link(path, link));
        QCOMPARE(VirtualStockClient::readPolicyFile(link, getuid()), Policy::AttachOrCreate);
        // A FIFO neither blocks the broker nor counts.
        const QString fifo = dir.filePath(u"fifo"_s);
        QCOMPARE(mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(VirtualStockClient::readPolicyFile(fifo, getuid()), Policy::AttachOrCreate);
        QVERIFY(timer.elapsed() < 1000);
        // A directory is not a config file.
        QCOMPARE(VirtualStockClient::readPolicyFile(dir.path(), getuid()), Policy::AttachOrCreate);
    }

    void standardErrorCodes()
    {
        using Refusal = VirtualStockClient::Refusal;
        QCOMPARE(VirtualStockClient::errorInfo(Refusal::Policy), quint32(ERRINFO_SERVER_DENIED_CONNECTION));
        QCOMPARE(VirtualStockClient::errorInfo(Refusal::NoFreeSlot), quint32(ERRINFO_CB_DESTINATION_POOL_NOT_FREE));
        QCOMPARE(VirtualStockClient::errorInfo(Refusal::StartFailed), quint32(ERRINFO_CB_SESSION_ONLINE_VM_SESSMON_FAILED));
        QCOMPARE(VirtualStockClient::errorInfo(Refusal::StartTimeout), quint32(ERRINFO_CB_SESSION_ONLINE_VM_BOOT_TIMEOUT));
        QCOMPARE(VirtualStockClient::errorInfo(Refusal::Displaced), quint32(ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION));
        // Never the code KRDPCTL-V2 reserves for a rejected password.
        for (const auto refusal : {Refusal::Policy, Refusal::NoFreeSlot, Refusal::StartFailed, Refusal::StartTimeout, Refusal::Displaced})
            QVERIFY(VirtualStockClient::errorInfo(refusal) != quint32(ERRINFO_SERVER_INSUFFICIENT_PRIVILEGES));
    }

    void singleOutputFromCoreDesktopSize()
    {
        const VirtualStockClient::Limits limits{.maxOutputs = 16};
        auto outputs = VirtualStockClient::initialOutputs(display(QSize(1366, 768)), limits);
        QCOMPARE(outputs.size(), 1);
        QCOMPARE(outputs[0].pixels, QSize(1366, 768));
        QCOMPARE(outputs[0].position, QPoint(0, 0));
        QCOMPARE(outputs[0].scale, 1.0);
        QVERIFY(outputs[0].primary);
        // Odd sizes round down to even; too large is clamped, keeping the aspect's width limit.
        outputs = VirtualStockClient::initialOutputs(display(QSize(1365, 767)), limits);
        QCOMPARE(outputs[0].pixels, QSize(1364, 766));
        outputs = VirtualStockClient::initialOutputs(display(QSize(5120, 1440)), limits);
        QCOMPARE(outputs[0].pixels, QSize(4096, 1440));
        // No size at all: the fallback.
        outputs = VirtualStockClient::initialOutputs(display(QSize()), limits);
        QCOMPARE(outputs[0].pixels, QSize(1920, 1080));
        // A backend with a smaller per-output limit.
        outputs = VirtualStockClient::initialOutputs(display(QSize(3840, 2160)), {.maxOutputs = 16, .maxOutputDimension = 2560});
        QCOMPARE(outputs[0].pixels, QSize(2560, 2160));
    }

    void multiMonitorFromMcsMonitorData()
    {
        // TS_UD_CS_MONITOR: a secondary left of the primary, in client coordinates.
        const auto info = display(QSize(3840, 1080),
            {{QRect(-1920, 0, 1920, 1080), false}, {QRect(0, 0, 1920, 1080), true}});
        auto outputs = VirtualStockClient::initialOutputs(info, {.maxOutputs = 16});
        QCOMPARE(outputs.size(), 2);
        QVERIFY(outputs[0].primary); // the primary first, as selected-screen create wants it
        QCOMPARE(outputs[0].position, QPoint(1920, 0));
        QCOMPARE(outputs[0].pixels, QSize(1920, 1080));
        QVERIFY(!outputs[1].primary);
        QCOMPARE(outputs[1].position, QPoint(0, 0));
        // Selected-screen create unavailable (or too few outputs allowed): one output at the union.
        outputs = VirtualStockClient::initialOutputs(info, {.maxOutputs = 1});
        QCOMPARE(outputs.size(), 1);
        QCOMPARE(outputs[0].pixels, QSize(3840, 1080));
        outputs = VirtualStockClient::initialOutputs(info, {.maxOutputs = 0});
        QCOMPARE(outputs.size(), 1);
        // An unusable list (overlapping) falls back to the core desktop size.
        outputs = VirtualStockClient::initialOutputs(display(QSize(1920, 1080),
            {{QRect(0, 0, 1920, 1080), true}, {QRect(100, 0, 1920, 1080), false}}), {.maxOutputs = 16});
        QCOMPARE(outputs.size(), 1);
        QCOMPARE(outputs[0].pixels, QSize(1920, 1080));
    }

    void displayControlSize()
    {
        QCOMPARE(VirtualStockClient::displayControlSize({{QRect(0, 0, 1600, 900), true}}), std::optional(QSize(1600, 900)));
        QCOMPARE(VirtualStockClient::displayControlSize({{QRect(0, 0, 1601, 901), true}}), std::optional(QSize(1600, 900)));
        QCOMPARE(VirtualStockClient::displayControlSize({{QRect(0, 0, 8000, 100), true}}), std::optional(QSize(4096, 200)));
        QVERIFY(!VirtualStockClient::displayControlSize({}));
        QVERIFY(!VirtualStockClient::displayControlSize({{QRect(0, 0, 1920, 1080), true}, {QRect(1920, 0, 1920, 1080), false}}));
    }
};

QTEST_GUILESS_MAIN(VirtualStockClientTest)
#include "VirtualStockClientTest.moc"
