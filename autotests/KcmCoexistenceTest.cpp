// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// FARSIDE-KCM: telling Farside and KDE's own remote desktop apart (who holds
// the port, which units run and start at login), the page's three actions,
// and the host checks that decide which choices the page offers.

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "coexistence.h"
#include "farsideidentity.h"
#include "hostdevices.h"
#include "krdpserversettings.h"
#include "settingsdefaults.h"

using namespace Qt::StringLiterals;
using namespace Coexistence;

namespace
{
const QString serverUnit = u"app-io.github.westers.farside.server.service"_s;
const QString stockUnit = u"app-org.kde.krdpserver.service"_s;
const QString serverExe = u"/usr/bin/farside-server"_s;
const QString stockExe = u"/usr/bin/krdpserver"_s;

// The identity after the rename stage, when the two can be told apart.
Farside::Identity renamed()
{
    Farside::Identity identity;
    identity.serverUnit = serverUnit;
    identity.serverExecutables = {serverExe};
    identity.stockUnit = stockUnit;
    identity.stockExecutables = {stockExe};
    identity.stockPort = 3389;
    identity.virtualHostUnit = u"farside-virtual-host.service"_s;
    identity.reservedPorts = {3391, 3395};
    return identity;
}

// A fake /proc: net/tcp{,6} rows and processes with fd, exe and comm.
class FakeProc
{
public:
    FakeProc()
    {
        QDir(root()).mkpath(u"net"_s);
        writeTables();
    }
    QString root() const
    {
        return m_dir.path();
    }
    // A listening socket on `port`; `pid` 0 leaves it without a process
    // (another user's).
    void listen(quint16 port, quint64 inode, qint64 pid, const QString &exe, bool ipv6 = false)
    {
        m_rows << Row{port, inode, ipv6, u"0A"_s};
        if (pid > 0) {
            const QString dir = root() + u"/%1"_s.arg(pid);
            QDir().mkpath(dir + u"/fd"_s);
            QFile::link(u"socket:[%1]"_s.arg(inode), dir + u"/fd/7"_s);
            QFile::link(u"/dev/null"_s, dir + u"/fd/0"_s);
            QFile::link(exe, dir + u"/exe"_s);
            QFile comm(dir + u"/comm"_s);
            QVERIFY(comm.open(QIODevice::WriteOnly));
            comm.write(exe.section(u'/', -1).toUtf8() + '\n');
        }
        writeTables();
    }
    // An established (not listening) connection on `port`.
    void connection(quint16 port, quint64 inode)
    {
        m_rows << Row{port, inode, false, u"01"_s};
        writeTables();
    }

private:
    struct Row {
        quint16 port;
        quint64 inode;
        bool ipv6;
        QString state;
    };
    void writeTables()
    {
        for (const bool v6 : {false, true}) {
            QFile file(root() + (v6 ? u"/net/tcp6"_s : u"/net/tcp"_s));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
            int sl = 0;
            for (const auto &row : std::as_const(m_rows)) {
                if (row.ipv6 != v6) {
                    continue;
                }
                const QString address = v6 ? u"00000000000000000000000000000000"_s : u"00000000"_s;
                const QString port = QString::number(row.port, 16).toUpper().rightJustified(4, u'0');
                const QString line = u"   %1: %2:%3 %4:0000 %5 00000000:00000000 00:00000000 00000000  1000        0 %6 1 0000000000000000 100 0 0 10 0\n"_s.arg(sl++)
                                         .arg(address, port, address, row.state)
                                         .arg(row.inode);
                file.write(line.toLatin1());
            }
        }
    }
    QTemporaryDir m_dir;
    QList<Row> m_rows;
};

// Records what the page asked systemd to do, and answers asynchronously.
class FakeServiceManager : public ServiceManager
{
public:
    QHash<QString, UnitState> units;
    QHash<QString, QString> failStop;
    QHash<QString, QString> failDisable;
    QStringList installedSystemUnits;
    QStringList log;

