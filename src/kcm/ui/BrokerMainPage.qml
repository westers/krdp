// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM
KCM.AbstractKCM {
    id: root
    objectName: "mainPage"
    property var navigation: kcm
    property var administration: kcm.brokerServices
    property var consoleHost: kcm.consoleHostSettings
    property var virtualHost: kcm.virtualHostSettings
    property var sessionSettings: kcm.virtualSessionSettings
    property var authentication: kcm.brokerAuthentication
    property var preferences: kcm.brokerPreferences
    property string hostName: kcm.hostName
    property int currentPage: 0
    property var history: []
    readonly property var pages: [overview, consolePage, virtualPage, accessPage, preferencesPage, consoleCertificate, virtualCertificate, hardwarePage, consoleDetails, virtualDetails]
    readonly property var flickable: pages[currentPage].flickable
    title: pages[currentPage].title
    framedView: false
    function openPage(index) {
        if (index === currentPage) return;
        if (index === 5 && !consoleHost.beginCertificateEdit()) return;
        if (index === 6 && !virtualHost.beginCertificateEdit()) return;
        history = history.concat([currentPage]); currentPage = index;
    }
    function goBack() {
        if (currentPage === 5) consoleHost.cancelCertificateEdit();
        if (currentPage === 6) virtualHost.cancelCertificateEdit();
        const previous = history.length ? history[history.length - 1] : 0;
        history = history.slice(0, -1); currentPage = previous;
    }
    function showPreferences() { openPage(4); preferencesPage.showDisplays(); }
    function copyAddressToClipboard(address) { navigation.copyAddressToClipboard(address); }
    Component.onCompleted: if (!administration.busy && administration.services.some(service => !service.known)) administration.refresh(false)
    header: QQC2.ToolBar {
        contentItem: RowLayout {
            QQC2.ToolButton {
                objectName: "settingsBack"
                visible: root.currentPage !== 0
                icon.name: "go-previous"
                text: i18nc("@action:button", "Back")
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered
                onClicked: root.goBack()
            }
            Kirigami.Heading { Layout.fillWidth: true; level: 2; text: root.title; elide: Text.ElideRight }
        }
    }
    StackLayout {
        anchors.fill: parent
        currentIndex: root.currentPage
        // Persistent pages preserve focus, unfinished text, scroll and drafts.
        // Service event updates never replace the editor tree.
        KCM.SimpleKCM {
            id: overview
            objectName: "settingsOverview"
            Layout.minimumWidth: 0; Layout.minimumHeight: 0
            title: i18nc("@title:window", "Farside Remote Desktop")
            ColumnLayout {
              ColumnLayout {
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 38
                Layout.alignment: Qt.AlignLeft
                spacing: Kirigami.Units.largeSpacing
                QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Choose which desktop to make available. Service switches apply immediately.") }
                Kirigami.Heading { level: 3; text: i18nc("@title:group", "Console") }
                QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Share this computer's desktop.") }
                BrokerServiceControls { Layout.fillWidth: true; administration: root.administration; route: "console"; host: root.consoleHost; navigation: root; hostName: root.hostName; showDetailsToggle: false; showBoot: true }
                Flow {
                    Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
                    QQC2.Button { objectName: "configureConsole"; text: root.consoleHost.modified ? i18nc("@action:button", "Configure Console… (unsaved)") : i18nc("@action:button", "Configure Console…"); onClicked: root.openPage(1) }
                    QQC2.Button { objectName: "consoleServiceDetails"; text: i18nc("@action:button", "Service Details…"); onClicked: root.openPage(8) }
                }
                Kirigami.Separator { Layout.fillWidth: true }
                Kirigami.Heading { level: 3; text: i18nc("@title:group", "Virtual") }
                QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Separate desktops for remote users.") }
                BrokerServiceControls { Layout.fillWidth: true; administration: root.administration; route: "virtual"; host: root.virtualHost; navigation: root; hostName: root.hostName; showDetailsToggle: false; showBoot: true }
                Flow {
                    Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
                    QQC2.Button { objectName: "configureVirtual"; text: root.virtualHost.modified || root.sessionSettings.modified ? i18nc("@action:button", "Configure Virtual… (unsaved)") : i18nc("@action:button", "Configure Virtual…"); onClicked: root.openPage(2) }
                    QQC2.Button { objectName: "virtualServiceDetails"; text: i18nc("@action:button", "Service Details…"); onClicked: root.openPage(9) }
                }
              }
            }
            footer: QQC2.ToolBar {
                contentItem: RowLayout {
                    QQC2.Button { objectName: "configureAccess"; text: root.authentication.modified ? i18nc("@action:button", "Who Can Connect… (unsaved)") : i18nc("@action:button", "Who Can Connect…"); onClicked: root.openPage(3) }
                    QQC2.Button { objectName: "configurePreferences"; text: root.preferences.modified ? i18nc("@action:button", "My Preferences… (unsaved)") : i18nc("@action:button", "My Preferences…"); onClicked: root.openPage(4) }
                    Item { Layout.fillWidth: true }
                }
            }
        }
        BrokerHostsPage { id: consolePage; objectName: "consoleSettingsPage"; Layout.minimumWidth: 0; Layout.minimumHeight: 0; fixedScope: 0; consoleSettings: root.consoleHost; virtualSettings: root.virtualHost; sessionSettings: root.sessionSettings; administration: root.administration; navigation: root }
        BrokerHostsPage { id: virtualPage; objectName: "virtualSettingsPage"; Layout.minimumWidth: 0; Layout.minimumHeight: 0; fixedScope: 1; consoleSettings: root.consoleHost; virtualSettings: root.virtualHost; sessionSettings: root.sessionSettings; administration: root.administration; navigation: root }
        BrokerSignInPage { id: accessPage; Layout.minimumWidth: 0; Layout.minimumHeight: 0; administration: root.authentication; serviceAdministration: root.administration }
        BrokerPreferencesPage { id: preferencesPage; Layout.minimumWidth: 0; Layout.minimumHeight: 0; preferences: root.preferences }
        BrokerCertificatePage { id: consoleCertificate; Layout.minimumWidth: 0; Layout.minimumHeight: 0; host: root.consoleHost; navigation: root }
        BrokerCertificatePage { id: virtualCertificate; Layout.minimumWidth: 0; Layout.minimumHeight: 0; host: root.virtualHost; navigation: root }
        BrokerHostsPage { id: hardwarePage; objectName: "virtualHardwarePage"; Layout.minimumWidth: 0; Layout.minimumHeight: 0; fixedScope: 2; consoleSettings: root.consoleHost; virtualSettings: root.virtualHost; sessionSettings: root.sessionSettings; administration: root.administration; navigation: root }
        BrokerServiceDetailsPage { id: consoleDetails; Layout.minimumWidth: 0; Layout.minimumHeight: 0; host: root.consoleHost; administration: root.administration; route: "console"; navigation: root; hostName: root.hostName }
        BrokerServiceDetailsPage { id: virtualDetails; Layout.minimumWidth: 0; Layout.minimumHeight: 0; host: root.virtualHost; administration: root.administration; route: "virtual"; navigation: root; hostName: root.hostName }
    }
}
