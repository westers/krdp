// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <algorithm>

#include <QDataStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

#include "LayoutControl.h"

using namespace KRdp;
using namespace KRdp::LayoutControl;

namespace
{
HostMonitor realMonitor(const QString &id, QSize size, QPoint position, bool primary)
{
    return HostMonitor{
        .id = id,
        .name = id,
        .kind = Kind::Real,
        .size = size,
        .position = position,
        .scale = 1.0,
        .primary = primary,
        .lit = true,
        .standIn = false,
        .owner = QString(),
    };
}

/** DP-1 2560x1440 @(0,0) primary, HDMI-A-1 2560x1440 @(2560,0): hal9000's layout. */
Layout hal9000Layout()
{
    Layout layout;
    layout.monitors = {
        realMonitor(QStringLiteral("DP-1"), QSize(2560, 1440), QPoint(0, 0), true),
        realMonitor(QStringLiteral("HDMI-A-1"), QSize(2560, 1440), QPoint(2560, 0), false),
    };
    layout.owner = QString();
    layout.you = QStringLiteral("none");
    layout.caps = Caps{};
    return layout;
}

const HostMonitor *findMonitor(const QList<HostMonitor> &monitors, const QString &id)
{
    const auto it = std::find_if(monitors.cbegin(), monitors.cend(), [&id](const HostMonitor &m) {
        return m.id == id;
    });
    return it == monitors.cend() ? nullptr : &(*it);
}

/** An `apply` entry mentioning an existing monitor by id. */
ApplyMonitor existingMonitorEntry(const QString &id, std::optional<bool> lit = {}, std::optional<QSize> size = {}, std::optional<qreal> scale = {})
{
    ApplyMonitor entry;
    entry.id = id;
    entry.lit = lit;
    entry.size = size;
    entry.scale = scale;
    return entry;
}

/** An `apply` entry asking for a brand new virtual monitor. */
ApplyMonitor newMonitorEntry(QSize size, std::optional<qreal> scale = {})
{
    ApplyMonitor entry;
    entry.isNew = true;
    entry.size = size;
    entry.scale = scale;
    return entry;
}
}

class LayoutControlTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // --- Codec ---

    void layoutRoundTripsAllFields()
    {
        auto layout = hal9000Layout();
        layout.owner = QStringLiteral("conn-1");
        layout.you = QStringLiteral("owner");
        layout.monitors[0].standIn = true;
        layout.monitors[0].lit = false;
        layout.monitors.append(HostMonitor{
            .id = QStringLiteral("virtual-1"),
            .name = QStringLiteral("virtual-1"),
            .kind = Kind::Virtual,
            .size = QSize(1920, 1080),
            .position = QPoint(5120, 0),
            .scale = 1.25,
            .primary = false,
            .lit = true,
            .standIn = false,
            .owner = QStringLiteral("conn-2"),
        });

        const QJsonObject object = toJson(layout);
        const auto roundTripped = layoutFromJson(object);
        QVERIFY(roundTripped.has_value());
        QCOMPARE(*roundTripped, layout);

        // The wire shapes are binding: size {w,h}, position {x,y}, kind as a
        // string, owner null when there is none.
        const auto monitors = object.value(QStringLiteral("monitors")).toArray();
        const auto dp1 = monitors.at(0).toObject();
        QCOMPARE(dp1.value(QStringLiteral("kind")).toString(), QStringLiteral("real"));
        QCOMPARE(dp1.value(QStringLiteral("size")).toObject(), (QJsonObject{{QStringLiteral("w"), 2560}, {QStringLiteral("h"), 1440}}));
        QCOMPARE(dp1.value(QStringLiteral("position")).toObject(), (QJsonObject{{QStringLiteral("x"), 0}, {QStringLiteral("y"), 0}}));
        QVERIFY(dp1.value(QStringLiteral("owner")).isNull());

        const auto virtual1 = monitors.at(2).toObject();
        QCOMPARE(virtual1.value(QStringLiteral("kind")).toString(), QStringLiteral("virtual"));
        QCOMPARE(virtual1.value(QStringLiteral("owner")).toString(), QStringLiteral("conn-2"));

        QCOMPARE(object.value(QStringLiteral("owner")).toString(), QStringLiteral("conn-1"));
        QCOMPARE(object.value(QStringLiteral("you")).toString(), QStringLiteral("owner"));
        const auto caps = object.value(QStringLiteral("caps")).toObject();
        QCOMPARE(caps.value(QStringLiteral("maxOutputPx")).toInt(), ClientDisplay::MaxDimension);
        QCOMPARE(caps.value(QStringLiteral("maxUnionPx")).toInt(), ClientDisplay::MaxDesktopDimension);
    }

    void layoutWithNoOwnerRoundTripsToNull()
    {
        const auto layout = hal9000Layout();
        const auto object = toJson(layout);
        QVERIFY(object.value(QStringLiteral("owner")).isNull());
        const auto roundTripped = layoutFromJson(object);
        QVERIFY(roundTripped.has_value());
        QCOMPARE(roundTripped->owner, QString());
    }

    void applyRequestRoundTripsAllFields()
    {
        ApplyRequest request;
        request.privateMode = true;
        request.takeoverLayout = true;
        request.monitors = {
            existingMonitorEntry(QStringLiteral("DP-1"), true, QSize(1920, 1080), 1.0),
            newMonitorEntry(QSize(1920, 1080), 1.25),
        };

        const auto object = toJson(request);
        const auto roundTripped = applyFromJson(object);
        QVERIFY(roundTripped.has_value());
        QCOMPARE(*roundTripped, request);
    }

    void applyRequestOptionalFieldsAreOmittedWhenUnset()
    {
        ApplyRequest request;
        request.monitors = {existingMonitorEntry(QStringLiteral("DP-1"))};

        const auto object = toJson(request);
        QVERIFY(!object.contains(QStringLiteral("takeoverLayout")));
        const auto monitor = object.value(QStringLiteral("monitors")).toArray().at(0).toObject();
        QVERIFY(!monitor.contains(QStringLiteral("lit")));
        QVERIFY(!monitor.contains(QStringLiteral("size")));
        QVERIFY(!monitor.contains(QStringLiteral("scale")));
        QVERIFY(!monitor.contains(QStringLiteral("new")));
        QCOMPARE(monitor.value(QStringLiteral("id")).toString(), QStringLiteral("DP-1"));

        const auto roundTripped = applyFromJson(object);
        QVERIFY(roundTripped.has_value());
        QCOMPARE(*roundTripped, request);
    }

    void newApplyMonitorHasNoIdField()
    {
        ApplyRequest request;
        request.monitors = {newMonitorEntry(QSize(1920, 1080))};

        const auto object = toJson(request);
        const auto monitor = object.value(QStringLiteral("monitors")).toArray().at(0).toObject();
        QVERIFY(!monitor.contains(QStringLiteral("id")));
        QCOMPARE(monitor.value(QStringLiteral("new")).toBool(), true);

        const auto roundTripped = applyFromJson(object);
        QVERIFY(roundTripped.has_value());
        QCOMPARE(*roundTripped, request);
    }

    void errorLayoutAndTakeoverRecordsCarryType()
    {
        QCOMPARE(errorRecord(Error{QStringLiteral("invalid"), QStringLiteral("boom")}).value(QStringLiteral("type")).toString(), QStringLiteral("error"));
        QCOMPARE(layoutRecord(hal9000Layout()).value(QStringLiteral("type")).toString(), QStringLiteral("layout"));
        QCOMPARE(takeoverRecord(hal9000Layout()).value(QStringLiteral("type")).toString(), QStringLiteral("takeover"));
    }

    // --- Framing ---

    void frameAddsVersion()
    {
        const QJsonObject record{{QStringLiteral("type"), QStringLiteral("query")}};
        const auto framed = frame(record);
        QVERIFY(framed.size() > 4);

        quint32 length = 0;
        QDataStream header(framed);
        header.setByteOrder(QDataStream::BigEndian);
        header >> length;
        QCOMPARE(static_cast<int>(length), framed.size() - 4);

        const auto document = QJsonDocument::fromJson(framed.mid(4));
        QVERIFY(document.isObject());
        QCOMPARE(document.object().value(QStringLiteral("v")).toInt(), 1);
        QCOMPARE(document.object().value(QStringLiteral("type")).toString(), QStringLiteral("query"));
    }

    void deframerSplitsTwoRecordsFromOneBuffer()
    {
        const auto one = frame(QJsonObject{{QStringLiteral("type"), QStringLiteral("a")}});
        const auto two = frame(QJsonObject{{QStringLiteral("type"), QStringLiteral("b")}});

        Deframer deframer;
        deframer.feed(one + two);

        const auto first = deframer.next();
        QVERIFY(first.has_value());
        QCOMPARE(first->value(QStringLiteral("type")).toString(), QStringLiteral("a"));

        const auto second = deframer.next();
        QVERIFY(second.has_value());
        QCOMPARE(second->value(QStringLiteral("type")).toString(), QStringLiteral("b"));

        QVERIFY(!deframer.next().has_value());
        QVERIFY(!deframer.overflowed());
    }

    void deframerAssemblesOneRecordFromTwoChunks()
    {
        const auto whole = frame(QJsonObject{{QStringLiteral("type"), QStringLiteral("query")}});
        const auto splitPoint = whole.size() / 2;

        Deframer deframer;
        deframer.feed(whole.left(splitPoint));
        QVERIFY(!deframer.next().has_value());

        deframer.feed(whole.mid(splitPoint));
        const auto record = deframer.next();
        QVERIFY(record.has_value());
        QCOMPARE(record->value(QStringLiteral("type")).toString(), QStringLiteral("query"));
        QVERIFY(!deframer.overflowed());
    }

    void deframerOverflowsPastSixtyFourKiB()
    {
        const QJsonObject big{
            {QStringLiteral("type"), QStringLiteral("layout")},
            {QStringLiteral("padding"), QString(70 * 1024, QLatin1Char('x'))},
        };
        const auto framed = frame(big);
        QVERIFY(framed.size() > 64 * 1024);

        Deframer deframer;
        deframer.feed(framed);
        QVERIFY(!deframer.next().has_value());
        QVERIFY(deframer.overflowed());
    }

    // --- Planner ---

    void privateModeDarkensAllRealMonitors() // (a)
    {
        ApplyRequest request;
        request.privateMode = true;

        const auto result = plan(hal9000Layout(), request, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Plan>(result));
        const auto &p = std::get<Plan>(result);

        QCOMPARE(p.actions.size(), 2);
        QCOMPARE(p.actions[0].kind, ActionKind::DarkenReal);
        QCOMPARE(p.actions[0].id, QStringLiteral("DP-1"));
        QCOMPARE(p.actions[1].kind, ActionKind::DarkenReal);
        QCOMPARE(p.actions[1].id, QStringLiteral("HDMI-A-1"));

        for (const auto &monitor : p.resulting.monitors) {
            QVERIFY(!monitor.lit);
        }
        QCOMPARE(p.resulting.owner, QStringLiteral("conn-1"));
        QCOMPARE(p.resulting.you, QStringLiteral("owner"));
    }

    void sizedRealMonitorCreatesStandIn() // (b)
    {
        ApplyRequest request;
        request.monitors = {existingMonitorEntry(QStringLiteral("DP-1"), {}, QSize(1920, 1080), 1.0)};

        const auto result = plan(hal9000Layout(), request, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Plan>(result));
        const auto &p = std::get<Plan>(result);

        QCOMPARE(p.actions.size(), 1);
        QCOMPARE(p.actions[0].kind, ActionKind::CreateStandIn);
        QCOMPARE(p.actions[0].id, QStringLiteral("DP-1"));
        QCOMPARE(p.actions[0].size, QSize(1920, 1080));
        QCOMPARE(p.actions[0].position, QPoint(0, 0));

        const auto *dp1 = findMonitor(p.resulting.monitors, QStringLiteral("DP-1"));
        QVERIFY(dp1);
        QVERIFY(dp1->standIn);
        QVERIFY(!dp1->lit);

        const auto *hdmi = findMonitor(p.resulting.monitors, QStringLiteral("HDMI-A-1"));
        QVERIFY(hdmi);
        QVERIFY(hdmi->lit);
        QVERIFY(!hdmi->standIn);
    }

    void removingStandInSizeRestoresRealMonitor() // (c)
    {
        ApplyRequest first;
        first.monitors = {existingMonitorEntry(QStringLiteral("DP-1"), {}, QSize(1920, 1080), 1.0)};
        const auto afterFirst = plan(hal9000Layout(), first, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Plan>(afterFirst));

        ApplyRequest second;
        second.monitors = {existingMonitorEntry(QStringLiteral("DP-1"), true)};
        const auto result = plan(std::get<Plan>(afterFirst).resulting, second, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Plan>(result));
        const auto &p = std::get<Plan>(result);

        QCOMPARE(p.actions.size(), 2);
        QCOMPARE(p.actions[0].kind, ActionKind::RemoveStandIn);
        QCOMPARE(p.actions[0].id, QStringLiteral("DP-1"));
        QCOMPARE(p.actions[1].kind, ActionKind::LightReal);
        QCOMPARE(p.actions[1].id, QStringLiteral("DP-1"));

        const auto *dp1 = findMonitor(p.resulting.monitors, QStringLiteral("DP-1"));
        QVERIFY(dp1);
        QVERIFY(!dp1->standIn);
        QVERIFY(dp1->lit);
        QCOMPARE(dp1->size, QSize(2560, 1440)); // native size, never touched
    }

    void newMonitorIsPlacedAtUnionRightEdge() // (d)
    {
        ApplyRequest request;
        request.monitors = {newMonitorEntry(QSize(1920, 1080))};

        const auto result = plan(hal9000Layout(), request, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Plan>(result));
        const auto &p = std::get<Plan>(result);

        QCOMPARE(p.actions.size(), 1);
        QCOMPARE(p.actions[0].kind, ActionKind::CreateVirtual);
        QCOMPARE(p.actions[0].id, QStringLiteral("virtual-1"));
        QCOMPARE(p.actions[0].position, QPoint(5120, 0));
        QCOMPARE(p.actions[0].size, QSize(1920, 1080));

        QCOMPARE(p.resulting.monitors.size(), 3);
        const auto *created = findMonitor(p.resulting.monitors, QStringLiteral("virtual-1"));
        QVERIFY(created);
        QCOMPARE(created->kind, Kind::Virtual);
        QCOMPARE(created->position, QPoint(5120, 0));
        QCOMPARE(created->owner, QStringLiteral("conn-1"));
    }

    void twoNewMonitorsInOneApplyLandSideBySide()
    {
        // Small enough that the union (5120 wide already) stays under the
        // 8192px cap with both extras added.
        ApplyRequest request;
        request.monitors = {
            newMonitorEntry(QSize(800, 600)),
            newMonitorEntry(QSize(640, 480)),
        };

        const auto result = plan(hal9000Layout(), request, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Plan>(result));
        const auto &p = std::get<Plan>(result);

        QCOMPARE(p.actions.size(), 2);
        QCOMPARE(p.actions[0].id, QStringLiteral("virtual-1"));
        QCOMPARE(p.actions[0].position, QPoint(5120, 0));
        QCOMPARE(p.actions[1].id, QStringLiteral("virtual-2"));
        QCOMPARE(p.actions[1].position, QPoint(5120 + 800, 0));
    }

    void omittedOwnedVirtualIsRemovedOthersStay() // (e)
    {
        ApplyRequest createForConn1;
        createForConn1.monitors = {newMonitorEntry(QSize(800, 600))};
        const auto afterConn1 = plan(hal9000Layout(), createForConn1, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Plan>(afterConn1));

        ApplyRequest createForConn2;
        createForConn2.monitors = {newMonitorEntry(QSize(640, 480))};
        const auto afterConn2 = plan(std::get<Plan>(afterConn1).resulting, createForConn2, QStringLiteral("conn-2"), Caps{});
        QVERIFY(std::holds_alternative<Plan>(afterConn2));
        const auto layoutWithTwoVirtuals = std::get<Plan>(afterConn2).resulting;
        QCOMPARE(layoutWithTwoVirtuals.monitors.size(), 4);

        // conn-1 applies again mentioning nothing of its own: its virtual is removed.
        const auto result = plan(layoutWithTwoVirtuals, ApplyRequest{}, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Plan>(result));
        const auto &p = std::get<Plan>(result);

        QCOMPARE(p.actions.size(), 1);
        QCOMPARE(p.actions[0].kind, ActionKind::RemoveVirtual);
        QCOMPARE(p.actions[0].id, QStringLiteral("virtual-1"));

        QCOMPARE(p.resulting.monitors.size(), 3);
        QVERIFY(!findMonitor(p.resulting.monitors, QStringLiteral("virtual-1")));
        const bool conn2VirtualStays = std::any_of(p.resulting.monitors.cbegin(), p.resulting.monitors.cend(), [](const HostMonitor &m) {
            return m.kind == Kind::Virtual && m.owner == QStringLiteral("conn-2");
        });
        QVERIFY(conn2VirtualStays);
    }

    void newMonitorTooWideIsInvalid() // (f), part 1
    {
        ApplyRequest request;
        request.monitors = {newMonitorEntry(QSize(5000, 1080))};

        const auto result = plan(hal9000Layout(), request, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Error>(result));
        QCOMPARE(std::get<Error>(result).code, QStringLiteral("invalid"));
    }

    void unionOverLimitIsInvalid() // (f), part 2
    {
        ApplyRequest request;
        request.monitors = {
            newMonitorEntry(QSize(4096, 1440)),
            newMonitorEntry(QSize(4096, 1440)),
        };

        const auto result = plan(hal9000Layout(), request, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Error>(result));
        QCOMPARE(std::get<Error>(result).code, QStringLiteral("invalid"));
    }

    void untouchedMonitorsKeepState() // (g)
    {
        auto current = hal9000Layout();
        current.monitors[1].lit = false; // HDMI-A-1 already dark, for some unrelated reason

        ApplyRequest request;
        request.monitors = {existingMonitorEntry(QStringLiteral("DP-1"), true)};
        const auto result = plan(current, request, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Plan>(result));
        const auto &p = std::get<Plan>(result);

        const auto *hdmi = findMonitor(p.resulting.monitors, QStringLiteral("HDMI-A-1"));
        QVERIFY(hdmi);
        QCOMPARE(*hdmi, current.monitors[1]);
        const bool touchedHdmi = std::any_of(p.actions.cbegin(), p.actions.cend(), [](const Action &action) {
            return action.id == QStringLiteral("HDMI-A-1");
        });
        QVERIFY(!touchedHdmi);
    }

    void unknownIdIsInvalid() // (h)
    {
        ApplyRequest request;
        request.monitors = {existingMonitorEntry(QStringLiteral("DP-9"), true)};

        const auto result = plan(hal9000Layout(), request, QStringLiteral("conn-1"), Caps{});
        QVERIFY(std::holds_alternative<Error>(result));
        QCOMPARE(std::get<Error>(result).code, QStringLiteral("invalid"));
    }
};

QTEST_GUILESS_MAIN(LayoutControlTest)

#include "LayoutControlTest.moc"