    void queryUnit(const QString &unit, StateDone done) override
    {
        log << u"query "_s + unit;
        const auto state = units.value(unit);
        QTimer::singleShot(0, [state, done]() {
            done(state);
        });
    }
    void stopUnit(const QString &unit, Done done) override
    {
        log << u"stop "_s + unit;
        const QString error = failStop.value(unit);
        if (error.isEmpty()) {
            units[unit].activeState = u"inactive"_s;
            units[unit].mainPid = 0;
        }
        QTimer::singleShot(0, [error, done]() {
            done(error);
        });
    }
    void disableUnitFile(const QString &unit, Done done) override
    {
        log << u"disable "_s + unit;
        const QString error = failDisable.value(unit);
        if (error.isEmpty()) {
            units[unit].unitFileState = u"disabled"_s;
        }
        QTimer::singleShot(0, [error, done]() {
            done(error);
        });
    }
    bool systemUnitInstalled(const QString &unit) const override
    {
        return installedSystemUnits.contains(unit);
    }
};

UnitState unit(const QString &active, const QString &file, qint64 pid = 0)
{
    return UnitState{true, active, file, pid};
}

struct Rig {
    FakeProc proc;
    FakeServiceManager *manager = new FakeServiceManager;
    quint16 port = 3389;
    std::unique_ptr<Controller> controller;

    explicit Rig(const Farside::Identity &identity = renamed())
    {
        controller = std::make_unique<Controller>(std::unique_ptr<ServiceManager>(manager), identity, proc.root(), [this]() {
            return port;
        });
    }
    bool refresh()
    {
        QSignalSpy spy(controller.get(), &Controller::refreshed);
        controller->refresh();
        return spy.wait(2000);
    }
};
}

class KcmCoexistenceTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_config;

