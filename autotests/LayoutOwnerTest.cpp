// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "LayoutOwner.h"

using Role = LayoutOwner::Role;

namespace
{
const QString A = QStringLiteral("c1");
const QString B = QStringLiteral("c2");
const QString C = QStringLiteral("c3");
}

class LayoutOwnerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void nobodyOwnsAtStart()
    {
        LayoutOwner owner;
        QVERIFY(!owner.hasOwner());
        QVERIFY(owner.owner().isEmpty());
        QCOMPARE(owner.roleOf(A), Role::None);
        QVERIFY(!owner.canAcquire(A, false).has_value());
        QVERIFY(owner.viewers().isEmpty());
    }

    void firstApplyAcquires()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        QVERIFY(owner.hasOwner());
        QCOMPARE(owner.owner(), A);
        QCOMPARE(owner.roleOf(A), Role::Owner);
        QCOMPARE(owner.roleOf(B), Role::None);
        QCOMPARE(owner.missedHeartbeats(), 0);
    }

    void ownerMayApplyAgain()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        QVERIFY(!owner.canAcquire(A, false).has_value());
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        QCOMPARE(owner.owner(), A);
        QVERIFY(owner.viewers().isEmpty());
    }

    void secondAcquireRefusedAsNotOwner()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        const auto refused = owner.canAcquire(B, false);
        QVERIFY(refused.has_value());
        QCOMPARE(refused->code, QStringLiteral("not-owner"));
        QVERIFY(refused->message.contains(A));
        const auto refusedAgain = owner.tryAcquire(B, false);
        QVERIFY(refusedAgain.has_value());
        QCOMPARE(refusedAgain->code, QStringLiteral("not-owner"));
        // Nothing moved: the layout is still A's and B is not even a viewer
        // until the controller registers it as one.
        QCOMPARE(owner.owner(), A);
        QCOMPARE(owner.roleOf(B), Role::None);
    }

    void takeoverSwitchesOwnerAndDemotesThePrevious()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        QVERIFY(!owner.canAcquire(B, true).has_value());
        QVERIFY(!owner.tryAcquire(B, true).has_value());
        QCOMPARE(owner.owner(), B);
        QCOMPARE(owner.roleOf(B), Role::Owner);
        QCOMPARE(owner.roleOf(A), Role::Viewer);
        QCOMPARE(owner.viewers(), QList<QString>{A});
    }

    void takeoverWithoutAnOwnerJustAcquires()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, true).has_value());
        QCOMPARE(owner.owner(), A);
        QVERIFY(owner.viewers().isEmpty());
    }

    void viewerRegistration()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        owner.addViewer(B);
        owner.addViewer(B); // idempotent
        owner.addViewer(A); // the owner is never a viewer
        QCOMPARE(owner.roleOf(B), Role::Viewer);
        QCOMPARE(owner.roleOf(A), Role::Owner);
        QCOMPARE(owner.viewers(), QList<QString>{B});
        // A viewer leaving releases nothing.
        QVERIFY(!owner.release(B));
        QCOMPARE(owner.roleOf(B), Role::None);
        QCOMPARE(owner.owner(), A);
    }

    void viewerTakingOverLeavesTheViewerList()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        owner.addViewer(B);
        QVERIFY(!owner.tryAcquire(B, true).has_value());
        QCOMPARE(owner.owner(), B);
        QCOMPARE(owner.viewers(), QList<QString>{A});
    }

    void releaseOnThreeMissedHeartbeats()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        QVERIFY(!owner.heartbeatMissed(A));
        QVERIFY(!owner.heartbeatMissed(A));
        QCOMPARE(owner.missedHeartbeats(), 2);
        QCOMPARE(owner.owner(), A);
        QVERIFY(owner.heartbeatMissed(A));
        QVERIFY(!owner.hasOwner());
        QCOMPARE(owner.roleOf(A), Role::None);
        QCOMPARE(owner.missedHeartbeats(), 0);
    }

    void pongResetsTheMissCount()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        QVERIFY(!owner.heartbeatMissed(A));
        QVERIFY(!owner.heartbeatMissed(A));
        owner.heartbeatOk(A);
        QCOMPARE(owner.missedHeartbeats(), 0);
        QVERIFY(!owner.heartbeatMissed(A));
        QVERIFY(!owner.heartbeatMissed(A));
        QCOMPARE(owner.owner(), A);
    }

    void heartbeatsFromNonOwnersAreIgnored()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        owner.addViewer(B);
        for (int i = 0; i < 5; ++i) {
            QVERIFY(!owner.heartbeatMissed(B));
            QVERIFY(!owner.heartbeatMissed(C));
        }
        owner.heartbeatOk(B);
        QCOMPARE(owner.owner(), A);
        QCOMPARE(owner.missedHeartbeats(), 0);
    }

    void releaseOnDisconnect()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        QVERIFY(!owner.heartbeatMissed(A));
        QVERIFY(owner.release(A));
        QVERIFY(!owner.hasOwner());
        QCOMPARE(owner.roleOf(A), Role::None);
        QCOMPARE(owner.missedHeartbeats(), 0);
        // Idempotent: a second release of the same id finds nothing.
        QVERIFY(!owner.release(A));
        // And anybody may acquire again.
        QVERIFY(!owner.tryAcquire(B, false).has_value());
        QCOMPARE(owner.owner(), B);
    }

    void viewersSurviveTheOwnerLeaving()
    {
        LayoutOwner owner;
        QVERIFY(!owner.tryAcquire(A, false).has_value());
        owner.addViewer(B);
        QVERIFY(owner.release(A));
        QCOMPARE(owner.roleOf(B), Role::Viewer);
        QVERIFY(owner.owner().isEmpty());
        // A viewer may become the owner without a takeover once nobody owns.
        QVERIFY(!owner.tryAcquire(B, false).has_value());
        QCOMPARE(owner.roleOf(B), Role::Owner);
        QVERIFY(owner.viewers().isEmpty());
    }

    void roleNames()
    {
        QCOMPARE(LayoutOwner::roleName(Role::Owner), QStringLiteral("owner"));
        QCOMPARE(LayoutOwner::roleName(Role::Viewer), QStringLiteral("viewer"));
        QCOMPARE(LayoutOwner::roleName(Role::None), QStringLiteral("none"));
    }
};

QTEST_APPLESS_MAIN(LayoutOwnerTest)

#include "LayoutOwnerTest.moc"
