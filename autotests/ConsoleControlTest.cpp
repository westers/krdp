// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "ConsoleControl.h"

class ConsoleControlTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void microphoneRequiresControllerAndFreshConsentAfterRelease()
    {
        KRdp::ConsoleControl control;
        QVERIFY(!control.setMedia(1, {false, false, true}));
        control.admit(1);
        control.admit(2);
        QVERIFY(!control.setMedia(2, {false, false, true}));
        QVERIFY(control.setMedia(1, {false, false, true}));
        QVERIFY(control.media().microphone);
        QVERIFY(!control.media().playback);
        QVERIFY(!control.media().silenceHost);
        QVERIFY(control.release(1));
        QVERIFY(!control.media().microphone);
        QVERIFY(control.acquire(1));
        QVERIFY(!control.media().microphone);
        QVERIFY(control.setMedia(1, {true, true, true}));
        control.remove(1);
        QVERIFY(!control.media().microphone);
        QVERIFY(control.acquire(2));
        QVERIFY(!control.media().microphone);
    }

    void transferRequiresExplicitRelease()
    {
        KRdp::ConsoleControl control;
        QVERIFY(!control.acquire(1));
        control.admit(1);
        control.admit(2);
        QVERIFY(control.setMedia(1, {true, true}));
        QVERIFY(!control.acquire(2));
        QVERIFY(!control.release(2));
        QVERIFY(control.ownsControl(1));
        QVERIFY(control.media().silenceHost);
        QVERIFY(control.release(1));
        QVERIFY(control.media().playback);
        QVERIFY(!control.media().silenceHost);
        QVERIFY(!control.ownsControl(2));
        QVERIFY(control.acquire(2));
        QVERIFY(!control.acquire(1));
        QVERIFY(control.ownsControl(2));
        QVERIFY(control.release(2));
        QVERIFY(control.acquire(1));
        QVERIFY(!control.media().silenceHost); // Requires a fresh policy request.
    }

    void onlyFirstAdmittedClientControls()
    {
        KRdp::ConsoleControl control;
        QVERIFY(!control.ownsControl(1));
        QVERIFY(!control.setMedia(1, {true, true}));
        control.admit(0);
        QVERIFY(!control.ownsControl(0));
        control.admit(1);
        control.admit(2);
        QVERIFY(control.ownsControl(1));
        QVERIFY(!control.ownsControl(2));
        control.remove(1);
        QVERIFY(!control.ownsControl(2));
        control.admit(2); // Repeated Streaming signals must not promote a viewer.
        QVERIFY(!control.ownsControl(2));
        control.admit(3);
        QVERIFY(control.ownsControl(3));
    }

    void viewerCannotOverrideRouting()
    {
        KRdp::ConsoleControl control;
        control.admit(1);
        control.admit(2);
        QVERIFY(control.setMedia(1, {true, true}));
        QVERIFY(control.setMedia(2, {false, false}));
        QVERIFY(control.media().playback);
        QVERIFY(control.media().silenceHost);
        QVERIFY(!control.setMedia(2, {true, true}));
        QVERIFY(control.media().silenceHost);
        QVERIFY(control.setMedia(2, {true, false}));
        control.remove(1);
        QVERIFY(control.media().playback);
        QVERIFY(!control.media().silenceHost);
        control.remove(2);
        QVERIFY(!control.media().playback);
        QVERIFY(!control.media().silenceHost);
    }

    void repeatedAdmissionPreservesMedia()
    {
        KRdp::ConsoleControl control;
        control.admit(1);
        QVERIFY(control.setMedia(1, {true, true}));
        control.admit(1);
        QVERIFY(control.media().silenceHost);
        QVERIFY(control.setMedia(1, {false, true}));
        QVERIFY(!control.media().playback);
        QVERIFY(!control.media().silenceHost);
    }
};

QTEST_GUILESS_MAIN(ConsoleControlTest)
#include "ConsoleControlTest.moc"
