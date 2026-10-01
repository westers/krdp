// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
import QtQuick.Controls as QQC2

QQC2.Label {
    property var service
    text: {
        if (!service || !service.known) return i18nc("@info:status", "Status unavailable");
        if (service.loadState === "not-found") return i18nc("@info:status", "Not installed");
        if (service.loadState === "masked" || service.unitFileState.startsWith("masked")) return i18nc("@info:status", "Blocked by the administrator");
        switch (service.activeState) {
        case "active": return i18nc("@info:status", "Running");
        case "inactive": return i18nc("@info:status", "Stopped");
        case "failed": return i18nc("@info:status", "Failed");
        case "activating": return i18nc("@info:status", "Starting…");
        case "deactivating": return i18nc("@info:status", "Stopping…");
        case "reloading": return i18nc("@info:status", "Reloading…");
        default: return i18nc("@info:status", "Status unavailable");
        }
    }
}
