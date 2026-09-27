// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include <linux/input-event-codes.h>

#include <memory>
#include <utility>
#include <vector>

#include "PressedInputTracker.h"

using namespace KRdp;
using Kind = PressedInputTracker::Kind;
using Release = std::pair<Kind, uint32_t>;

namespace
{
// Stands in for PlasmaScreencastV1Session: owns its input object (the sink)
// and the tracker declared after it, so destroying it releases what is held.
struct FakeSession {
    std::vector<Release> *sent;
    PressedInputTracker tracker{[this](Kind kind, uint32_t code) {
        sent->emplace_back(kind, code);
    }};
    explicit FakeSession(std::vector<Release> *s)
        : sent(s)
    {
    }
};
}

class PressedInputTrackerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void destroyingTheSessionReleasesEverythingHeld()
    {
        std::vector<Release> sent;
        {
            auto session = std::make_unique<FakeSession>(&sent);
            // Ctrl held while the client drops; a left drag in progress; a
            // key that was pressed and released is not re-released.
            session->tracker.key(KEY_LEFTCTRL, true);
            session->tracker.key(KEY_A, true);
            session->tracker.key(KEY_A, false);
            session->tracker.button(BTN_LEFT, true);
            QVERIFY(session->tracker.keyDown(KEY_LEFTCTRL));
            QVERIFY(!session->tracker.keyDown(KEY_A));
            QVERIFY(session->tracker.buttonDown(BTN_LEFT));
        }
        // Buttons first, then keys.
        const std::vector<Release> expected{{Kind::Button, BTN_LEFT}, {Kind::Key, KEY_LEFTCTRL}};
        QCOMPARE(sent, expected);
    }

    void releaseAllIsIdempotent()
    {
        std::vector<Release> sent;
        PressedInputTracker tracker([&sent](Kind kind, uint32_t code) {
            sent.emplace_back(kind, code);
        });
        tracker.key(KEY_LEFTSHIFT, true);
        tracker.key(KEY_RIGHTALT, true);
        tracker.releaseAll();
        QCOMPARE(sent.size(), size_t(2));
        QVERIFY(tracker.empty());
        tracker.releaseAll();
        QCOMPARE(sent.size(), size_t(2));
    }

    void repeatedPressIsOneRelease()
    {
        std::vector<Release> sent;
        {
            PressedInputTracker tracker([&sent](Kind kind, uint32_t code) {
                sent.emplace_back(kind, code);
            });
            // Autorepeat sends press after press.
            tracker.key(KEY_B, true);
            tracker.key(KEY_B, true);
            tracker.key(KEY_B, true);
        }
        const std::vector<Release> expected{{Kind::Key, KEY_B}};
        QCOMPARE(sent, expected);
    }

    void nothingHeldSendsNothing()
    {
        std::vector<Release> sent;
        {
            PressedInputTracker tracker([&sent](Kind kind, uint32_t code) {
                sent.emplace_back(kind, code);
            });
            tracker.button(BTN_RIGHT, true);
            tracker.button(BTN_RIGHT, false);
        }
        QVERIFY(sent.empty());
    }
};

QTEST_GUILESS_MAIN(PressedInputTrackerTest)

#include "PressedInputTrackerTest.moc"
