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
    readonly property var runtime: host.runtime
    spacing: Kirigami.Units.smallSpacing
    function fieldNames(keys) {
        return (keys || []).map(key => { const definition = host.definitions.find(row => row.key === key); return definition ? definition.label : key; }).join(", ");
    }
    function runtimeState(state) {
        switch (state) {
        case "verified": return i18nc("@info", "Running with the saved settings");
        case "different": return i18nc("@info", "The running service does not match the saved settings");
        case "custom": return i18nc("@info", "A custom setup was found; the settings this page knows about match");
        case "partial": return i18nc("@info", "The check could not confirm everything");
        case "missing": return i18nc("@info", "The service was not found");
        case "inactive": return i18nc("@info", "The service is not running");
        case "stale": return i18nc("@info", "Something changed during the check. Check again");
        case "denied": return i18nc("@info", "The check was not allowed");
        case "malformed": return i18nc("@info", "The service information could not be validated");
        default: return i18nc("@info", "The running service could not be checked");
        }
    }
    function runtimeReason(reason) {
        switch (reason) {
        case "unsupported-command": case "custom-unit": return i18nc("@info", "The unit's command or service context is not fully supported for inspection.");
        case "unknown-option": case "duplicate-option": case "invalid-field": return i18nc("@info", "Unrecognized, repeated or invalid startup arguments prevent complete verification.");
        case "missing-field": return i18nc("@info", "Some startup values were omitted. Their defaults are not inferred from this version.");
        case "custom-worker": case "custom-authentication": case "custom-runtime": case "incomplete-context": return i18nc("@info", "The service's worker, authentication or runtime context cannot be fully verified.");
        case "unsafe-file": case "missing-file": case "invalid-environment": return i18nc("@info", "An environment file is missing, unsafe or invalid.");
        case "unknown-expansion": case "inherited-environment": return i18nc("@info", "Environment inheritance or command expansion prevents complete verification.");
        case "manager-reload": return i18nc("@info", "systemd has not reloaded changed unit files. Loaded unit settings may differ from disk.");
        case "process-identity": return i18nc("@info", "The running process could not be matched safely to the installed service.");
        case "stale": return i18nc("@info", "Settings, unit or process identity changed during inspection.");
        case "denied": return i18nc("@info", "The system manager refused inspection.");
        case "bounds": case "malformed": return i18nc("@info", "Host information exceeds limits or is invalid.");
        default: return i18nc("@info", "The system manager or running process is unavailable.");
        }
    }
    Kirigami.InlineMessage { Layout.fillWidth: true; type: Kirigami.MessageType.Error; visible: root.host.error !== ""; text: root.host.error }
    RowLayout {
        Layout.fillWidth: true
        QQC2.Button { objectName: "inspectHostRuntime"; text: i18nc("@action:button", "Check Running Service…"); enabled: !root.host.busy; onClicked: root.host.loaded ? root.host.inspectRuntime() : root.host.refresh() }
        QQC2.ToolButton {
            objectName: root.route + "RefreshStatus"
            icon.name: "view-refresh"; text: i18nc("@action:button", "Refresh status")
            enabled: root.administration && !root.administration.busy
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered
            onClicked: root.administration.refresh()
        }
        Kirigami.ContextualHelpButton { toolTipText: i18nc("@info:tooltip", "Compares the saved settings with how the service was started. It is a snapshot, not a network check: it does not prove the service is reachable or which certificate it has loaded.") }
        Item { Layout.fillWidth: true }
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: root.host.runtimeCheckedAt !== ""
        Kirigami.Heading { level: 3; text: i18nc("@title:group", "Running Service Check") }
        QQC2.Label {
            objectName: "hostRuntimeSummary"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.runtimeState(root.runtime.state)
        }
        Kirigami.InlineMessage {
            objectName: "hostRuntimeStale"
            Layout.fillWidth: true
            visible: root.host.runtimeCheckedAt !== "" && root.host.runtimeStale
            type: Kirigami.MessageType.Warning
            text: i18nc("@info", "This check is out of date: the saved settings changed, or something changed while it ran. Check again. Your pending edits are not part of it.")
        }
        QQC2.CheckBox {
            id: runtimeDetails
            objectName: "showHostRuntimeDetails"
            text: i18nc("@option:check", "Show details")
        }
        // Hidden, never unloaded: the same rows stay in place while the check is repeated.
        ColumnLayout {
            objectName: "hostRuntimeDetails"
            Layout.fillWidth: true
            visible: runtimeDetails.checked
            spacing: Kirigami.Units.smallSpacing
            QQC2.Label {
                objectName: "hostRuntimeCheckedAt"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: root.host.runtimeCheckedAt !== ""
                text: root.host.runtimeCheckedAt === "" ? "" : i18nc("@info", "Checked at %1. This is a snapshot; check again after changing settings or restarting the service.", root.host.runtimeCheckedAt)
            }
            QQC2.Label {
                objectName: "hostRuntimeVerification"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: i18nc("@info", "Service configuration files: %1. How the running service was started: %2.",
                    root.runtime.configuredVerified ? i18nc("@info", "verified") : i18nc("@info", "unverified"),
                    root.runtime.runningVerified ? i18nc("@info", "verified") : i18nc("@info", "unverified"))
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: root.runtime.custom === true
                text: i18nc("@info", "The service has custom startup commands, overrides or environment files. They can override the saved settings.")
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
                text: i18nc("@info", "Settings that could not be observed: %1.", root.fieldNames(root.runtime.missing))
            }
            QQC2.Label {
                objectName: "hostRuntimeDifferences"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: (root.runtime.configuredDifferences || []).length + (root.runtime.runningDifferences || []).length > 0
                text: i18nc("@info", "Configuration files differ for: %1. The running service differs for: %2.",
                    root.fieldNames(root.runtime.configuredDifferences) || i18nc("@info", "none"),
                    root.fieldNames(root.runtime.runningDifferences) || i18nc("@info", "none"))
            }
            QQC2.CheckBox {
                id: runtimeValues
                objectName: "showHostRuntimeValues"
                text: i18nc("@option:check", "Show the values found")
            }
            Repeater {
                model: runtimeValues.checked ? root.host.definitions : []
                delegate: QQC2.Label {
                    required property var modelData
                    objectName: "runtime_" + modelData.key
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: i18nc("@info", "%1 — configuration: %2; running service: %3.", modelData.label,
                        Object.prototype.hasOwnProperty.call(root.runtime.configured || {}, modelData.key) ? root.runtime.configured[modelData.key] : i18nc("@info", "unverified"),
                        Object.prototype.hasOwnProperty.call(root.runtime.running || {}, modelData.key) ? root.runtime.running[modelData.key] : i18nc("@info", "not observed"))
                }
            }
        }
    }
}
