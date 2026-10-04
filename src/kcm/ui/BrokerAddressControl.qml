// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
// A listen address: the definition's modes (inherit, wildcards, "@custom") in a list, and a text field only for the custom one.
ColumnLayout {
    id: root
    required property BrokerFieldState field
    readonly property var modes: field.definition.modes
    readonly property int customIndex: modes.length - 1
    spacing: Kirigami.Units.smallSpacing
    Binding { target: root.field; property: "primary"; value: mode }
    QQC2.ComboBox {
        id: mode
        objectName: root.field.prefix + "AddressMode"
        enabled: root.field.editable
        Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        model: root.modes.map(option => option.text)
        currentIndex: {
            if (!root.field.overridden) return 0;
            const index = root.modes.findIndex(option => option.value === root.field.savedValue);
            return index < 1 ? root.customIndex : index;
        }
        Accessible.name: root.field.label
        onActivated: index => {
            if (index === 0) root.field.settings.inherit(root.field.key);
            else root.field.settings.setValue(root.field.key, index === root.customIndex ? "" : root.modes[index].value);
        }
    }
    BrokerTextControl {
        field: root.field
        reportsPrimary: false
        visible: mode.currentIndex === root.customIndex
        Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24
    }
}
