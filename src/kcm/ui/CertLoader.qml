// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Dialogs as QtDialogs

Loader {
    id: certLoader
    required property var settings
    property bool selectKey
    active: false
    sourceComponent: QtDialogs.FileDialog {
        title: certLoader.selectKey ? i18nc("@title:window", "Choose Key File") : i18nc("@title:window", "Choose Certificate File")
        Component.onCompleted: open()
        onAccepted: {
            const file = kcm.toLocalFile(selectedFile);
            if (certLoader.selectKey) {
                certLoader.settings.certificateKey = file;
            } else {
                certLoader.settings.certificate = file;
            }
            certLoader.active = false;
        }
        onRejected: {
            certLoader.active = false;
        }
    }
}
