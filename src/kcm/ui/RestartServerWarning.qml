// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// AUD-K6: shown whenever the running server was started with settings that
// differ from the saved ones, including after the page was closed and opened.
Kirigami.InlineMessage {
    type: Kirigami.MessageType.Warning
    position: Kirigami.InlineMessage.Position.Header
    Layout.fillWidth: true
    text: i18nc("@info:status %1 list of setting names", "Restart the server to apply these changed settings: %1. Restarting disconnects active connections.", kcm.restartReasons.join(", "))
    actions: [
        Kirigami.Action {
            icon.name: "system-reboot-symbolic"
            text: i18nc("@action:button restart the RDP server", "Restart Server")
            enabled: !kcm.serverBusy
            onTriggered: source => {
                kcm.restartServer();
            }
        }
    ]
}
