// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QStringList>

namespace KRdp
{
/**
 * The Wayland globals the compositor advertises to this process (AUD-FIX F3).
 *
 * KWin only offers its restricted globals (zkde_screencast_unstable_v1,
 * org_kde_kwin_fake_input) to a client whose desktop file asks for them, so
 * this is the real answer to "can the Plasma backend work here". Uses its own
 * event queue, so Qt's queue and objects are untouched. Empty when the
 * application is not on the Wayland platform.
 */
QStringList advertisedWaylandGlobals();
}
