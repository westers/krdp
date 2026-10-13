// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>

#include "InputMapping.h"

class InputMappingTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void standardButtonsKeepTheirCodes()
    {
        QCOMPARE(KRdp::evdevButtonFromQt(Qt::LeftButton), int(BTN_LEFT));
        QCOMPARE(KRdp::evdevButtonFromQt(Qt::RightButton), int(BTN_RIGHT));
        QCOMPARE(KRdp::evdevButtonFromQt(Qt::MiddleButton), int(BTN_MIDDLE));
    }

    void backAndForwardMapToSideAndExtra()
    {
        // RDP XBUTTON1/XBUTTON2 arrive as Qt::BackButton/ForwardButton.
        QCOMPARE(KRdp::evdevButtonFromQt(Qt::BackButton), int(BTN_SIDE));
        QCOMPARE(KRdp::evdevButtonFromQt(Qt::ForwardButton), int(BTN_EXTRA));
    }

    void unsupportedButtonsAreRejected()
    {
        QCOMPARE(KRdp::evdevButtonFromQt(Qt::NoButton), 0);
        QCOMPARE(KRdp::evdevButtonFromQt(Qt::TaskButton), 0);
        QCOMPARE(KRdp::evdevButtonFromQt(Qt::ExtraButton5), 0);
    }
};

QTEST_GUILESS_MAIN(InputMappingTest)
#include "InputMappingTest.moc"
