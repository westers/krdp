// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

// Settings with good defaults that few people change, and the developer
// options behind one more click.
KCM.SimpleKCM {
    id: root
    objectName: "advancedPage"

    title: i18nc("@title:window", "Advanced")

    readonly property var settings: kcm.settings()
    readonly property var coexistence: kcm.coexistence

    Kirigami.FormLayout {
        // --- Network --------------------------------------------------------
        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "Network")
        }

        RowLayout {
            Kirigami.FormData.label: i18nc("@label:listbox", "Listening address:")
            spacing: Kirigami.Units.smallSpacing
            SettingComboBox {
                objectName: "listenAddressCombo"
                configObject: root.settings
                settingName: "listenAddress"
                model: {
                    const entries = [{text: i18nc("@item:inlistbox listen on every network interface", "All addresses"), value: ""}];
                    const addresses = kcm.interfaceAddresses();
                    for (const address of addresses) {
                        entries.push({text: address, value: address});
                    }
                    const current = root.settings.listenAddress;
                    // krdpserver reads "*" like "" (every interface): SettingComboBox
                    // shows an unknown value as the first entry, "All addresses".
                    if (current.trim() !== "" && current.trim() !== "*" && !addresses.includes(current)) {
                        entries.push({text: i18nc("@item:inlistbox %1 an address that no interface has now", "%1 (not present)", current), value: current});
                    }
                    return entries;
                }
            }
            RestartIcon {}
        }

        RowLayout {
            Kirigami.FormData.label: i18nc("@label:spinbox", "Port:")
            spacing: Kirigami.Units.smallSpacing
            QQC2.SpinBox {
                id: portField
                objectName: "portField"
                // AUD-K4: the server refuses anything outside this range.
                from: 1
                to: 65535
                editable: true
                value: root.settings.listenPort
                textFromValue: (value, locale) => String(value) // no digit grouping
                valueFromText: (text, locale) => parseInt(text)
                validator: IntValidator {
                    bottom: portField.from
                    top: portField.to
                }
                Accessible.name: i18nc("@label:spinbox", "Port")
                onValueModified: root.settings.listenPort = value
                KCM.SettingStateBinding {
                    configObject: root.settings
                    settingName: "listenPort"
                }
            }
            RestartIcon {}
            Kirigami.ContextualHelpButton {
                toolTipText: i18nc("@info:tooltip", "Remote desktop apps use port 3389 unless you add another one to the address, as in computer:3390.")
            }
        }

        QQC2.Label {
            objectName: "stockInstalledNote"
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            visible: root.coexistence.stockInstalled
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            color: Kirigami.Theme.disabledTextColor
            text: i18nc("@info %1 port number", "KDE's built-in remote desktop is installed too. Only one of them can use port %1 at a time.", String(root.coexistence.stockPort))
        }

        // --- Encoding -------------------------------------------------------
        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group video encoding", "Encoding")
        }

        SettingComboBox {
            objectName: "vaapiCombo"
            Kirigami.FormData.label: i18nc("@label:listbox", "Hardware video driver:")
            configObject: root.settings
            settingName: "vaapiDriverMode"
            model: {
                const names = {
                    radeonsi: i18nc("@item:inlistbox", "AMD (radeonsi)"),
                    iHD: i18nc("@item:inlistbox", "Intel (iHD)"),
                    i965: i18nc("@item:inlistbox", "Intel, older GPUs (i965)")
                };
                const entries = [
                    {text: i18nc("@item:inlistbox", "Automatic"), value: "auto"},
                    {text: i18nc("@item:inlistbox", "Off (encode in software)"), value: "off"}
                ];
                const installed = kcm.vaapiDrivers();
                for (const driver of installed) {
                    entries.push({text: names[driver], value: driver});
                }
                const current = root.settings.vaapiDriverMode;
                if (names[current] !== undefined && !installed.includes(current)) {
                    entries.push({text: i18nc("@item:inlistbox %1 driver name", "%1 (not installed)", names[current]), value: current});
                }
                return entries;
            }
        }

        RowLayout {
            Kirigami.FormData.label: i18nc("@label:listbox", "Software encoding:")
            spacing: Kirigami.Units.smallSpacing
            SettingComboBox {
                objectName: "softwareEncodingCombo"
                configObject: root.settings
                settingName: "softwareEncoding"
                model: [
                    {text: i18nc("@item:inlistbox software video encoding", "When there is no hardware encoder"), value: "auto"},
                    {text: i18nc("@item:inlistbox software video encoding", "Prefer the best compression"), value: "prefer"},
                    {text: i18nc("@item:inlistbox software video encoding", "Only as a last resort"), value: "never"}
                ]
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18nc("@info:tooltip", "Software encoding uses more processor time. On a slow connection the Farside client may get a better-compressing format encoded in software, as long as the processor keeps up.")
            }
        }

        ColumnLayout {
            Kirigami.FormData.label: i18nc("@label:listbox", "AV1 tiles:")
            Kirigami.FormData.buddyFor: av1TilesCombo
            spacing: Kirigami.Units.smallSpacing
            SettingComboBox {
                id: av1TilesCombo
                objectName: "av1TilesCombo"
                configObject: root.settings
                settingName: "av1Tiles"
                model: [
                    {text: i18nc("@item:inlistbox AV1 tiles", "Automatic (recommended)"), value: "auto"},
                    {text: i18nc("@item:inlistbox AV1 tile count", "1"), value: "1"},
                    {text: i18nc("@item:inlistbox AV1 tile count", "2"), value: "2"},
                    {text: i18nc("@item:inlistbox AV1 tile count", "4"), value: "4"},
                    {text: i18nc("@item:inlistbox AV1 tile count", "8"), value: "8"},
                    {text: i18nc("@item:inlistbox AV1 tile count", "16"), value: "16"}
                ]
            }
            QQC2.Label {
                objectName: "av1TilesNote"
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                text: i18nc("@info", "More tiles let slower computers decode AV1 faster, at a small size cost.")
            }
        }

        // --- Virtual monitors -----------------------------------------------
        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "Virtual Monitors")
        }

        ColumnLayout {
            Kirigami.FormData.label: i18nc("@label:textbox", "Fallback size:")
            Kirigami.FormData.buddyFor: fallbackSizeRow
            spacing: Kirigami.Units.smallSpacing
            readonly property bool valid: kcm.isValidFallbackSize(root.settings.virtualMonitorFallbackSize)
            RowLayout {
                id: fallbackSizeRow
                spacing: Kirigami.Units.smallSpacing
                QQC2.TextField {
                    id: fallbackSizeField
                    objectName: "fallbackSizeField"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 8
                    placeholderText: "1920x1080"
                    text: root.settings.virtualMonitorFallbackSize
                    onTextEdited: root.settings.virtualMonitorFallbackSize = text.trim()
                    KCM.SettingStateBinding {
                        configObject: root.settings
                        settingName: "virtualMonitorFallbackSize"
                    }
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18nc("@info:tooltip", "The size of a virtual monitor when the viewer doesn't say how big its screen is.")
                }
            }
            QQC2.Label {
                visible: !parent.valid
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.negativeTextColor
                text: i18nc("@info", "Enter WIDTHxHEIGHT between 640 and 4096; Farside uses 1920x1080 otherwise.")
            }
        }

        // --- Devices --------------------------------------------------------
        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "Devices")
        }

        RowLayout {
            Kirigami.FormData.label: i18nc("@label:listbox", "Camera for older apps:")
            spacing: Kirigami.Units.smallSpacing
            SettingComboBox {
                objectName: "cameraCombo"
                configObject: root.settings
                settingName: "cameraLoopbackDevice"
                model: {
                    const entries = [{text: i18nc("@item:inlistbox no loopback camera", "None"), value: ""}];
                    const cameras = kcm.loopbackCameras();
                    for (const camera of cameras) {
                        entries.push({
                            text: camera.name !== "" ? i18nc("@item:inlistbox %1 device path, %2 name", "%1 (%2)", camera.path, camera.name) : camera.path,
                            value: camera.path
                        });
                    }
                    const current = root.settings.cameraLoopbackDevice;
                    if (current !== "" && !cameras.some(camera => camera.path === current)) {
                        entries.push({text: i18nc("@item:inlistbox %1 device path", "%1 (not present)", current), value: current});
                    }
                    return entries;
                }
            }
            Kirigami.ContextualHelpButton {
                toolTipText: xi18nc("@info:tooltip", "A v4l2loopback device that also receives the viewer's camera, for apps that can't use PipeWire. Only loopback devices that exist now are listed.")
            }
        }

        // --- Virtual desktop service ----------------------------------------
        Item {
            Kirigami.FormData.isSection: true
            visible: root.coexistence.virtualHostInstalled
            Kirigami.FormData.label: i18nc("@title:group", "Virtual Desktop Service")
        }

        ColumnLayout {
            visible: root.coexistence.virtualHostInstalled
            Kirigami.FormData.label: i18nc("@label:listbox other remote desktop apps", "Other RDP apps:")
            spacing: Kirigami.Units.smallSpacing
            SettingComboBox {
                objectName: "stockClientPolicyCombo"
                configObject: root.settings
                settingName: "virtualStockClientPolicy"
                model: [
                    {text: i18nc("@item:inlistbox", "Resume my latest desktop or start one"), value: "attach-or-create"},
                    {text: i18nc("@item:inlistbox", "Turn them away"), value: "refuse"}
                ]
            }
            QQC2.Label {
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                text: i18nc("@info", "Applies to your account on this computer's virtual desktop service. Windows Remote Desktop, Remmina and FreeRDP can't choose a desktop; the Farside client always lets you choose.")
            }
        }

        // --- Developer ------------------------------------------------------
        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "Developer")
        }

        QQC2.Button {
            id: developerToggle
            objectName: "developerToggle"
            checkable: true
            icon.name: checked ? "arrow-up-symbolic" : "arrow-down-symbolic"
            text: checked ? i18nc("@action:button", "Hide Developer Options") : i18nc("@action:button", "Show Developer Options")
        }

        ColumnLayout {
            objectName: "colorTimingColumn"
            visible: developerToggle.checked
            Kirigami.FormData.label: i18nc("@label", "Full-color timing:")
            spacing: Kirigami.Units.smallSpacing

            readonly property bool valid: kcm.isValidChromaPolicy(root.settings.avc444MotionGapMs, root.settings.avc444RestMs, root.settings.avc444MaxGapMs)

            Repeater {
                model: [
                    {setting: "avc444MotionGapMs", label: i18nc("@label:spinbox", "Quiet time before color (ms):")},
                    {setting: "avc444RestMs", label: i18nc("@label:spinbox", "Color refresh after motion stops (ms):")},
                    {setting: "avc444MaxGapMs", label: i18nc("@label:spinbox", "Longest time without color (ms):")}
                ]
                RowLayout {
                    id: chromaRow
                    required property var modelData
                    spacing: Kirigami.Units.smallSpacing
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: chromaRow.modelData.label
                    }
                    QQC2.SpinBox {
                        from: 16
                        to: 5000
                        stepSize: 10
                        editable: true
                        value: root.settings[chromaRow.modelData.setting]
                        textFromValue: (value, locale) => String(value)
                        valueFromText: (text, locale) => parseInt(text)
                        Accessible.name: chromaRow.modelData.label
                        onValueModified: root.settings[chromaRow.modelData.setting] = value
                        KCM.SettingStateBinding {
                            configObject: root.settings
                            settingName: chromaRow.modelData.setting
                        }
                    }
                }
            }
            RowLayout {
                spacing: Kirigami.Units.smallSpacing
                QQC2.Label {
                    Layout.fillWidth: true
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 18
                    wrapMode: Text.Wrap
                    font: Kirigami.Theme.smallFont
                    color: parent.parent.valid ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.negativeTextColor
                    text: parent.parent.valid
                        ? i18nc("@info", "Only used with full color (AVC444).")
                        : i18nc("@info", "Each value must be 16–5000 ms and must not decrease from top to bottom; otherwise Farside uses 100, 150 and 1500.")
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18nc("@info:tooltip", "Full color is sent as a second picture. These times decide when it follows the main picture while things move and after they stop.")
                }
            }
        }

        ColumnLayout {
            objectName: "commandLineColumn"
            visible: developerToggle.checked
            Kirigami.FormData.label: i18nc("@label", "Service command line:")
            spacing: Kirigami.Units.smallSpacing
            QQC2.Label {
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                wrapMode: Text.Wrap
                text: kcm.overriddenSettings.length > 0
                    ? xi18nc("@info %1 command-line options, %2 setting names",
                             "Sets <command>%1</command>, so Farside ignores these settings from this page: %2.",
                             kcm.commandLineOverrides.join(" "), kcm.overriddenSettings.join(", "))
                    : i18nc("@info", "Overrides no settings from this page.")
            }
            Repeater {
                model: kcm.serviceDropIns
                Kirigami.SelectableLabel {
                    required property string modelData
                    Layout.fillWidth: true
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                    wrapMode: Text.WrapAnywhere
                    font.family: Kirigami.Theme.fixedWidthFont.family
                    text: modelData
                }
            }
        }
    }
}
