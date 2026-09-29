// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

// Marks a setting that Farside reads only when it starts. The page shows one
// "Restart Now" message when a saved change is waiting for that.
Kirigami.Icon {
    id: icon
    source: "view-refresh-symbolic"
    implicitWidth: Kirigami.Units.iconSizes.small
    implicitHeight: Kirigami.Units.iconSizes.small
    color: Kirigami.Theme.disabledTextColor
    Accessible.name: tip.text
    Accessible.role: Accessible.StaticText

    HoverHandler {
        id: hover
    }
    QQC2.ToolTip {
        id: tip
        text: i18nc("@info:tooltip", "Takes effect when Farside restarts")
        visible: hover.hovered
    }
}
