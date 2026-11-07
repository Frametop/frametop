// Frametop Settings (Kirigami): every Frametop setting in one window. The app: ft_settings.py
// ("settings"). Each area's pages are in its own file (settings.areaPages), loaded with its
// backend the first time one of its sections opens.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root
    title: "Frametop Settings"
    width: Kirigami.Units.gridUnit * 56
    height: Kirigami.Units.gridUnit * 44
    // The sidebar always shows (a drawer that slid away was easy to miss), so the window is at
    // least the sidebar and a narrow page wide: the pages' forms stack their labels from there.
    minimumWidth: Kirigami.Units.gridUnit * 34
    minimumHeight: Kirigami.Units.gridUnit * 20

    // The section showing now (its name, as in ft_settings.SECTIONS).
    property string current: ""
    readonly property string pageTitle: pageStack.currentItem ? pageStack.currentItem.title : ""
    function section(name) {
        return settings.sections.find(s => s.name === name) || null
    }

    // The area's pages (made with its backend the first time), or null when they didn't load.
    function areaPages(area) {
        const loader = loaders[area]
        if (!loader)
            return null
        if (loader.status === Loader.Null)
            loader.setSource(settings.areaPages[area], { backend: settings.backend(area), window: root })
        return loader.status === Loader.Ready ? loader.item : null
    }

    function showSection(name) {
        const s = section(name) || settings.sections[0]
        const pages = areaPages(s.area)
        const page = pages ? pages.page(s.name) : null
        if (!page) {
            showPassiveNotification("Couldn't open " + s.title, "long")
            return
        }
        current = s.name
        pageStack.clear()
        pageStack.push(page)
    }

    readonly property var loaders: ({ input: inputArea, display: displayArea, remote: remoteArea, access: accessArea })
    Loader { id: inputArea }
    Loader { id: displayArea }
    Loader { id: remoteArea }
    Loader { id: accessArea }

    Connections {
        target: settings
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    // The sections, by group, and the help, always there.
    globalDrawer: Kirigami.OverlayDrawer {
        id: sidebar
        edge: Qt.application.layoutDirection === Qt.RightToLeft ? Qt.RightEdge : Qt.LeftEdge
        modal: false
        handleVisible: false
        drawerOpen: true
        width: Kirigami.Units.gridUnit * 13
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0

        contentItem: ColumnLayout {
            id: side
            spacing: 0

            Controls.ToolBar {
                Layout.fillWidth: true
                Layout.preferredHeight: root.pageStack.globalToolBar.preferredHeight
                Kirigami.SearchField {
                    id: search
                    objectName: "search"
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    placeholderText: "Search settings"
                    // Enter opens the first section found, and only Enter: typing filters the list
                    // without opening pages (which would take the keyboard's focus mid-word).
                    autoAccept: false
                    onAccepted: if (side.found.length) root.showSection(side.found[0].name)
                }
            }

            // What the search finds: in a section's title, its group, or its words.
            readonly property var found: {
                const q = search.text.trim().toLowerCase()
                const words = q.split(/\s+/).filter(w => w !== "")
                return settings.sections.filter(s => {
                    const hay = (s.title + " " + s.group + " " + s.keywords).toLowerCase()
                    return words.every(w => hay.indexOf(w) >= 0)
                })
            }

            ListView {
                id: list
                objectName: "sections"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                Controls.ScrollBar.vertical: Controls.ScrollBar {}
                model: side.found
                section.property: "group"
                section.delegate: Kirigami.ListSectionHeader {
                    required property string section
                    width: ListView.view.width
                    text: section
                }
                delegate: Controls.ItemDelegate {
                    required property var modelData
                    width: ListView.view.width
                    text: modelData.title.replace(/&/g, "&&")  // a lone & would be a mnemonic
                    icon.name: modelData.icon
                    highlighted: modelData.name === root.current
                    onClicked: root.showSection(modelData.name)
                }

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 2
                    visible: list.count === 0
                    text: "No settings match"
                    explanation: "Try another word, such as mouse, screen or gaze."
                }
            }

            Kirigami.Separator { Layout.fillWidth: true }

            Controls.ItemDelegate {
                Layout.fillWidth: true
                text: "Controls guide"
                icon.name: "help-contents"
                onClicked: settings.openControlsGuide()
            }
            Controls.ItemDelegate {
                Layout.fillWidth: true
                text: "Frametop Tour"
                icon.name: "help-about"
                onClicked: settings.openTour()
            }
        }
    }

    Component.onCompleted: showSection(settings.startSection)
}
