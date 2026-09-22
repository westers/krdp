// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QString>
#include <QUuid>
#include <optional>

namespace KRdp
{
/** Explicit launch mode, not inferred from the presence of physical outputs.
 * A virtual ID names an authenticated registry entry, never a logind seat.
 * Choosing a mode does not itself establish launcher identity or isolation.
 */
struct CaptureWorkerMode {
    QString sessionId;
    bool virtualSession = false;
    bool physicalActions() const { return !virtualSession; }

    static std::optional<CaptureWorkerMode> parse(const QString &logind, const QString &virtualId)
    {
        if (logind.isEmpty() == virtualId.isEmpty()) {
            return {};
        }
        if (!virtualId.isEmpty()) {
            const QUuid uuid(virtualId);
            if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != virtualId) {
                return {};
            }
            return CaptureWorkerMode{virtualId, true};
        }
        return CaptureWorkerMode{logind, false};
    }
};
}
