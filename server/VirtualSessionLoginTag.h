// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QString>
#include <QUuid>
namespace KRdp {
// Set before registration, so recovery does not depend on receiving Ready.
// This is logind metadata for the keeper, not the desktop app environment.
inline QString virtualLoginTag(const QString &launch)
{
    if (QUuid(launch).isNull() || QUuid(launch).toString(QUuid::WithoutBraces) != launch) return {};
    return QStringLiteral("krdp-") + launch;
}
}
