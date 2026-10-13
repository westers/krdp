// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>

#include "LockKeySync.h"

using KRdp::LockKeySync;
using Codes = std::vector<uint32_t>;

class LockKeySyncTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void firstSyncOnlySetsTheBaseline()
    {
        LockKeySync sync;
        QVERIFY(!sync.baselineKnown());
        QVERIFY(sync.synchronize(LockKeySync::CapsLock | LockKeySync::NumLock).empty());
        QVERIFY(sync.baselineKnown());
        QCOMPARE(sync.state(), LockKeySync::CapsLock | LockKeySync::NumLock);
    }

    void laterSyncTogglesOnlyWhatDiffers()
    {
        LockKeySync sync;
        sync.synchronize(LockKeySync::NumLock);
        // Caps turned on while the client had no focus, Num turned off.
        QCOMPARE(sync.synchronize(LockKeySync::CapsLock), (Codes{KEY_CAPSLOCK, KEY_NUMLOCK}));
        QVERIFY(sync.synchronize(LockKeySync::CapsLock).empty());
    }

    void scrollLockIsHandled()
    {
        LockKeySync sync;
        sync.synchronize(0);
        QCOMPARE(sync.synchronize(LockKeySync::ScrollLock), (Codes{KEY_SCROLLLOCK}));
    }

    void kanaIsIgnored()
    {
        LockKeySync sync;
        sync.synchronize(0);
        QVERIFY(sync.synchronize(0x8).empty());
        QCOMPARE(sync.state(), 0u);
    }

    void pressedLockKeyIsFollowedSoSyncDoesNotDoubleToggle()
    {
        LockKeySync sync;
        sync.synchronize(0);
        sync.key(KEY_CAPSLOCK, true);
        sync.key(KEY_CAPSLOCK, false);
        QCOMPARE(sync.state(), LockKeySync::CapsLock);
        QVERIFY(sync.synchronize(LockKeySync::CapsLock).empty());
    }

    void autoRepeatPressesDoNotToggle()
    {
        LockKeySync sync;
        sync.synchronize(0);
        sync.key(KEY_NUMLOCK, true);
        sync.key(KEY_NUMLOCK, true);
        sync.key(KEY_NUMLOCK, true);
        sync.key(KEY_NUMLOCK, false);
        QCOMPARE(sync.state(), LockKeySync::NumLock);
        sync.key(KEY_NUMLOCK, true);
        sync.key(KEY_NUMLOCK, false);
        QCOMPARE(sync.state(), 0u);
    }

    void keysBeforeTheBaselineDoNotInventState()
    {
        LockKeySync sync;
        sync.key(KEY_CAPSLOCK, true);
        sync.key(KEY_CAPSLOCK, false);
        QCOMPARE(sync.state(), 0u);
        QVERIFY(sync.synchronize(0).empty());
    }

    void otherKeysAreIgnored()
    {
        LockKeySync sync;
        sync.synchronize(0);
        sync.key(KEY_A, true);
        sync.key(KEY_A, false);
        QCOMPARE(sync.state(), 0u);
    }
};

QTEST_GUILESS_MAIN(LockKeySyncTest)
#include "LockKeySyncTest.moc"
