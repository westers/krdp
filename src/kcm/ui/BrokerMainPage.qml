// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.delegates as KirigamiDelegates
import org.kde.kcmutils as KCM
// The sidebar of a master-detail module: four rows (Console, Virtual, Who Can
// Connect, My Preferences). The selected row's page is pushed beside it through
// the KCM page stack. Below one detail-pane's worth of room the shell shows one
// page at a time instead, and Back returns to this list.
KCM.ScrollViewKCM {
    id: root
    objectName: "mainPage"
    property var navigation: kcm
    property var administration: kcm.brokerServices
    property var consoleHost: kcm.consoleHostSettings
    property var virtualHost: kcm.virtualHostSettings
    property var sessionSettings: kcm.virtualSessionSettings
    property var authentication: kcm.brokerAuthentication
    property var preferences: kcm.brokerPreferences
    readonly property real sidebarWidth: Kirigami.Units.gridUnit * 14
    // Index of the row whose page is open (-1: none, e.g. after Back in single-column mode).
    property int selected: -1
    readonly property int currentIndex: navigation.depth > 1 ? selected : -1
    // One page at a time (narrow): the list is only a menu, so it shows no selection.
    readonly property bool singleColumn: !!Kirigami.ColumnView.view && Kirigami.ColumnView.view.columnResizeMode === Kirigami.ColumnView.SingleColumn
    readonly property var rows: [
        {key: "console", title: i18nc("@title", "Console"), icon: "monitor-symbolic", route: "console"},
        {key: "virtual", title: i18nc("@title", "Virtual"), icon: "virtual-desktops-symbolic", route: "virtual"},
        {key: "access", title: i18nc("@title", "Who Can Connect"), icon: "system-users-symbolic", route: ""},
        {key: "preferences", title: i18nc("@title", "My Preferences"), icon: "user-identity-symbolic", route: ""}
    ]
    title: i18nc("@title:window", "Farside Remote Desktop")
    // The framework's sidebar mode: beside the detail page when the view is at least 36 grid units wide,
    // otherwise one page at a time with Back (the standard drill-down).
    sidebarMode: true
    function hostProperties(scope) {
        return {objectName: scope === 0 ? "consoleSettingsPage" : "virtualSettingsPage", fixedScope: scope, consoleSettings: consoleHost, virtualSettings: virtualHost, sessionSettings: sessionSettings, administration: administration, navigation: root};
    }
    // Replace whatever detail page is open with the one for this row.
    function select(index, properties) {
        if (index === selected && navigation.depth > 1 && !properties) {
            // Already open: in one-column mode the page is behind the list, so bring it forward.
            if (navigation.currentIndex !== undefined) navigation.currentIndex = navigation.depth - 1;
            return;
        }
        while (navigation.depth > 1) navigation.pop();
        selected = index;
        switch (index) {
        case 0: navigation.push("BrokerHostsPage.qml", hostProperties(0)); break;
        case 1: navigation.push("BrokerHostsPage.qml", hostProperties(1)); break;
        case 2:
            // The access policy is administrator protected: asked for once, when the user opens it.
            if (!authentication.loaded && !authentication.busy) authentication.reload();
            navigation.push("BrokerSignInPage.qml", {administration: authentication, serviceAdministration: administration});
            break;
        default: navigation.push("BrokerPreferencesPage.qml", Object.assign({preferences: preferences}, properties || {}));
        }
    }
    function openConsole() { select(0); }
    function openVirtual() { select(1); }
    function openAccess() { select(2); }
    function openPreferences() { select(3); }
    function showPreferences() { select(3, {scrollToDisplays: true}); }
    function copyAddressToClipboard(address) { navigation.copyAddressToClipboard(address); }
    // The confirmed stop/restart used by the sidebar switches and by the route pages.
    function requestOperation(route, operation) { (route === "console" ? consoleDialog : virtualDialog).request(operation); }
    function summaryFor(route) { return route === "console" ? consoleSummary : virtualSummary; }
    Component.onCompleted: {
        if (navigation.columnWidth !== undefined) navigation.columnWidth = sidebarWidth;
        // Beside the sidebar an empty detail pane would be a blank page, so Console opens at once.
        // It has to happen now, while the page row is still deciding between one and two columns.
        if (navigation.depth <= 1) select(0);
        if (!administration.busy && administration.services.some(service => !service.known)) administration.refresh(false);
    }
    BrokerRouteSummary { id: consoleSummary; service: root.administration.services[0]; host: root.consoleHost }
    BrokerRouteSummary { id: virtualSummary; service: root.administration.services[1]; host: root.virtualHost }
    BrokerServiceDialog { id: consoleDialog; administration: root.administration; route: "console" }
    BrokerServiceDialog { id: virtualDialog; administration: root.administration; route: "virtual" }
    view: ListView {
        id: list
        objectName: "sidebarList"
        model: root.rows
        currentIndex: root.currentIndex
        keyNavigationEnabled: true
        activeFocusOnTab: true
        delegate: QQC2.ItemDelegate {
            id: row
            required property int index
            required property var modelData
            readonly property var summary: modelData.route === "" ? null : root.summaryFor(modelData.route)
            readonly property var host: modelData.key === "console" ? root.consoleHost : modelData.key === "virtual" ? root.virtualHost : null
            readonly property bool pending: modelData.key === "console" ? root.consoleHost.modified : modelData.key === "virtual" ? root.virtualHost.modified || root.sessionSettings.modified : modelData.key === "access" ? root.authentication.modified : root.preferences.modified
            objectName: "sidebar_" + modelData.key
            width: ListView.view.width
            highlighted: root.currentIndex === index && !root.singleColumn
            text: modelData.title
            // A single-choice list of pages: announced as one, with its selection, and operable by assistive technology.
            Accessible.name: modelData.title
            Accessible.role: Accessible.RadioButton
            Accessible.checkable: true
            Accessible.checked: highlighted
            Accessible.onPressAction: clicked()
            onClicked: root.select(index)
            contentItem: RowLayout {
                spacing: Kirigami.Units.smallSpacing
                KirigamiDelegates.IconTitleSubtitle {
                    Layout.fillWidth: true
                    icon.name: row.modelData.icon
                    title: row.modelData.title
                    objectName: row.modelData.key + "Subtitle"
                    subtitle: row.summary ? row.summary.subtitle : row.modelData.key === "access" ? i18nc("@info", "Accounts and remote logins") : i18nc("@info", "Your own display and quality choices")
                    selected: row.highlighted || row.down
                    elide: Text.ElideRight
                }
                Kirigami.Icon {
                    objectName: row.modelData.key + "Modified"
                    visible: row.pending
                    source: "document-edit"
                    selected: row.highlighted
                    implicitWidth: Kirigami.Units.iconSizes.small; implicitHeight: Kirigami.Units.iconSizes.small
                    QQC2.ToolTip.text: i18nc("@info:tooltip", "Unsaved changes")
                    QQC2.ToolTip.visible: false
                }
                QQC2.Switch {
                    objectName: row.modelData.route + "HostEnabled"
                    visible: row.summary !== null
                    // On the highlighted row the switch takes the palette meant for selected items, as Kirigami's own list items do.
                    Kirigami.Theme.inherit: !row.highlighted
                    Kirigami.Theme.colorSet: Kirigami.Theme.Selection
                    Accessible.name: i18nc("@option:check %1 service", "Allow connections to %1", row.modelData.title)
                    readonly property var service: row.summary ? row.summary.service : null
                    checked: row.summary ? row.summary.running : false
                    enabled: !!service && (service.canStart || service.canStop)
                    onClicked: {
                        root.requestOperation(row.modelData.route, checked ? "start" : "stop");
                        checked = Qt.binding(() => row.summary ? row.summary.running : false);
                    }
                }
            }
        }
    }
}
