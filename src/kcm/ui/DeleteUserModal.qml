// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.PromptDialog {
    id: deleteUserModal
    // if oldUsername is empty, we're adding a new user
    property string selectedUsername

    showCloseButton: false
    title: i18nc("@title:window", "Remove User?")
    subtitle: i18nc("@info %1 user name", "%1 will no longer be able to sign in to Farside.", selectedUsername)

    standardButtons: Kirigami.Dialog.Cancel
    customFooterActions: [
        Kirigami.Action {
            icon.name: "edit-delete-remove-symbolic"
            text: i18nc("@action:button", "Remove User")
            onTriggered: {
                kcm.deleteUser(deleteUserModal.selectedUsername);
                deleteUserModal.close();
            }
        }
    ]
}
