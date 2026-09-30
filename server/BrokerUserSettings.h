// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "CodecPolicy.h"
#include "VideoCodecSupport.h"
#include <QByteArray>
#include <QSize>
#include <QString>
#include <functional>
#include <optional>

namespace KRdp::BrokerUserSettings
{
/** Preferences only: no TLS, listener, admission, credentials or device grants.
 * Missing keys inherit the configured host defaults. The entire preference
 * transaction is rejected on an invalid recognized value.
 */
struct Preferences {
    std::optional<quint8> quality;
    std::optional<bool> adaptiveQuality;
    std::optional<bool> preferAudioQuality;
    std::optional<CodecPreference> codec;
    std::optional<CodecPolicy::SoftwareEncoding> softwareEncoding;
    std::optional<int> av1Tiles;
    std::optional<ChromaPolicy> chroma;
    std::optional<QString> monitorMode;
    std::optional<int> monitorIndex;
    std::optional<QString> virtualMonitorPolicy;
    std::optional<QString> virtualMonitorLayout;
    std::optional<QSize> virtualMonitorFallbackSize;
    std::optional<bool> wakeDisplayOnConnect;
    std::optional<bool> standardClientMedia;
    std::optional<QString> virtualStockClientPolicy;
};

struct Result {
    Preferences preferences;
    // Key/reason only, never configuration contents or credentials.
    QString error;
};
using Reader = std::function<Result(quint32)>;
Result parse(const QByteArray &contents);
Result readUser(quint32 authenticatedUid);
}
