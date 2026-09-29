// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The last lines Farside logged before it stopped.
Kirigami.Dialog {
    id: dialog
    title: i18nc("@title:window", "Farside Log")
    standardButtons: Kirigami.Dialog.Close
    preferredWidth: Kirigami.Units.gridUnit * 36

    QQC2.ScrollView {
        implicitHeight: Math.min(logText.implicitHeight, Kirigami.Units.gridUnit * 20)
        Kirigami.SelectableLabel {
            id: logText
            width: parent.width
            wrapMode: Text.Wrap
            font.family: Kirigami.Theme.fixedWidthFont.family
            text: kcm.errorMessage
        }
    }
}
