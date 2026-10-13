// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <Qt>

#include <linux/input-event-codes.h>

namespace KRdp
{

/**
 * Evdev button code (BTN_*) for a Qt pointer button, or 0 when the button has
 * no host equivalent. RDP extended mouse events XBUTTON1/XBUTTON2 reach the
 * sessions as Qt::BackButton/Qt::ForwardButton (OPT-069).
 *
 * Pure so every injection backend shares one table and it is unit tested.
 */
constexpr int evdevButtonFromQt(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:
        return BTN_LEFT;
    case Qt::MiddleButton:
        return BTN_MIDDLE;
    case Qt::RightButton:
        return BTN_RIGHT;
    case Qt::BackButton:
        return BTN_SIDE;
    case Qt::ForwardButton:
        return BTN_EXTRA;
    default:
        return 0;
    }
}

}
