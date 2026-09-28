// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

namespace KRdp
{
/// The video codec of an RDPGFX stream. Its own header (AUD-FIX7) so the broker/worker wire and
/// VideoFrame can name it without FreeRDP; VideoCodecSupport.h has the RDPGFX side.
enum class VideoCodec { Avc420, Avc444, Avc444v2, Hevc, Av1 };
}
