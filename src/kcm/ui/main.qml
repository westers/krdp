// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs as QtDialogs
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

KCM.ScrollViewKCM {
    id: root

    property var settings: kcm.settings()

    // Used to avoid showing error when KCM is opened first time
    // and last run ended in an error, to avoid confusing users.
    property bool kcmJustOpened: true

    extraFooterTopPadding: true // This makes separator below scrollview visible

    EditUserModal {
        id: editUserModal
        parent: root
        // This dialog benefits from being able to stretch with the window; let it
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
        function onServerStatusChanged(): void {
            toggleServerSwitch.checked = kcm.isServerRunning();
        }
        function onErrorMessageChanged(): void {
            root.kcmJustOpened = false;
        }
    }

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

    actions: [
        Kirigami.Action {
            id: toggleServerSwitch
            text: i18nc("@option:check Enable RDP server", "Enable RDP server")
            checkable: true
            visible: kcm.managementAvailable
            // Wait for systemd to finish starting or stopping.
            enabled: !kcm.serverBusy
            onTriggered: source => {
                root.kcmJustOpened = false;
                kcm.toggleServer(source.checked);
            }
            Component.onCompleted: {
                kcm.updateServerStatus();
            }
            displayComponent: QQC2.Switch {
                action: toggleServerSwitch
            }
        }
    ]

    headerPaddingEnabled: false // Let the InlineMessages touch the edges
    header: ColumnLayout {
        id: headerLayout
        readonly property int spacings: Kirigami.Units.largeSpacing
        spacing: 0
        Layout.margins: spacings

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
            visible: !kcm.managementAvailable
            position: Kirigami.InlineMessage.Position.Header
            Layout.fillWidth: true
            text: i18nc("@info:status", "Systemd not found. krdpserver will require manual activation.")
        }

        Kirigami.InlineMessage {
            type: Kirigami.MessageType.Warning
            visible: kcm.overriddenSettings.length > 0
            position: Kirigami.InlineMessage.Position.Header
            Layout.fillWidth: true
            text: xi18nc("@info:status %1 command-line options, %2 setting names, %3 files",
                         "The server's service command line sets <command>%1</command>, so the server ignores these settings from this page: %2.<nl/>Remove the option from the service override to use them: %3",
                         kcm.commandLineOverrides.join(" "),
                         kcm.overriddenSettings.join(", "),
                         kcm.serviceDropIns.length > 0 ? kcm.serviceDropIns.join(", ") : i18nc("@info:status", "the service unit"))
        }

        Kirigami.InlineMessage {
            type: Kirigami.MessageType.Warning
            visible: kcm.users.loginMethodCount === 0
            position: Kirigami.InlineMessage.Position.Header
            Layout.fillWidth: true
            text: i18nc("@info:status", "Nobody can log in yet. Enable login with your system password or add a user; the server does not start without one.")
        }

        Kirigami.InlineMessage {
            id: startupErrorMessage
            type: Kirigami.MessageType.Error
            showCloseButton: true
            position: Kirigami.InlineMessage.Position.Header
            Layout.fillWidth: true
            text: xi18nc("@info:status", "Error message from the RDP server:<nl/>%1", kcm.errorMessage)
        }
        // Closing the error message breaks the visibility binding, so handle it separately here
        Binding {
            target: startupErrorMessage
            property: "visible"
            value: (kcm.errorMessage !== "" && !root.kcmJustOpened)
        }
        // Non-InlineMessage header content does need margins; put it all in here
        // so we can do that in a single place
        ColumnLayout {
            spacing: headerLayout.spacings
            Layout.margins: headerLayout.spacings

            QQC2.Label {
                text: i18n("Set up remote login to connect using apps supporting the “RDP” remote desktop protocol.")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                Layout.alignment: Qt.AlignHCenter
            }

            QQC2.Label {
                text: i18nc("@info", "These settings belong to the remote desktop server of your user account. The login-screen console host and the virtual-session host are set up separately.")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font: Kirigami.Theme.smallFont
                opacity: 0.8
            }

            QQC2.Label {
                visible: toggleServerSwitch.checked
                text: i18nc("@info:usagetip", "Use any of the following addresses to connect to this device:")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                Layout.alignment: Qt.AlignHCenter
            }

            QQC2.ScrollView {
                id: addressScrollView
                implicitHeight: Math.min(contentHeight, Kirigami.Units.gridUnit * 4)
                visible: toggleServerSwitch.checked
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.gridUnit
                Flow {
                    id: serverAddressLayout
                    spacing: Kirigami.Units.largeSpacing
                    width: addressScrollView.availableWidth

                    Repeater {
                        id: addressesRepeater
                        model: {
                            root.settings.listenAddress; // re-evaluate when it changes
                            return kcm.listenAddressList();
                        }

                        RowLayout {
                            spacing: Kirigami.Units.smallSpacing

                            Kirigami.SelectableLabel {
                                id: addressLabel
                                text: modelData
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
            }
        }
    }

    view: UserListView {
        id: userListView
    }

    footer: Kirigami.FormLayout {
        id: settingsLayout

        readonly property bool showAdvancedCertUI: !autoGenCertSwitch.checked
        readonly property string restartHint: i18nc("@info:usagetip", "Takes effect when the server restarts.")

        // --- Server ---------------------------------------------------------
        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("title:group Group of RDP server settings", "Server Settings")
        }

        QQC2.CheckBox {
            id: autostartOnLogin
            visible: kcm.managementAvailable
            text: i18nc("@option:check", "Autostart on login")
            checked: kcm.autostart
            onToggled: {
                kcm.autostart = checked;
            }
        }

        SettingComboBox {
            id: listenAddressCombo
            Kirigami.FormData.label: i18nc("@label:listbox", "Listening address:")
            configObject: root.settings
            settingName: "listenAddress"
            model: {
                const entries = [{text: i18nc("@item:inlistbox listen on every network interface", "All interfaces"), value: ""}];
                const addresses = kcm.interfaceAddresses();
                for (const address of addresses) {
                    entries.push({text: address, value: address});
                }
                const current = root.settings.listenAddress;
                // krdpserver reads "*" like "" (every interface): SettingComboBox
                // shows an unknown value as the first entry, "All interfaces".
                if (current.trim() !== "" && current.trim() !== "*" && !addresses.includes(current)) {
                    entries.push({text: i18nc("@item:inlistbox %1 an address that no interface has now", "%1 (not present)", current), value: current});
                }
                return entries;
            }
        }

        QQC2.SpinBox {
            id: portField
            Kirigami.FormData.label: i18nc("@label:spinbox", "Listening port:")
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
            onValueModified: {
                root.settings.listenPort = value;
            }
            KCM.SettingStateBinding {
                configObject: root.settings
                settingName: "listenPort"
            }
        }

        QQC2.Label {
            text: settingsLayout.restartHint
            font: Kirigami.Theme.smallFont
            opacity: 0.8
        }

        // --- Display --------------------------------------------------------
        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("title:group", "Display")
        }

        SettingComboBox {
            id: monitorModeCombo
            Kirigami.FormData.label: i18nc("@label:listbox", "Display target:")
            configObject: root.settings
            settingName: "monitorMode"
            model: [
                {text: i18nc("@item:inlistbox", "All monitors as one desktop (workspace)"), value: "workspace"},
                {text: i18nc("@item:inlistbox", "Primary monitor"), value: "primary"},
                {text: i18nc("@item:inlistbox", "Specific monitor ID"), value: "specific"},
                {text: i18nc("@item:inlistbox", "Each monitor separately (multi)"), value: "multi"},
                {text: i18nc("@item:inlistbox", "Virtual monitors sized for the client (virtual)"), value: "virtual"}
            ]
        }

        QQC2.Label {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
            wrapMode: Text.WordWrap
            font: Kirigami.Theme.smallFont
            visible: kcm.monitorModeNeedsPlasma(root.settings.monitorMode)
            text: kcm.plasmaBackendAvailable
                ? i18nc("@info", "Uses the Plasma capture backend, which the server selects by itself for this mode.")
                : i18nc("@info", "Unavailable: this build has no Plasma capture backend. The server streams all monitors as one desktop instead.")
            color: kcm.plasmaBackendAvailable ? Kirigami.Theme.textColor : Kirigami.Theme.negativeTextColor
        }

        QQC2.SpinBox {
            id: monitorIdField
            Kirigami.FormData.label: i18nc("@label:spinbox", "Monitor ID:")
            visible: root.settings.monitorMode === "specific"
            from: 0
            to: Math.max(0, kcm.availableMonitorIds().length - 1)
            value: root.settings.monitorIndex
            onValueModified: {
                root.settings.monitorIndex = value;
            }
            KCM.SettingStateBinding {
                configObject: root.settings
                settingName: "monitorIndex"
            }
        }

        ColumnLayout {
            Kirigami.FormData.label: i18nc("@label", "Monitor ID map:")
            visible: root.settings.monitorMode === "specific"
            Layout.fillWidth: true
            Repeater {
                model: kcm.availableMonitorIds()
                QQC2.Label {
                    text: modelData
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }
        }

        SettingComboBox {
            Kirigami.FormData.label: i18nc("@label:listbox", "Physical monitors:")
            visible: root.settings.monitorMode === "virtual"
            configObject: root.settings
            settingName: "virtualMonitorPolicy"
            model: [
                {text: i18nc("@item:inlistbox", "Turn off while a client is connected"), value: "replace"},
                {text: i18nc("@item:inlistbox", "Keep on"), value: "extend"}
            ]
        }

        SettingComboBox {
            Kirigami.FormData.label: i18nc("@label:listbox", "Virtual monitors:")
            visible: root.settings.monitorMode === "virtual"
            configObject: root.settings
            settingName: "virtualMonitorLayout"
            model: [
                {text: i18nc("@item:inlistbox", "One per client monitor"), value: "client"},
                {text: i18nc("@item:inlistbox", "One at the client's desktop size"), value: "single"},
                {text: i18nc("@item:inlistbox", "One per physical monitor, same arrangement"), value: "physical"}
            ]
        }

        ColumnLayout {
            Kirigami.FormData.label: i18nc("@label:textbox", "Fallback size:")
            visible: root.settings.monitorMode === "virtual"
            spacing: Kirigami.Units.smallSpacing
            QQC2.TextField {
                id: fallbackSizeField
                Layout.maximumWidth: Kirigami.Units.gridUnit * 8
                placeholderText: "1920x1080"
                text: root.settings.virtualMonitorFallbackSize
                onTextEdited: {
                    root.settings.virtualMonitorFallbackSize = text.trim();
                }
                KCM.SettingStateBinding {
                    configObject: root.settings
                    settingName: "virtualMonitorFallbackSize"
                }
            }
            QQC2.Label {
                font: Kirigami.Theme.smallFont
                wrapMode: Text.WordWrap
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                readonly property bool valid: kcm.isValidFallbackSize(root.settings.virtualMonitorFallbackSize)
                color: valid ? Kirigami.Theme.textColor : Kirigami.Theme.negativeTextColor
                text: valid ? i18nc("@info", "Used when the client does not say how big its screen is.")
                            : i18nc("@info", "Enter WIDTHxHEIGHT between 640 and 4096; the server uses 1920x1080 otherwise.")
            }
        }

        SettingComboBox {
            id: stockClientPolicy
            Kirigami.FormData.label: i18nc("@label:listbox", "Standard clients on virtual desktops:")
            configObject: root.settings
            settingName: "virtualStockClientPolicy"
            model: [
                {text: i18nc("@item:inlistbox", "Resume my latest desktop, or start a new one"), value: "attach-or-create"},
                {text: i18nc("@item:inlistbox", "Turn them away"), value: "refuse"}
            ]
        }
        QQC2.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
            wrapMode: Text.WordWrap
            font: Kirigami.Theme.smallFont
            text: i18nc("@info", "For the virtual desktop service. Clients such as the Windows Remote Desktop app, FreeRDP and Remmina can't pick a virtual desktop themselves. A new desktop matches the size of their screen. The KRDP client always lets you choose.")
        }

        QQC2.CheckBox {
            text: i18nc("@option:check", "Wake the display when a client connects")
            checked: root.settings.wakeDisplayOnConnect
            onToggled: root.settings.wakeDisplayOnConnect = checked
            KCM.SettingStateBinding {
                configObject: root.settings
                settingName: "wakeDisplayOnConnect"
            }
        }

        // --- Video ----------------------------------------------------------
        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("title:group", "Video")
        }

        SettingComboBox {
            Kirigami.FormData.label: i18nc("@label:listbox", "Codec:")
            configObject: root.settings
            settingName: "codec"
            model: [
                {text: i18nc("@item:inlistbox", "Automatic: full color (AVC444) when the client supports it"), value: "auto"},
                {text: i18nc("@item:inlistbox", "Always AVC420"), value: "avc420"},
                {text: i18nc("@item:inlistbox", "AVC444, warn when the client cannot"), value: "avc444"}
            ]
        }

        SettingComboBox {
            id: softwareEncodingCombo
            Kirigami.FormData.label: i18nc("@label:listbox", "Software encoding:")
            configObject: root.settings
            settingName: "softwareEncoding"
            model: [
                {text: i18nc("@item:inlistbox software video encoding", "Use it when hardware encoding isn't available"), value: "auto"},
                {text: i18nc("@item:inlistbox software video encoding", "Prefer the best compression, even in software"), value: "prefer"},
                {text: i18nc("@item:inlistbox software video encoding", "Only as a last resort"), value: "never"}
            ]
        }
        QQC2.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
            wrapMode: Text.WordWrap
            font: Kirigami.Theme.smallFont
            text: i18nc("@info", "Use software encoding when hardware encoding isn't available (uses more CPU). On a slow connection the KRDP client may get a better-compressing codec encoded in software, as long as the processor keeps up.")
        }

        QQC2.CheckBox {
            id: adaptiveQualityCheck
            text: i18nc("@option:check", "Adapt quality to the network")
            checked: root.settings.adaptiveQuality
            onToggled: root.settings.adaptiveQuality = checked
            KCM.SettingStateBinding {
                configObject: root.settings
                settingName: "adaptiveQuality"
            }
        }

        ColumnLayout {
            Layout.preferredWidth: certKeyLayout.implicitWidth

            Kirigami.FormData.label: adaptiveQualityCheck.checked ? i18nc("@label:slider", "Maximum quality:") : i18nc("@label:slider", "Video quality:")
            Kirigami.FormData.buddyFor: qualitySlider
            QQC2.Slider {
                id: qualitySlider
                Layout.fillWidth: true
                from: 50
                to: 100
                stepSize: 5
                snapMode: QQC2.Slider.SnapAlways
                value: root.settings.quality
                // AUD-K12: only a user's move writes the setting, so opening
                // the page with a value off the 5-step grid does not dirty it.
                onMoved: {
                    if (root.settings.quality !== value) {
                        root.settings.quality = value;
                    }
                }
                KCM.SettingStateBinding {
                    configObject: root.settings
                    settingName: "quality"
                }
            }
            RowLayout {
                QQC2.Label {
                    text: i18nc("@label:slider", "Responsiveness")
                }
                Item {
                    Layout.fillWidth: true
                }
                QQC2.Label {
                    text: i18nc("@label:slider", "Quality")
                }
            }
            QQC2.Label {
                visible: adaptiveQualityCheck.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font: Kirigami.Theme.smallFont
                text: i18nc("@info", "The server lowers quality when the network is slow and never goes above this.")
            }
        }

        SettingComboBox {
            id: vaapiModeCombo
            Kirigami.FormData.label: i18nc("@label:listbox", "VAAPI driver:")
            configObject: root.settings
            settingName: "vaapiDriverMode"
            model: [
                {text: i18nc("@item:inlistbox", "Automatic (recommended)"), value: "auto"},
                {text: i18nc("@item:inlistbox", "Disabled"), value: "off"},
                {text: i18nc("@item:inlistbox", "AMD (radeonsi)"), value: "radeonsi"},
                {text: i18nc("@item:inlistbox", "Intel (iHD)"), value: "iHD"},
                {text: i18nc("@item:inlistbox", "Intel, older GPUs (i965)"), value: "i965"}
            ]
        }

        QQC2.CheckBox {
            id: advancedVideoCheck
            text: i18nc("@option:check", "Show advanced video settings")
        }

        ColumnLayout {
            Kirigami.FormData.label: i18nc("@label", "AVC444 color timing:")
            visible: advancedVideoCheck.checked
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
                    QQC2.Label {
                        text: modelData.label
                    }
                    QQC2.SpinBox {
                        id: chromaSpin
                        from: 16
                        to: 5000
                        stepSize: 10
                        editable: true
                        value: root.settings[modelData.setting]
                        textFromValue: (value, locale) => String(value)
                        valueFromText: (text, locale) => parseInt(text)
                        onValueModified: root.settings[modelData.setting] = value
                        KCM.SettingStateBinding {
                            configObject: root.settings
                            settingName: chromaRow.modelData.setting
                        }
                    }
                }
            }
            QQC2.Label {
                Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                wrapMode: Text.WordWrap
                font: Kirigami.Theme.smallFont
                color: parent.valid ? Kirigami.Theme.textColor : Kirigami.Theme.negativeTextColor
                text: parent.valid
                    ? i18nc("@info", "Only used with AVC444. Each value is 16–5000 ms.")
                    : i18nc("@info", "Each value must be 16–5000 ms and the three must not decrease from top to bottom; otherwise the server uses 100, 150 and 1500.")
            }
        }

        // --- Audio and devices ----------------------------------------------
        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("title:group", "Audio and Devices")
        }

        QQC2.CheckBox {
            text: i18nc("@option:check", "Prefer audio over video when the network is busy")
            checked: root.settings.preferAudioQuality
            onToggled: root.settings.preferAudioQuality = checked
            KCM.SettingStateBinding {
                configObject: root.settings
                settingName: "preferAudioQuality"
            }
        }
        QQC2.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
            wrapMode: Text.WordWrap
            font: Kirigami.Theme.smallFont
            text: i18nc("@info", "The default for new connections; the KRDP client can change it during a session.")
        }

        QQC2.CheckBox {
            text: i18nc("@option:check", "Let standard RDP clients share audio, microphone and camera")
            checked: root.settings.standardClientMedia
            onToggled: root.settings.standardClientMedia = checked
            KCM.SettingStateBinding {
                configObject: root.settings
                settingName: "standardClientMedia"
            }
        }
        QQC2.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
            wrapMode: Text.WordWrap
            font: Kirigami.Theme.smallFont
            text: i18nc("@info", "Clients such as the Windows Remote Desktop app, FreeRDP and Remmina ask for these themselves. The KRDP client turns each device on and off during the session.")
        }

        QQC2.TextField {
            Kirigami.FormData.label: i18nc("@label:textbox", "Camera loopback device:")
            Layout.maximumWidth: Kirigami.Units.gridUnit * 10
            placeholderText: i18nc("@info:placeholder", "None (PipeWire only)")
            text: root.settings.cameraLoopbackDevice
            onTextEdited: root.settings.cameraLoopbackDevice = text.trim()
            KCM.SettingStateBinding {
                configObject: root.settings
                settingName: "cameraLoopbackDevice"
            }
        }
        QQC2.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
            wrapMode: Text.WordWrap
            font: Kirigami.Theme.smallFont
            text: xi18nc("@info", "A v4l2loopback device such as <filename>/dev/video10</filename> that also receives the client's camera, for applications that cannot use PipeWire.")
        }

        // --- Certificate ----------------------------------------------------
        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("title:group Group of RDP server settings", "Security Certificate")
        }

        QQC2.CheckBox {
            id: autoGenCertSwitch
            text: i18nc("@label:check generate security certificates automatically", "Create and renew automatically")
            checked: root.settings.autogenerateCertificates
            onToggled: {
                root.settings.autogenerateCertificates = checked;
            }
            KCM.SettingStateBinding {
                configObject: root.settings
                settingName: "autogenerateCertificates"
            }
        }

        ColumnLayout {
            Kirigami.FormData.label: i18nc("@label", "Certificate:")
            spacing: Kirigami.Units.smallSpacing
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: {
                    switch (kcm.certificateState) {
                    case "valid":
                        return i18nc("@info %1 algorithm, %2 date", "%1, valid until %2", kcm.certificateAlgorithm, kcm.certificateExpiry);
                    case "expiring":
                        return autoGenCertSwitch.checked
                            ? i18nc("@info %1 date", "Expires on %1; the server renews it when it starts.", kcm.certificateExpiry)
                            : i18nc("@info %1 date", "Expires on %1. Replace it soon.", kcm.certificateExpiry);
                    case "expired":
                        return autoGenCertSwitch.checked
                            ? i18nc("@info", "Expired; the server renews it when it starts.")
                            : i18nc("@info %1 date", "Expired on %1.", kcm.certificateExpiry);
                    case "missing":
                        return autoGenCertSwitch.checked
                            ? i18nc("@info", "None yet; the server creates one when it starts.")
                            : i18nc("@info", "No certificate or key at these paths.");
                    default:
                        return autoGenCertSwitch.checked
                            ? i18nc("@info", "Unreadable; the server replaces it when it starts.")
                            : i18nc("@info", "The certificate or key cannot be read, or they do not belong together.");
                    }
                }
            }
            Kirigami.SelectableLabel {
                Layout.fillWidth: true
                visible: kcm.certificateFingerprint !== ""
                font.family: "monospace"
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                wrapMode: Text.WrapAnywhere
                text: i18nc("@info %1 SHA-256 fingerprint", "SHA-256: %1", kcm.certificateFingerprint)
            }
            QQC2.Label {
                Layout.fillWidth: true
                visible: kcm.certificateFingerprint !== ""
                wrapMode: Text.WordWrap
                font: Kirigami.Theme.smallFont
                opacity: 0.8
                text: i18nc("@info", "Compare this with the fingerprint your client shows the first time it connects.")
            }
        }

        RowLayout {
            id: certLayout
            visible: settingsLayout.showAdvancedCertUI
            spacing: Kirigami.Units.smallSpacing
            Kirigami.FormData.label: i18nc("@label:textbox", "Certificate path:")
            QQC2.TextField {
                id: certPathField
                implicitWidth: Kirigami.Units.gridUnit * 14
                text: root.settings.certificate
                onTextChanged: {
                    if (root.settings.certificate !== text) {
                        root.settings.certificate = text;
                    }
                }
                KCM.SettingStateBinding {
                    configObject: root.settings
                    settingName: "certificate"
                }
            }
            QQC2.Button {
                icon.name: "folder-open-symbolic"
                text: i18nc("@action:button", "Choose Certificate File…")
                display: QQC2.AbstractButton.IconOnly
                onClicked: {
                    certLoader.selectKey = false;
                    certLoader.active = true;
                }
            }
        }

        RowLayout {
            id: certKeyLayout
            spacing: Kirigami.Units.smallSpacing
            Kirigami.FormData.label: i18nc("@label:textbox", "Certificate key path:")
            visible: settingsLayout.showAdvancedCertUI
            QQC2.TextField {
                id: certKeyPathField
                implicitWidth: Kirigami.Units.gridUnit * 14
                text: root.settings.certificateKey
                onTextChanged: {
                    if (root.settings.certificateKey !== text) {
                        root.settings.certificateKey = text;
                    }
                }
                KCM.SettingStateBinding {
                    configObject: root.settings
                    settingName: "certificateKey"
                }
            }
            QQC2.Button {
                icon.name: "folder-open-symbolic"
                text: i18nc("@action:button", "Choose Certificate Key File…")
                display: QQC2.AbstractButton.IconOnly
                onClicked: {
                    certLoader.selectKey = true;
                    certLoader.active = true;
                }
            }
        }
    }

    CertLoader {
        id: certLoader
    }
}
