// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QSignalSpy>
#include "VirtualSessionGuardian.h"
#include "VirtualSessionHostController.h"
#include <QJsonArray>

namespace { int dismissalFailSync = 0; }
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
    if (dismissalFailSync > 0 && --dismissalFailSync == 0) { errno = EIO; return -1; }
    return __real_fsync(fd);
}

namespace KRdp
{
class VirtualSessionHostControllerTest : public QObject
{
    Q_OBJECT
    static QJsonObject command(const QString &action, const QString &session = {}) {
        QJsonObject r{{QStringLiteral("type"), QStringLiteral("virtual-session")}, {QStringLiteral("v"), 1},
            {QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("action"), action}};
        if (!session.isEmpty()) r.insert(QStringLiteral("session"), session);
        return r;
    }
private Q_SLOTS:
    void maintenanceAdmissionHasNoCreateSideEffects()
    {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        Server server; VirtualSessionHostController host(&server, {});
        QVERIFY(host.recover(*journal));
        bool permitted = false; int admissions = 0, commits = 0, starts = 0;
        QVERIFY(host.enableIndependentCreates(*journal, [&](const auto &, const auto &) { ++starts; return false; },
            [&](quint32 uid) { ++admissions; return uid == 1000 && permitted
                ? VirtualSessionHostController::CreateAdmission::Permitted : VirtualSessionHostController::CreateAdmission::Maintenance; }));
        const auto commit = host.m_commitIntent;
        host.m_commitIntent = [&](const auto &record) { ++commits; return commit(record); };
        const auto files = QDir(dir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        const auto request = command(QStringLiteral("create"));
        const auto refused = host.m_control.request(1000, 1, request);
        QVERIFY(!refused.value(QStringLiteral("ok")).toBool());
        QCOMPARE(refused.value(QStringLiteral("message")).toString(), QStringLiteral("session creation unavailable during maintenance"));
        QCOMPARE(host.m_control.request(1000, 1, request), refused); QCOMPARE(admissions, 1);
        QVERIFY(!host.m_control.request(1000, 1, command(QStringLiteral("create"))).value(QStringLiteral("ok")).toBool());
        QCOMPARE(admissions, 2); QCOMPARE(starts, 0); QCOMPARE(commits, 0);
        QVERIFY(journal->records()->isEmpty()); QVERIFY(host.m_supervisor.list(1000).isEmpty());
        QVERIFY(host.m_newIntents.empty()); QVERIFY(host.m_workers.empty()); QVERIFY(!host.m_creationBlocked);
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), files);
        permitted = true;
        QCOMPARE(host.m_control.request(1000, 1, request), refused); QCOMPARE(admissions, 2);
        for (int i = 0; i < 4; ++i) {
            const auto accepted = host.m_control.request(1000, 1, command(QStringLiteral("create")));
            QVERIFY(accepted.value(QStringLiteral("ok")).toBool());
            // Failed submission AFTER durable intent remains accepted, not retryable.
            QCOMPARE(accepted.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        }
        QCOMPARE(starts, 4); QCOMPARE(commits, 4); QCOMPARE(journal->records()->size(), 4);
        const auto full = host.m_control.request(1000, 1, command(QStringLiteral("create")));
        QCOMPARE(full.value(QStringLiteral("message")).toString(), QStringLiteral("session creation refused"));
        QCOMPARE(starts, 4); QCOMPARE(commits, 4);
    }
    void maintenanceAdmissionPrecedesReconciliation()
    {
        QTemporaryDir dir; auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        Server server; VirtualSessionHostController host(&server, {}); QVERIFY(host.recover(*journal));
        bool permitted = true;
        QVERIFY(host.enableIndependentCreates(*journal, [](const auto &, const auto &) { return false; }, [&] (quint32) {
            return permitted ? VirtualSessionHostController::CreateAdmission::Permitted : VirtualSessionHostController::CreateAdmission::Maintenance;
        }));
        QVERIFY(host.createIndependent(1000));
        const auto record = journal->records()->first();
        QVERIFY(journal->claimRecord(record, nullptr)); QVERIFY(journal->writeReconciled(record, nullptr));
        QVERIFY(journal->writeOrderedExit(record, nullptr));
        permitted = false;
        QCOMPARE(host.createIndependent(1000).refusal, VirtualSessionControl::CreateResult::Refusal::Maintenance);
        QCOMPARE(host.m_supervisor.list(1000).size(), 1); QCOMPARE(host.m_newIntents.size(), size_t(1));
        QCOMPARE(journal->records()->size(), 1);
        // Proves the row was eligible to retire, but denied create did not do it.
        host.reconcileCleanExits(); QVERIFY(host.m_supervisor.list(1000).isEmpty());
    }
    void maintenanceAdmissionDoesNotGateExistingSessionCommands()
    {
        QTemporaryDir dir; auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        Server server; VirtualSessionHostController host(&server, {}); QVERIFY(host.recover(*journal));
        int admissions = 0, starts = 0;
        QVERIFY(host.enableIndependentCreates(*journal, [&](const auto &, const auto &) { ++starts; return false; }, [&](quint32) {
            ++admissions; return VirtualSessionHostController::CreateAdmission::Maintenance;
        }));
        // Existing-session control fixture only: a local sleeper plus synthetic
        // capture readiness, not a real guardian/PAM/media acceptance claim.
        host.m_supervisor.m_factory = [](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            return VirtualSessionSupervisor::Launch{QStringLiteral("/usr/bin/sleep"), {QStringLiteral("60")}, {}, {}};
        };
        const auto handle = host.m_supervisor.create(1000); QVERIFY(handle);
        bool ready = false; QTRY_VERIFY(ready || (ready = host.m_supervisor.captureReady(*handle)));
        QVERIFY(!host.m_control.request(1000, 1, command(QStringLiteral("create"))).value(QStringLiteral("ok")).toBool());
        const auto list = host.m_control.request(1000, 1, command(QStringLiteral("list")));
        QVERIFY(list.value(QStringLiteral("ok")).toBool()); QCOMPARE(list.value(QStringLiteral("sessions")).toArray().size(), 1);
        QVERIFY(host.m_control.request(1000, 1, command(QStringLiteral("attach"), handle->id)).value(QStringLiteral("ok")).toBool());
        QVERIFY(host.m_control.request(1000, 1, command(QStringLiteral("detach"))).value(QStringLiteral("ok")).toBool());
        host.m_control.disconnected(1);
        QVERIFY(host.m_control.request(1000, 2, command(QStringLiteral("attach"), handle->id)).value(QStringLiteral("ok")).toBool());
        QVERIFY(host.m_control.request(1000, 2, command(QStringLiteral("stop"), handle->id)).value(QStringLiteral("ok")).toBool());
        QTRY_COMPARE(host.m_supervisor.list(1000).first().phase, VirtualSessionState::Phase::Absent);
        QCOMPARE(admissions, 1); QCOMPARE(starts, 0); QVERIFY(journal->records()->isEmpty());
    }
    void maintenanceAdmissionCallbackLifetimeAndReentrancy_data()
    {
        QTest::addColumn<bool>("destroy"); QTest::newRow("destroy") << true; QTest::newRow("reenter") << false;
    }
    void maintenanceAdmissionCallbackLifetimeAndReentrancy()
    {
        QFETCH(bool, destroy);
        QTemporaryDir dir; auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        Server server; auto host = std::make_unique<VirtualSessionHostController>(&server, VirtualSessionHostController::Prepare{});
        QVERIFY(host->recover(*journal)); int admissions = 0, starts = 0; bool guarded = false; QList<QJsonObject> nested;
        QVERIFY(host->enableIndependentCreates(*journal, [&](const auto &, const auto &) { ++starts; return false; }, [&](quint32 uid) {
            ++admissions; guarded = host->m_control.dispatchActive();
            if (destroy) host.reset();
            else {
                nested.append(host->m_control.request(uid, 1, command(QStringLiteral("create"))));
                nested.append(host->m_control.request(1001, 2, command(QStringLiteral("create"))));
                host->m_admitCreate = {}; // Copied callback survives its own removal.
            }
            // A permissive result must still not continue after destruction.
            return destroy ? VirtualSessionHostController::CreateAdmission::Permitted : VirtualSessionHostController::CreateAdmission::Maintenance;
        }));
        const auto response = host->m_control.request(1000, 1, command(QStringLiteral("create")));
        QVERIFY(!response.value(QStringLiteral("ok")).toBool()); QVERIFY(guarded); QCOMPARE(admissions, 1); QCOMPARE(starts, 0);
        QVERIFY(journal->records()->isEmpty());
        if (destroy) { QVERIFY(!host); QVERIFY(response.value(QStringLiteral("message")).toString().contains(QStringLiteral("uncertain"))); }
        else {
            QCOMPARE(nested.size(), 2);
            for (const auto &reply : nested) QCOMPARE(reply.value(QStringLiteral("message")).toString(), QStringLiteral("virtual-session command in progress; retry"));
            QVERIFY(!host->m_control.dispatchActive()); QVERIFY(host->m_supervisor.list(1000).isEmpty());
            QVERIFY(host->m_control.request(1000, 1, command(QStringLiteral("create"))).value(QStringLiteral("ok")).toBool());
            QCOMPARE(starts, 1);
        }
    }
    void oldBootAndMalformedEvidenceStayConservative_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"old-boot", "missing-proof", "malformed-proof", "malformed-dismissal", "ordered-with-malformed-dismissal"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void oldBootAndMalformedEvidenceStayConservative()
    {
        QFETCH(QString, kind); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        QFile bootFile(QStringLiteral("/proc/sys/kernel/random/boot_id")); QVERIFY(bootFile.open(QIODevice::ReadOnly));
        const auto boot = QString::fromLatin1(bootFile.readAll()).trimmed();
        const VirtualSessionJournal::Record r{1000, uuid(), uuid(), uuid(), kind == QStringLiteral("old-boot") ? uuid() : boot, QByteArray(32, 't')};
        QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        if (kind != QStringLiteral("missing-proof")) QVERIFY(journal->writeReconciled(r, nullptr));
        if (kind == QStringLiteral("old-boot")) QVERIFY(journal->recordDismissed(r));
        if (kind == QStringLiteral("malformed-proof")) {
            QFile marker(dir.filePath(QStringLiteral(".reconciled-") + r.session));
            QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::Truncate)); marker.close();
        }
        if (kind.contains(QStringLiteral("malformed-dismissal"))) {
            QFile marker(dir.filePath(QStringLiteral(".dismissed-") + r.session));
            QVERIFY(marker.open(QIODevice::WriteOnly)); marker.close();
        }
        if (kind == QStringLiteral("ordered-with-malformed-dismissal")) QVERIFY(journal->writeOrderedExit(r, nullptr));
        Server server; VirtualSessionHostController host(&server, {});
        QVERIFY(host.recover(*journal)); QVERIFY(host.enableIndependentCreates(*journal, [](const auto &, const auto &) { return false; }));
        QVERIFY(!host.dismissalEligible(1000, r.session));
        const auto response = host.m_control.request(1000, 1, command(QStringLiteral("dismiss"), r.session));
        QVERIFY(!response.value(QStringLiteral("ok")).toBool());
        host.reconcileCleanExits();
        QCOMPARE(host.m_supervisor.list(1000).size(), kind == QStringLiteral("ordered-with-malformed-dismissal") ? 0 : 1);
        QCOMPARE(journal->records()->size(), 1); QVERIFY(!journal->claimRecord(r, nullptr));
    }
    void ownerDismissalReclaimsQuotaAndSurvivesRecovery()
    {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        Server server;
        auto host = std::make_unique<VirtualSessionHostController>(&server, VirtualSessionHostController::Prepare{});
        QVERIFY(host->recover(*journal));
        QVERIFY(host->enableIndependentCreates(*journal, [](const auto &, const auto &) { return false; }));
        for (int i = 0; i < 4; ++i) QVERIFY(host->createIndependent(1000));
        QVERIFY(!host->createIndependent(1000));
        const auto r = journal->records()->first();
        QVERIFY(journal->claimRecord(r, nullptr)); QVERIFY(journal->writeReconciled(r, nullptr));
        const auto list = host->m_control.request(1000, 1, command(QStringLiteral("list")));
        int eligible = 0;
        for (const auto &value : list.value(QStringLiteral("sessions")).toArray()) {
            const auto row = value.toObject(); QVERIFY(row.value(QStringLiteral("dismissible")).isBool());
            if (row.value(QStringLiteral("dismissible")).toBool()) { ++eligible; QCOMPARE(row.value(QStringLiteral("session")).toString(), r.session); }
        }
        QCOMPARE(eligible, 1);
        const auto wrong = host->m_control.request(1001, 2, command(QStringLiteral("dismiss"), r.session));
        const auto missing = host->m_control.request(1001, 2, command(QStringLiteral("dismiss"), QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(!wrong.value(QStringLiteral("ok")).toBool()); QCOMPARE(wrong.value(QStringLiteral("message")), missing.value(QStringLiteral("message")));
        const auto request = command(QStringLiteral("dismiss"), r.session);
        const auto reply = host->m_control.request(1000, 1, request);
        QVERIFY(reply.value(QStringLiteral("ok")).toBool()); QCOMPARE(reply.value(QStringLiteral("session")).toString(), r.session);
        QCOMPARE(reply.value(QStringLiteral("state")).toString(), QStringLiteral("dismissed"));
        QCOMPARE(host->m_supervisor.list(1000).size(), 4); // No inline retirement on reply stack.
        QCOMPARE(host->m_control.request(1000, 1, request), reply);
        QCOMPARE(journal->orderedExit(r), std::optional<bool>(false));
        QTRY_COMPARE(host->m_supervisor.list(1000).size(), 3);
        QVERIFY(host->createIndependent(1000)); QCOMPARE(journal->records()->size(), 5);
        QVERIFY(!journal->claimRecord(r, nullptr));
        host.reset();
        host = std::make_unique<VirtualSessionHostController>(&server, VirtualSessionHostController::Prepare{});
        QVERIFY(host->recover(*journal)); QCOMPARE(host->m_supervisor.list(1000).size(), 4);
        QVERIFY(host->enableIndependentCreates(*journal, [](const auto &, const auto &) { return false; }));
        for (const auto &entry : host->m_supervisor.list(1000)) QVERIFY(entry.id != r.session);
        const auto retry = host->m_control.request(1000, 3, command(QStringLiteral("dismiss"), r.session));
        QVERIFY(retry.value(QStringLiteral("ok")).toBool()); QCOMPARE(journal->records()->size(), 5);
        QVERIFY(!host->m_control.request(1001, 4, command(QStringLiteral("dismiss"), r.session)).value(QStringLiteral("ok")).toBool());
    }
    void dismissalRequiresFailedLiveState_data()
    {
        QTest::addColumn<QString>("phase");
        for (const auto *phase : {"starting", "retained", "attached", "stopping", "absent", "failed"})
            QTest::newRow(phase) << QString::fromLatin1(phase);
    }
    void dismissalRequiresFailedLiveState()
    {
        QFETCH(QString, phase); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        Server server; VirtualSessionHostController host(&server, {});
        QVERIFY(host.recover(*journal)); QVERIFY(host.enableIndependentCreates(*journal, [](const auto &, const auto &) { return true; }));
        const auto handle = host.createIndependent(1000); QVERIFY(handle);
        // Construct lifecycle states without falsely claiming real capture readiness.
        auto &registry = host.m_supervisor.m_registry;
        if (phase == QStringLiteral("retained") || phase == QStringLiteral("attached")) QVERIFY(registry.ready(*handle));
        if (phase == QStringLiteral("attached")) QVERIFY(registry.attach(1000, handle->id, 99));
        if (phase == QStringLiteral("stopping") || phase == QStringLiteral("absent")) QVERIFY(registry.stop(1000, handle->id));
        if (phase == QStringLiteral("absent")) QVERIFY(registry.exited(*handle));
        if (phase == QStringLiteral("failed")) QVERIFY(registry.unavailable(*handle));
        const auto r = journal->records()->first();
        QVERIFY(journal->claimRecord(r, nullptr)); QVERIFY(journal->writeReconciled(r, nullptr));
        QCOMPARE(host.dismissalEligible(1000, r.session), phase == QStringLiteral("failed"));
        const auto reply = host.m_control.request(1000, 1, command(QStringLiteral("dismiss"), r.session));
        QCOMPARE(reply.value(QStringLiteral("ok")).toBool(), phase == QStringLiteral("failed"));
        QCOMPARE(journal->dismissed(r), std::optional<bool>(phase == QStringLiteral("failed")));
    }
    void staleCleanupAndAbsentUndismissedHistoryCannotAuthorize()
    {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        Server server; VirtualSessionHostController host(&server, {});
        QVERIFY(host.recover(*journal)); QVERIFY(host.enableIndependentCreates(*journal, [](const auto &, const auto &) { return false; }));
        QVERIFY(host.createIndependent(1000));
        const auto r = journal->records()->first(); QVERIFY(journal->claimRecord(r, nullptr));
        QVERIFY(journal->writeReconciled(r, nullptr)); QVERIFY(host.dismissalEligible(1000, r.session));
        const QString path = dir.filePath(QStringLiteral(".reconciled-") + r.session);
        QFile proof(path); QVERIFY(proof.open(QIODevice::ReadOnly)); const auto bytes = proof.readAll(); proof.close();
        QVERIFY(proof.open(QIODevice::WriteOnly | QIODevice::Truncate)); proof.close();
        QVERIFY(!host.m_control.request(1000, 1, command(QStringLiteral("dismiss"), r.session)).value(QStringLiteral("ok")).toBool());
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral(".dismissed-") + r.session)));
        QVERIFY(proof.open(QIODevice::WriteOnly)); QCOMPARE(proof.write(bytes), qint64(bytes.size())); proof.close();
        QVERIFY(journal->writeOrderedExit(r, nullptr)); host.reconcileCleanExits(); QVERIFY(host.m_supervisor.list(1000).isEmpty());
        QVERIFY(!host.dismissalEligible(1000, r.session));
        QVERIFY(!host.m_control.request(1000, 1, command(QStringLiteral("dismiss"), r.session)).value(QStringLiteral("ok")).toBool());
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral(".dismissed-") + r.session)));
    }
    void uncertainDismissalCannotRetireWithoutDurability_data()
    {
        QTest::addColumn<int>("sync"); QTest::addColumn<QString>("path");
        for (int sync : {1, 2}) for (const auto *path : {"timer", "create", "recovery"})
            QTest::newRow(qPrintable(QString::number(sync) + QLatin1Char('-') + QString::fromLatin1(path))) << sync << QString::fromLatin1(path);
    }
    void uncertainDismissalCannotRetireWithoutDurability()
    {
        QFETCH(int, sync); QFETCH(QString, path); QTemporaryDir dir;
        const auto reset = qScopeGuard([] { dismissalFailSync = 0; });
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        Server server;
        auto host = std::make_unique<VirtualSessionHostController>(&server, VirtualSessionHostController::Prepare{});
        QVERIFY(host->recover(*journal)); QVERIFY(host->enableIndependentCreates(*journal, [](const auto &, const auto &) { return false; }));
        for (int i = 0; i < 4; ++i) QVERIFY(host->createIndependent(1000));
        const auto r = journal->records()->first(); QVERIFY(journal->claimRecord(r, nullptr)); QVERIFY(journal->writeReconciled(r, nullptr));
        const auto request = command(QStringLiteral("dismiss"), r.session);
        dismissalFailSync = sync;
        const auto uncertain = host->m_control.request(1000, 1, request);
        QVERIFY(!uncertain.value(QStringLiteral("ok")).toBool());
        QVERIFY(uncertain.value(QStringLiteral("message")).toString().contains(QStringLiteral("uncertain")));
        QCOMPARE(journal->dismissed(r), std::optional<bool>(true));
        QCOMPARE(host->m_control.request(1000, 1, request), uncertain); // Failure retry never executes again.
        dismissalFailSync = sync;
        if (path == QStringLiteral("timer")) {
            QVERIFY(QMetaObject::invokeMethod(&host->m_reconcileTimer, "timeout", Qt::DirectConnection));
        } else if (path == QStringLiteral("create")) {
            QVERIFY(!host->createIndependent(1000));
        } else {
            host.reset(); host = std::make_unique<VirtualSessionHostController>(&server, VirtualSessionHostController::Prepare{});
            QVERIFY(host->recover(*journal));
            QVERIFY(host->enableIndependentCreates(*journal, [](const auto &, const auto &) { return false; }));
        }
        QCOMPARE(dismissalFailSync, 0); QCOMPARE(host->m_supervisor.list(1000).size(), 4);
        QCOMPARE(journal->records()->size(), 4);
        dismissalFailSync = 0;
        QVERIFY(host->m_control.request(1000, 1, command(QStringLiteral("dismiss"), r.session)).value(QStringLiteral("ok")).toBool());
        QTRY_COMPARE(host->m_supervisor.list(1000).size(), 3);
        QVERIFY(!journal->claimRecord(r, nullptr));
    }
    void controlDispatchDefersRetirementEvenInNestedEventLoop()
    {
        QTemporaryDir directory;
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr); QVERIFY(journal);
        Server server;
        auto host = std::make_unique<VirtualSessionHostController>(&server, VirtualSessionHostController::Prepare{});
        QVERIFY(host->recover(*journal));
        QVERIFY(host->enableIndependentCreates(*journal, [](const auto &, const auto &) { return false; }));
        QVERIFY(host->createIndependent(1000));
        const auto records = journal->records(); QVERIFY(records); QCOMPARE(records->size(), 1);
        const auto record = records->first();
        QVERIFY(journal->claimRecord(record, nullptr));
        QVERIFY(journal->writeOrderedExit(record, nullptr)); QVERIFY(journal->writeReconciled(record, nullptr));
        int retired = 0;
        host->m_supervisor.setUnavailableCallback([&](const auto &handle) {
            if (handle.id == record.session) { ++retired; host.reset(); }
        });
        bool nestedRan = false;
        host->m_startService = [&](const auto &, const auto &) {
            QTimer::singleShot(0, host.get(), [&] {
                nestedRan = true;
                host->reconcileCleanExits();
            });
            QCoreApplication::processEvents();
            return false;
        };
        const QJsonObject request{{QStringLiteral("type"), QStringLiteral("virtual-session")},
            {QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("create")},
            {QStringLiteral("action"), QStringLiteral("create")}};
        const auto response = host->m_control.request(1000, 1, request);
        QVERIFY(response.value(QStringLiteral("ok")).toBool()); QVERIFY(nestedRan);
        QCOMPARE(retired, 0); QVERIFY(host); QVERIFY(!host->m_control.dispatchActive());
        host->reconcileCleanExits(); // Once dispatch unwinds, the same callback is allowed.
        QCOMPARE(retired, 1); QVERIFY(!host);
        QCOMPARE(journal->records()->size(), 2);
    }
    void controlCreateMayLoseHostInServiceCallback()
    {
        QTemporaryDir directory;
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr); QVERIFY(journal);
        Server server;
        auto host = std::make_unique<VirtualSessionHostController>(&server, VirtualSessionHostController::Prepare{});
        QVERIFY(host->recover(*journal));
        QVERIFY(host->enableIndependentCreates(*journal, [&](const auto &, const auto &) { host.reset(); return false; }));
        const QJsonObject request{{QStringLiteral("type"), QStringLiteral("virtual-session")},
            {QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("create")},
            {QStringLiteral("action"), QStringLiteral("create")}};
        const auto response = host->m_control.request(1000, 1, request);
        QVERIFY(!response.value(QStringLiteral("ok")).toBool()); QVERIFY(!host);
        QCOMPARE(journal->records()->size(), 1); // Published intent retained despite lost reply.
    }
    void retirementGuardsRecursionAndSupervisorDestruction()
    {
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        VirtualSessionGuardianClient::Identity identity{1000, uuid(), uuid(), QStringLiteral("/test/socket"), QByteArray(32, 't')};
        auto supervisor = std::make_unique<VirtualSessionSupervisor>(VirtualSessionSupervisor::LaunchFactory{});
        QVERIFY(supervisor->rememberUnavailable(identity));
        auto wrong = identity; wrong.incarnation = uuid();
        QVERIFY(!supervisor->forgetReconciled(wrong));
        wrong = identity; wrong.token.fill('x'); QVERIFY(!supervisor->forgetReconciled(wrong));
        wrong = identity; wrong.socket += QStringLiteral("-wrong"); QVERIFY(!supervisor->forgetReconciled(wrong));
        wrong = identity; ++wrong.uid; QVERIFY(!supervisor->forgetReconciled(wrong));
        int callbacks = 0;
        supervisor->setUnavailableCallback([&](const auto &) {
            ++callbacks;
            QVERIFY(!supervisor->forgetReconciled(identity));
            QVERIFY(!supervisor->forget(identity.uid, identity.session));
        });
        QVERIFY(supervisor->forgetReconciled(identity)); QCOMPARE(callbacks, 1);
        QVERIFY(supervisor->list(1000).isEmpty());
        QVERIFY(supervisor->rememberUnavailable(identity));
        supervisor->setUnavailableCallback([&](const auto &) { supervisor.reset(); });
        QVERIFY(!supervisor->forgetReconciled(identity)); QVERIFY(!supervisor);
    }
    void retirementCallbackMayDestroyHostDuringCreate()
    {
        QTemporaryDir directory;
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr); QVERIFY(journal);
        Server server;
        auto host = std::make_unique<VirtualSessionHostController>(&server, VirtualSessionHostController::Prepare{});
        QVERIFY(host->recover(*journal));
        QVERIFY(host->enableIndependentCreates(*journal, [](const auto &, const auto &) { return false; }));
        QVERIFY(host->createIndependent(1000));
        const auto records = journal->records(); QVERIFY(records); QCOMPARE(records->size(), 1);
        const auto r = records->first();
        QVERIFY(journal->claimRecord(r, nullptr));
        QVERIFY(journal->writeOrderedExit(r, nullptr)); QVERIFY(journal->writeReconciled(r, nullptr));
        host->m_supervisor.setUnavailableCallback([&](const auto &) { host.reset(); });
        QVERIFY(!host->createIndependent(1000)); QVERIFY(!host);
        QCOMPARE(journal->records()->size(), 1);
    }
    void completedHistoryDoesNotConsumeLiveAdmissionAfterRestart_data()
    {
        QTest::addColumn<int>("history");
        QTest::newRow("beyond-live-limit") << 8;
        QTest::newRow("journal-capacity") << 256;
    }
    void completedHistoryDoesNotConsumeLiveAdmissionAfterRestart()
    {
        QFETCH(int, history);
        QTemporaryDir directory;
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr); QVERIFY(journal);
        QFile file(QStringLiteral("/proc/sys/kernel/random/boot_id")); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto boot = QString::fromLatin1(file.readAll()).trimmed();
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        for (int i = 0; i < history; ++i) {
            const VirtualSessionJournal::Record r{1000, uuid(), uuid(), uuid(), boot, QByteArray(32, 'x')};
            QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
            QVERIFY(journal->writeOrderedExit(r, nullptr)); QVERIFY(journal->writeReconciled(r, nullptr));
        }
        Server server; VirtualSessionHostController host(&server, {});
        QVERIFY(host.recover(*journal)); QVERIFY(host.m_supervisor.list(1000).isEmpty());
        int started = 0;
        QVERIFY(host.enableIndependentCreates(*journal, [&](const auto &, const auto &) { ++started; return false; }));
        const auto created = host.createIndependent(1000);
        QCOMPARE(bool(created), history < 256);
        QCOMPARE(started, history < 256 ? 1 : 0);
        QCOMPARE(journal->records()->size(), history + started); // Never publish an unreadable 257th intent.
    }
    void eachTerminalProofAloneAndOldBootRemainUnavailable_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"ordered-only", "reconciled-only", "corrupt-ordered", "old-boot"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void eachTerminalProofAloneAndOldBootRemainUnavailable()
    {
        QFETCH(QString, kind);
        QTemporaryDir directory;
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr); QVERIFY(journal);
        QFile file(QStringLiteral("/proc/sys/kernel/random/boot_id")); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto boot = QString::fromLatin1(file.readAll()).trimmed();
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        const VirtualSessionJournal::Record r{1000, uuid(), uuid(), uuid(), kind == QStringLiteral("old-boot") ? uuid() : boot, QByteArray(32, 'x')};
        QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        if (kind != QStringLiteral("reconciled-only")) QVERIFY(journal->writeOrderedExit(r, nullptr));
        if (kind != QStringLiteral("ordered-only")) QVERIFY(journal->writeReconciled(r, nullptr));
        if (kind == QStringLiteral("corrupt-ordered")) {
            QFile marker(directory.filePath(QStringLiteral(".ordered-") + r.session));
            QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::Truncate)); marker.close();
        }
        Server server; VirtualSessionHostController host(&server, {});
        QVERIFY(host.recover(*journal));
        QCOMPARE(host.m_supervisor.list(1000).size(), 1);
        QCOMPARE(host.m_supervisor.list(1000).first().phase, VirtualSessionState::Phase::Failed);
        QVERIFY(journal->records()->first() == r);
    }
    void sameBrokerReclaimsOnlyProvenCleanHistory()
    {
        QTemporaryDir directory;
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr); QVERIFY(journal);
        Server server; VirtualSessionHostController host(&server, {});
        QVERIFY(host.recover(*journal));
        QVERIFY(host.enableIndependentCreates(*journal, [](const auto &, const auto &) { return false; }));
        for (int i = 0; i < 8; ++i) {
            const auto handle = host.createIndependent(1000); QVERIFY(handle);
            VirtualSessionJournal::Record r;
            const auto records = journal->records(); QVERIFY(records);
            for (const auto &candidate : *records) if (candidate.session == handle->id) r = candidate;
            QVERIFY(r.valid()); QVERIFY(journal->claimRecord(r, nullptr));
            QVERIFY(journal->writeOrderedExit(r, nullptr));
            host.reconcileCleanExits(); QCOMPARE(host.m_supervisor.list(1000).size(), 1);
            QVERIFY(journal->writeReconciled(r, nullptr));
            if (i == 7) QTRY_VERIFY_WITH_TIMEOUT(host.m_supervisor.list(1000).isEmpty(), 2000);
            else { host.reconcileCleanExits(); QVERIFY(host.m_supervisor.list(1000).isEmpty()); }
            QVERIFY(!journal->claimRecord(r, nullptr));
        }
        QCOMPARE(journal->records()->size(), 8);
    }
    void uncertainPublicationBlocksFurtherCreationUntilRecovery()
    {
        QTemporaryDir directory;
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr);
        QVERIFY(journal);
        int starts = 0;
        {
            Server server;
            VirtualSessionHostController host(&server, {});
            QVERIFY(host.recover(*journal));
            QVERIFY(host.enableIndependentCreates(*journal, [&](const auto &, const auto &) { ++starts; return true; }));
            bool published = false;
            host.m_commitIntent = [&](const auto &record) {
                published = journal->insert(record);
                return false; // simulate an uncertain fsync result after publication
            };
            QVERIFY(!host.createIndependent(1000)); QVERIFY(published);
            QVERIFY(host.m_creationBlocked);
            QVERIFY(!host.createIndependent(1000));
            QVERIFY(!host.createIndependent(1001));
            const auto records = journal->records(); QVERIFY(records); QCOMPARE(records->size(), 1);
            QCOMPARE(starts, 0);
        }
        Server server;
        VirtualSessionHostController next(&server, {});
        QVERIFY(next.recover(*journal));
        QCOMPARE(next.m_supervisor.list(1000).size(), 1);
        QCOMPARE(next.m_supervisor.list(1000).first().phase, VirtualSessionState::Phase::Failed);
    }
    void independentCreateWaitsForGuardianThenCapture()
    {
        if (!getuid()) QSKIP("Nonroot guardian fixture");
        const QString userRuntime = QStringLiteral("/run/user/%1").arg(getuid());
        if (!QFileInfo(userRuntime).isDir()) QSKIP("No canonical user runtime");
        const QString base = userRuntime + QStringLiteral("/krdp-virtual");
        const bool createdBase = QDir().mkdir(base);
        auto cleanBase = qScopeGuard([&] { if (createdBase) QDir().rmdir(base); });
        if (createdBase) QVERIFY(!chmod(QFile::encodeName(base).constData(), 0700));
        QVERIFY(!QFileInfo(base).isSymLink()); QCOMPARE(QFileInfo(base).ownerId(), quint32(getuid()));
        QString runtime;
        auto cleanRuntime = qScopeGuard([&] { if (!runtime.isEmpty()) QDir(runtime).removeRecursively(); });
        QTemporaryDir directory;
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr);
        QVERIFY(journal);
        VirtualSessionGuardian guardian;
        qint64 child = 0;
        {
            Server server;
            VirtualSessionHostController host(&server, {});
            QVERIFY(host.recover(*journal));
            QVERIFY(host.enableIndependentCreates(*journal, [&](const auto &, const auto &) {
                const auto records = journal->records();
                if (!records || records->size() != 1) return false;
                const auto &r = records->first();
                const QString path = QFileInfo(r.workerSocket()).absolutePath();
                if (!QDir().mkdir(path)) return false;
                runtime = path;
                if (chmod(QFile::encodeName(path).constData(), 0700)) return false;
                return guardian.start(getuid(), r.session, r.token, r.identity().socket,
                    {QStringLiteral("/usr/bin/sleep"), {QStringLiteral("60")}, {}, {}}, nullptr, r.incarnation);
            }));
            const auto handle = host.createIndependent(getuid());
            QVERIFY(handle);
            QVERIFY(host.m_workers.empty());
            QTRY_VERIFY(host.m_workers.contains(handle->id));
            QCOMPARE(host.m_supervisor.list(getuid()).first().phase, VirtualSessionState::Phase::Starting);
            QVERIFY(!host.m_supervisor.attach(getuid(), handle->id, 1));
            const auto r = journal->records()->first();
            QLocalSocket worker; worker.connectToServer(r.workerSocket());
            QVERIFY(worker.waitForConnected(1000));
            worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{r.session, quint32(getuid()), r.token}));
            worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
            worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Outputs{{{QStringLiteral("Virtual-1"), QRect(0, 0, 1280, 720), 1, true}}}));
            VideoFrame frame; frame.size = QSize(1280, 720); frame.data = "fixture"; frame.isKeyFrame = true;
            frame.monitors = {{QRect(0, 0, 1280, 720), true}};
            worker.write(ConsoleWorkerWire::frame(frame)); QVERIFY(worker.waitForBytesWritten(1000));
            QTRY_COMPARE(host.m_supervisor.list(getuid()).first().phase, VirtualSessionState::Phase::Retained);
            QVERIFY(host.m_supervisor.attach(getuid(), handle->id, 1));
            child = guardian.processId(); QVERIFY(child > 0);
        }
        QCOMPARE(guardian.phase(), QStringLiteral("running")); QCOMPARE(guardian.processId(), child);
    }
    void independentCreatePersistsBeforeOneStartAndDeduplicates()
    {
        QTemporaryDir directory;
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr);
        QVERIFY(journal);
        Server server;
        VirtualSessionHostController host(&server, {});
        QVERIFY(host.recover(*journal));
        int starts = 0;
        bool persistedBeforeStart = false;
        QVERIFY(host.enableIndependentCreates(*journal, [&](const QString &unit, const auto &handle) {
            ++starts;
            const auto records = journal->records();
            persistedBeforeStart = records && records->size() == 1 && records->first().session == handle.id
                && unit == QStringLiteral("krdp-virtual-session@%1.service").arg(handle.id);
            return true; // accepted request, no guardian or capture yet
        }));
        const QJsonObject request{{QStringLiteral("type"), QStringLiteral("virtual-session")}, {QStringLiteral("v"), 1},
            {QStringLiteral("id"), QStringLiteral("create-once")}, {QStringLiteral("action"), QStringLiteral("create")}};
        const auto reply = host.m_control.request(1000, 1, request);
        QVERIFY(reply.value(QStringLiteral("ok")).toBool());
        QVERIFY(persistedBeforeStart); QCOMPARE(starts, 1);
        QCOMPARE(reply.value(QStringLiteral("state")).toString(), QStringLiteral("starting"));
        QCOMPARE(host.m_control.request(1000, 1, request), reply);
        QCOMPARE(starts, 1);
        const auto session = reply.value(QStringLiteral("session")).toString();
        QVERIFY(!host.m_supervisor.attach(1000, session, 1));
        QVERIFY(host.m_workers.empty());
        QVERIFY(!host.m_supervisor.forget(1000, session));
        QVERIFY(!host.m_supervisor.recreate(1000, session));
        host.m_control.disconnected(1);
        QCOMPARE(journal->records()->size(), 1);
        QCOMPARE(host.m_supervisor.list(1000).first().phase, VirtualSessionState::Phase::Starting);
    }
    void failedServiceSubmissionPreservesIntent()
    {
        QTemporaryDir directory;
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr);
        QVERIFY(journal);
        Server server;
        VirtualSessionHostController host(&server, {});
        QVERIFY(!host.enableIndependentCreates(*journal, [](const auto &, const auto &) { return false; }));
        QVERIFY(host.recover(*journal));
        QTemporaryDir otherDirectory;
        auto otherJournal = VirtualSessionJournal::openAt(otherDirectory.path(), getuid(), nullptr);
        QVERIFY(otherJournal);
        QVERIFY(!host.enableIndependentCreates(*otherJournal, [](const auto &, const auto &) { return false; }));
        int starts = 0;
        QVERIFY(host.enableIndependentCreates(*journal, [&](const auto &, const auto &) { ++starts; return false; }));
        const auto handle = host.createIndependent(1000);
        QVERIFY(handle); QCOMPARE(starts, 1);
        QCOMPARE(host.m_supervisor.list(1000).first().phase, VirtualSessionState::Phase::Failed);
        const auto records = journal->records();
        QVERIFY(records); QCOMPARE(records->size(), 1);
        QCOMPARE(records->first().session, handle->id);
        QVERIFY(!host.m_supervisor.recreate(1000, handle->id));
        QVERIFY(!host.m_supervisor.forget(1000, handle->id));
        QVERIFY(host.m_workers.empty());
    }
    void journalRecoveryReattachesOnlyAfterNewAuthenticatedCapture()
    {
        if (!getuid()) QSKIP("Guardian requires a nonroot desktop UID");
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        QFile bootFile(QStringLiteral("/proc/sys/kernel/random/boot_id"));
        QVERIFY(bootFile.open(QIODevice::ReadOnly));
        const auto boot = QString::fromLatin1(bootFile.readAll()).trimmed();
        const QString userRuntime = QStringLiteral("/run/user/%1").arg(getuid());
        if (!QFileInfo(userRuntime).isDir()) QSKIP("No user runtime directory for canonical recovery-path fixture");
        const QString base = userRuntime + QStringLiteral("/krdp-virtual");
        const bool createdBase = QDir().mkdir(base);
        auto cleanBase = qScopeGuard([&] { if (createdBase) QDir().rmdir(base); });
        if (createdBase) QVERIFY(!chmod(QFile::encodeName(base).constData(), 0700));
        const QFileInfo info(base);
        QVERIFY(info.isDir()); QVERIFY(!info.isSymLink()); QCOMPARE(info.ownerId(), quint32(getuid()));
        const VirtualSessionJournal::Record record{quint32(getuid()), uuid(), uuid(), uuid(), boot, QByteArray(32, 'k')};
        const QString runtime = QFileInfo(record.workerSocket()).absolutePath();
        QVERIFY(QDir().mkdir(runtime)); // exclusive fixture; never reuse a live runtime
        auto cleanRuntime = qScopeGuard([&] { QDir(runtime).removeRecursively(); });
        QVERIFY(!chmod(QFile::encodeName(runtime).constData(), 0700));
        QTemporaryDir directory;
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr);
        QVERIFY(journal); QVERIFY(journal->insert(record));
        VirtualSessionGuardian guardian;
        QVERIFY(guardian.start(getuid(), record.session, record.token, record.identity().socket,
            {QStringLiteral("/usr/bin/sleep"), {QStringLiteral("60")}, {}, {}}, nullptr, record.incarnation));
        QTRY_COMPARE(guardian.phase(), QStringLiteral("running"));
        const auto child = guardian.processId();
        QVERIFY(child > 0);
        VirtualSessionRegistry::Handle oldHandle;
        QString oldTopologyGeneration;
        for (int attempt = 0; attempt < 2; ++attempt) {
            journal.reset();
            journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr);
            QVERIFY(journal); // restart reacquires lease and rereads durable state
            Server server;
            VirtualSessionHostController host(&server, {});
            QVERIFY(host.recover(*journal));
            QCOMPARE(host.m_supervisor.list(getuid()).first().phase, VirtualSessionState::Phase::Starting);
            QVERIFY(!host.m_supervisor.attach(getuid(), record.session, 1));
            QLocalSocket worker;
            worker.connectToServer(record.workerSocket());
            QVERIFY(worker.waitForConnected(1000));
            worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{record.session, quint32(getuid()), record.token}));
            worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
            const ConsoleWorkerWire::Outputs outputs{{{QStringLiteral("Virtual-1"), QRect(0, 0, 1280, 720), 1, true}}};
            worker.write(ConsoleWorkerWire::frame(outputs));
            QVERIFY(worker.waitForBytesWritten(1000));
            auto &runtimeState = *host.m_supervisor.m_runtimes.at(record.session);
            auto &workerState = *host.m_workers.at(record.session);
            QTRY_VERIFY(runtimeState.observedRunning);
            QTRY_VERIFY(workerState.endpoint->ready());
            QTRY_COMPARE(workerState.outputs.monitors.size(), 1);
            QVERIFY(!host.m_supervisor.attach(getuid(), record.session, 1)); // Ready != capture
            if (attempt) {
                QVERIFY(runtimeState.handle.manager != oldHandle.manager);
                QVERIFY(!host.m_supervisor.captureReady(oldHandle));
                QCOMPARE(host.m_supervisor.list(getuid()).first().phase, VirtualSessionState::Phase::Starting);
                QVERIFY(!host.m_supervisor.attach(getuid(), record.session, 1));
            }
            QSignalSpy received(workerState.endpoint.get(), &ConsoleWorkerEndpoint::frameReceived);
            VideoFrame frame;
            frame.size = QSize(1280, 720); frame.data = "fixture"; frame.isKeyFrame = false;
            frame.monitors = {{QRect(0, 0, 1280, 720), true}};
            worker.write(ConsoleWorkerWire::frame(frame));
            QVERIFY(worker.waitForBytesWritten(1000));
            QTRY_COMPARE(received.count(), 1);
            QVERIFY(!host.m_supervisor.attach(getuid(), record.session, 1));
            frame.isKeyFrame = true;
            frame.monitors = {{QRect(0, 0, 640, 480), true}};
            worker.write(ConsoleWorkerWire::frame(frame));
            QVERIFY(worker.waitForBytesWritten(1000));
            QTRY_COMPARE(received.count(), 2);
            QVERIFY(!host.m_supervisor.attach(getuid(), record.session, 1));
            frame.monitors = {{QRect(0, 0, 1280, 720), true}};
            worker.write(ConsoleWorkerWire::frame(frame));
            QVERIFY(worker.waitForBytesWritten(1000));
            QTRY_COMPARE(host.m_supervisor.list(getuid()).first().phase, VirtualSessionState::Phase::Retained);
            const auto &topology = workerState.topology.snapshot();
            QVERIFY(workerState.topology.observed());
            QCOMPARE(topology.revision, quint64(1));
            QCOMPARE(topology.outputs.size(), 1);
            QCOMPARE(topology.outputs.first().output.logicalGeometry, QRect(0, 0, 1280, 720));
            QCOMPARE(topology.outputs.first().output.nativePixels, QSize(1280, 720));
            if (attempt) QVERIFY(topology.generation != oldTopologyGeneration);
            oldTopologyGeneration = topology.generation;
            const auto handle = host.m_supervisor.attach(getuid(), record.session, 1);
            QVERIFY(handle);
            if (attempt) {
                QVERIFY(handle->manager != oldHandle.manager);
            }
            oldHandle = *handle;
            const auto stableId = topology.outputs.first().id;
            worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Outputs{{
                {QStringLiteral("Virtual-1"), QRect(0, 0, 1024, 576), 1.25, true}}}));
            worker.write(ConsoleWorkerWire::frame(frame));
            QVERIFY(worker.waitForBytesWritten(1000));
            QTRY_COMPARE(workerState.topology.snapshot().revision, quint64(2));
            QCOMPARE(workerState.topology.snapshot().outputs.first().id, stableId);
            QCOMPARE(workerState.topology.snapshot().outputs.first().output.logicalGeometry, QRect(0, 0, 1024, 576));
            QVERIFY(host.m_supervisor.disconnect(*handle, 1));
            QCOMPARE(guardian.processId(), child);
        }
        QCOMPARE(guardian.phase(), QStringLiteral("running"));
        QCOMPARE(guardian.processId(), child);
        const auto saved = journal->records();
        QVERIFY(saved); QCOMPARE(saved->first().incarnation, record.incarnation);
    }
    void recoveryReadsJournalAndBootWithoutChangingIntent()
    {
        QTemporaryDir directory;
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        QFile bootFile(QStringLiteral("/proc/sys/kernel/random/boot_id"));
        QVERIFY(bootFile.open(QIODevice::ReadOnly));
        const auto boot = QString::fromLatin1(bootFile.readAll()).trimmed();
        const VirtualSessionJournal::Record record{1000, uuid(), uuid(), uuid(), uuid(), QByteArray(32, 'x')};
        QVERIFY(record.boot != boot);
        auto journal = VirtualSessionJournal::openAt(directory.path(), getuid(), nullptr);
        QVERIFY(journal); QVERIFY(journal->insert(record));
        Server server;
        VirtualSessionHostController host(&server, {});
        QVERIFY(host.recover(*journal));
        QCOMPARE(host.m_supervisor.list(1000).size(), 1);
        QCOMPARE(host.m_supervisor.list(1000).first().phase, VirtualSessionState::Phase::Failed);
        QVERIFY(host.m_workers.empty());
        const auto saved = journal->records();
        QVERIFY(saved); QCOMPARE(saved->size(), 1);
        QCOMPARE(saved->first().incarnation, record.incarnation);
        QCOMPARE(saved->first().token, record.token);
        QVERIFY(!host.recover(*journal));
    }
    void recoveryRefusesAfterClientAdmission()
    {
        Server server;
        VirtualSessionHostController host(&server, {});
        auto connection = std::make_unique<RdpConnection>(&server, -1);
        Q_EMIT server.newConnectionCreated(connection.get());
        connection.reset();
        QVERIFY(!host.recoverRecords({}, QUuid::createUuid().toString(QUuid::WithoutBraces), nullptr));
    }
    void unavailableRecoveryNeverCreatesOrForgetsApps()
    {
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        const auto boot = uuid();
        Server server;
        int launched = 0;
        VirtualSessionHostController host(&server, [&](quint32, const auto &, const auto &) -> std::optional<VirtualSessionHostController::PreparedLaunch> {
            ++launched; return {};
        });
        const VirtualSessionJournal::Record oldBoot{quint32(getuid()), uuid(), uuid(), uuid(), uuid(), QByteArray(32, 'x')};
        const VirtualSessionJournal::Record missing{quint32(getuid()), uuid(), uuid(), uuid(), boot, QByteArray(32, 'y')};
        QVERIFY(!QFileInfo::exists(QFileInfo(missing.workerSocket()).absolutePath()));
        QString error;
        QVERIFY2(host.recoverRecords({oldBoot, missing}, boot, &error), qPrintable(error));
        QCOMPARE(launched, 0);
        QCOMPARE(host.m_supervisor.list(getuid()).size(), 2);
        QVERIFY(host.m_supervisor.list(getuid() + 1).isEmpty());
        for (const auto &summary : host.m_supervisor.list(getuid())) {
            QCOMPARE(summary.phase, VirtualSessionState::Phase::Failed);
            QVERIFY(!host.m_supervisor.attach(getuid(), summary.id, 1));
            QVERIFY(!host.m_supervisor.stop(getuid(), summary.id));
            QVERIFY(!host.m_supervisor.recreate(getuid(), summary.id));
            QVERIFY(!host.m_supervisor.forget(getuid(), summary.id));
        }
        QVERIFY(!host.recoverRecords({}, boot, &error));
        QVERIFY(!QFileInfo::exists(QFileInfo(missing.workerSocket()).absolutePath()));
        QCOMPARE(launched, 0);
    }
    void recoveryPreflightIsAllOrNothing()
    {
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        const auto boot = uuid();
        const VirtualSessionJournal::Record good{1000, uuid(), uuid(), uuid(), boot, QByteArray(32, 'x')};
        for (const auto &kind : {QStringLiteral("invalid"), QStringLiteral("duplicate"), QStringLiteral("alias"), QStringLiteral("capacity")}) {
            Server server;
            VirtualSessionHostController host(&server, {});
            QVector<VirtualSessionJournal::Record> records{good};
            auto second = good;
            if (kind == QStringLiteral("invalid")) second.token.clear();
            if (kind == QStringLiteral("alias")) { second.session = uuid(); second.incarnation = uuid(); }
            records.append(second);
            if (kind == QStringLiteral("capacity")) {
                records.clear();
                for (int i = 0; i < 5; ++i) records.append({1000, uuid(), uuid(), uuid(), boot, QByteArray(32, 'x')});
            }
            QVERIFY(!host.recoverRecords(records, boot, nullptr));
            QVERIFY(host.m_supervisor.list(1000).isEmpty());
            QVERIFY(host.m_workers.empty());
        }
    }
    void brokerLeaseReclaimsOnlyRefusedSocket()
    {
        if (!getuid()) QSKIP("Nonroot lease fixture");
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("worker.sock"));
        const auto encoded = QFile::encodeName(path);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        QVERIFY(encoded.size() < qsizetype(sizeof(address.sun_path)));
        std::memcpy(address.sun_path, encoded.constData(), size_t(encoded.size()) + 1);
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        QVERIFY(fd >= 0);
        QCOMPARE(bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
        QCOMPARE(listen(fd, 4), 0);
        QVERIFY(!VirtualSessionBrokerLease::acquire(getuid(), path));
        QVERIFY(QFileInfo::exists(path)); // active listener must never be removed
        close(fd); // dead broker leaves the bound socket inode behind
        auto lease = VirtualSessionBrokerLease::acquire(getuid(), path);
        QVERIFY(lease);
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!VirtualSessionBrokerLease::acquire(getuid(), path));
        lease.reset();
        QVERIFY(VirtualSessionBrokerLease::acquire(getuid(), path));
    }
    void brokerLeaseRejectsUnsafeArtifacts()
    {
        if (!getuid()) QSKIP("Nonroot lease fixture");
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("worker.sock"));
        QFile ordinary(path);
        QVERIFY(ordinary.open(QIODevice::WriteOnly));
        ordinary.close();
        QVERIFY(!VirtualSessionBrokerLease::acquire(getuid(), path));
        QVERIFY(ordinary.exists());
        QVERIFY(ordinary.remove());
        QVERIFY(QFile::link(directory.filePath(QStringLiteral("missing")), path));
        QVERIFY(!VirtualSessionBrokerLease::acquire(getuid(), path));
        QVERIFY(QFileInfo(path).isSymLink());
        QVERIFY(!VirtualSessionBrokerLease::acquire(getuid() + 1, path));
    }
    void hostMayBeDestroyedBeforeAnOpenConnection()
    {
        Server server;
        auto connection = std::make_unique<RdpConnection>(&server, -1);
        {
            VirtualSessionHostController host(&server, {});
            Q_EMIT server.newConnectionCreated(connection.get());
            QCOMPARE(host.m_clients.size(), size_t(1));
        }
        connection.reset(); // no stale host/adapter callbacks
    }
    void connectionDestructionDoesNotLaunchDesktop()
    {
        Server server;
        int preparations = 0;
        VirtualSessionHostController host(&server, [&](quint32, const auto &, const auto &) -> std::optional<VirtualSessionHostController::PreparedLaunch> {
            ++preparations;
            return {};
        });
        auto connection = std::make_unique<RdpConnection>(&server, -1);
        Q_EMIT server.newConnectionCreated(connection.get());
        QCOMPARE(host.m_clients.size(), size_t(1));
        connection.reset();
        QCOMPARE(host.m_clients.size(), size_t(0));
        QCOMPARE(preparations, 0);
    }
    void ownedEndpointAndGenerationResolution()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Server server;
        QByteArray delivered;
        VirtualSessionHostController host(&server, [&](quint32 uid, const auto &handle, const QByteArray &token)
                -> std::optional<VirtualSessionHostController::PreparedLaunch> {
            if (uid != 1000 || handle.id.isEmpty()) return {};
            delivered = token;
            return VirtualSessionHostController::PreparedLaunch{directory.filePath(QStringLiteral("worker.sock")),
                {QStringLiteral("/usr/bin/sleep"), {QStringLiteral("60")}, {}, {}}};
        });
        const auto handle = host.m_supervisor.create(1000);
        QVERIFY(handle);
        QCOMPARE(delivered.size(), 32);
        auto *endpoint = host.resolve(*handle);
        QVERIFY(endpoint);
        QCOMPARE(endpoint->target().adapter, ConsoleSeat::Adapter::VirtualUser);
        QCOMPARE(endpoint->target().uid, quint32(1000));
        QCOMPARE(endpoint->target().sessionId, handle->id);
        QVERIFY(!endpoint->ready());
        auto stale = *handle;
        ++stale.generation;
        QVERIFY(!host.resolve(stale));
        stale = *handle;
        stale.manager = QUuid::createUuid();
        QVERIFY(!host.resolve(stale));
        QVERIFY(host.m_supervisor.stop(1000, handle->id));
        QTRY_COMPARE(host.m_supervisor.list(1000).first().phase, VirtualSessionState::Phase::Absent);
    }
};
}
QTEST_GUILESS_MAIN(KRdp::VirtualSessionHostControllerTest)
#include "VirtualSessionHostControllerTest.moc"
