// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
// Names every scope the last Apply could not save. Those drafts stay pending
// (and the Apply button stays enabled); scopes not listed here were saved.
Kirigami.InlineMessage {
    id: root
    objectName: "applyFailures"
    readonly property var apply: (typeof kcm !== "undefined" && kcm && kcm.settingsApply) ? kcm.settingsApply : null
    Layout.fillWidth: true
    type: Kirigami.MessageType.Error
    visible: apply !== null && apply.failures.length > 0
    text: apply ? i18nc("@info", "Not saved, still pending: %1", apply.failures.join("  ·  ")) : ""
}
