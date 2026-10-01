// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

KCM.SimpleKCM {
    id: root
    objectName: "brokerHostsPage"
    title: i18nc("@title:window", "Console and Virtual Host Settings")
    property var consoleSettings: kcm.consoleHostSettings
    property var virtualSettings: kcm.virtualHostSettings
    property var sessionSettings: kcm.virtualSessionSettings
    property int initialScope: 0
    readonly property var host: scopeChoice.currentIndex === 0 ? consoleSettings : scopeChoice.currentIndex === 1 ? virtualSettings : sessionSettings
    readonly property bool session: host.scope === "session"
    readonly property var certificate: host.metadata.tls || ({})
    property url certificateFile
    property url privateKeyFile
    readonly property var runtime: host.runtime
    function fieldNames(keys) {
        return (keys || []).map(key => { const definition = host.definitions.find(row => row.key === key); return definition ? definition.label : key; }).join(", ");
    }
    function runtimeState(state) {
        switch (state) {
        case "verified": return i18nc("@info", "Startup values agree with stored settings");
        case "different": return i18nc("@info", "Inspected values differ from stored settings");
        case "custom": return i18nc("@info", "Custom unit; recognized startup values agree");
        case "partial": return i18nc("@info", "Verification incomplete");
        case "missing": return i18nc("@info", "Host service not found");
        case "inactive": return i18nc("@info", "Host is not running");
        case "stale": return i18nc("@info", "Host changed during inspection; inspect again");
        case "denied": return i18nc("@info", "Inspection was not authorized");
        case "malformed": return i18nc("@info", "Host information could not be validated");
        default: return i18nc("@info", "Running host information unavailable");
        }
    }
    function runtimeReason(reason) {
        switch (reason) {
        case "unsupported-command": case "custom-unit": return i18nc("@info", "The unit's command or service context is not fully supported for inspection.");
        case "unknown-option": case "duplicate-option": case "invalid-field": return i18nc("@info", "Unrecognized, repeated or invalid startup arguments prevent complete verification.");
        case "missing-field": return i18nc("@info", "Some startup values were omitted. Their defaults are not inferred from this version.");
        case "custom-worker": case "custom-authentication": case "custom-runtime": case "incomplete-context": return i18nc("@info", "The broker's worker, authentication or runtime context cannot be fully verified.");
        case "unsafe-file": case "missing-file": case "invalid-environment": return i18nc("@info", "An environment file is missing, unsafe or invalid.");
        case "unknown-expansion": case "inherited-environment": return i18nc("@info", "Environment inheritance or command expansion prevents complete verification.");
        case "manager-reload": return i18nc("@info", "systemd has not reloaded changed unit files. Loaded unit settings may differ from disk.");
        case "process-identity": return i18nc("@info", "The running process could not be matched safely to the installed broker and service.");
        case "stale": return i18nc("@info", "Settings, unit or process identity changed during inspection.");
        case "denied": return i18nc("@info", "The system manager refused inspection.");
        case "bounds": case "malformed": return i18nc("@info", "Host information exceeds limits or is invalid.");
        default: return i18nc("@info", "The system manager or running process is unavailable.");
        }
    }
    function clearFileSelection() { certificateFile = ""; privateKeyFile = ""; }
    function certificateState(state) {
        switch (state) {
        case "valid": return i18nc("@info", "Current");
        case "expiring": return i18nc("@info", "Expires soon");
        case "expired": return i18nc("@info", "Expired");
        case "not-yet-valid": return i18nc("@info", "Not yet valid");
        case "missing": return i18nc("@info", "Not found");
        case "unsafe": return i18nc("@info", "Unsafe file or directory permissions");
        case "invalid": return i18nc("@info", "Unreadable or mismatched material");
        default: return i18nc("@info", "Unavailable");
        }
    }
    onHostChanged: clearFileSelection()

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: i18nc("@info", "These settings belong to the system host and require administrator authorization. Console shares the signed-in desktop; Virtual provides separate desktops. Each section saves separately, and switching sections preserves its unsaved changes.")
        }
        QQC2.ComboBox {
            id: scopeChoice
            currentIndex: root.initialScope
            objectName: "hostScope"
            model: [i18nc("@item:inlistbox", "Console Host"), i18nc("@item:inlistbox", "Virtual Host"), i18nc("@item:inlistbox", "New Virtual Desktops")]
            implicitContentWidthPolicy: QQC2.ComboBox.WidestText
        }
        Kirigami.InlineMessage {
            objectName: "hostError"
            Layout.fillWidth: true
            visible: root.host.error !== ""
            type: Kirigami.MessageType.Error
            text: root.host.error
        }
        Kirigami.InlineMessage {
            objectName: "hostSavedNotice"
            Layout.fillWidth: true
            visible: root.host.applicationRequired
            type: Kirigami.MessageType.Information
            text: root.session ? i18nc("@info", "Saved settings apply to newly created Virtual desktops. Existing desktops retain their device grants; restarting the broker does not change them.")
                : i18nc("@info", "Settings saved. Use Console and Virtual Services to explicitly restart this host when ready; restarting disconnects its clients. Inspect the host separately to check its startup values.")
        }
        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            QQC2.Button {
                objectName: "loadHostSettings"
                icon.name: "view-refresh"
                text: root.host.loaded ? i18nc("@action:button", "Reload Settings…") : i18nc("@action:button", "Load Settings…")
                enabled: !root.host.busy
                onClicked: {
                    if (root.host.modified) { discardDialog.target = root.host; discardDialog.open(); }
                    else root.host.reload();
                }
            }
            QQC2.Button {
                objectName: "saveHostSettings"
                icon.name: "document-save"
                text: i18nc("@action:button", "Save Settings…")
                enabled: root.host.canSave
                onClicked: root.host.save()
            }
            QQC2.Button {
                objectName: "defaultHostSettings"
                text: i18nc("@action:button", "Use Unit Defaults")
                enabled: root.host.loaded && !root.host.busy
                onClicked: { root.host.defaults(); root.clearFileSelection(); }
            }
            QQC2.Button {
                objectName: "discardHostSettings"
                text: i18nc("@action:button", "Discard Changes")
                enabled: root.host.modified && !root.host.busy
                onClicked: { root.host.discard(); root.clearFileSelection(); }
            }
            QQC2.Button {
                objectName: "inspectHostRuntime"
                visible: !root.session
                text: i18nc("@action:button", "Inspect Running Host…")
                enabled: root.host.loaded && !root.host.busy
                onClicked: root.host.inspectRuntime()
            }
        }
        QQC2.BusyIndicator { visible: root.host.busy; running: visible }
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.session ? i18nc("@info", "GPU permissions and VA-API policy apply only when a desktop namespace is created. Empty GPU identities grant no GPU. This does not select an encoder or split work between GPUs.")
                : i18nc("@info", "Host changes need an explicit broker restart. Use unit default removes only that field's stored override. Values shown here describe the stored file and shipped defaults; custom systemd units or drop-ins can differ, and a successful save does not prove the running host uses them.")
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: !root.session && root.host.runtimeCheckedAt !== ""
            Kirigami.Heading { level: 3; text: i18nc("@title:group", "Host Inspection") }
            QQC2.Label {
                objectName: "hostRuntimeSummary"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: root.runtimeState(root.runtime.state)
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: i18nc("@info", "Checked at %1. This is a snapshot; inspect again after changing settings or restarting the host.", root.host.runtimeCheckedAt)
            }
            Kirigami.InlineMessage {
                objectName: "hostRuntimeStale"
                Layout.fillWidth: true
                visible: root.host.runtimeCheckedAt !== "" && root.host.runtimeStale
                type: Kirigami.MessageType.Warning
                text: i18nc("@info", "This inspection does not match the loaded settings revision or changed during reading. Reload stored settings and inspect again. Pending edits are separate.")
            }
            QQC2.Label {
                objectName: "hostRuntimeVerification"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: i18nc("@info", "Loaded unit and current environment files: %1. Running broker startup arguments: %2.",
                    root.runtime.configuredVerified ? i18nc("@info", "verified") : i18nc("@info", "unverified"),
                    root.runtime.runningVerified ? i18nc("@info", "verified") : i18nc("@info", "unverified"))
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: root.runtime.custom === true
                text: i18nc("@info", "Custom unit commands, drop-ins or environment files are present. They can override saved settings.")
            }
            Repeater {
                model: root.runtime.reasons || []
                delegate: QQC2.Label {
                    required property string modelData
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.runtimeReason(modelData)
                }
            }
            QQC2.Label {
                objectName: "hostRuntimeMissing"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: (root.runtime.missing || []).length > 0
                text: i18nc("@info", "Startup values not observed: %1.", root.fieldNames(root.runtime.missing))
            }
            QQC2.Label {
                objectName: "hostRuntimeDifferences"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: (root.runtime.configuredDifferences || []).length + (root.runtime.runningDifferences || []).length > 0
                text: i18nc("@info", "Loaded unit differs for: %1. Startup arguments differ for: %2.",
                    root.fieldNames(root.runtime.configuredDifferences) || i18nc("@info", "none"),
                    root.fieldNames(root.runtime.runningDifferences) || i18nc("@info", "none"))
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: i18nc("@info", "Startup values do not verify listening sockets, live client preferences or the TLS certificate already loaded into memory. Certificate details below describe stored material.")
            }
            QQC2.CheckBox {
                id: runtimeValues
                objectName: "showHostRuntimeValues"
                text: i18nc("@option:check", "Show inspected values")
            }
            Repeater {
                model: runtimeValues.checked && !root.session ? root.host.definitions : []
                delegate: QQC2.Label {
                    required property var modelData
                    objectName: "runtime_" + modelData.key
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: i18nc("@info", "%1 — Loaded unit: %2; startup argument: %3.", modelData.label,
                        Object.prototype.hasOwnProperty.call(root.runtime.configured || {}, modelData.key) ? root.runtime.configured[modelData.key] : i18nc("@info", "unverified"),
                        Object.prototype.hasOwnProperty.call(root.runtime.running || {}, modelData.key) ? root.runtime.running[modelData.key] : i18nc("@info", "not observed"))
                }
            }
        }
        ColumnLayout {
            visible: root.host.loaded && !root.session
            Layout.fillWidth: true
            Kirigami.Heading { level: 3; text: i18nc("@title:group", "TLS Certificate") }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: i18nc("@info", "Stored certificate state: %1.", root.certificateState(root.certificate.state))
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: (root.certificate.fingerprint || "") !== ""
                text: i18nc("@info", "SHA-256: %1. Valid from %2 until %3.", root.certificate.fingerprint || "", root.certificate.notBefore || "", root.certificate.notAfter || "")
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: root.certificate.administratorManaged ? i18nc("@info", "The administrator manages this material and its renewal. The broker will preserve it.")
                    : i18nc("@info", "At the standard paths, the broker can generate missing or renew eligible material on restart. Saving or choosing defaults never regenerates a certificate.")
            }
            QQC2.ComboBox {
                id: tlsChoice
                objectName: "hostTlsOperation"
                enabled: !root.host.busy
                model: [
                    { value: "keep", text: i18nc("@item:inlistbox", "Keep configured TLS paths") },
                    { value: "existing", text: i18nc("@item:inlistbox", "Use existing root-managed paths") },
                    { value: "standard", text: i18nc("@item:inlistbox", "Return to standard TLS paths") },
                    { value: "import", text: i18nc("@item:inlistbox", "Import certificate and private key") }
                ]
                textRole: "text"
                valueRole: "value"
                Layout.maximumWidth: root.width - Kirigami.Units.largeSpacing * 2
                implicitContentWidthPolicy: QQC2.ComboBox.WidestText
                currentIndex: { for (let i = 0; i < model.length; ++i) if (model[i].value === root.host.tlsMode) return i; return 0; }
                onActivated: { root.host.chooseTls(currentValue); root.clearFileSelection(); }
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: root.host.tlsMode === "standard"
                text: i18nc("@info", "Both path overrides will be removed. Existing certificates, keys and previous imports are preserved. Clients may need to verify the standard certificate's fingerprint after restart.")
            }
            ColumnLayout {
                visible: root.host.tlsMode === "import"
                enabled: !root.host.busy
                Layout.fillWidth: true
                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    QQC2.Button {
                        objectName: "selectHostCertificate"
                        text: i18nc("@action:button", "Choose Certificate…")
                        onClicked: { certDialog.target = root.host; certDialog.open(); }
                    }
                    QQC2.Button {
                        objectName: "selectHostPrivateKey"
                        text: i18nc("@action:button", "Choose Private Key…")
                        onClicked: { keyDialog.target = root.host; keyDialog.open(); }
                    }
                    QQC2.Button {
                        objectName: "inspectHostImport"
                        text: i18nc("@action:button", "Check Selected Pair")
                        enabled: root.certificateFile.toString() !== "" && root.privateKeyFile.toString() !== ""
                        onClicked: root.host.importTls(root.certificateFile, root.privateKeyFile)
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: i18nc("@info", "Select local PEM files containing a current matching certificate and unencrypted private key. Private key contents are never displayed. Imported material is administrator-managed; you are responsible for renewal.")
                }
                QQC2.Label {
                    objectName: "hostImportPreview"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: Object.prototype.hasOwnProperty.call(root.host.importMetadata, "fingerprint")
                    text: i18nc("@info", "Selected certificate SHA-256: %1. Valid until %2.", root.host.importMetadata.fingerprint || "", root.host.importMetadata.notAfter || "")
                }
            }
        }
        Repeater {
            model: root.host.loaded ? root.host.definitions : []
            delegate: ColumnLayout {
                id: row
                required property var modelData
                required property int index
                readonly property string key: modelData.key
                readonly property bool overridden: Object.prototype.hasOwnProperty.call(root.host.values, key)
                readonly property string value: overridden ? root.host.values[key] : ""
                readonly property var choices: modelData.choices
                readonly property bool tlsPath: key === "Certificate" || key === "CertificateKey"
                readonly property bool available: !root.host.busy && (!tlsPath || root.host.tlsMode === "existing") && !(root.host.scope === "virtual" && key === "CameraLoopbackDevice")
                Layout.fillWidth: true
                Kirigami.Heading {
                    level: 3
                    visible: row.index === 0 || root.host.definitions[row.index - 1].group !== row.modelData.group
                    text: row.modelData.group
                }
                QQC2.Label { text: row.modelData.label }
                QQC2.ComboBox {
                    objectName: row.choices.length > 0 ? "host_" + row.key : ""
                    visible: row.choices.length > 0
                    enabled: row.available
                    model: row.choices
                    textRole: "text"
                    valueRole: "value"
                    implicitContentWidthPolicy: QQC2.ComboBox.WidestText
                    Layout.maximumWidth: root.width - Kirigami.Units.largeSpacing * 2
                    currentIndex: { for (let i = 0; i < row.choices.length; ++i) if (row.choices[i].value === row.value) return i; return 0; }
                    onActivated: {
                        if (currentValue === "") root.host.inherit(row.key);
                        else root.host.setValue(row.key, currentValue);
                    }
                }
                RowLayout {
                    visible: row.choices.length === 0
                    Layout.fillWidth: true
                    QQC2.TextField {
                        objectName: row.choices.length === 0 ? "host_" + row.key : ""
                        enabled: row.available
                        Layout.fillWidth: true
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 25
                        text: row.value
                        placeholderText: root.host.unitDefaults[row.key] === "" ? i18nc("@info:placeholder", "No GPU grant") : i18nc("@info:placeholder", "Unit default: %1", root.host.unitDefaults[row.key])
                        maximumLength: 4096
                        onTextEdited: {
                            if (text === "" && row.key !== "RenderPci" && !row.tlsPath) root.host.inherit(row.key);
                            else root.host.setValue(row.key, text);
                        }
                    }
                    QQC2.ToolButton {
                        objectName: "inheritHost_" + row.key
                        visible: !row.tlsPath
                        enabled: !root.host.busy && row.overridden
                        icon.name: "edit-undo"
                        text: i18nc("@action:button", "Use unit default")
                        display: QQC2.AbstractButton.IconOnly
                        QQC2.ToolTip.text: text
                        QQC2.ToolTip.visible: hovered
                        onClicked: root.host.inherit(row.key)
                    }
                }
                QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: row.modelData.help }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: row.key === "CameraLoopbackDevice"
                    text: i18nc("@info", "Stored device availability: %1.", root.host.metadata.cameraLoopback ? root.host.metadata.cameraLoopback.state : "")
                }
            }
        }
        Repeater {
            model: root.host.loaded && root.session ? root.host.metadata.renderDevices : []
            delegate: QQC2.Label {
                required property var modelData
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: i18nc("@info", "Available device: %1 (%2), %3.", modelData.pci, modelData.driver, modelData.render)
            }
        }
    }
    QQC2.Dialog {
        id: discardDialog
        objectName: "reloadHostConfirmation"
        property var target
        modal: true
        width: Math.min(root.width - Kirigami.Units.largeSpacing * 2, Kirigami.Units.gridUnit * 26)
        x: Math.max(0, (root.width - width) / 2)
        y: Math.max(0, (root.height - height) / 2)
        title: i18nc("@title:window", "Discard Host Changes")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        contentItem: QQC2.Label { wrapMode: Text.Wrap; text: i18nc("@info", "Reloading discards unsaved changes in this section. Continue?") }
        onAccepted: { target.reload(); root.clearFileSelection(); }
    }
    Dialogs.FileDialog {
        id: certDialog
        objectName: "hostCertificateDialog"
        property var target
        title: i18nc("@title:window", "Choose Certificate")
        nameFilters: [i18nc("@info", "PEM certificates (*.pem *.crt)"), i18nc("@info", "All files (*)")]
        onAccepted: { if (target === root.host && !target.busy) { target.clearTlsImport(); root.certificateFile = selectedFile; } }
    }
    Dialogs.FileDialog {
        id: keyDialog
        objectName: "hostPrivateKeyDialog"
        property var target
        title: i18nc("@title:window", "Choose Unencrypted Private Key")
        nameFilters: [i18nc("@info", "PEM keys (*.pem *.key)"), i18nc("@info", "All files (*)")]
        onAccepted: { if (target === root.host && !target.busy) { target.clearTlsImport(); root.privateKeyFile = selectedFile; } }
    }
}
