// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

// What a viewer sees: which monitors, and what happens to the desk's
// monitors while someone is connected.
KCM.SimpleKCM {
    id: root
    objectName: "screensPage"

    title: i18nc("@title:window", "Screens and Displays")

    readonly property var settings: kcm.settings()
    readonly property var monitors: kcm.monitors()
    readonly property bool virtualMode: settings.monitorMode === "virtual"

    function available(mode: string): bool {
        return kcm.plasmaBackendAvailable || !kcm.monitorModeNeedsPlasma(mode) || settings.monitorMode === mode;
    }

    Kirigami.FormLayout {
        QQC2.ButtonGroup {
            id: modeGroup
        }

        ColumnLayout {
            objectName: "shareColumn"
            Kirigami.FormData.label: i18nc("@label which screens a viewer sees", "Share:")
            Kirigami.FormData.buddyFor: workspaceRadio
            spacing: Kirigami.Units.smallSpacing

            KCM.SettingStateBinding {
                configObject: root.settings
                settingName: "monitorMode"
            }

            QQC2.RadioButton {
                id: workspaceRadio
                QQC2.ButtonGroup.group: modeGroup
                text: i18nc("@option:radio", "All monitors as one")
                checked: root.settings.monitorMode === "workspace"
                onToggled: root.settings.monitorMode = "workspace"
            }
            QQC2.RadioButton {
                QQC2.ButtonGroup.group: modeGroup
                visible: root.available("primary")
                text: i18nc("@option:radio", "The primary monitor")
                checked: root.settings.monitorMode === "primary"
                onToggled: root.settings.monitorMode = "primary"
            }
            RowLayout {
                visible: root.available("specific")
                spacing: Kirigami.Units.smallSpacing
                QQC2.RadioButton {
                    id: specificRadio
                    QQC2.ButtonGroup.group: modeGroup
                    text: i18nc("@option:radio followed by a list of monitors", "One monitor:")
                    checked: root.settings.monitorMode === "specific"
                    onToggled: root.settings.monitorMode = "specific"
                }
                QQC2.ComboBox {
                    objectName: "monitorCombo"
                    enabled: specificRadio.checked
                    textRole: "text"
                    valueRole: "index"
                    Accessible.name: i18nc("@label:listbox", "Monitor")
                    model: {
                        const entries = root.monitors.slice();
                        const index = root.settings.monitorIndex;
                        if (!entries.some(entry => entry.index === index)) {
                            entries.push({index: index, text: i18nc("@item:inlistbox %1 monitor number", "Monitor %1 (not connected)", index + 1)});
                        }
                        return entries;
                    }
                    currentIndex: Math.max(0, indexOfValue(root.settings.monitorIndex))
                    onActivated: root.settings.monitorIndex = currentValue
                    KCM.SettingStateBinding {
                        configObject: root.settings
                        settingName: "monitorIndex"
                        extraEnabledConditions: specificRadio.checked
                    }
                }
            }
            QQC2.RadioButton {
                QQC2.ButtonGroup.group: modeGroup
                visible: root.available("multi")
                text: i18nc("@option:radio", "Each monitor separately")
                checked: root.settings.monitorMode === "multi"
                onToggled: root.settings.monitorMode = "multi"
            }
            QQC2.RadioButton {
                QQC2.ButtonGroup.group: modeGroup
                visible: root.available("virtual")
                text: i18nc("@option:radio", "Virtual monitors sized for the viewer")
                checked: root.virtualMode
                onToggled: root.settings.monitorMode = "virtual"
            }
            QQC2.Label {
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                visible: !kcm.plasmaBackendAvailable && kcm.monitorModeNeedsPlasma(root.settings.monitorMode)
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.negativeTextColor
                text: i18nc("@info", "This Farside build can't do that, so it shares all monitors as one.")
            }
        }

        SettingComboBox {
            objectName: "virtualPolicyCombo"
            Kirigami.FormData.label: i18nc("@label:listbox the physical monitors at the desk", "Desk monitors:")
            visible: root.virtualMode
            configObject: root.settings
            settingName: "virtualMonitorPolicy"
            model: [
                {text: i18nc("@item:inlistbox", "Turn off while connected"), value: "replace"},
                {text: i18nc("@item:inlistbox", "Keep on"), value: "extend"}
            ]
        }

        SettingComboBox {
            objectName: "virtualLayoutCombo"
            Kirigami.FormData.label: i18nc("@label:listbox", "Layout:")
            visible: root.virtualMode
            configObject: root.settings
            settingName: "virtualMonitorLayout"
            model: [
                {text: i18nc("@item:inlistbox", "One per viewer monitor"), value: "client"},
                {text: i18nc("@item:inlistbox", "One at the viewer's desktop size"), value: "single"},
                {text: i18nc("@item:inlistbox", "One per desk monitor, same arrangement"), value: "physical"}
            ]
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        QQC2.CheckBox {
            objectName: "wakeCheck"
            text: i18nc("@option:check", "Wake the screen when someone connects")
            checked: root.settings.wakeDisplayOnConnect
            onToggled: root.settings.wakeDisplayOnConnect = checked
            KCM.SettingStateBinding {
                configObject: root.settings
                settingName: "wakeDisplayOnConnect"
            }
        }
    }
}
