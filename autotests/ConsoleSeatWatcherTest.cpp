// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-C-8 / C-1: ConsoleSeatWatcher's per-session PropertiesChanged
// subscriptions follow logind's own object paths. logind escapes ids that
// start with a digit ("3" lives at .../session/_33); a path rebuilt from the
// id made every numeric session look dead on each publish.

#include "ConsoleSeatWatcher.h"
#include "FakeLogindBus.h"

#include <QSignalSpy>
#include <QTest>

namespace KRdp
{
namespace
{
const QString Numeric = QStringLiteral("/org/freedesktop/login1/session/_33");
const QString Greeter = QStringLiteral("/org/freedesktop/login1/session/c1");
}

class ConsoleSeatWatcherTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void numericSessionSubscriptionSurvivesPublish()
    {
        auto logind = std::make_shared<FakeLogindBus::State>();
        logind->sessions = {logindSession(QStringLiteral("3"), Numeric, 1000, QStringLiteral("user")),
                            logindSession(QStringLiteral("c1"), Greeter, 107, QStringLiteral("greeter"))};
        logind->sessions.last().active = false;
        ConsoleSeatWatcher watcher(std::make_unique<FakeLogindBus>(logind));
        QSignalSpy published(&watcher, &ConsoleSeatWatcher::sessionsChanged);
        watcher.start();
        QTRY_COMPARE(published.size(), 1);

        // The published record carries logind's path, not one built from the id.
        QCOMPARE(watcher.session(QStringLiteral("3"))->objectPath, Numeric);
        // Both subscriptions survived the publish, keyed by the real paths.
        QCOMPARE(watcher.watchedSessionPaths(), (QSet<QString>{Numeric, Greeter}));
        QCOMPARE(logind->subscribed, (QSet<QString>{Numeric, Greeter}));
        QVERIFY(!logind->subscribed.contains(QStringLiteral("/org/freedesktop/login1/session/3")));

        // A LockedHint change on the numeric session still reaches the watcher.
        logind->find(QStringLiteral("3"))->locked = true;
        QVERIFY(logind->propertiesChanged(Numeric));
        QTRY_COMPARE(published.size(), 2);
        QVERIFY(watcher.session(QStringLiteral("3"))->locked);
        QCOMPARE(logind->subscribed, (QSet<QString>{Numeric, Greeter})); // ... and survives that publish too.

        logind->find(QStringLiteral("3"))->locked = false;
        QVERIFY(logind->propertiesChanged(Numeric));
        QTRY_COMPARE(published.size(), 3);
        QVERIFY(!watcher.session(QStringLiteral("3"))->locked);

        // A session logind forgets loses its subscription; the live one keeps it.
        logind->sessions.removeLast();
        logind->sessionsChanged();
        QTRY_COMPARE(published.size(), 4);
        QCOMPARE(logind->subscribed, (QSet<QString>{Numeric}));
        QCOMPARE(watcher.watchedSessionPaths(), (QSet<QString>{Numeric}));
        QVERIFY(!logind->propertiesChanged(Greeter));
    }
};
}

QTEST_GUILESS_MAIN(KRdp::ConsoleSeatWatcherTest)

#include "ConsoleSeatWatcherTest.moc"
