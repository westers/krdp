// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM
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
    title: fixedScope === 0 ? i18nc("@title:window", "Console Settings") : i18nc("@title:window", "Virtual Settings")
    function fields(keys) { return host.definitions.filter(row => keys.includes(row.key)); }
    // Leaving the page drops an open certificate draft; staged TLS and other drafts stay.
    readonly property string pendingSummary: {
        const scope = fixedScope === 0 ? i18nc("@info", "Console only") : i18nc("@info", "Virtual only");
        const tls = host.tlsMode !== "keep" ? i18nc("@info", " (certificate change staged)") : "";
        if (fixedScope === 1 && sessionSettings.modified) return host.modified ? i18nc("@info", "Unsaved changes · Virtual only%1 · New desktop defaults — Apply saves it too", tls) : i18nc("@info", "Unsaved changes · New desktop defaults — Apply saves it too");
        return i18nc("@info", "Unsaved changes · %1%2", scope, tls);
    }
    // Native navigation: popping (destroyed) or covering (hidden) the page, or
    // pushing another page from it, all drop an open certificate draft.
    function leave() { if (certificateSection.open) certificateSection.cancel(); }
    Component.onDestruction: if (host) host.cancelCertificateEdit()
    onVisibleChanged: if (!visible) leave()
    function closeCertificate() { if (certificateSection.open) certificateSection.cancel(); }
    Connections { target: root.apply; ignoreUnknownSignals: true; function onDraftsReplaced() { root.closeCertificate(); } }
    ColumnLayout {
      ColumnLayout {
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 48
        Layout.alignment: Qt.AlignLeft
        spacing: Kirigami.Units.largeSpacing
        QQC2.Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: i18nc("@info", "Defaults for everyone connecting to this service. Apply your changes, then explicitly restart the service.")
        }
        BrokerApplyFailures { }
        Kirigami.InlineMessage { objectName: "hostError"; Layout.fillWidth: true; type: Kirigami.MessageType.Error; visible: root.host.error !== ""; text: root.host.error }
        Kirigami.InlineMessage {
            objectName: "hostSavedNotice"; Layout.fillWidth: true; visible: root.host.applicationRequired; type: Kirigami.MessageType.Information
            text: i18nc("@info", "Saved. Restart this service to apply the changes.")
            actions: Kirigami.Action { text: i18nc("@action", "Restart…"); enabled: root.administration && root.administration.services[root.fixedScope].canRestart; onTriggered: restartConfirmation.open() }
        }
        ColumnLayout {
            Layout.fillWidth: true; visible: root.host.loaded; spacing: Kirigami.Units.smallSpacing
            Kirigami.Heading { level: 2; text: i18nc("@title:group", "Connection") }
            Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 32;
            Layout.alignment: Qt.AlignLeft
                Layout.fillWidth: true
                id: connectionForm
                twinFormLayouts: [videoForm, mediaForm, advancedForm]
                Repeater { model: root.fields(["Address", "Port"]); delegate: hostField }
                RowLayout {
                    Kirigami.FormData.label: i18nc("@label", "Certificate:")
                    QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.host.tlsMode !== "keep" ? i18nc("@info", "Certificate change staged") : (root.host.metadata.tls || {}).state === "valid" ? i18nc("@info", "Current · no changes") : i18nc("@info", "View certificate details") }
                    QQC2.Button { objectName: "editHostCertificate"; visible: !certificateSection.open; text: i18nc("@action:button", "Change…"); enabled: !root.host.busy && !root.host.outcomeUnknown; onClicked: certificateSection.begin() }
                }
            }
            BrokerCertificateSection { id: certificateSection; Layout.fillWidth: true; host: root.host; twins: [connectionForm, videoForm, mediaForm, advancedForm] }
        }
        ColumnLayout {
            Layout.fillWidth: true; visible: root.host.loaded; spacing: Kirigami.Units.smallSpacing
            Kirigami.Heading { level: 2; text: i18nc("@title:group", "Video") }
            Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 32; id: videoForm; twinFormLayouts: [connectionForm, mediaForm, advancedForm]; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft; Repeater { model: root.fields(["Quality", "AdaptiveQuality"]); delegate: hostField } }
        }
        ColumnLayout {
            Layout.fillWidth: true; visible: root.host.loaded; spacing: Kirigami.Units.smallSpacing
            Kirigami.Heading { level: 2; text: i18nc("@title:group", "Sound and devices") }
            Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 32; id: mediaForm; twinFormLayouts: [connectionForm, videoForm, advancedForm]; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft; Repeater { model: root.fields(["PreferAudioQuality", "StandardClientMedia"]); delegate: hostField } }
            QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Host media permission does not grant microphone or camera access; client consent is still required."); color: Kirigami.Theme.disabledTextColor }
            Kirigami.InlineMessage {
                objectName: "cameraReadiness"; Layout.fillWidth: true; visible: true
                readonly property bool staged: root.fixedScope === 0 && root.host.loaded && (root.host.values.CameraLoopbackDevice || root.host.unitDefaults.CameraLoopbackDevice) !== (root.host.metadata.effective || {}).CameraLoopbackDevice
                type: staged || root.fixedScope === 1 ? Kirigami.MessageType.Information : root.camera.state === "available" ? Kirigami.MessageType.Positive : root.camera.state ? Kirigami.MessageType.Warning : Kirigami.MessageType.Information
                text: staged ? i18nc("@info", "Camera: bridge change staged — save, then reload settings to check the new device.") : root.fixedScope === 1 ? i18nc("@info", "Camera: sharing is unavailable in the current Virtual device namespace.") : root.camera.state === "available" ? i18nc("@info", "Camera: bridge device available — apps and clients still need access.") : root.camera.state === "disabled" ? i18nc("@info", "Camera: setup required — configure a V4L2 loopback device in Advanced.") : root.camera.state ? i18nc("@info", "Camera: bridge unavailable (%1) — check the V4L2 device and account permissions in Advanced.", root.camera.state) : i18nc("@info", "Camera: not checked — reload saved settings to check the bridge device.")
            }
        }
        Flow {
            Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing; visible: root.host.loaded
            QQC2.Button { objectName: "hostAdvancedButton"; text: i18nc("@action:button", "Advanced Options"); icon.name: root.showAdvanced ? "arrow-down" : "arrow-right"; onClicked: root.showAdvanced = !root.showAdvanced }
            QQC2.Button { objectName: "consoleDisplayPreferences"; visible: root.fixedScope === 0; text: i18nc("@action:button", "My Display Preferences…"); onClicked: { root.leave(); root.navigation.showPreferences(); } }
        }
        ColumnLayout {
            Layout.fillWidth: true; visible: root.host.loaded && root.showAdvanced; spacing: Kirigami.Units.smallSpacing
            Kirigami.Heading { level: 2; text: i18nc("@title:group", "Advanced: encoding and camera bridge") }
            Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 32;
            Layout.alignment: Qt.AlignLeft
                Layout.fillWidth: true
                id: advancedForm
                twinFormLayouts: [connectionForm, videoForm, mediaForm]
                Repeater { model: root.fields(["SoftwareEncoding", "Av1Tiles", "VaapiDriver", "RenderPci", "CameraLoopbackDevice"]); delegate: hostField }
            }
        }
        BrokerHardwareSection { objectName: "desktopHardwareSection"; Layout.fillWidth: true; visible: root.fixedScope === 1; settings: root.sessionSettings; navigation: root.navigation }
        QQC2.Label { objectName: "hostPendingSummary"; visible: root.host.modified || (root.fixedScope === 1 && root.sessionSettings.modified); Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.pendingSummary; color: Kirigami.Theme.disabledTextColor }
    }
    Component {
        id: hostField
        BrokerSettingField {
            required property var modelData
            settings: root.host; definition: modelData
            visible: !(key === "CameraLoopbackDevice" && root.fixedScope === 1)
            editable: !root.host.busy
            showHelp: ["Address", "SoftwareEncoding", "Av1Tiles", "VaapiDriver", "RenderPci", "CameraLoopbackDevice"].includes(key)
        }
    }
    }
    Kirigami.PromptDialog {
        parent: root.QQC2.Overlay.overlay
        popupType: QQC2.Popup.Item
        id: restartConfirmation
        title: root.fixedScope === 0 ? i18nc("@title:window", "Restart Console?") : i18nc("@title:window", "Restart Virtual?")
        subtitle: i18nc("@info", "Remote clients using this service will disconnect. Saving settings does not restart it.")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: Kirigami.Action { text: i18nc("@action:button", "Restart"); onTriggered: { restartConfirmation.close(); root.administration.perform(root.serviceRoute, "restart"); } }
    }
}
