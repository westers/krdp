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
    void rejectsOverlapAndOversizedDesktop()
    {
        auto document = QJsonDocument::fromJson(snapshot);
        auto root = document.object();
        auto outputs = root.value(QStringLiteral("outputs")).toArray();
        auto other = outputs.first().toObject();
        other.insert(QStringLiteral("name"), QStringLiteral("DP-4"));
        other.insert(QStringLiteral("scale"), 1);
        other.insert(QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), 1000}, {QStringLiteral("y"), 0}});
        outputs.append(other);
        root.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!KRdp::ConsoleResize::plan(QJsonDocument(root).toJson(), QStringLiteral("DP-3"), {1280, 720}, 1).valid());
        // Was x=4000 (refused by the old single-surface 4096 union check, OPT-059); now x=7000 exceeds the 8192 desktop limit.
        other.insert(QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), 7000}, {QStringLiteral("y"), 0}});
        outputs[1] = other;
        root.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!KRdp::ConsoleResize::plan(QJsonDocument(root).toJson(), QStringLiteral("DP-3"), {1280, 720}, 1).valid());
        other.insert(QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), 1280}, {QStringLiteral("y"), 0}});
        outputs[1] = other;
        root.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(KRdp::ConsoleResize::plan(QJsonDocument(root).toJson(), QStringLiteral("DP-3"), {1280, 720}, 1).valid());
    }

public:
    static QByteArray outputJson(const QString &name, int x, int w, int h, double scale = 1)
    {
        return QStringLiteral(R"({"name":"%1","connected":true,"enabled":true,"currentModeId":"cur","scale":%2,"pos":{"x":%3,"y":0},"modes":[
            {"id":"cur","refreshRate":60,"size":{"width":%4,"height":%5}},
            {"id":"fhd","refreshRate":60,"size":{"width":1920,"height":1080}},
            {"id":"hd","refreshRate":60,"size":{"width":1280,"height":720}},
            {"id":"uhd","refreshRate":60,"size":{"width":3840,"height":2160}},
            {"id":"big","refreshRate":60,"size":{"width":4400,"height":2400}},
            {"id":"wide","refreshRate":60,"size":{"width":4096,"height":2160}}]})")
            .arg(name).arg(scale).arg(x).arg(w).arg(h).toUtf8();
    }
    static QByteArray layout(const QList<QByteArray> &outputs)
    {
        return "{\"outputs\":[" + outputs.join(',') + "]}";
    }
    const QByteArray hal = layout({outputJson(QStringLiteral("DP-1"), 0, 2560, 1440), outputJson(QStringLiteral("HDMI-A-1"), 2560, 2560, 1440)});

