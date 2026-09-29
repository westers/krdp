// SPDX-FileCopyrightText: 2025 Sebastian Kügler <sebas@kde.org>
// SPDX-FileCopyrightText: 2026 Steve Westers
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

// Who can sign in, and the certificate viewers check. The whole page scrolls;
// the user list is part of it rather than a view squeezed between blocks.
KCM.SimpleKCM {
    id: root
    objectName: "usersPage"

    title: i18nc("@title:window", "Users and Security")

    readonly property var settings: kcm.settings()

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

    Connections {
        target: kcm
        function onKeychainError(errorText: string): void {
            keychainErrorDialog.errorText = errorText;
            keychainErrorDialog.open();
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.FormLayout {
            id: signInForm
            Layout.fillWidth: true
            twinFormLayouts: [certificateForm]

            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Sign-In")
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Your account:")
                spacing: Kirigami.Units.smallSpacing
                QQC2.CheckBox {
                    objectName: "systemUserCheck"
                    text: i18nc("@option:check %1 login name", "Allow %1 to sign in with its password", kcm.systemUserName)
                    checked: root.settings.systemUserEnabled
                    onToggled: root.settings.systemUserEnabled = checked
                    KCM.SettingStateBinding {
                        configObject: root.settings
                        settingName: "systemUserEnabled"
                    }
                }
                RestartIcon {}
                Kirigami.ContextualHelpButton {
                    toolTipText: i18nc("@info:tooltip", "Uses the same password as logging in to this computer (PAM). The account must be the one that is logged in here.")
                }
            }
        }

        QQC2.Frame {
            objectName: "usersFrame"
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 30
            Layout.alignment: Qt.AlignHCenter
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            padding: 0

            contentItem: ColumnLayout {
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.smallSpacing
                    Layout.leftMargin: Kirigami.Units.largeSpacing
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Heading {
                        Layout.fillWidth: true
                        level: 4
                        text: i18nc("@title:group users with their own password", "Other Users")
                    }
                    RestartIcon {}
                    QQC2.Button {
                        objectName: "addUserButton"
                        icon.name: "list-add-symbolic"
                        text: i18nc("@action:button", "Add User…")
                        enabled: !kcm.keychainBusy
                        onClicked: root.addUser()
                    }
                }

                Kirigami.Separator {
                    Layout.fillWidth: true
                }

                Repeater {
                    model: root.settings.users
                    delegate: QQC2.ItemDelegate {
                        id: userDelegate
                        required property string modelData
                        Layout.fillWidth: true
                        text: modelData
                        onClicked: root.modifyUser(modelData)

                        contentItem: Kirigami.TitleSubtitleWithActions {
                            title: userDelegate.modelData
                            subtitle: i18nc("@info:usagetip", "Signs in with a password for remote desktop only")
                            elide: Text.ElideRight
                            selected: userDelegate.pressed || userDelegate.highlighted
                            displayHint: QQC2.Button.IconOnly
                            actions: [
                                Kirigami.Action {
                                    icon.name: "edit-entry-symbolic"
                                    text: i18nc("@action:button", "Edit User…")
                                    tooltip: text
                                    enabled: !kcm.keychainBusy
                                    onTriggered: root.modifyUser(userDelegate.modelData)
                                },
                                Kirigami.Action {
                                    icon.name: "edit-delete-remove-symbolic"
                                    text: i18nc("@action:button", "Remove User…")
                                    tooltip: text
                                    enabled: !kcm.keychainBusy
                                    onTriggered: root.deleteUser(userDelegate.modelData)
                                }
                            ]
                        }
                    }
                }

                Kirigami.PlaceholderMessage {
                    objectName: "usersPlaceholder"
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing
                    visible: root.settings.users.length === 0
                    readonly property bool nobody: kcm.users.loginMethodCount === 0
                    icon.name: nobody ? "im-user-offline" : "system-users"
                    text: nobody ? i18nc("@info:placeholder", "No one can sign in yet") : i18nc("@info:placeholder", "No other users")
                    explanation: nobody
                        ? i18nc("@info:placeholder", "Allow your account, or add a user with a password just for remote desktop.")
                        : i18nc("@info:placeholder", "Add a user with a password just for remote desktop, for example for someone else.")
                    helpfulAction: nobody ? allowMyAccountAction : addUserAction
                    Kirigami.Action {
                        id: allowMyAccountAction
                        icon.name: "user-symbolic"
                        text: i18nc("@action:button", "Allow My Account")
                        onTriggered: root.settings.systemUserEnabled = true
                    }
                    Kirigami.Action {
                        id: addUserAction
                        icon.name: "list-add-symbolic"
                        text: i18nc("@action:button", "Add User…")
                        enabled: !kcm.keychainBusy
                        onTriggered: root.addUser()
                    }
                }
            }
        }

        Kirigami.FormLayout {
            id: certificateForm
            Layout.fillWidth: true
            twinFormLayouts: [signInForm]

            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18nc("@title:group", "Certificate")
            }

            RowLayout {
                objectName: "certificateStateRow"
                Kirigami.FormData.label: i18nc("@label", "Certificate:")
                spacing: Kirigami.Units.smallSpacing
                QQC2.Label {
                    Layout.fillWidth: true
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    readonly property bool managed: root.settings.autogenerateCertificates
                    color: kcm.certificateState === "valid" || (managed && kcm.certificateState !== "unusable")
                        ? Kirigami.Theme.textColor : Kirigami.Theme.negativeTextColor
                    text: {
                        switch (kcm.certificateState) {
                        case "valid":
                            return managed
                                ? i18nc("@info %1 date", "Created by Farside, valid until %1", kcm.certificateExpiry)
                                : i18nc("@info %1 algorithm, %2 date", "%1, valid until %2", kcm.certificateAlgorithm, kcm.certificateExpiry);
                        case "expiring":
                            return managed
                                ? i18nc("@info %1 date", "Expires on %1; Farside renews it when it starts.", kcm.certificateExpiry)
                                : i18nc("@info %1 date", "Expires on %1. Replace it soon.", kcm.certificateExpiry);
                        case "expired":
                            return managed
                                ? i18nc("@info", "Expired; Farside renews it when it starts.")
                                : i18nc("@info %1 date", "Expired on %1.", kcm.certificateExpiry);
                        case "missing":
                            return managed
                                ? i18nc("@info", "None yet; Farside creates one when it starts.")
                                : i18nc("@info", "No certificate or key at these paths.");
                        default:
                            return managed
                                ? i18nc("@info", "Unreadable; Farside replaces it when it starts.")
                                : i18nc("@info", "The certificate or key can't be read, or they don't belong together.");
                        }
                    }
                }
                RestartIcon {}
            }

            FingerprintLabel {
                objectName: "certificateFingerprint"
                Kirigami.FormData.label: i18nc("@label the certificate fingerprint", "Fingerprint:")
            }

            QQC2.CheckBox {
                id: ownCertificateCheck
                objectName: "ownCertificateCheck"
                text: i18nc("@option:check", "Use my own certificate")
                checked: !root.settings.autogenerateCertificates
                onToggled: root.settings.autogenerateCertificates = !checked
                KCM.SettingStateBinding {
                    configObject: root.settings
                    settingName: "autogenerateCertificates"
                }
            }

            RowLayout {
                visible: ownCertificateCheck.checked
                Kirigami.FormData.label: i18nc("@label:textbox", "Certificate file:")
                spacing: Kirigami.Units.smallSpacing
                QQC2.TextField {
                    id: certPathField
                    Layout.fillWidth: true
                    Layout.minimumWidth: Kirigami.Units.gridUnit * 10
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 18
                    text: root.settings.certificate
                    placeholderText: i18nc("@info:placeholder", "/path/to/certificate.crt")
                    onTextEdited: root.settings.certificate = text
                    KCM.SettingStateBinding {
                        configObject: root.settings
                        settingName: "certificate"
                    }
                }
                QQC2.Button {
                    icon.name: "document-open-folder-symbolic"
                    text: i18nc("@action:button", "Choose Certificate File…")
                    display: QQC2.AbstractButton.IconOnly
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    onClicked: {
                        certLoader.selectKey = false;
                        certLoader.active = true;
                    }
                }
            }

            RowLayout {
                visible: ownCertificateCheck.checked
                Kirigami.FormData.label: i18nc("@label:textbox", "Key file:")
                spacing: Kirigami.Units.smallSpacing
                QQC2.TextField {
                    Layout.fillWidth: true
                    Layout.minimumWidth: Kirigami.Units.gridUnit * 10
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 18
                    text: root.settings.certificateKey
                    placeholderText: i18nc("@info:placeholder", "/path/to/certificate.key")
                    onTextEdited: root.settings.certificateKey = text
                    KCM.SettingStateBinding {
                        configObject: root.settings
                        settingName: "certificateKey"
                    }
                }
                QQC2.Button {
                    icon.name: "document-open-folder-symbolic"
                    text: i18nc("@action:button", "Choose Key File…")
                    display: QQC2.AbstractButton.IconOnly
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    onClicked: {
                        certLoader.selectKey = true;
                        certLoader.active = true;
                    }
                }
            }
        }
    }

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

    CertLoader {
        id: certLoader
        settings: root.settings
    }
}
