// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>
#include <QFile>

#include "RetainedKScreenReadback.h"

using namespace KRdp::RetainedKScreenReadback;

namespace
{
QJsonObject output(const QString &name, int id, int x, int y, double scale, int priority)
{
    return {{QStringLiteral("name"), name}, {QStringLiteral("id"), id},
        {QStringLiteral("connected"), true}, {QStringLiteral("enabled"), true},
        {QStringLiteral("rotation"), 1}, {QStringLiteral("replicationSource"), 0},
        {QStringLiteral("currentModeId"), QString::number(id)},
        {QStringLiteral("priority"), priority}, {QStringLiteral("scale"), scale},
        {QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), x}, {QStringLiteral("y"), y}}},
        {QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), 1280}, {QStringLiteral("height"), 720}}},
        {QStringLiteral("modes"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), QString::number(id)},
            {QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), 1280}, {QStringLiteral("height"), 720}}},
        }}}};
}

QJsonObject root()
{
    return {{QStringLiteral("screen"), QJsonObject{{QStringLiteral("maxActiveOutputsCount"), 2}}},
        {QStringLiteral("outputs"), QJsonArray{output(QStringLiteral("Virtual-0"), 1, 0, 0, 1.25, 1),
            output(QStringLiteral("Virtual-1"), 2, 1024, 100, 1.0, 2)}}};
}

std::optional<Snapshot> decoded(const QJsonObject &object)
{
    return parse(QJsonDocument(object).toJson(), QStringLiteral("lease-1"));
}
}

class RetainedKScreenReadbackTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void privateTwoOutputReadbackAndPositionCommands()
    {
        const auto state = decoded(root());
        QVERIFY(state);
        QCOMPARE(state->maxActiveOutputs, 2);
        QCOMPARE(state->outputs.size(), 2);
        QCOMPARE(state->outputs[0].logicalGeometry, QRect(0, 0, 1024, 576));
        QCOMPARE(state->outputs[1].logicalGeometry, QRect(1024, 100, 1280, 720));
        QVERIFY(state->outputs[0].primary);
        QVERIFY(!state->outputs[1].primary);
        QVERIFY(!state->outputs[1].physical);
        QCOMPARE(state->outputs[1].owner, QStringLiteral("lease-1"));
        const auto args = positionArguments(*state, {{QStringLiteral("Virtual-1"), QPoint(-1280, 0)}});
        QVERIFY(args);
        QCOMPARE(*args, QStringList{QStringLiteral("output.Virtual-1.position.-1280,0")});
        QVERIFY(!positionArguments(*state, {{QStringLiteral("DP-1"), QPoint(0, 0)}}));
        QVERIFY(!positionArguments(*state, {{QStringLiteral("Virtual-1"), QPoint(0, 0)},
            {QStringLiteral("Virtual-1"), QPoint(0, 0)}}));
    }

    void refusesAmbiguousOrUnsupportedReadback()
    {
        auto object = root();
        auto outputs = object.value(QStringLiteral("outputs")).toArray();
        outputs[1] = output(QStringLiteral("Virtual-0"), 2, 1024, 100, 1.0, 2);
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object)); // Duplicate name cannot be an identity.
        object = root();
        outputs = object.value(QStringLiteral("outputs")).toArray();
        auto second = outputs[1].toObject();
        second.insert(QStringLiteral("currentModeId"), QStringLiteral("missing"));
        outputs[1] = second;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
        object = root();
        outputs = object.value(QStringLiteral("outputs")).toArray();
        second = outputs[1].toObject();
        second.insert(QStringLiteral("rotation"), 2);
        outputs[1] = second;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
        object = root();
        outputs = object.value(QStringLiteral("outputs")).toArray();
        second = outputs[1].toObject();
        second.insert(QStringLiteral("replicationSource"), 1);
        outputs[1] = second;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
        object = root();
        outputs = object.value(QStringLiteral("outputs")).toArray();
        second = outputs[1].toObject();
        second.insert(QStringLiteral("enabled"), false);
        outputs[1] = second;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
    }

    void refusesMissingPrimaryWrongModeAndCapacity()
    {
        auto object = root();
        auto outputs = object.value(QStringLiteral("outputs")).toArray();
        auto first = outputs[0].toObject();
        first.insert(QStringLiteral("priority"), 2);
        outputs[0] = first;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
        object = root();
        outputs = object.value(QStringLiteral("outputs")).toArray();
        first = outputs[0].toObject();
        first.insert(QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), 800}, {QStringLiteral("height"), 600}});
        outputs[0] = first;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
        object = root();
        object.insert(QStringLiteral("screen"), QJsonObject{{QStringLiteral("maxActiveOutputsCount"), 1}});
        QVERIFY(!decoded(object));
        QVERIFY(!parse(QJsonDocument(root()).toJson(), {}));
    }

    void realPrivateKScreenFixtureIfProvided()
    {
        const QString path = qEnvironmentVariable("KRDP_KSCREEN_FIXTURE");
        if (path.isEmpty()) QSKIP("No isolated KScreen fixture requested");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto state = parse(file.readAll(), QStringLiteral("fixture-owner"));
        QVERIFY(state);
        QCOMPARE(state->outputs.size(), 2);
        QCOMPARE(state->outputs[0].logicalGeometry, QRect(0, 0, 1024, 576));
        QCOMPARE(state->outputs[1].logicalGeometry, QRect(1024, 100, 1280, 720));
    }
};

QTEST_GUILESS_MAIN(RetainedKScreenReadbackTest)

#include "RetainedKScreenReadbackTest.moc"
