// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
import QtQuick
import QtQuick.Controls as QQC2
// Free text or a path. Clearing it means "inherit" unless the definition says an empty value is meaningful.
QQC2.TextField {
    id: root
    required property BrokerFieldState field
    // Inside a composite control (the address list) the composite is the primary item, not this field.
    property bool reportsPrimary: true
    Binding { target: root.field; property: "primary"; value: root; when: root.reportsPrimary }
    objectName: field.prefix + field.key
    enabled: field.editable
    Accessible.name: field.label
    text: field.savedValue
    placeholderText: field.accountPreference ? i18nc("@info:placeholder", "Use host setting") : field.defaultValue
    maximumLength: field.accountPreference ? 256 : 4096
    onTextEdited: {
        if (text === "" && !field.definition.keepEmpty) field.settings.inherit(field.key);
        else field.settings.setValue(field.key, text);
    }
}
