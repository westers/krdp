// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
// The one confirmation shared by every stop/restart of a service. Starting needs none.
Kirigami.PromptDialog {
    id: root
    required property var administration
    required property string route
    property string operation: "restart"
    readonly property string routeTitle: route === "console" ? i18nc("@title", "Console") : i18nc("@title", "Virtual")
    function request(op) {
        if (op === "start") administration.perform(route, op);
        else { operation = op; open(); }
    }
    objectName: route + "ConfirmServiceOperation"
    parent: QQC2.Overlay.overlay
    popupType: QQC2.Popup.Item
    y: parent ? Math.round((parent.height - implicitHeight) / 2) : 0
    title: operation === "stop" ? i18nc("@title:window", "Stop %1?", routeTitle) : i18nc("@title:window", "Restart %1?", routeTitle)
    subtitle: i18nc("@info", "Remote clients using this service will disconnect. The other service is unaffected.")
    standardButtons: Kirigami.Dialog.Cancel
    customFooterActions: Kirigami.Action {
        objectName: root.route + "ConfirmServiceAction"
        text: root.operation === "stop" ? i18nc("@action:button", "Stop") : i18nc("@action:button", "Restart")
        onTriggered: { root.close(); root.administration.perform(root.route, root.operation); }
    }
}
