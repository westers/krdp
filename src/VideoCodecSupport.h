// SPDX-FileCopyrightText: 2026 KDE Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QtGlobal>

namespace KRdp
{

// True when experimental AVC444 wire transport is enabled.
// This currently uses single-stream AVC444/AVC444v2 payloads that carry the
// existing H.264 bitstream from the encoder path.
inline bool LocalAvc444EncodingAvailable()
{
    return qEnvironmentVariableIntValue("KRDP_EXPERIMENTAL_TRUE_AVC444") > 0;
}

}
