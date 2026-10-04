// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM
// One route's detail page (Console or Virtual): service state, connection,
// picture and sound, camera, (Virtual) new-desktop graphics, and a collapsed
// Advanced group. One form, one scrolling page; every draft lives in the models.
KCM.SimpleKCM {
    id: root
    objectName: "brokerHostsPage"
    property var consoleSettings: kcm.consoleHostSettings
    property var virtualSettings: kcm.virtualHostSettings
    property var sessionSettings: kcm.virtualSessionSettings
    property int fixedScope: 0
    property bool showAdvanced: false
    property var administration: null
    property var navigation
    readonly property var apply: (typeof kcm !== "undefined" && kcm) ? kcm.settingsApply : null
    readonly property var host: fixedScope === 0 ? consoleSettings : virtualSettings
    readonly property string serviceRoute: fixedScope === 0 ? "console" : "virtual"
    readonly property var camera: host.metadata.cameraLoopback || ({})
    // A definition that carries a reason is not offered as a control (today: the camera bridge for Virtual).
    readonly property string unavailableReason: { const row = host.definitions.find(definition => definition.unavailable !== ""); return row ? row.unavailable : ""; }
    readonly property var preferences: navigation && navigation.preferences ? navigation.preferences : null
    readonly property var devices: sessionSettings.loaded ? (sessionSettings.metadata.renderDevices || []) : []
    readonly property var selectedDevices: (sessionSettings.values.RenderPci || "").split(",").map(value => value.trim()).filter(value => value !== "")
    readonly property var summary: BrokerRouteSummary {
        service: root.administration ? root.administration.services[root.fixedScope] : null
        host: root.host
    }
    title: fixedScope === 0 ? i18nc("@title:window", "Console") : i18nc("@title:window", "Virtual")
    function displayModeText() {
        const mode = preferences ? (preferences.values.MonitorMode || "") : "";
        if (mode === "") return i18nc("@info", "Follows the host setting");
        const definition = preferences.definitions.find(row => row.key === "MonitorMode");
        const choice = definition ? definition.choices.find(row => row.value === mode) : null;
        return choice ? choice.text : mode;
    }
    // Navigation: popping (destroyed) or covering (hidden) the page drops an open certificate draft.
    function leave() { if (certificateSection.open) certificateSection.cancel(); }
    function closeCertificate() { if (certificateSection.open) certificateSection.cancel(); }
    Component.onDestruction: if (host) host.cancelCertificateEdit()
    onVisibleChanged: if (!visible) leave()
    Connections { target: root.apply; ignoreUnknownSignals: true; function onDraftsReplaced() { root.closeCertificate(); } }
    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        BrokerApplyFailures { }
        Kirigami.InlineMessage { objectName: "hostError"; Layout.fillWidth: true; type: Kirigami.MessageType.Error; visible: root.host.error !== ""; text: root.host.error }
        Kirigami.InlineMessage { objectName: "desktopHardwareError"; Layout.fillWidth: true; type: Kirigami.MessageType.Error; visible: root.fixedScope === 1 && root.sessionSettings.error !== ""; text: root.sessionSettings.error }
        Kirigami.InlineMessage {
            objectName: "hostSavedNotice"; Layout.fillWidth: true; visible: root.host.applicationRequired; type: Kirigami.MessageType.Information
            text: i18nc("@info", "Saved. Restart to use the new settings.")
            actions: Kirigami.Action {
                objectName: "hostSavedRestart"
                text: i18nc("@action", "Restart…")
                enabled: !!root.administration && root.administration.services[root.fixedScope].canRestart
                onTriggered: root.navigation.requestOperation(root.serviceRoute, "restart")
            }
        }
        Kirigami.InlineMessage { objectName: "desktopHardwareSavedNotice"; Layout.fillWidth: true; visible: root.fixedScope === 1 && root.sessionSettings.applicationRequired; type: Kirigami.MessageType.Information; text: i18nc("@info", "Saved. New desktops will use these defaults.") }
        Kirigami.PlaceholderMessage {
            objectName: "hostNotPublished"
            Layout.fillWidth: true; Layout.topMargin: Kirigami.Units.gridUnit * 2
            visible: !root.host.loaded
            icon.name: "network-server"
            text: i18nc("@info", "Settings not published yet")
            explanation: i18nc("@info", "Turn this service on once and its settings will appear here.")
        }
        Kirigami.FormLayout {
            id: form
            Layout.fillWidth: true
            visible: root.host.loaded
            Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Service") }
            QQC2.Label {
                objectName: root.serviceRoute + "HostStatus"
                Kirigami.FormData.label: i18nc("@label", "Status:")
                text: root.summary.stateText
            }
            RowLayout {
                objectName: root.serviceRoute + "StoredEndpointRow"
                Kirigami.FormData.label: i18nc("@label", "Listening on:")
                // A plain label (not a selectable text edit, which stays left-aligned in a right-to-left layout); the button copies a concrete address.
                QQC2.Label {
                    objectName: root.serviceRoute + "StoredEndpoint"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.summary.listenText !== "" ? root.summary.listenText : i18nc("@info", "Available after the service has started once")
                }
                QQC2.ToolButton {
                    objectName: root.serviceRoute + "CopyStoredEndpoint"
                    visible: root.summary.address !== ""
                    icon.name: "edit-copy"; text: i18nc("@action:button", "Copy address")
                    display: QQC2.AbstractButton.IconOnly
                    QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered
                    onClicked: root.navigation.copyAddressToClipboard(root.summary.address)
                }
            }
            QQC2.Switch {
                objectName: root.serviceRoute + "ServiceAutostart"
                Kirigami.FormData.label: i18nc("@label", "Start when computer boots:")
                Accessible.name: i18nc("@option:check", "Start when this computer boots")
                readonly property var service: root.summary.service
                enabled: !!service && service.canAutostart
                checked: !!service && service.autostart
                onClicked: {
                    root.administration.perform(root.serviceRoute, service.autostart ? "disable" : "enable");
                    checked = Qt.binding(() => !!service && service.autostart);
                }
            }
            QQC2.Label {
                visible: !!root.summary.service && root.summary.service.unitFileState === "enabled-runtime"
                text: i18nc("@info", "Enabled for this boot only.")
            }
            QQC2.Button {
                objectName: root.serviceRoute + "ServiceRestart"
                text: i18nc("@action:button", "Restart…")
                icon.name: "view-refresh"
                enabled: !!root.summary.service && root.summary.service.canRestart
                onClicked: root.navigation.requestOperation(root.serviceRoute, "restart")
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                type: Kirigami.MessageType.Error
                visible: !!root.summary.service && root.summary.service.error !== ""
                text: root.summary.service ? root.summary.service.error : ""
            }

            Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: root.host.sectionTitle("connection") }
            BrokerFieldRepeater { settings: root.host; section: "connection"; busy: root.host.busy }
            // One line: validity and a short fingerprint, a copy button for the full fingerprint, and the inline editor behind Change.
            RowLayout {
                objectName: "certificateSummaryRow"
                Kirigami.FormData.label: i18nc("@label", "Certificate:")
                QQC2.Label { objectName: "certificateSummary"; Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.host.tlsMode !== "keep" ? i18nc("@info", "Will change when you apply") : certificateSection.summary }
                QQC2.ToolButton {
                    objectName: "copyCertificateFingerprint"
                    visible: root.host.tlsMode === "keep" && certificateSection.fingerprint !== ""
                    icon.name: "edit-copy"; text: i18nc("@action:button", "Copy fingerprint")
                    display: QQC2.AbstractButton.IconOnly
                    QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered
                    onClicked: root.navigation.copyAddressToClipboard(certificateSection.fingerprint)
                }
                QQC2.Button { objectName: "editHostCertificate"; visible: !certificateSection.open; text: i18nc("@action:button", "Change…"); enabled: !root.host.busy && !root.host.outcomeUnknown; onClicked: certificateSection.begin() }
            }
            BrokerCertificateSection { id: certificateSection; Layout.fillWidth: true; host: root.host }

            Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: root.host.sectionTitle("picture") }
            BrokerFieldRepeater { settings: root.host; section: "picture"; busy: root.host.busy }
            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Camera sharing:")
                QQC2.Label {
                    id: cameraReadiness
                    objectName: "cameraReadiness"
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    readonly property bool staged: root.fixedScope === 0 && root.host.loaded && (root.host.values.CameraLoopbackDevice || root.host.unitDefaults.CameraLoopbackDevice) !== (root.host.metadata.effective || {}).CameraLoopbackDevice
                    // Whether the camera device (under Advanced) is what the user has to fix.
                    readonly property bool needsSetup: root.unavailableReason === "" && !staged && !!root.camera.state && root.camera.state !== "available"
                    text: root.unavailableReason !== "" ? root.unavailableReason
                        : staged ? i18nc("@info", "The camera device changed. Apply, then check again.")
                        : root.camera.state === "available" ? i18nc("@info", "Ready")
                        : root.camera.state === "disabled" ? i18nc("@info", "Not set up")
                        : root.camera.state ? i18nc("@info", "Not working (%1)", root.camera.state)
                        : i18nc("@info", "Not checked yet")
                }
                QQC2.Button {
                    objectName: "cameraSetup"
                    visible: cameraReadiness.needsSetup
                    text: i18nc("@action:button", "Set Up…")
                    // The device field is under Advanced; opening that group is the way to set it up.
                    onClicked: root.showAdvanced = true
                }
            }

            Kirigami.Separator { visible: root.fixedScope === 0; Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Displays") }
            RowLayout {
                visible: root.fixedScope === 0
                Kirigami.FormData.label: i18nc("@label", "Shared screens:")
                QQC2.Label { objectName: "consoleDisplaySummary"; Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.displayModeText() }
                QQC2.Button {
                    objectName: "consoleDisplayPreferences"
                    text: i18nc("@action:button", "Change…")
                    visible: !!root.navigation && typeof root.navigation.showPreferences === "function"
                    // Deferred: the switch destroys this page, which must not happen inside its own button's click.
                    onClicked: { root.leave(); const navigation = root.navigation; Qt.callLater(() => navigation.showPreferences()); }
                }
            }

            Kirigami.Separator { visible: root.fixedScope === 1; Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "New Desktops") }
            ColumnLayout {
                id: hardware
                objectName: "desktopHardwareSection"
                visible: root.fixedScope === 1
                Kirigami.FormData.label: i18nc("@label", "Graphics acceleration:")
                Kirigami.FormData.buddyFor: noGpu
                spacing: Kirigami.Units.smallSpacing
                QQC2.CheckBox {
                    id: noGpu
                    objectName: "desktopNoGpu"
                    text: i18nc("@option:check", "No graphics acceleration")
                    checked: root.selectedDevices.length === 0; enabled: !root.sessionSettings.busy
                    onClicked: { if (checked) root.sessionSettings.setValue("RenderPci", ""); checked = Qt.binding(() => root.selectedDevices.length === 0); }
                }
                Repeater {
                    model: root.devices
                    delegate: QQC2.CheckBox {
                        required property var modelData
                        objectName: "desktopGpu_" + modelData.pci
                        text: i18nc("@option:check %1 PCI address %2 driver name", "%2 graphics at %1", modelData.pci, modelData.driver)
                        checked: root.selectedDevices.includes(modelData.pci); enabled: !root.sessionSettings.busy
                        onClicked: {
                            const values = root.selectedDevices.filter(value => value !== modelData.pci);
                            if (checked) values.push(modelData.pci);
                            root.sessionSettings.setValue("RenderPci", values.join(","));
                            checked = Qt.binding(() => root.selectedDevices.includes(modelData.pci));
                        }
                    }
                }
                QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; font: Kirigami.Theme.smallFont; text: i18nc("@info", "Applies to new desktops only.") }
            }

            QQC2.Button {
                objectName: "hostAdvancedButton"
                flat: true
                text: i18nc("@action:button", "Advanced options")
                icon.name: root.showAdvanced ? "arrow-down" : "arrow-right"
                onClicked: root.showAdvanced = !root.showAdvanced
            }
            Kirigami.Separator { visible: root.showAdvanced; Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Advanced") }
            BrokerFieldRepeater { settings: root.host; advanced: true; shown: root.showAdvanced; busy: root.host.busy }
            BrokerFieldRepeater { settings: root.sessionSettings; advanced: true; included: root.fixedScope === 1; shown: root.showAdvanced; busy: root.sessionSettings.busy; prefix: "desktop_" }
            BrokerServiceDetails {
                id: troubleshooting
                visible: root.showAdvanced && root.administration !== null
                Kirigami.FormData.label: i18nc("@label", "Troubleshooting:")
                Layout.fillWidth: true
                host: root.host; administration: root.administration; route: root.serviceRoute; navigation: root.navigation
            }
        }
    }
}
