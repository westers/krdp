// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <QSize>
#include <cmath>

namespace KRdp::WorkspaceFrameGeometry
{
// KWin may round each logical axis independently. A private resize supplies
// the KScreen scale explicitly; Qt Wayland's QScreen DPR can round it to 2.
inline bool matches(QSize pixels, QSize logical, double scale)
{
    return pixels.width() > 0 && pixels.height() > 0 && logical.width() > 0 && logical.height() > 0
        && std::isfinite(scale) && scale >= 1 && scale <= 4
        && std::abs(double(logical.width()) - pixels.width() / scale) <= 1.0 + 0.000001
        && std::abs(double(logical.height()) - pixels.height() / scale) <= 1.0 + 0.000001;
}
}
