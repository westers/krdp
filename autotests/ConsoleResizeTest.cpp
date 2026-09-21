// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "ConsoleResize.h"

class ConsoleResizeTest : public QObject
{
    Q_OBJECT
    const QByteArray snapshot = R"({"outputs":[{"name":"DP-3","connected":true,"enabled":true,"currentModeId":"2","scale":1.25,"modes":[
        {"id":"1","refreshRate":60,"size":{"width":1920,"height":1080}},
        {"id":"2","refreshRate":120,"size":{"width":1920,"height":1080}},
        {"id":"3","refreshRate":60,"size":{"width":1280,"height":720}},
        {"id":"4","refreshRate":100,"size":{"width":1280,"height":720}}]}]})";
private Q_SLOTS:
    void preservesCurrentMode()
    {
        const auto plan = KRdp::ConsoleResize::plan(snapshot, QStringLiteral("DP-3"), {1920, 1080}, 1.25);
        QVERIFY(plan.valid());
        QCOMPARE(plan.mode, QStringLiteral("2"));
        QCOMPARE(plan.apply, plan.restore);
    }
    void selectsNearestRefreshAndRecordsRollback()
    {
        const auto plan = KRdp::ConsoleResize::plan(snapshot, QStringLiteral("DP-3"), {1280, 720}, 1);
        QVERIFY(plan.valid());
        QCOMPARE(plan.mode, QStringLiteral("4"));
        QCOMPARE(plan.apply, QStringList({QStringLiteral("output.DP-3.mode.4"), QStringLiteral("output.DP-3.scale.1")}));
        QCOMPARE(plan.restore, QStringList({QStringLiteral("output.DP-3.mode.2"), QStringLiteral("output.DP-3.scale.1.25")}));
    }
    void refusesUnsupportedOrUnsafeRequests()
    {
        QVERIFY(!KRdp::ConsoleResize::plan(snapshot, QStringLiteral("DP-3"), {1536, 864}, 1).valid());
        QVERIFY(!KRdp::ConsoleResize::plan(snapshot, QStringLiteral("DP-3.disable"), {1280, 720}, 1).valid());
        QVERIFY(!KRdp::ConsoleResize::plan(snapshot, QStringLiteral("Virtual-test"), {1280, 720}, 1).valid());
        QVERIFY(!KRdp::ConsoleResize::plan(snapshot, QStringLiteral("DP-3"), {1280, 720}, qQNaN()).valid());
        QVERIFY(!KRdp::ConsoleResize::plan(snapshot, QStringLiteral("DP-3"), {8192, 4320}, 1).valid());
        QVERIFY(!KRdp::ConsoleResize::plan("{}", QStringLiteral("DP-3"), {1280, 720}, 1).valid());
    }
    void verifiesReadbackAndDoesNotOverwriteLocalChanges()
    {
        const auto plan = KRdp::ConsoleResize::plan(snapshot, QStringLiteral("DP-3"), {1280, 720}, 1);
        QVERIFY(KRdp::ConsoleResize::matches(snapshot, plan, true));
        QVERIFY(!KRdp::ConsoleResize::matches(snapshot, plan));
        auto applied = snapshot;
        applied.replace("\"currentModeId\":\"2\"", "\"currentModeId\":\"4\"");
        applied.replace("\"scale\":1.25", "\"scale\":1");
        QVERIFY(KRdp::ConsoleResize::matches(applied, plan));
        QVERIFY(!KRdp::ConsoleResize::matches(applied, plan, true));
        applied.replace("\"scale\":1,", "\"scale\":1.5,");
        QVERIFY(!KRdp::ConsoleResize::matches(applied, plan));
        QVERIFY(!KRdp::ConsoleResize::matches("{}", plan));
    }
    void refusesDisconnectedOrMissingRollback()
    {
        auto disconnected = snapshot;
        disconnected.replace("\"connected\":true", "\"connected\":false");
        QVERIFY(!KRdp::ConsoleResize::plan(disconnected, QStringLiteral("DP-3"), {1280, 720}, 1).valid());
        auto missing = snapshot;
        missing.replace("\"currentModeId\":\"2\"", "\"currentModeId\":\"missing\"");
        QVERIFY(!KRdp::ConsoleResize::plan(missing, QStringLiteral("DP-3"), {1280, 720}, 1).valid());
    }
};
QTEST_GUILESS_MAIN(ConsoleResizeTest)
#include "ConsoleResizeTest.moc"
