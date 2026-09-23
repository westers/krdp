// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QSize>
#include <optional>

namespace KRdp
{
// Independent payload evidence for a resize completion. Called only on
// candidate keyframes while Fit/recovery is gated, never on normal video.
// Requires self-contained Annex-B SPS/PPS/IDR, bounded dimensions and a clean
// software decode. No hardware device or persistent decoder is created.
std::optional<QSize> h264KeyframeSize(const QByteArray &packet);
}