private Q_SLOTS:
    void initTestCase()
    {
        // A config directory of our own: KcmSettingsTest may run at the same time.
        QVERIFY(m_config.isValid());
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_config.path()));
    }

    // --- /proc ------------------------------------------------------------
    void scannerFindsTheListenerAndItsProcess()
    {
        FakeProc proc;
        proc.connection(3389, 11); // an established connection is not a listener
        ProcScanner scanner(proc.root());
        QVERIFY(!scanner.isListening(3389));

        proc.listen(3389, 4242, 1234, stockExe, /*ipv6=*/true);
        proc.listen(3390, 5151, 0, {}); // another user's process
        QVERIFY(scanner.isListening(3389));
        const auto holder = scanner.holder(3389);
        QVERIFY(holder.listening);
        QCOMPARE(holder.pid, 1234);
        QCOMPARE(holder.executable, stockExe);
        QCOMPARE(holder.name, u"krdpserver"_s);

        const auto unknown = scanner.holder(3390);
        QVERIFY(unknown.listening);
        QCOMPARE(unknown.pid, 0);
        QVERIFY(unknown.name.isEmpty());

        QVERIFY(!scanner.holder(3391).listening);
    }

    void firstFreePortSkipsBusyAndReservedPorts()
    {
        FakeProc proc;
        proc.listen(3390, 1, 0, {});
        ProcScanner scanner(proc.root());
        QCOMPARE(scanner.firstFreePort(3390, {3391}), quint16(3392));
        QCOMPARE(scanner.firstFreePort(3390, {}), quint16(3391));
    }

    void scannerToleratesAMissingProc()
    {
        ProcScanner scanner(u"/nonexistent-proc"_s);
        QVERIFY(!scanner.isListening(3389));
        QVERIFY(!scanner.holder(3389).listening);
    }

    // --- The decision -----------------------------------------------------
    void evaluate_data()
    {
        QTest::addColumn<bool>("listening");
        QTest::addColumn<qint64>("holderPid");
        QTest::addColumn<QString>("holderExe");
        QTest::addColumn<QString>("serverActive");
        QTest::addColumn<qint64>("serverPid");
        QTest::addColumn<QString>("stockActive");
        QTest::addColumn<QString>("stockFile");
        QTest::addColumn<qint64>("stockPid");
        QTest::addColumn<int>("port");
        QTest::addColumn<int>("expected");

        const int none = int(Conflict::None), stock = int(Conflict::StockHoldsPort), autostart = int(Conflict::StockAutostart), other = int(Conflict::OtherHoldsPort);
        //                                        listen  hpid  hexe       server       spid  stock        sfile        stpid port  expected
        QTest::newRow("free, stock idle") << false << qint64(0) << QString() << u"inactive"_s << qint64(0) << u"inactive"_s << u"disabled"_s << qint64(0) << 3389 << none;
        QTest::newRow("Farside holds it") << true << qint64(10) << serverExe << u"active"_s << qint64(10) << u"inactive"_s << u"disabled"_s << qint64(0) << 3389 << none;
        QTest::newRow("Farside, pid unknown to systemd") << true << qint64(10) << serverExe << u"active"_s << qint64(0) << u"inactive"_s << u"disabled"_s << qint64(0) << 3389 << none;
        QTest::newRow("stock by pid") << true << qint64(20) << u"/opt/x"_s << u"failed"_s << qint64(0) << u"active"_s << u"enabled"_s << qint64(20) << 3389 << stock;
        QTest::newRow("stock by binary") << true << qint64(21) << stockExe << u"inactive"_s << qint64(0) << u"inactive"_s << u"disabled"_s << qint64(0) << 3389 << stock;
        QTest::newRow("unreadable while stock runs") << true << qint64(0) << QString() << u"inactive"_s << qint64(0) << u"active"_s << u"disabled"_s << qint64(0) << 3389 << stock;
        QTest::newRow("unreadable, stock idle") << true << qint64(0) << QString() << u"inactive"_s << qint64(0) << u"inactive"_s << u"disabled"_s << qint64(0) << 3389 << other;
        QTest::newRow("another program") << true << qint64(30) << u"/usr/bin/python3"_s << u"inactive"_s << qint64(0) << u"inactive"_s << u"disabled"_s << qint64(0) << 3389 << other;
        QTest::newRow("stock starts at login") << false << qint64(0) << QString() << u"inactive"_s << qint64(0) << u"inactive"_s << u"enabled"_s << qint64(0) << 3389 << autostart;
        QTest::newRow("stock starts at login, Farside running") << true << qint64(10) << serverExe << u"active"_s << qint64(10) << u"inactive"_s << u"enabled"_s << qint64(0) << 3389 << autostart;
        QTest::newRow("stock starts at login, other port") << false << qint64(0) << QString() << u"inactive"_s << qint64(0) << u"inactive"_s << u"enabled"_s << qint64(0) << 3390 << none;
    }

    void evaluate()
    {
        QFETCH(bool, listening);
        QFETCH(qint64, holderPid);
        QFETCH(QString, holderExe);
        QFETCH(QString, serverActive);
        QFETCH(qint64, serverPid);
        QFETCH(QString, stockActive);
        QFETCH(QString, stockFile);
        QFETCH(qint64, stockPid);
        QFETCH(int, port);
        QFETCH(int, expected);

        Inputs inputs;
        inputs.port = quint16(port);
        inputs.holder = PortHolder{listening, holderPid, holderExe, holderExe.section(u'/', -1)};
        inputs.server = unit(serverActive, u"enabled"_s, serverPid);
        inputs.stock = unit(stockActive, stockFile, stockPid);
        const auto result = Coexistence::evaluate(renamed(), inputs);
        QCOMPARE(int(result.conflict), expected);
        if (result.conflict == Conflict::OtherHoldsPort && holderPid > 0) {
            QCOMPARE(result.holderName, holderExe.section(u'/', -1));
            QCOMPARE(result.holderPid, holderPid);
        }
    }

    void notInstalledStockIsNeverBlamed()
    {
        Inputs inputs;
        inputs.port = 3389;
        inputs.holder = PortHolder{false, 0, {}, {}};
        inputs.stock = UnitState{}; // unit file not found
        QCOMPARE(Coexistence::evaluate(renamed(), inputs).conflict, Conflict::None);
    }

    void beforeTheRenameOnlyThePortCounts()
    {
        // Today Farside still installs the stock names: the two can't be told
        // apart by unit or binary, so a stranger on the port is "another program".
        const auto identity = Farside::defaultIdentity();
        QVERIFY(!identity.stockIsDistinct());
        Inputs inputs;
        inputs.port = 3389;
        inputs.holder = PortHolder{true, 77, stockExe, u"krdpserver"_s};
        inputs.server = unit(u"inactive"_s, u"enabled"_s, 0);
        inputs.stock = unit(u"active"_s, u"enabled"_s, 77);
        QCOMPARE(Coexistence::evaluate(identity, inputs).conflict, Conflict::OtherHoldsPort);
        // ...and Farside's own running server is recognised.
        inputs.server = unit(u"active"_s, u"enabled"_s, 77);
        QCOMPARE(Coexistence::evaluate(identity, inputs).conflict, Conflict::None);
    }

    void identityNamesOnePlace()
    {
        const auto identity = Farside::defaultIdentity();
        QCOMPARE(identity.serverUnit, u"app-org.kde.krdpserver.service"_s);
        QCOMPARE(identity.stockUnit, u"app-org.kde.krdpserver.service"_s);
        QCOMPARE(identity.stockPort, quint16(3389));
        QVERIFY(identity.reservedPorts.contains(3391));
        QVERIFY(identity.reservedPorts.contains(3395));

        qputenv("FARSIDE_KCM_SERVER_UNIT", serverUnit.toUtf8());
        const auto overridden = Farside::defaultIdentity();
        qunsetenv("FARSIDE_KCM_SERVER_UNIT");
        QCOMPARE(overridden.serverUnit, serverUnit);
        QVERIFY(overridden.stockIsDistinct());
    }

    // --- The controller ---------------------------------------------------
    void controllerReportsStockOnThePort()
    {
        Rig rig;
        rig.proc.listen(3389, 99, 4321, stockExe);
        rig.manager->units[serverUnit] = unit(u"failed"_s, u"enabled"_s);
        rig.manager->units[stockUnit] = unit(u"active"_s, u"enabled"_s, 4321);
        QSignalSpy changed(rig.controller.get(), &Controller::changed);
        QVERIFY(rig.refresh());
        QCOMPARE(rig.controller->conflict(), Conflict::StockHoldsPort);
        QCOMPARE(rig.controller->stateName(), u"stock"_s);
        QCOMPARE(rig.controller->holderName(), u"krdpserver"_s);
        QCOMPARE(rig.controller->port(), 3389);
        QCOMPARE(rig.controller->alternativePort(), 3390);
        QVERIFY(rig.controller->stockInstalled());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(rig.manager->log, QStringList({u"query "_s + serverUnit, u"query "_s + stockUnit}));

        // Nothing changed: no second `changed`.
        QVERIFY(rig.refresh());
        QCOMPARE(changed.count(), 1);
    }

    void stopItAndStartFarside()
    {
        Rig rig;
        rig.proc.listen(3389, 99, 4321, stockExe);
        rig.manager->units[serverUnit] = unit(u"inactive"_s, u"enabled"_s);
        rig.manager->units[stockUnit] = unit(u"active"_s, u"enabled"_s, 4321);
        QVERIFY(rig.refresh());

        QSignalSpy start(rig.controller.get(), &Controller::startServerRequested);
        QSignalSpy busy(rig.controller.get(), &Controller::busyChanged);
        rig.manager->log.clear();
        rig.controller->stopStockAndStartServer();
        QVERIFY(rig.controller->busy());
        // A second click while stopping does nothing.
        rig.controller->stopStockAndStartServer();
        QVERIFY(start.wait(2000));
        QCOMPARE(start.count(), 1);
        QVERIFY(!rig.controller->busy());
        QCOMPARE(busy.count(), 2);
        QCOMPARE(rig.manager->log.first(), u"stop "_s + stockUnit);
        QCOMPARE(rig.manager->log.count(u"stop "_s + stockUnit), 1);
        QVERIFY(rig.controller->lastError().isEmpty());
        // Stock stays enabled at login, so the page goes on to offer turning that off.
        QVERIFY(QTest::qWaitFor([&]() {
            return rig.manager->log.contains(u"query "_s + stockUnit);
        }));
    }

    void stopFailureIsReportedAndFarsideNotStarted()
    {
        Rig rig;
        rig.proc.listen(3389, 99, 4321, stockExe);
        rig.manager->units[stockUnit] = unit(u"active"_s, u"enabled"_s, 4321);
        rig.manager->failStop[stockUnit] = u"Access denied"_s;
        QVERIFY(rig.refresh());
        QSignalSpy start(rig.controller.get(), &Controller::startServerRequested);
        QSignalSpy error(rig.controller.get(), &Controller::lastErrorChanged);
        rig.controller->stopStockAndStartServer();
        QVERIFY(error.wait(2000));
        QCOMPARE(rig.controller->lastError(), u"Access denied"_s);
        QCOMPARE(start.count(), 0);
        QVERIFY(!rig.controller->busy());
    }

    void stopDoesNothingBeforeTheRename()
    {
        Rig rig(Farside::defaultIdentity());
        rig.controller->stopStockAndStartServer();
        rig.controller->disableStockAutostart();
        QVERIFY(!rig.controller->busy());
        QVERIFY(rig.manager->log.isEmpty());
    }

    void usePortN()
    {
        Rig rig;
        rig.proc.listen(3389, 99, 4321, stockExe);
        rig.proc.listen(3390, 98, 0, {}); // taken: the suggestion moves on
        rig.manager->units[stockUnit] = unit(u"active"_s, u"enabled"_s, 4321);
        QVERIFY(rig.refresh());
        QCOMPARE(rig.controller->alternativePort(), 3392); // 3391 is the console service's

        QSignalSpy port(rig.controller.get(), &Controller::portChangeRequested);
        rig.controller->useAlternativePort();
        QCOMPARE(port.count(), 1);
        QCOMPARE(port.first().first().toInt(), 3392);

        // After the page saved the new port, the conflict is gone.
        rig.port = 3392;
        QVERIFY(rig.refresh());
        QCOMPARE(rig.controller->conflict(), Conflict::None);
        QCOMPARE(rig.controller->port(), 3392);
        QCOMPARE(rig.controller->alternativePort(), 3393);
    }

    void usePortForAnotherProgram()
    {
        Rig rig;
        rig.proc.listen(3389, 99, 555, u"/usr/bin/xrdp"_s);
        QVERIFY(rig.refresh());
        QCOMPARE(rig.controller->conflict(), Conflict::OtherHoldsPort);
        QCOMPARE(rig.controller->holderName(), u"xrdp"_s);
        QVERIFY(!rig.controller->stockInstalled());
        QSignalSpy port(rig.controller.get(), &Controller::portChangeRequested);
        rig.controller->useAlternativePort();
        QCOMPARE(port.count(), 1);
        QCOMPARE(port.first().first().toInt(), 3390);
    }

    void turnItsAutostartOff()
    {
        Rig rig;
        rig.manager->units[serverUnit] = unit(u"active"_s, u"enabled"_s, 10);
        rig.manager->units[stockUnit] = unit(u"inactive"_s, u"enabled"_s);
        rig.proc.listen(3389, 99, 10, serverExe);
        QVERIFY(rig.refresh());
        QCOMPARE(rig.controller->conflict(), Conflict::StockAutostart);
        QCOMPARE(rig.controller->stateName(), u"stock-autostart"_s);

        QSignalSpy refreshed(rig.controller.get(), &Controller::refreshed);
        rig.controller->disableStockAutostart();
        QVERIFY(refreshed.wait(2000));
        QVERIFY(rig.manager->log.contains(u"disable "_s + stockUnit));
        QCOMPARE(rig.controller->conflict(), Conflict::None);
        // Nothing else of stock's was touched.
        QVERIFY(!rig.manager->log.contains(u"stop "_s + stockUnit));
    }

    void turnItsAutostartOffFailure()
    {
        Rig rig;
        rig.manager->units[stockUnit] = unit(u"inactive"_s, u"enabled"_s);
        rig.manager->failDisable[stockUnit] = u"Read-only file system"_s;
        QVERIFY(rig.refresh());
        QSignalSpy refreshed(rig.controller.get(), &Controller::refreshed);
        rig.controller->disableStockAutostart();
        QVERIFY(refreshed.wait(2000));
        QCOMPARE(rig.controller->lastError(), u"Read-only file system"_s);
        QCOMPARE(rig.controller->conflict(), Conflict::StockAutostart);
    }

    void aLateReplyDoesNotOverwriteANewerOne()
    {
        Rig rig;
        rig.manager->units[stockUnit] = unit(u"inactive"_s, u"enabled"_s);
        QSignalSpy refreshed(rig.controller.get(), &Controller::refreshed);
        rig.controller->refresh();
        rig.controller->refresh();
        QVERIFY(refreshed.wait(2000));
        QTest::qWait(50);
        QCOMPARE(refreshed.count(), 1);
    }

    void virtualHostServiceDetection()
    {
        auto *manager = new FakeServiceManager;
        manager->installedSystemUnits = {u"farside-virtual-host.service"_s};
        FakeProc proc;
        Controller withService(std::unique_ptr<ServiceManager>(manager), renamed(), proc.root(), []() {
            return quint16(3389);
        });
        QVERIFY(withService.virtualHostInstalled());

        Controller without(std::make_unique<FakeServiceManager>(), renamed(), proc.root(), []() {
            return quint16(3389);
        });
        QVERIFY(!without.virtualHostInstalled());
    }

    // --- "Use Port N" saves at once -----------------------------------------
    void saveSettingNowKeepsOtherEditsUnsaved()
    {
        KRDPServerSettings settings(nullptr);
        settings.load();
        QSignalSpy portChanged(&settings, &KRDPServerSettings::ListenPortChanged);
        settings.setQuality(55); // an unapplied edit on the page
        QVERIFY(saveSettingNow(&settings, u"ListenPort"_s, 3390));
        QCOMPARE(settings.listenPort(), 3390);
        QCOMPARE(portChanged.count(), 1);
        QCOMPARE(settings.quality(), 55);
        QVERIFY(settings.isSaveNeeded()); // the quality edit is still pending

        KRDPServerSettings saved(nullptr);
        saved.load();
        QCOMPARE(saved.listenPort(), 3390);
        QCOMPARE(saved.quality(), saved.defaultQualityValue());

        QVERIFY(!saveSettingNow(&settings, u"NoSuchKey"_s, 1));
    }

    // --- Host checks ----------------------------------------------------------
    void vaapiDriversOnlyThoseInstalled()
    {
        QTemporaryDir a, b;
        for (const auto &path : {a.filePath(u"radeonsi_drv_video.so"_s), b.filePath(u"i965_drv_video.so"_s), b.filePath(u"nvidia_drv_video.so"_s)}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QCOMPARE(HostDevices::vaapiDrivers({a.path(), b.path()}), QStringList({u"radeonsi"_s, u"i965"_s}));
        QVERIFY(HostDevices::vaapiDrivers({u"/nonexistent"_s}).isEmpty());
    }

    void loopbackCamerasAreTheVirtualNodes()
    {
        QTemporaryDir sys;
        const QString devices = sys.path() + u"/devices"_s;
        const QString cls = sys.path() + u"/class/video4linux"_s;
        QVERIFY(QDir().mkpath(cls));
        // A USB camera, and two loopback devices (virtual, out of order).
        for (const auto &[node, parent, name] : {std::tuple{u"video0"_s, u"/pci0000:00/usb1/1-1/video4linux"_s, u"Integrated Camera"_s},
                                                std::tuple{u"video11"_s, u"/virtual/video4linux"_s, u"Farside Camera"_s},
                                                std::tuple{u"video2"_s, u"/virtual/video4linux"_s, u""_s}}) {
            const QString dir = devices + parent + u'/' + node;
            QVERIFY(QDir().mkpath(dir));
            QFile file(dir + u"/name"_s);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(name.toUtf8() + '\n');
            QVERIFY(QFile::link(dir, cls + u'/' + node));
        }
        const auto cameras = HostDevices::loopbackCameras(sys.path(), u"/dev"_s);
        QCOMPARE(cameras.size(), 2);
        QCOMPARE(cameras.at(0).path, u"/dev/video2"_s);
        QCOMPARE(cameras.at(1).path, u"/dev/video11"_s);
        QCOMPARE(cameras.at(1).name, u"Farside Camera"_s);
    }
};

QTEST_GUILESS_MAIN(KcmCoexistenceTest)
#include "KcmCoexistenceTest.moc"
