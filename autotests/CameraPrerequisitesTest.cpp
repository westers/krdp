// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "ConsoleCameraSession.h"
#include <QSignalSpy>
#include <QTest>

using namespace KRdp;

// Missing-device checks stop before acquiring PipeWire or opening a producer.
class CameraPrerequisitesTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void missingBridgeCannotPublishSuccess()
    {
        PipeWireCamera camera;
        QVERIFY(!camera.start(QStringLiteral("missing-bridge"), 640, 480, 30, QStringLiteral("/dev/video999999")));
        QVERIFY(!camera.ready());
        QVERIFY(camera.error().contains(QStringLiteral("v4l2loopback-dkms")));
        camera.stop(); // failed startup cleanup is safe
    }
    void workerReturnsActionableFailureWithoutReadyOrCapture()
    {
        ConsoleCameraSession worker(true);
        QSignalSpy results(&worker, &ConsoleCameraSession::result);
        QSignalSpy demands(&worker, &ConsoleCameraSession::demand);
        worker.setControl({1, true});
        worker.request({1, 1, true, QStringLiteral("/dev/video999999")});
        QVERIFY(!worker.format({1, 1, 640, 480, 30}));
        QCOMPARE(results.size(), 1);
        QVERIFY(results.first().first().value<ConsoleWorkerWire::CameraResult>().error.contains(QStringLiteral("v4l2loopback-dkms")));
        QCOMPARE(demands.size(), 0);
    }
};
QTEST_GUILESS_MAIN(CameraPrerequisitesTest)
#include "CameraPrerequisitesTest.moc"