private Q_SLOTS:
    void fitOnTwoOutputHostIsNotRefusedByUnion_data()
    {
        QTest::addColumn<QString>("output");
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QString>("mode");
        QTest::newRow("DP-1 to 1920x1080") << "DP-1" << QSize(1920, 1080) << "fhd";
        QTest::newRow("HDMI-A-1 to 1920x1080") << "HDMI-A-1" << QSize(1920, 1080) << "fhd";
        QTest::newRow("DP-1 to 1280x720") << "DP-1" << QSize(1280, 720) << "hd";
        QTest::newRow("HDMI-A-1 to 1280x720") << "HDMI-A-1" << QSize(1280, 720) << "hd";
        QTest::newRow("HDMI-A-1 to 3840x2160") << "HDMI-A-1" << QSize(3840, 2160) << "uhd";
    }
    void fitOnTwoOutputHostIsNotRefusedByUnion()
    {
        QFETCH(QString, output);
        QFETCH(QSize, size);
        QFETCH(QString, mode);
        const auto plan = KRdp::ConsoleResize::plan(hal, output, size, 1);
        QVERIFY2(plan.valid(), qPrintable(plan.error));
        QCOMPARE(plan.mode, mode);
    }
    void growingAnOutputOverItsNeighbourIsRefusedAsOverlap()
    {
        // Positions are preserved: DP-1 at 3840 wide would run into HDMI-A-1 at x=2560.
        const auto plan = KRdp::ConsoleResize::plan(hal, QStringLiteral("DP-1"), {3840, 2160}, 1);
        QVERIFY(!plan.valid());
        QVERIFY(plan.error.contains(QStringLiteral("overlap")));
    }
    void oldFiveThousandWideUnionWithNoChangeIsAccepted()
    {
        // Regression: both outputs unchanged (5120 wide union) must not be refused by the planner.
        const auto plan = KRdp::ConsoleResize::plan(hal, QStringLiteral("DP-1"), {2560, 1440}, 1);
        QVERIFY2(plan.valid(), qPrintable(plan.error));
        QVERIFY(KRdp::ConsoleResize::matches(hal, plan));
    }
    void scaleOnlyChangeOnTwoOutputHost()
    {
        const auto plan = KRdp::ConsoleResize::plan(hal, QStringLiteral("HDMI-A-1"), {2560, 1440}, 1.5);
        QVERIFY2(plan.valid(), qPrintable(plan.error));
        QCOMPARE(plan.scale, 1.5);
    }
    void threeMonitorHost()
    {
        const auto three = layout({outputJson(QStringLiteral("A"), 0, 2560, 1440), outputJson(QStringLiteral("B"), 2560, 2560, 1440),
                                   outputJson(QStringLiteral("C"), 5120, 2560, 1440)}); // 7680 wide
        QVERIFY(KRdp::ConsoleResize::plan(three, QStringLiteral("B"), {1920, 1080}, 1).valid());
        // A fourth 2560 output pushes the union to 10240 > 8192, even when resizing another output.
        const auto four = layout({outputJson(QStringLiteral("A"), 0, 2560, 1440), outputJson(QStringLiteral("B"), 2560, 2560, 1440),
                                  outputJson(QStringLiteral("C"), 5120, 2560, 1440), outputJson(QStringLiteral("D"), 7680, 2560, 1440)});
        const auto refused = KRdp::ConsoleResize::plan(four, QStringLiteral("A"), {1920, 1080}, 1);
        QVERIFY(!refused.valid());
        QVERIFY2(refused.error.contains(QStringLiteral("10240x1440")) && refused.error.contains(QStringLiteral("8192")), qPrintable(refused.error));
    }
    void outputOverEncoderLimitIsRefusedWithSpecificMessage()
    {
        const auto plan = KRdp::ConsoleResize::plan(hal, QStringLiteral("DP-1"), {4400, 2400}, 1);
        QVERIFY(!plan.valid());
        QCOMPARE(plan.error, QStringLiteral("Output DP-1 would be 4400x2400, which is over the 4096-pixel limit of the video encoder for this connection"));
        // 4096 itself is allowed on a single output, and the union cap still applies.
        const auto single = layout({outputJson(QStringLiteral("A"), 0, 2560, 1440)});
        QVERIFY(KRdp::ConsoleResize::plan(single, QStringLiteral("A"), {4096, 2160}, 1).valid());
    }
    void desktopOverLimitIsRefusedWithSpecificMessage()
    {
        // B spans x 6000..8560, past the 8192 desktop limit; its size is unchanged.
        const auto wide = layout({outputJson(QStringLiteral("A"), 0, 2560, 1440), outputJson(QStringLiteral("B"), 6000, 2560, 1440)});
        const auto plan = KRdp::ConsoleResize::plan(wide, QStringLiteral("A"), {4096, 2160}, 1);
        QVERIFY(!plan.valid());
        QVERIFY2(plan.error.contains(QStringLiteral("The desktop would be 8560x")) && plan.error.contains(QStringLiteral("8192")), qPrintable(plan.error));
    }
};
QTEST_GUILESS_MAIN(ConsoleResizeTest)
#include "ConsoleResizeTest.moc"
