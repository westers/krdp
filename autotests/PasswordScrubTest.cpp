// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// GAP-G2: the password FreeRDP parsed into the settings is overwritten and dropped.

#include <QTest>

#include <freerdp/settings.h>

#include "PasswordScrub.h"

using namespace KRdp;

class PasswordScrubTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void theStoredPasswordIsOverwrittenAndDropped()
    {
        rdpSettings *settings = freerdp_settings_new(FREERDP_SETTINGS_SERVER_MODE);
        QVERIFY(settings);
        // A made-up value, never a real password.
        QVERIFY(freerdp_settings_set_string(settings, FreeRDP_Password, "not-a-real-password"));
        // Only the dropped state is asserted: the bytes are overwritten (explicit_bzero) just before
        // FreeRDP frees them, which cannot be observed without reading freed memory.
        QVERIFY(freerdp_settings_get_string(settings, FreeRDP_Password));
        scrubPeerPassword(settings);
        QVERIFY(!freerdp_settings_get_string(settings, FreeRDP_Password));
        freerdp_settings_free(settings);
    }

    void itIsIdempotentAndNullSafe()
    {
        scrubPeerPassword(nullptr);
        rdpSettings *settings = freerdp_settings_new(FREERDP_SETTINGS_SERVER_MODE);
        QVERIFY(settings);
        scrubPeerPassword(settings); // none set
        scrubPeerPassword(settings);
        QVERIFY(freerdp_settings_set_string(settings, FreeRDP_Password, ""));
        scrubPeerPassword(settings); // empty
        QVERIFY(freerdp_settings_set_string(settings, FreeRDP_Username, "someone"));
        scrubPeerPassword(settings);
        QCOMPARE(QByteArray(freerdp_settings_get_string(settings, FreeRDP_Username)), QByteArray("someone")); // only the password is touched
        freerdp_settings_free(settings);
    }
};

QTEST_GUILESS_MAIN(PasswordScrubTest)
#include "PasswordScrubTest.moc"
