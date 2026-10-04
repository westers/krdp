// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
// "Use host setting" / "Custom" for an account preference that has no natural "unset" choice (numbers, sizes).
QQC2.ComboBox {
    id: root
    required property BrokerFieldState field
    objectName: "inherit_" + field.key
    enabled: field.editable
    Layout.fillWidth: true
    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
    model: [field.inheritanceText, i18nc("@item:inlistbox", "Custom")]
    currentIndex: field.overridden ? 1 : 0
    Accessible.name: field.label
    onActivated: index => {
        if (index === 0) field.settings.inherit(field.key);
        else if (!field.overridden) field.settings.setValue(field.key, ""); // incomplete, not a fabricated default
    }
}
