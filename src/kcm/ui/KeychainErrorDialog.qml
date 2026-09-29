// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.PromptDialog {
    id: keychainErrorDialog

    property string errorText

    showCloseButton: false
    title: i18nc("@title:window", "Password Storage Error")
    subtitle: i18nc("@info %1 error text", "The password could not be read or saved: %1", errorText)

    standardButtons: Kirigami.Dialog.Ok
}
