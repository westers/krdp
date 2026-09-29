// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

// How the picture and sound behave, in effects rather than codec names.
KCM.SimpleKCM {
    id: root
    objectName: "videoAudioPage"

    title: i18nc("@title:window", "Video and Audio")

    readonly property var settings: kcm.settings()

    Kirigami.FormLayout {
        RowLayout {
            Kirigami.FormData.label: i18nc("@label:listbox", "Color detail:")
            spacing: Kirigami.Units.smallSpacing
            SettingComboBox {
                objectName: "colorDetailCombo"
                configObject: root.settings
                settingName: "codec"
                model: [
                    {text: i18nc("@item:inlistbox", "Automatic (full color when the viewer supports it)"), value: "auto"},
                    {text: i18nc("@item:inlistbox", "Always standard"), value: "avc420"},
                    {text: i18nc("@item:inlistbox", "Full color, warn when the viewer can't"), value: "avc444"}
                ]
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18nc("@info:tooltip", "Full color keeps small colored text and thin lines crisp, at a little more bandwidth. Standard color halves the color resolution, like most video.")
            }
        }

        ColumnLayout {
            objectName: "busyNetworkColumn"
            Kirigami.FormData.label: i18nc("@label", "When the network is busy:")
            Kirigami.FormData.buddyFor: keepVideoRadio
            spacing: Kirigami.Units.smallSpacing

            KCM.SettingStateBinding {
                configObject: root.settings
                settingName: "preferAudioQuality"
            }
            QQC2.ButtonGroup {
                id: busyGroup
            }
            QQC2.RadioButton {
                id: keepVideoRadio
                QQC2.ButtonGroup.group: busyGroup
                text: i18nc("@option:radio", "Keep video sharp")
                checked: !root.settings.preferAudioQuality
                onToggled: root.settings.preferAudioQuality = false
            }
            RowLayout {
                spacing: Kirigami.Units.smallSpacing
                QQC2.RadioButton {
                    QQC2.ButtonGroup.group: busyGroup
                    text: i18nc("@option:radio", "Keep sound smooth")
                    checked: root.settings.preferAudioQuality
                    onToggled: root.settings.preferAudioQuality = true
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18nc("@info:tooltip", "The starting choice for each connection. The Farside client can change it during a session.")
                }
            }
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        RowLayout {
            Kirigami.FormData.label: i18nc("@label other remote desktop apps", "Other RDP apps:")
            spacing: Kirigami.Units.smallSpacing
            QQC2.CheckBox {
                objectName: "standardMediaCheck"
                text: i18nc("@option:check", "Share sound, microphone and camera")
                checked: root.settings.standardClientMedia
                onToggled: root.settings.standardClientMedia = checked
                KCM.SettingStateBinding {
                    configObject: root.settings
                    settingName: "standardClientMedia"
                }
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18nc("@info:tooltip", "Windows Remote Desktop, Remmina and FreeRDP ask for these themselves when they connect. The Farside client turns each device on and off during the session.")
            }
        }
    }
}
