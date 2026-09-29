// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

// An icon-only button that copies `value` to the clipboard.
QQC2.ToolButton {
    id: button
    required property string value
    icon.name: "edit-copy-symbolic"
    display: QQC2.AbstractButton.IconOnly
    Accessible.name: text
    onClicked: kcm.copyAddressToClipboard(value)

    QQC2.ToolTip.text: text
    QQC2.ToolTip.visible: hovered || (Kirigami.Settings.tabletMode && pressed)
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
}
