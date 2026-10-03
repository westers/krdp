// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
// Read-only startup inspection for one service, embedded in its overview group.
// The expander only toggles `visible` on this item: its state is never unloaded.
ColumnLayout {
    id: root
    required property var host
    required property var administration
    required property string route
    property var navigation
    property string hostName: ""
    readonly property var runtime: host.runtime
    spacing: Kirigami.Units.smallSpacing
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
    QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Startup inspection is a snapshot, not a network reachability check."); color: Kirigami.Theme.disabledTextColor }
    Kirigami.InlineMessage { Layout.fillWidth: true; type: Kirigami.MessageType.Error; visible: root.host.error !== ""; text: root.host.error }
    RowLayout {
        Layout.fillWidth: true
        QQC2.Button { objectName: "inspectHostRuntime"; text: root.host.loaded ? i18nc("@action:button", "Inspect Running Host…") : i18nc("@action:button", "Load Saved Settings…"); enabled: !root.host.busy; onClicked: root.host.loaded ? root.host.inspectRuntime() : root.host.reload() }
        QQC2.ToolButton {
            objectName: root.route + "RefreshStatus"
            icon.name: "view-refresh"; text: i18nc("@action:button", "Refresh status")
            enabled: root.administration && !root.administration.busy
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered
            onClicked: root.administration.refresh()
        }
        Item { Layout.fillWidth: true }
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: root.host.runtimeCheckedAt !== ""
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
            text: i18nc("@info", "Startup values do not verify listening sockets, live client preferences or the TLS certificate already loaded into memory. The certificate section describes stored material.")
        }
        QQC2.CheckBox {
            id: runtimeValues
            objectName: "showHostRuntimeValues"
            text: i18nc("@option:check", "Show inspected values")
        }
        Repeater {
            model: runtimeValues.checked ? root.host.definitions : []
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
}
