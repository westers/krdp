// SPDX-FileCopyrightText: 2025 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs as QtDialogs
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard 1 as FormCard
import org.kde.kcmutils as KCM

FormCard.FormCardPage {
    id: root

    property var settings: kcm.settings()

    KCM.ConfigModule.buttons: KCM.ConfigModule.NoAdditionalButton

    EditUserModal {
        id: editUserModal
        parent: root
        implicitWidth: Math.max(Kirigami.Units.gridUnit * 15, Math.round(root.width / 2))
    }

    DeleteUserModal {
        id: deleteUserModal
        parent: root
    }

    KeychainErrorDialog {
        id: keychainErrorDialog
        parent: root
    }

    Connections {
        target: kcm
        function onKeychainError(errorText: string): void {
            keychainErrorDialog.errorText = errorText;
            keychainErrorDialog.open();
        }
    }

    Component.onCompleted: kcm.updateServerStatus()

    // User changes are saved right away; RestartServerWarning below follows
    // what the running server loaded (AUD-K6).
    function modifyUser(user: string): void {
        editUserModal.oldUsername = user;
        editUserModal.open();
    }

    function addUser(): void {
        modifyUser("");
    }

    function deleteUser(user: string): void {
        deleteUserModal.selectedUsername = user;
        deleteUserModal.open();
    }

    // Non-InlineMessage header content does need margins; put it all in here
    // so we can do that in a single place
    ColumnLayout {
        RestartServerWarning {
            id: restartServerWarning
            visible: kcm.serverRunning && kcm.restartRequired
        }

        CodecError {}

        CertError {
            id: certificateError
        }

        Kirigami.InlineMessage {
            type: Kirigami.MessageType.Warning
            visible: kcm.users.loginMethodCount === 0
            Layout.fillWidth: true
            text: i18nc("@info:status", "Nobody can log in yet. Enable login with your system password or add a user; the server does not start without one.")
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Remote Desktop Access")
        }

        FormCard.FormCard {
            padding: Math.round(Kirigami.Units.gridUnit / 2)

            FormCard.FormTextDelegate {
                description: i18n("Set up remote login to connect using apps supporting the “RDP” remote desktop protocol.")
            }
            FormCard.FormSwitchDelegate {
                id: enableServer
                text: i18nc("@option:check", "Enable RDP Server")
                checked: kcm.serverRunning
                enabled: !kcm.serverBusy
                // The mobile page combines starting the server and autostart,
                // and has no Apply button.
                onToggled: {
                    kcm.toggleServer(checked);
                    kcm.autostart = checked;
                    kcm.save();
                }
            }
            FormCard.FormDelegateSeparator {
                visible: kcm.serverRunning
            }

            FormCard.FormTextDelegate {
                text: i18n("Hostname:")
                description: kcm.hostName
                visible: kcm.serverRunning
            }

            FormCard.FormTextDelegate {
                description: i18n("Use any of the following addresses to connect to this device:")
                visible: kcm.serverRunning
            }

            Repeater {
                id: addressesRepeater
                model: kcm.listenAddressList()

                RowLayout {
                    spacing: Kirigami.Units.mediumSpacing
                    visible: kcm.serverRunning

                    Kirigami.SelectableLabel {
                        id: addressLabel
                        text: modelData
                        Layout.leftMargin: Kirigami.Units.gridUnit
                        Layout.alignment: Qt.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    QQC2.Button {
                        id: copyAddressButton
                        icon.name: "edit-copy-symbolic"
                        text: i18nc("@action:button", "Copy Address to Clipboard")
                        display: QQC2.AbstractButton.IconOnly
                        onClicked: {
                            kcm.copyAddressToClipboard(addressLabel.text);
                        }
                        QQC2.ToolTip {
                            text: copyAddressButton.text
                            visible: copyAddressButton.hovered || (Kirigami.Settings.tabletMode && copyAddressButton.pressed)
                        }
                    }
                }
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Server Settings")
        }

        FormCard.FormCard {
            UserListView {
                id: userListView
                implicitHeight: Kirigami.Units.gridUnit * 16
                implicitWidth: parent.width
            }
        }
    }
}
