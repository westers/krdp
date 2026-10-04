// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
// The fields of one page section, in definition order. Pages name a section (or the advanced group); the models'
// definitions decide which keys that is. Advanced rows are always created and only hidden: Kirigami's FormLayout
// warns when a row is destroyed while the form lives.
Repeater {
    id: root
    required property var settings
    property string section: ""
    property bool advanced: false
    property bool shown: true
    // False for a group that does not exist on this page at all (no rows are created).
    property bool included: true
    property bool busy: false
    property string prefix: "host_"
    property bool accountPreference: false
    property var lockedKeys: []
    model: !root.included ? [] : root.settings.definitions.filter(row => root.advanced ? row.advanced : (!row.advanced && row.section === root.section))
    delegate: BrokerSettingField {
        required property var modelData
        settings: root.settings
        definition: modelData
        prefix: root.prefix
        accountPreference: root.accountPreference
        shown: root.shown
        busy: root.busy
        lockedKeys: root.lockedKeys
    }
}
