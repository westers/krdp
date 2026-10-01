// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
// SPDX-FileCopyrightText: 2026 Steve Westers
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

// The top page: on/off, how to connect, who can sign in and the two choices
// most people change. Everything else is on the four pages it links to
// (REBRAND-PLAN.md §4). SimpleKCM scrolls the whole page, so nothing can
// cover anything else at any window size.
KCM.SimpleKCM {
    id: root
    objectName: "mainPage"

    title: i18nc("@title", "Farside Remote Desktop")

    readonly property var settings: kcm.settings()
    readonly property var coexistence: kcm.coexistence

    // Opening the page must not greet the user with the error of an earlier
    // run; only errors from now on are shown.
    property bool kcmJustOpened: true
    property bool errorDismissed: false

    readonly property bool certificateBroken: !settings.autogenerateCertificates
        && (kcm.certificateState === "missing" || kcm.certificateState === "unusable" || kcm.certificateState === "expired")

    // At most one message, the most important first.
    readonly property string banner: {
        if (!kcm.isH264Supported()) {
            return "codec";
        }
        if (coexistence.state === "stock" || coexistence.state === "other") {
            return coexistence.state;
        }
        if (kcm.errorMessage !== "" && !kcmJustOpened && !errorDismissed) {
            return "error";
        }
        if (certificateBroken) {
            return "certificate";
        }
        if (kcm.serverRunning && kcm.restartRequired) {
            return "restart";
        }
        if (kcm.users.loginMethodCount === 0) {
            return "nologin";
        }
        if (coexistence.state === "stock-autostart") {
            return "stock-autostart";
        }
        if (kcm.overriddenSettings.length > 0) {
            return "overrides";
        }
        if (!kcm.managementAvailable) {
            return "nosystemd";
        }
        return "";
    }

    readonly property string lastErrorLine: {
        const lines = kcm.errorMessage.split("\n").filter(line => line.trim() !== "");
        return lines.length > 0 ? lines[lines.length - 1] : "";
    }

    function openPage(page: string): void {
        kcm.push(page + ".qml");
    }

    function formatAddress(address: string, port: int): string {
        return address.includes(":") ? "[" + address + "]:" + port : address + ":" + port;
    }

    Connections {
        target: kcm
        function onServerStatusChanged(): void {
            serverSwitch.checked = kcm.serverRunning || kcm.serverBusy && serverSwitch.checked;
        }
        function onErrorMessageChanged(): void {
            root.kcmJustOpened = false;
            root.errorDismissed = false;
        }
    }

    actions: [
        Kirigami.Action {
            displayComponent: Kirigami.ContextualHelpButton {
                toolTipText: xi18nc("@info:tooltip", "Farside lets you use this computer from another one with an RDP app, such as the Farside client, Remmina, FreeRDP or Windows Remote Desktop.<nl/><nl/>These settings are for your account's remote desktop. The sign-in screen and virtual desktop services are set up separately.")
            }
        },
        Kirigami.Action {
            id: serverSwitch
            objectName: "serverSwitch"
            text: i18nc("@option:check turn the remote desktop server on or off", "Enabled")
            checkable: true
            visible: kcm.managementAvailable
            // Wait for systemd to finish starting or stopping.
            enabled: !kcm.serverBusy && kcm.isH264Supported()
            onTriggered: source => {
                root.kcmJustOpened = false;
                kcm.toggleServer(source.checked);
            }
            Component.onCompleted: {
                kcm.updateServerStatus();
            }
            displayComponent: QQC2.Switch {
                action: serverSwitch
            }
        }
    ]

    headerPaddingEnabled: false // Let the InlineMessages touch the edges
    header: ColumnLayout {
        spacing: 0

        Kirigami.InlineMessage {
            objectName: "codecMessage"
            Layout.fillWidth: true
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Error
            visible: root.banner === "codec"
            text: i18nc("@info:status", "Farside can't run because this computer can't encode H.264 video. Ask your distribution how to enable it.")
        }

        Kirigami.InlineMessage {
            objectName: "stockConflictMessage"
            Layout.fillWidth: true
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Warning
            visible: root.banner === "stock"
            text: i18nc("@info:status %1 port number", "KDE's built-in remote desktop is using port %1, so Farside can't start.", String(root.coexistence.port))
            actions: [
                Kirigami.Action {
                    objectName: "stopStockAction"
                    icon.name: "media-playback-stop-symbolic"
                    text: i18nc("@action:button stop KDE's remote desktop and start this one", "Stop It and Start Farside")
                    enabled: !root.coexistence.busy && kcm.managementAvailable
                    onTriggered: root.coexistence.stopStockAndStartServer()
                },
                Kirigami.Action {
                    objectName: "useOtherPortAction"
                    icon.name: "network-connect-symbolic"
                    text: i18nc("@action:button %1 port number", "Use Port %1", String(root.coexistence.alternativePort))
                    visible: root.coexistence.alternativePort > 0
                    enabled: !root.coexistence.busy
                    onTriggered: root.coexistence.useAlternativePort()
                }
            ]
        }

        Kirigami.InlineMessage {
            objectName: "portBusyMessage"
            Layout.fillWidth: true
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Error
            visible: root.banner === "other"
            text: root.coexistence.holderName !== ""
                ? xi18nc("@info:status %1 port, %2 program name", "Port %1 is in use by <application>%2</application>, so Farside can't start.", String(root.coexistence.port), root.coexistence.holderName)
                : i18nc("@info:status %1 port", "Port %1 is in use by another program, so Farside can't start.", String(root.coexistence.port))
            actions: [
                Kirigami.Action {
                    icon.name: "network-connect-symbolic"
                    text: i18nc("@action:button %1 port number", "Use Port %1", String(root.coexistence.alternativePort))
                    visible: root.coexistence.alternativePort > 0
                    onTriggered: root.coexistence.useAlternativePort()
                },
                Kirigami.Action {
                    icon.name: "configure-symbolic"
                    text: i18nc("@action:button", "Change Port…")
                    onTriggered: root.openPage("AdvancedPage")
                }
            ]
        }

        Kirigami.InlineMessage {
            id: errorBanner
            objectName: "errorMessage"
            Layout.fillWidth: true
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Error
            showCloseButton: true
            onVisibleChanged: {
                // The close button hides the message; remember that.
                if (!visible && root.banner === "error") {
                    root.errorDismissed = true;
                }
            }
            text: i18nc("@info:status %1 the last line Farside logged", "Farside reported a problem: %1", root.lastErrorLine)
            actions: [
                Kirigami.Action {
                    icon.name: "view-list-text-symbolic"
                    text: i18nc("@action:button", "Show Log")
                    visible: kcm.errorMessage.includes("\n")
                    onTriggered: logDialog.open()
                }
            ]
        }

        // The close button breaks a plain binding; this one is re-applied.
        Binding {
            target: errorBanner
            property: "visible"
            value: root.banner === "error"
        }

        Kirigami.InlineMessage {
            objectName: "certificateMessage"
            Layout.fillWidth: true
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Error
            visible: root.banner === "certificate"
            text: i18nc("@info:status", "Farside can't accept connections: the chosen certificate is missing, unreadable, expired or doesn't match its key.")
            actions: [
                Kirigami.Action {
                    icon.name: "security-medium-symbolic"
                    text: i18nc("@action:button", "Choose Another…")
                    onTriggered: root.openPage("UsersPage")
                }
            ]
        }

        Kirigami.InlineMessage {
            objectName: "restartMessage"
            Layout.fillWidth: true
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Warning
            visible: root.banner === "restart"
            text: i18nc("@info:status %1 list of setting names", "Restart Farside to use the changed settings (%1). Restarting disconnects anyone who is connected.", kcm.restartReasons.join(", "))
            actions: [
                Kirigami.Action {
                    icon.name: "view-refresh-symbolic"
                    text: i18nc("@action:button restart the remote desktop server", "Restart Now")
                    enabled: !kcm.serverBusy
                    onTriggered: kcm.restartServer()
                }
            ]
        }

        Kirigami.InlineMessage {
            objectName: "noLoginMessage"
            Layout.fillWidth: true
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Warning
            visible: root.banner === "nologin"
            text: i18nc("@info:status", "No one can sign in yet, so Farside won't start.")
            actions: [
                Kirigami.Action {
                    icon.name: "user-symbolic"
                    text: i18nc("@action:button", "Allow My Account")
                    onTriggered: root.settings.systemUserEnabled = true
                },
                Kirigami.Action {
                    icon.name: "system-users-symbolic"
                    text: i18nc("@action:button", "Manage Users…")
                    onTriggered: root.openPage("UsersPage")
                }
            ]
        }

        Kirigami.InlineMessage {
            objectName: "stockAutostartMessage"
            Layout.fillWidth: true
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Information
            visible: root.banner === "stock-autostart"
            text: i18nc("@info:status %1 port number", "KDE's built-in remote desktop also starts at login and may take port %1 first.", String(root.coexistence.stockPort))
            actions: [
                Kirigami.Action {
                    objectName: "disableStockAutostartAction"
                    icon.name: "system-run-symbolic"
                    text: i18nc("@action:button", "Turn Its Autostart Off")
                    enabled: !root.coexistence.busy
                    onTriggered: root.coexistence.disableStockAutostart()
                }
            ]
        }

        Kirigami.InlineMessage {
            objectName: "overridesMessage"
            Layout.fillWidth: true
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Information
            visible: root.banner === "overrides"
            text: i18nc("@info:status %1 list of setting names", "Farside's service command line overrides some settings here: %1.", kcm.overriddenSettings.join(", "))
            actions: [
                Kirigami.Action {
                    icon.name: "documentinfo-symbolic"
                    text: i18nc("@action:button", "Details…")
                    onTriggered: root.openPage("AdvancedPage")
                }
            ]
        }

        Kirigami.InlineMessage {
            objectName: "noSystemdMessage"
            Layout.fillWidth: true
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Warning
            visible: root.banner === "nosystemd"
            text: i18nc("@info:status", "systemd isn't available, so this page can't start or stop Farside. Start it by hand.")
        }
    }

    Kirigami.FormLayout {
        id: form

        RowLayout {
            objectName: "statusRow"
            Kirigami.FormData.label: i18nc("@label", "Status:")
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                id: statusLabel
                text: {
                    if (!kcm.managementAvailable) {
                        return i18nc("@info server status", "Not managed here");
                    }
                    if (kcm.serverBusy) {
                        return i18nc("@info server status", "Starting or stopping…");
                    }
                    if (kcm.serverRunning) {
                        return i18nc("@info server status", "Running · Waiting for connections");
                    }
                    if (kcm.serverStatus === 3 /* Failed */) {
                        return i18nc("@info server status", "Stopped after an error");
                    }
                    return i18nc("@info server status", "Stopped");
                }
                color: kcm.serverRunning ? Kirigami.Theme.positiveTextColor
                    : kcm.serverStatus === 3 ? Kirigami.Theme.negativeTextColor
                    : Kirigami.Theme.textColor
            }
            QQC2.Button {
                visible: kcm.errorMessage.includes("\n")
                icon.name: "view-list-text-symbolic"
                text: i18nc("@action:button", "Show Log")
                onClicked: logDialog.open()
            }
        }

        QQC2.CheckBox {
            id: autostartCheck
            objectName: "autostartCheck"
            Kirigami.FormData.label: i18nc("@label", "Autostart:")
            visible: kcm.managementAvailable
            text: i18nc("@option:check", "Start when I log in")
            checked: kcm.autostart
            onToggled: kcm.autostart = checked
        }

        ColumnLayout {
            id: connectColumn
            objectName: "connectRow"
            Kirigami.FormData.label: i18nc("@label", "Connect to:")
            Kirigami.FormData.buddyFor: primaryAddressRow
            spacing: 0

            readonly property var addresses: {
                root.settings.listenAddress; // re-evaluate when it changes
                return kcm.listenAddressList();
            }
            readonly property bool allInterfaces: {
                const configured = root.settings.listenAddress.trim();
                return configured === "" || configured === "*";
            }

            RowLayout {
                id: primaryAddressRow
                spacing: Kirigami.Units.smallSpacing
                Kirigami.SelectableLabel {
                    id: primaryAddress
                    text: connectColumn.allInterfaces
                        ? root.formatAddress(kcm.hostName, root.settings.listenPort)
                        : root.formatAddress(root.settings.listenAddress.trim(), root.settings.listenPort)
                    Accessible.name: i18nc("@info accessible name", "Address to connect to")
                }
                CopyButton {
                    value: primaryAddress.text
                    text: i18nc("@action:button", "Copy Address")
                }
                QQC2.ToolButton {
                    id: moreAddresses
                    visible: connectColumn.allInterfaces && connectColumn.addresses.length > 0
                    checkable: true
                    icon.name: checked ? "arrow-up-symbolic" : "arrow-down-symbolic"
                    text: i18nc("@action:button show the computer's IP addresses", "Other Addresses")
                }
            }

            Repeater {
                model: moreAddresses.checked ? connectColumn.addresses : []
                RowLayout {
                    required property string modelData
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.SelectableLabel {
                        id: addressLabel
                        text: root.formatAddress(modelData, root.settings.listenPort)
                    }
                    CopyButton {
                        value: addressLabel.text
                        text: i18nc("@action:button", "Copy Address")
                    }
                }
            }
        }

        RowLayout {
            objectName: "fingerprintRow"
            Kirigami.FormData.label: i18nc("@label the certificate fingerprint", "Fingerprint:")
            spacing: Kirigami.Units.smallSpacing
            FingerprintLabel {}
            Kirigami.ContextualHelpButton {
                Layout.alignment: Qt.AlignTop
                toolTipText: i18nc("@info:tooltip", "The first time you connect, your app shows the fingerprint of this computer's certificate. Check that it matches this one.")
            }
        }

        RowLayout {
            objectName: "signInRow"
            Kirigami.FormData.label: i18nc("@label who can sign in", "Sign-in:")
            spacing: Kirigami.Units.largeSpacing

            QQC2.Label {
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 16
                wrapMode: Text.Wrap
                readonly property int others: root.settings.users.length
                text: {
                    if (root.settings.systemUserEnabled) {
                        return others === 0
                            ? i18nc("@info %1 login name", "Your account (%1)", kcm.systemUserName)
                            : i18ncp("@info %2 login name", "Your account (%2) and %1 other user", "Your account (%2) and %1 other users", others, kcm.systemUserName);
                    }
                    return others === 0 ? i18nc("@info", "No one yet") : i18ncp("@info", "%1 user", "%1 users", others);
                }
                color: kcm.users.loginMethodCount === 0 ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
            }
            QQC2.Button {
                objectName: "manageUsersButton"
                icon.name: "system-users-symbolic"
                text: i18nc("@action:button", "Manage Users…")
                onClicked: root.openPage("UsersPage")
            }
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        SettingComboBox {
            id: screensCombo
            objectName: "screensCombo"
            Kirigami.FormData.label: i18nc("@label:listbox which screens to share", "Screens:")
            configObject: root.settings
            settingName: "monitorMode"
            model: {
                const all = [
                    {text: i18nc("@item:inlistbox", "All monitors as one"), value: "workspace"},
                    {text: i18nc("@item:inlistbox", "The primary monitor"), value: "primary"},
                    {text: i18nc("@item:inlistbox", "One monitor"), value: "specific"},
                    {text: i18nc("@item:inlistbox", "Each monitor separately"), value: "multi"},
                    {text: i18nc("@item:inlistbox", "Virtual monitors sized for the viewer"), value: "virtual"}
                ];
                // Hide what this build can't do, unless it is already chosen.
                return all.filter(entry => kcm.plasmaBackendAvailable || !kcm.monitorModeNeedsPlasma(entry.value) || entry.value === root.settings.monitorMode);
            }
        }

        ColumnLayout {
            objectName: "qualityRow"
            Kirigami.FormData.label: adaptiveCheck.checked ? i18nc("@label:slider", "Best quality:") : i18nc("@label:slider", "Quality:")
            Kirigami.FormData.buddyFor: qualitySliderRow
            spacing: Kirigami.Units.smallSpacing

            RowLayout {
                id: qualitySliderRow
                spacing: Kirigami.Units.smallSpacing
                QQC2.Label {
                    text: i18nc("@label:slider low end: smoother motion", "Smoother")
                }
                QQC2.Slider {
                    id: qualitySlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: Kirigami.Units.gridUnit * 8
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 14
                    from: 50
                    to: 100
                    stepSize: 5
                    snapMode: QQC2.Slider.SnapAlways
                    value: root.settings.quality
                    Accessible.name: i18nc("@label:slider", "Quality")
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
                QQC2.Label {
                    text: i18nc("@label:slider high end: sharper picture", "Sharper")
                }
            }
            QQC2.CheckBox {
                id: adaptiveCheck
                text: i18nc("@option:check", "Adapt to the network")
                checked: root.settings.adaptiveQuality
                onToggled: root.settings.adaptiveQuality = checked
                KCM.SettingStateBinding {
                    configObject: root.settings
                    settingName: "adaptiveQuality"
                }
            }
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        Flow {
            objectName: "pageButtons"
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 30
            QQC2.Button {
                objectName: "brokerSignInButton"
                text: i18nc("@action:button", "Console and Virtual Sign-In…")
                onClicked: root.openPage("BrokerSignInPage")
            }
            spacing: Kirigami.Units.smallSpacing

            QQC2.Button {
                objectName: "usersPageButton"
                icon.name: "security-high-symbolic"
                text: i18nc("@action:button opens a page", "Users and Security…")
                onClicked: root.openPage("UsersPage")
            }
            QQC2.Button {
                objectName: "screensPageButton"
                icon.name: "video-display-symbolic"
                text: i18nc("@action:button opens a page", "Screens and Displays…")
                onClicked: root.openPage("ScreensPage")
            }
            QQC2.Button {
                objectName: "videoAudioPageButton"
                icon.name: "media-playback-start-symbolic"
                text: i18nc("@action:button opens a page", "Video and Audio…")
                onClicked: root.openPage("VideoAudioPage")
            }
            QQC2.Button {
                objectName: "advancedPageButton"
                icon.name: "configure-symbolic"
                text: i18nc("@action:button opens a page", "Advanced…")
                onClicked: root.openPage("AdvancedPage")
            }
        }
    }

    ServerLogDialog {
        id: logDialog
        parent: root
    }
}
