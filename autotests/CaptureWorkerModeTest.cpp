// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "CaptureWorkerMode.h"

class CaptureWorkerModeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void explicitMode()
    {
        using Mode = KRdp::CaptureWorkerMode;
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(!Mode::parse({}, {}));
        QVERIFY(!Mode::parse(QStringLiteral("817"), id));
        const auto physical = Mode::parse(QStringLiteral("817"), {});
        QVERIFY(physical && physical->physicalActions());
        QCOMPARE(physical->sessionId, QStringLiteral("817"));
        const auto virtualMode = Mode::parse({}, id);
        QVERIFY(virtualMode && !virtualMode->physicalActions());
        QCOMPARE(virtualMode->sessionId, id);
        QVERIFY(!Mode::parse({}, QStringLiteral("817")));
        QVERIFY(!Mode::parse({}, QStringLiteral("../seat0")));
        QVERIFY(!Mode::parse({}, QUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(!Mode::parse({}, QUuid(id).toString()));
    }
};
QTEST_GUILESS_MAIN(CaptureWorkerModeTest)
#include "CaptureWorkerModeTest.moc"
