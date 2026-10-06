// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-060: a desk-input reclaim does its work once per episode, however many events follow.

#include "ConsoleReclaimEpisode.h"

#include <QTest>

using namespace KRdp;

class ConsoleReclaimEpisodeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void firstReclaimIsWorkRepeatsAreCounted()
    {
        ConsoleReclaimEpisode episode;
        QVERIFY(episode.needed());
        episode.done();
        for (int i = 0; i < 500; ++i) {
            QVERIFY(!episode.needed()); // five events a second at the desk: no more work, no more lines
        }
        QCOMPARE(episode.reset(), quint64(500));
    }
    void aFailedReclaimIsTriedAgain()
    {
        ConsoleReclaimEpisode episode;
        QVERIFY(episode.needed());
        // done() is not called (the restore could not be verified): the next event retries.
        QVERIFY(episode.needed());
        QCOMPARE(episode.reset(), quint64(0));
    }
    void aNewLayoutIsANewEpisode()
    {
        ConsoleReclaimEpisode episode;
        QVERIFY(episode.needed());
        episode.done();
        QVERIFY(!episode.needed());
        QCOMPARE(episode.reset(), quint64(1));
        QVERIFY(!episode.isDone());
        QVERIFY(episode.needed());
    }
};

QTEST_GUILESS_MAIN(ConsoleReclaimEpisodeTest)
#include "ConsoleReclaimEpisodeTest.moc"
