// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualResizeSession.h"
#include "ConsoleInputState.h"
#include "VirtualResizeTestFixture.h"
#include <QTest>

using namespace VirtualResizeFixture;
namespace KRdp
{
class VirtualResizeSessionTest : public QObject
{
    Q_OBJECT
    const ConsoleWorkerWire::Resize req{11, 1, QStringLiteral("virtual-desktop"), QSize(1920, 1080), 1.25};
    const ConsoleWorkerWire::Outputs target{{{QStringLiteral("Virtual-0"), QRect(0, 0, 1536, 864), 1.25, true}}};
    const ConsoleWorkerWire::Outputs original{{{QStringLiteral("Virtual-0"), QRect(0, 0, 1280, 720), 1, true}}};
    QJsonObject changed() const {
        auto out = output(); out[QStringLiteral("currentModeId")] = QStringLiteral("2"); out[QStringLiteral("scale")] = 1.25; return out;
    }
    void activate(VirtualResizeSession &session) {
        session.captureStateChanged(session.captureEpoch(), false);
        session.captureRestartReady(session.captureEpoch());
        session.captureStateChanged(session.captureEpoch(), true);
    }
    void apply(Deferred &io, VirtualResizeSession &session) {
        io.state(output()); io.answer(true); io.state(changed()); activate(session);
    }
    void rollback(Deferred &io, VirtualResizeSession &session) {
        io.state(changed()); io.answer(true); io.state(output()); activate(session);
    }
private Q_SLOTS:
    void matchingMetadataCannotCrossAnUnactivatedOrStaleEpoch()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        int results = 0;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { QVERIFY(r.error.isEmpty()); ++results; });
        session.setControl({1, true}); session.request(req);
        QVERIFY(!session.framesAllowed());
        io.state(output()); io.answer(true); io.state(changed());
        const auto firstEpoch = session.captureEpoch(); QVERIFY(firstEpoch != 0);
        session.captured(target, {1920, 1080}, {1920, 1080}, true, firstEpoch); QCOMPARE(results, 0);
        session.captureStateChanged(firstEpoch, false);
        session.captureStateChanged(firstEpoch, true); // A starting producer's queued true/keyframe is not a replacement.
        session.captured(target, {1920, 1080}, {1920, 1080}, true, firstEpoch); QCOMPARE(results, 0);
        session.captureRestartReady(firstEpoch + 1); // Wrong teardown epoch cannot authorize activation.
        session.captureStateChanged(firstEpoch, true);
        session.captured(target, {1920, 1080}, {1920, 1080}, true, firstEpoch); QCOMPARE(results, 0);
        activate(session);
        session.captured(target, {1920, 1080}, {1920, 1080}, true, firstEpoch); QCOMPARE(results, 1); QVERIFY(session.framesAllowed());
        auto second = req; second.requestId = 12;
        session.request(second); io.state(changed()); // No-op still establishes a new encoder/capture epoch.
        QVERIFY(session.captureEpoch() > firstEpoch); QVERIFY(!session.framesAllowed());
        session.captureStateChanged(firstEpoch, false); session.captureStateChanged(firstEpoch, true);
        session.captureRestartReady(firstEpoch);
        session.captureStateChanged(session.captureEpoch(), true);
        session.captured(target, {1920, 1080}, {1920, 1080}, true, session.captureEpoch()); QCOMPARE(results, 1);
        activate(session);
        session.captured(target, {1920, 1080}, {1920, 1080}, true, firstEpoch); QCOMPARE(results, 1);
        session.captured(target, {1920, 1080}, {1920, 1080}, true, session.captureEpoch()); QCOMPARE(results, 2);
    }
    void frameMetadataDimensionsAndKeyframeRequiredThenRetained()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        QList<ConsoleWorkerWire::ResizeResult> replies;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { replies.append(r); });
        session.setControl({1, true}); session.request(req); apply(io, session);
        QVERIFY(!session.inputAllowed()); QVERIFY(replies.isEmpty());
        session.captured(target, {1280, 720}, {1280, 720}, true, session.captureEpoch()); // Output geometry matches, frame-size metadata does not.
        session.captured(target, {1920, 1080}, {1920, 1080}, false, session.captureEpoch());
        auto wrongScale = target; wrongScale.monitors[0].scale = 1.5;
        session.captured(wrongScale, {1920, 1080}, {1920, 1080}, true, session.captureEpoch());
        QVERIFY(replies.isEmpty()); QVERIFY(!session.inputAllowed());
        session.captured(target, {1920, 1080}, {1920, 1080}, true, session.captureEpoch());
        QCOMPARE(replies.size(), 1); QVERIFY(replies.first().error.isEmpty()); QVERIFY(session.inputAllowed());
        const auto commands = io.history.size();
        session.setControl({2, false}); session.setControl({3, true});
        QVERIFY(session.inputAllowed()); QCOMPARE(io.history.size(), commands); // No disconnect restoration.
        session.stop(); QCOMPARE(io.history.size(), commands);
    }
    void activationMustFollowTeardownAndCannotBeReusedAfterStop()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        int results = 0; connect(&session, &VirtualResizeSession::result, this, [&](auto) { ++results; });
        session.setControl({1, true}); session.request(req);
        io.state(output()); io.answer(true); io.state(changed());
        const auto epoch = session.captureEpoch();
        session.captureStateChanged(epoch, true);
        session.captureRestartReady(epoch);
        session.captured(target, {1920, 1080}, {1920, 1080}, true, epoch); QCOMPARE(results, 0); // Earlier activation is not retroactive.
        session.captureStateChanged(epoch, true);
        session.captureStateChanged(epoch, false);
        session.captureRestartReady(epoch); // Duplicate teardown cannot rearm a stopped replacement.
        session.captureStateChanged(epoch, true);
        session.captured(target, {1920, 1080}, {1920, 1080}, true, epoch);
        QCOMPARE(results, 0); QVERIFY(!session.framesAllowed());
    }
    void freshProducerMetadataCannotAuthorizeStalePayload()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        QList<ConsoleWorkerWire::ResizeResult> replies;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { replies.append(r); });
        session.setControl({1, true}); session.request(req); apply(io, session);
        // Actual teardown/new activation already occurred. New metadata alone
        // must not authorize the old-size bitstream from a stale encoder setup.
        session.captured(target, {1920, 1080}, {1280, 720}, true, session.captureEpoch());
        QVERIFY(replies.isEmpty()); QVERIFY(!session.framesAllowed()); QVERIFY(!session.inputAllowed());
        session.captured(target, {1920, 1080}, QSize{}, true, session.captureEpoch()); // Parser refused payload.
        QVERIFY(replies.isEmpty()); QVERIFY(!session.framesAllowed());
        session.captured(target, {1280, 720}, {1920, 1080}, true, session.captureEpoch()); // Both sizes must match.
        QVERIFY(replies.isEmpty()); QVERIFY(!session.framesAllowed());
        session.captured(target, {1920, 1080}, {1920, 1080}, true, session.captureEpoch());
        QCOMPARE(replies.size(), 1); QVERIFY(replies.first().error.isEmpty()); QVERIFY(session.framesAllowed());
    }
    void restartFailureRollsBackAndRequiresNewTeardownEpoch()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        QList<ConsoleWorkerWire::ResizeResult> replies;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { replies.append(r); });
        session.setControl({1, true}); session.request(req);
        io.state(output()); io.answer(true); io.state(changed());
        const auto failedEpoch = session.captureEpoch();
        session.captureRestartFailed(failedEpoch + 1); QVERIFY(io.calls.isEmpty());
        session.captureRestartFailed(failedEpoch); QVERIFY(!io.calls.isEmpty());
        QVERIFY(!session.m_confirmedScale); // Failed activation cannot publish a newly verified scale.
        io.state(changed()); io.answer(true); io.state(output());
        QCOMPARE(replies.size(), 1); QVERIFY(!replies.first().error.isEmpty());
        QVERIFY(session.captureEpoch() > failedEpoch);
        session.captureRestartFailed(failedEpoch); // Old failure must not poison recovery.
        session.captureRestartReady(failedEpoch);
        session.captureStateChanged(session.captureEpoch(), true);
        session.captured(original, {1280, 720}, {1280, 720}, true, session.captureEpoch()); QVERIFY(!session.framesAllowed());
        activate(session);
        session.captured(original, {1280, 720}, {1280, 720}, true, session.captureEpoch()); QVERIFY(session.framesAllowed());
        QCOMPARE(session.outputScale(), std::optional(1.0)); // Recovery's verified original scale.
        QCOMPARE(replies.size(), 1); // Recovery never sends a success for the failed Fit.
    }
    void synchronousRestartAndRecoveryFailuresRemainBlocked()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        int refreshes = 0;
        QList<ConsoleWorkerWire::ResizeResult> replies;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { replies.append(r); });
        connect(&session, &VirtualResizeSession::captureRefreshNeeded, this, [&](quint64 epoch) {
            ++refreshes;
            session.captureRestartFailed(epoch); // Same synchronous seam as a false Plasma API return.
        });
        session.setControl({1, true}); session.request(req);
        io.state(output()); io.answer(true); io.state(changed());
        io.state(changed()); io.answer(true); io.state(output());
        QCOMPARE(refreshes, 2); QCOMPARE(replies.size(), 1);
        QVERIFY(replies.first().error.contains(QStringLiteral("recovery")));
        QVERIFY(!session.framesAllowed()); QVERIFY(!session.inputAllowed());
        activate(session); session.captured(original, {1280, 720}, {1280, 720}, true, session.captureEpoch());
        QVERIFY(!session.framesAllowed()); QVERIFY(io.calls.isEmpty());
        auto next = req; next.requestId = 12; session.request(next);
        QCOMPARE(replies.size(), 2); QVERIFY(!replies.last().error.isEmpty()); QVERIFY(io.calls.isEmpty());
    }
    void refusesForeignSelectorStaleAndOddRequests()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        int refused = 0;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { QVERIFY(!r.error.isEmpty()); ++refused; });
        session.setControl({2, true}); session.request(req);
        auto bad = req; bad.generation = 2; bad.output = QStringLiteral("Virtual-0"); session.request(bad);
        bad = req; bad.generation = 2; bad.pixels = {1921, 1080}; session.request(bad);
        QCOMPARE(refused, 3); QVERIFY(io.calls.isEmpty());
    }
    void cancellationDuringDiscoveryDoesNotApply()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        QList<ConsoleWorkerWire::ResizeResult> replies;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { replies.append(r); });
        session.setControl({1, true}); session.request(req); session.setControl({2, false}); io.state(output());
        QCOMPARE(replies.size(), 1); QVERIFY(!replies.first().error.isEmpty()); QCOMPARE(io.history.size(), 1);
        QVERIFY(!session.changing()); QVERIFY(!session.inputAllowed());
    }
    void cancelledApplyBlocksNewOwnerUntilRecoveryCapture()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        QList<ConsoleWorkerWire::ResizeResult> replies;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { replies.append(r); });
        session.setControl({1, true}); session.request(req); io.state(output());
        session.setControl({2, true});
        auto next = req; next.generation = 2; next.requestId = 12; session.request(next);
        QCOMPARE(replies.size(), 1); QCOMPARE(replies.first().requestId, quint64(12)); QVERIFY(!replies.first().error.isEmpty());
        io.answer(true); io.state(changed()); rollback(io, session);
        QCOMPARE(replies.size(), 2); QCOMPARE(replies.last().requestId, quint64(11)); QVERIFY(!replies.last().error.isEmpty());
        QVERIFY(!session.inputAllowed()); session.captured(target, {1920, 1080}, {1920, 1080}, true, session.captureEpoch()); QVERIFY(!session.inputAllowed());
        session.captured(original, {1280, 720}, {1280, 720}, true, session.captureEpoch()); QVERIFY(session.inputAllowed());
        QCOMPARE(replies.size(), 2);
    }
    void brokerTimeoutDoesNotReleaseUncertainApply()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        QList<ConsoleWorkerWire::ResizeResult> replies;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { replies.append(r); });
        session.setControl({1, true}); session.request(req); io.state(output());
        // Broker clears its own deadline/slot; no cancel record exists on this
        // IPC. The worker still owns the in-flight apply and must reject retry.
        auto retry = req; retry.requestId = 12; session.request(retry);
        QCOMPARE(replies.size(), 1); QCOMPARE(replies.first().requestId, quint64(12)); QVERIFY(!replies.first().error.isEmpty());
        QVERIFY(!session.inputAllowed()); QCOMPARE(io.calls.size(), 1);
        io.answer(true); io.state(changed()); activate(session); QVERIFY(!session.inputAllowed());
        session.request(retry); QCOMPARE(replies.size(), 2); QVERIFY(!replies.last().error.isEmpty());
        session.captured(target, {1920, 1080}, {1920, 1080}, true, session.captureEpoch());
        QCOMPARE(replies.size(), 3); QCOMPARE(replies.last().requestId, quint64(11)); QVERIFY(replies.last().error.isEmpty());
        QVERIFY(session.inputAllowed());
    }
    void realCaptureTimeoutRollsBackAndRecoveryTimeoutKeepsGate()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        QList<ConsoleWorkerWire::ResizeResult> replies;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { replies.append(r); });
        session.setControl({1, true}); session.request(req); apply(io, session);
        session.captured(target, {1920, 1080}, {1280, 720}, true, session.captureEpoch());
        QVERIFY(replies.isEmpty()); QVERIFY(!session.framesAllowed());
        session.m_deadline.start(1);
        QTRY_VERIFY(!io.calls.isEmpty()); // Actual QTimer enters recovery, not a manually faked result.
        auto retry = req; retry.requestId = 12; session.request(retry);
        QVERIFY(!replies.last().error.isEmpty()); rollback(io, session);
        QVERIFY(!replies.last().error.isEmpty()); QCOMPARE(replies.last().requestId, quint64(11));
        session.m_deadline.start(1); QTRY_VERIFY(!session.m_blockedError.isEmpty());
        QVERIFY(!session.inputAllowed());
        session.captured(original, {1280, 720}, {1280, 720}, true, session.captureEpoch()); QVERIFY(session.inputAllowed());
    }
    void unverifiedRollbackNeverReopensInput()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        session.setControl({1, true}); session.request(req); io.state(output()); io.answer(false); io.answer(false);
        QVERIFY(!io.calls.isEmpty()); io.answer(false); // Cannot even inspect rollback identity.
        QVERIFY(!session.inputAllowed());
        session.captured(original, {1280, 720}, {1280, 720}, true, session.captureEpoch()); QVERIFY(!session.inputAllowed());
        auto retry = req; retry.requestId = 12; session.request(retry); QVERIFY(io.calls.isEmpty());
    }
    void stopWaitsForApplyAndRollbackNotCapture()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        int stopped = 0;
        connect(&session, &VirtualResizeSession::stopped, this, [&](const QString &e) { QVERIFY(e.isEmpty()); ++stopped; });
        session.setControl({1, true}); session.request(req); io.state(output()); session.stop(); QCOMPARE(stopped, 0);
        io.answer(true); io.state(changed()); QCOMPARE(stopped, 0); rollback(io, session); QCOMPARE(stopped, 1);
        QVERIFY(!session.inputAllowed());
    }
    void noOpNeedsCaptureButNoMutation()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        int results = 0; connect(&session, &VirtualResizeSession::result, this, [&](auto r) { QVERIFY(r.error.isEmpty()); ++results; });
        session.setControl({1, true}); session.request(req); io.state(changed()); activate(session);
        QCOMPARE(io.history.size(), 1); QCOMPARE(results, 0);
        session.captured(target, {1920, 1080}, {1920, 1080}, true, session.captureEpoch()); QCOMPARE(results, 1);
    }
    void acceptedNoOpReleasesHeldInputBeforeDiscovery()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        ConsoleInputState held;
        ConsoleWorkerWire::Input key;
        key.type = ConsoleWorkerWire::Input::Type::Key; key.eventType = QEvent::KeyPress; key.nativeScanCode = 30;
        held.record(key);
        ConsoleWorkerWire::Input button;
        button.type = ConsoleWorkerWire::Input::Type::Mouse; button.eventType = QEvent::MouseButtonPress; button.button = Qt::LeftButton;
        held.record(button);
        int releases = 0;
        connect(&session, &VirtualResizeSession::mutationStarting, this, [&] {
            QVERIFY(io.history.isEmpty()); QVERIFY(!session.inputAllowed());
            const auto events = held.releaseAll(); QCOMPARE(events.size(), 2);
            QCOMPARE(events[0].eventType, int(QEvent::KeyRelease));
            QCOMPARE(events[1].eventType, int(QEvent::MouseButtonRelease));
            ++releases;
        });
        session.setControl({1, true}); session.request(req);
        QCOMPARE(releases, 1); QVERIFY(held.releaseAll().isEmpty());
        io.state(changed()); activate(session); QVERIFY(!session.inputAllowed());
        session.captured(target, {1920, 1080}, {1920, 1080}, true, session.captureEpoch());
        QCOMPARE(releases, 1); QVERIFY(session.inputAllowed());
    }
    void initialReleaseCanCancelWithoutStartingExecutorOrReentrantReplacement()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        QList<ConsoleWorkerWire::ResizeResult> replies;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { replies.append(r); });
        auto next = req; next.generation = 2; next.requestId = 12;
        const auto connection = connect(&session, &VirtualResizeSession::mutationStarting, this, [&] {
            session.setControl({2, true}); // Cancels the accepted request synchronously.
            session.request(next); // Must not replace it while its admission callback is running.
        });
        session.setControl({1, true}); session.request(req);
        QVERIFY(io.calls.isEmpty()); QCOMPARE(replies.size(), 2);
        QCOMPARE(replies[0].requestId, req.requestId); QVERIFY(!replies[0].error.isEmpty());
        QCOMPARE(replies[1].requestId, next.requestId); QVERIFY(!replies[1].error.isEmpty());
        QVERIFY(session.inputAllowed()); QVERIFY(session.framesAllowed());
        disconnect(connection);
        session.request(next); QCOMPARE(io.calls.size(), 1); // A later request is not permanently blocked.
    }
    void initialReleaseCanDestroyOrStopLifecycle()
    {
        Deferred io; auto *session = new VirtualResizeSession(nullptr, io.runner());
        connect(session, &VirtualResizeSession::mutationStarting, this, [&] { delete session; session = nullptr; });
        session->setControl({1, true}); session->request(req);
        QVERIFY(!session); QVERIFY(io.calls.isEmpty());
        VirtualResizeSession stopped(nullptr, io.runner());
        int stopSignals = 0;
        connect(&stopped, &VirtualResizeSession::stopped, this, [&](const QString &) { ++stopSignals; });
        connect(&stopped, &VirtualResizeSession::mutationStarting, &stopped, &VirtualResizeSession::stop);
        stopped.setControl({1, true}); stopped.request(req);
        QCOMPARE(stopSignals, 1); QVERIFY(io.calls.isEmpty()); QVERIFY(!stopped.inputAllowed());
    }
    void reentrantStopFromResultIsEmittedOnce()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        int stopped = 0;
        connect(&session, &VirtualResizeSession::stopped, this, [&](const QString &) { ++stopped; });
        connect(&session, &VirtualResizeSession::result, this, [&](auto) { session.stop(); });
        session.setControl({1, true}); session.request(req); apply(io, session);
        session.captured(target, {1920, 1080}, {1920, 1080}, true, session.captureEpoch());
        QCOMPARE(stopped, 1); QVERIFY(!session.framesAllowed());
    }
    void fractionalReadbackStillRequiresExactEncodedSize()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        auto request = req; request.scale = 160.0 / 120;
        session.setControl({1, true}); session.request(request);
        io.state(output()); io.answer(true);
        auto actual = changed(); actual[QStringLiteral("scale")] = request.scale; io.state(actual); activate(session);
        const ConsoleWorkerWire::Outputs fractional{{{QStringLiteral("Virtual-0"), QRect(0, 0, 1440, 810), 1.33203125, true}}};
        session.captured(fractional, {1918, 1080}, {1918, 1080}, true, session.captureEpoch()); QVERIFY(!session.framesAllowed());
        session.captured(fractional, {1920, 1080}, {1920, 1080}, true, session.captureEpoch()); QVERIFY(session.framesAllowed());
        QCOMPARE(session.outputScale(), std::optional(request.scale));
    }
    void framedRequestsAndRepliesKeepIdentity()
    {
        Deferred io; VirtualResizeSession session(nullptr, io.runner());
        ConsoleWorkerWire::Deframer requests, replies;
        connect(&session, &VirtualResizeSession::result, this, [&](auto r) { replies.feed(ConsoleWorkerWire::frame(r)); });
        session.setControl({1, true});
        const auto bytes = ConsoleWorkerWire::frame(req);
        requests.feed(bytes.first(3)); QVERIFY(!requests.next()); requests.feed(bytes.mid(3));
        const auto record = requests.next(); QVERIFY(record); const auto decoded = ConsoleWorkerWire::resize(*record); QVERIFY(decoded);
        session.request(*decoded); apply(io, session); QVERIFY(!replies.next()); session.captured(target, {1920, 1080}, {1920, 1080}, true, session.captureEpoch());
        const auto response = replies.next(); QVERIFY(response); const auto result = ConsoleWorkerWire::resizeResult(*response); QVERIFY(result);
        QCOMPARE(result->requestId, req.requestId); QCOMPARE(result->generation, req.generation); QVERIFY(result->error.isEmpty());
    }
    void deletionFromResultAndDelayedCallback()
    {
        Deferred io; auto *session = new VirtualResizeSession(nullptr, io.runner());
        connect(session, &VirtualResizeSession::result, this, [&](auto) { delete session; session = nullptr; });
        session->setControl({1, true}); session->request(req); apply(io, *session); session->captured(target, {1920, 1080}, {1920, 1080}, true, session->captureEpoch()); QVERIFY(!session);
        session = new VirtualResizeSession(nullptr, io.runner()); session->setControl({1, true}); session->request(req);
        delete session; session = nullptr; io.state(output()); QVERIFY(io.calls.isEmpty());
    }
};
}
QTEST_GUILESS_MAIN(KRdp::VirtualResizeSessionTest)
#include "VirtualResizeSessionTest.moc"
