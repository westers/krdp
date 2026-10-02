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
    // Keep the form objects alive across scope changes. Replacing labelled
    // delegates during a switch races Kirigami's deferred label updates.
    readonly property var allDefinitions: consoleSettings.definitions.concat(virtualSettings.definitions, sessionSettings.definitions)
        .filter((row, index, rows) => rows.findIndex(candidate => candidate.key === row.key && candidate.group === row.group) === index)
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
            text: i18nc("@info", "Configure this host. Loading and saving require administrator authentication.")
        }
        Kirigami.FormLayout {
            Layout.fillWidth: true
            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Settings for:")
                QQC2.ComboBox {
            id: scopeChoice
            currentIndex: root.initialScope
            objectName: "hostScope"
            model: [i18nc("@item:inlistbox", "Console Host"), i18nc("@item:inlistbox", "Virtual Host"), i18nc("@item:inlistbox", "New Virtual Desktops")]
            implicitContentWidthPolicy: QQC2.ComboBox.WidestText
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18nc("@info:tooltip", "Console shares this computer's desktop. Virtual provides separate desktops. New Virtual Desktop settings apply only to newly created desktops. Each section saves separately; switching sections preserves unsaved changes.")
                }
            }
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
            text: root.session ? i18nc("@info", "Settings saved. New Virtual desktops will use them.")
                : i18nc("@info", "Settings saved. Restart this host in Services to apply them.")
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
            QQC2.ToolButton {
                objectName: "defaultHostSettings"
                text: i18nc("@action:button", "Use Unit Defaults")
                icon.name: "document-revert"
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                enabled: root.host.loaded && !root.host.busy
                onClicked: { root.host.defaults(); root.clearFileSelection(); }
            }
            QQC2.ToolButton {
                objectName: "discardHostSettings"
                text: i18nc("@action:button", "Discard Changes")
                icon.name: "edit-undo"
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
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
            text: root.session ? i18nc("@info", "Changes apply to newly created Virtual desktops.")
                : i18nc("@info", "Save changes, then restart the host in Services to apply them.")
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
        Kirigami.FormLayout {
            visible: root.host.loaded && !root.session
            Layout.fillWidth: true
            Item { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Certificate") }
            QQC2.Label {
                Kirigami.FormData.label: i18nc("@label", "Saved certificate:")
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: root.certificateState(root.certificate.state)
            }
            Kirigami.SelectableLabel {
                Kirigami.FormData.label: i18nc("@label", "Fingerprint:")
                Layout.fillWidth: true
                Layout.preferredWidth: Kirigami.Units.gridUnit * 18
                Layout.maximumWidth: Kirigami.Units.gridUnit * 25
                wrapMode: Text.Wrap
                visible: (root.certificate.fingerprint || "") !== ""
                text: root.certificate.fingerprint || ""
            }
            QQC2.Label {
                Kirigami.FormData.label: i18nc("@label", "Validity:")
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: (root.certificate.fingerprint || "") !== ""
                text: i18nc("@info", "%1 to %2", root.certificate.notBefore || "", root.certificate.notAfter || "")
                Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                Layout.maximumWidth: Kirigami.Units.gridUnit * 25
            }
            RowLayout {
                Kirigami.FormData.label: i18nc("@label", "Source:")
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
                Kirigami.ContextualHelpButton {
                    toolTipText: root.certificate.administratorManaged ? i18nc("@info:tooltip", "The administrator manages this certificate and its renewal. Farside preserves it.")
                        : i18nc("@info:tooltip", "Farside can generate missing or renew eligible certificates at the standard paths when restarted. Saving settings never regenerates a certificate. These details describe the saved material; the running host may have loaded a different certificate.")
                }
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: root.host.tlsMode === "standard"
                Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                Layout.maximumWidth: Kirigami.Units.gridUnit * 25
                text: i18nc("@info", "Both path overrides will be removed. Existing certificates, keys and previous imports are preserved. Clients may need to verify the standard certificate's fingerprint after restart.")
            }
            ColumnLayout {
                visible: root.host.tlsMode === "import"
                enabled: !root.host.busy
                Layout.fillWidth: true
                Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                Layout.maximumWidth: Kirigami.Units.gridUnit * 25
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
            model: root.allDefinitions.map(row => row.group).filter((group, index, groups) => groups.indexOf(group) === index)
            delegate: Kirigami.FormLayout {
                id: group
                required property string modelData
                Layout.fillWidth: true
                visible: root.host.loaded && root.host.definitions.some(row => row.group === group.modelData)
                Item { Kirigami.FormData.isSection: true; Kirigami.FormData.label: group.modelData }
                Repeater {
                    model: root.allDefinitions.filter(row => row.group === group.modelData)
                    delegate: ColumnLayout {
                        id: row
                        required property var modelData
                        readonly property string key: modelData.key
                        readonly property var definition: root.host.definitions.find(field => field.key === row.key && field.group === group.modelData)
                        readonly property bool overridden: Object.prototype.hasOwnProperty.call(root.host.values, key)
                        readonly property string value: overridden ? root.host.values[key] : ""
                        readonly property var choices: definition ? definition.choices : []
                        readonly property bool tlsPath: key === "Certificate" || key === "CertificateKey"
                        readonly property bool available: !root.host.busy && (!tlsPath || root.host.tlsMode === "existing") && !(root.host.scope === "virtual" && key === "CameraLoopbackDevice")
                        visible: row.definition !== undefined
                        Kirigami.FormData.label: (row.definition ? row.definition.label : row.modelData.label) + ":"
                        Kirigami.FormData.buddyFor: inputs
                        RowLayout {
                            id: inputs
                            Layout.fillWidth: true
                            onActiveFocusChanged: if (activeFocus) (row.choices.length > 0 ? choice : textValue).forceActiveFocus()
                            QQC2.Slider {
                                visible: row.key === "Quality"
                                enabled: row.available
                                Layout.fillWidth: true
                                Layout.minimumWidth: Kirigami.Units.gridUnit * 6
                                Layout.maximumWidth: Kirigami.Units.gridUnit * 12
                                from: 0
                                to: 100
                                stepSize: 1
                                value: Number(row.overridden ? row.value : root.host.unitDefaults[row.key]) || 0
                                Accessible.name: row.modelData.label
                                onMoved: root.host.setValue(row.key, String(Math.round(value)))
                            }
                            QQC2.ComboBox {
                                id: choice
                                objectName: row.definition && row.choices.length > 0 ? "host_" + row.key : ""
                                visible: row.choices.length > 0
                                enabled: row.available
                                Accessible.name: row.definition ? row.definition.label : ""
                                model: row.choices
                                textRole: "text"
                                valueRole: "value"
                                Layout.fillWidth: true
                                Layout.maximumWidth: Kirigami.Units.gridUnit * 25
                                implicitContentWidthPolicy: QQC2.ComboBox.WidestText
                                currentIndex: { for (let i = 0; i < row.choices.length; ++i) if (row.choices[i].value === row.value) return i; return 0; }
                                onActivated: {
                                    if (currentValue === "") root.host.inherit(row.key);
                                    else root.host.setValue(row.key, currentValue);
                                }
                            }
                            QQC2.TextField {
                                id: textValue
                                objectName: row.definition && row.choices.length === 0 ? "host_" + row.key : ""
                                visible: row.choices.length === 0
                                enabled: row.available
                                Layout.fillWidth: true
                                Layout.preferredWidth: Kirigami.Units.gridUnit * (row.key === "Quality" ? 4 : 18)
                                Layout.maximumWidth: Kirigami.Units.gridUnit * 25
                                Accessible.name: row.modelData.label
                                text: row.value
                                placeholderText: row.key === "Quality" ? (root.host.unitDefaults[row.key] || "")
                                    : !root.host.unitDefaults[row.key] ? i18nc("@info:placeholder", "No GPU grant") : i18nc("@info:placeholder", "Unit default: %1", root.host.unitDefaults[row.key])
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
                                text: i18nc("@action:button", "Use Unit Default")
                                display: QQC2.AbstractButton.IconOnly
                                QQC2.ToolTip.text: text
                                QQC2.ToolTip.visible: hovered
                                onClicked: root.host.inherit(row.key)
                            }
                            Kirigami.ContextualHelpButton { toolTipText: row.definition ? row.definition.help : "" }
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            visible: row.key === "CameraLoopbackDevice"
                            text: i18nc("@info", "Device: %1", root.host.metadata.cameraLoopback ? root.host.metadata.cameraLoopback.state : "")
                        }
                    }
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
