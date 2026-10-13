// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

namespace KRdp
{
/// OPT-070: scale an RDP/Qt wheel angle delta (120 = one notch) to the value KWin's
/// org_kde_kwin_fake_input.axis expects.
///
/// fake_input forwards the value unchanged as a continuous axis delta with deltaV120 = 0
/// (KWin backends/fakeinput). A real libinput wheel notch is 15 units (and v120 = 120);
/// KWin's own MouseWheelAccumulator counts delta / 15 as notches. The old code sent
/// delta / 120.0 = 1.0 per notch, i.e. 1/15 of a native notch.
constexpr double WaylandAxisUnitsPerNotch = 15.0;
constexpr double RdpUnitsPerNotch = 120.0;

constexpr double fakeInputAxisValue(int angleDelta)
{
    return angleDelta * (WaylandAxisUnitsPerNotch / RdpUnitsPerNotch);
}
}
