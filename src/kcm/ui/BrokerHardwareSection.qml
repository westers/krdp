// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
// GPU grants and VA-API policy for newly created Virtual desktops. It has its own
// save scope (`settings` is kcm.virtualSessionSettings), separate from the host page.
ColumnLayout {
    id: root
    objectName: "desktopHardwareSection"
    required property var settings
    property var navigation
    property bool showPciEditor: false
    readonly property var selectedDevices: (settings.values.RenderPci || "").split(",").map(value => value.trim()).filter(value => value !== "")
    function fields(keys) { return settings.definitions.filter(row => keys.includes(row.key)); }
    spacing: Kirigami.Units.smallSpacing
    Kirigami.Heading { level: 2; text: i18nc("@title:group", "New desktop hardware") }
    QQC2.Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap
        text: i18nc("@info", "Applies to newly created desktops only. Existing desktops keep their current hardware grants. It is saved together with the settings above when you apply.")
    }
    Kirigami.InlineMessage { objectName: "desktopHardwareError"; Layout.fillWidth: true; type: Kirigami.MessageType.Error; visible: root.settings.error !== ""; text: root.settings.error }
    Kirigami.InlineMessage { objectName: "desktopHardwareSavedNotice"; Layout.fillWidth: true; visible: root.settings.applicationRequired; type: Kirigami.MessageType.Information; text: i18nc("@info", "Saved. New desktops will use these defaults.") }
    ColumnLayout {
        Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
        visible: root.settings.loaded && (root.settings.metadata.renderDevices || []).length > 0
        Kirigami.Heading { level: 4; text: i18nc("@title:group", "Graphics access") }
        QQC2.CheckBox {
            text: i18nc("@option:check", "Grant no GPU access")
            checked: root.selectedDevices.length === 0; enabled: !root.settings.busy
            onClicked: { if (checked) root.settings.setValue("RenderPci", ""); checked = Qt.binding(() => root.selectedDevices.length === 0); }
        }
        Repeater {
            model: root.settings.metadata.renderDevices || []
            delegate: QQC2.CheckBox {
                required property var modelData
                text: i18nc("@option:check", "%1 (%2)", modelData.pci, modelData.driver)
                checked: root.selectedDevices.includes(modelData.pci); enabled: !root.settings.busy
                onClicked: {
                    const values = root.selectedDevices.filter(value => value !== modelData.pci);
                    if (checked) values.push(modelData.pci);
                    root.settings.setValue("RenderPci", values.join(","));
                    checked = Qt.binding(() => root.selectedDevices.includes(modelData.pci));
                }
            }
        }
        QQC2.Button { text: i18nc("@action:button", "Edit PCI Identities…"); onClicked: root.showPciEditor = !root.showPciEditor }
    }
    Kirigami.FormLayout {
        wideMode: width >= Kirigami.Units.gridUnit * 32
        Layout.alignment: Qt.AlignLeft
        Layout.fillWidth: true
        visible: root.settings.loaded
        Repeater {
            model: root.fields(["VaapiDriver", "RenderPci"])
            delegate: BrokerSettingField {
                required property var modelData
                settings: root.settings; definition: modelData; prefix: "desktop_"
                visible: !(key === "RenderPci" && (root.settings.metadata.renderDevices || []).length > 0 && !root.showPciEditor)
                editable: !root.settings.busy
                showHelp: true
            }
        }
    }
    QQC2.Label { visible: root.settings.loaded; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "GPU grants control desktop device access, not which GPU encodes each stream. An empty PCI list grants no GPU.") }
    Repeater {
        model: root.settings.loaded ? root.settings.metadata.renderDevices || [] : []
        delegate: QQC2.Label { required property var modelData; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Available device: %1 (%2), %3.", modelData.pci, modelData.driver, modelData.render) }
    }
}
