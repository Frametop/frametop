// Frametop Tour (Kirigami). Backend: ft_tour.py ("backend"). One page at a time from
// backend.pages: a title and intro, the key table on the keyboard page, rows of what to do and
// what it does, a practice target on the gaze and hands pages, and buttons that try things or
// open their settings. Closing the window, Skip, and Finish all mark the tour done.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root
    title: "Frametop Tour"
    width: Kirigami.Units.gridUnit * 40
    height: Kirigami.Units.gridUnit * 34

    readonly property var pages: backend.pages
    property int index: Math.max(0, pages.findIndex(p => p.id === startPage))
    readonly property var page: pages[index] || ({})
    readonly property bool last: index === pages.length - 1
    property string how: "closed"  // what onClosing tells the backend: closed, skipped, finished

    onClosing: backend.finish(how)

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    // Two columns: what to do, and what it does.
    component Table: GridLayout {
        property var rows: []
        columns: 2
        columnSpacing: Kirigami.Units.largeSpacing
        rowSpacing: Kirigami.Units.smallSpacing
        Layout.fillWidth: true
        Repeater {
            model: rows.length * 2
            Controls.Label {
                required property int index
                readonly property bool doing: index % 2 === 0
                text: rows[Math.floor(index / 2)][doing ? 0 : 1]
                font.bold: doing
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.preferredWidth: doing ? 2 : 3  // with fillWidth, the share of the width
                Layout.alignment: Qt.AlignTop
            }
        }
    }

    pageStack.initialPage: Kirigami.ScrollablePage {
        title: root.page.title || ""
        // The page's own number, not Kirigami's breadcrumb
        actions: [
            Kirigami.Action {
                displayHint: Kirigami.DisplayHint.KeepVisible
                text: (root.index + 1) + " of " + root.pages.length
                enabled: false
            }
        ]

        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            RowLayout {
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Icon {
                    source: root.page.icon || ""
                    implicitWidth: Kirigami.Units.iconSizes.large
                    implicitHeight: Kirigami.Units.iconSizes.large
                    Layout.alignment: Qt.AlignTop
                }
                Controls.Label {
                    text: root.page.intro || ""
                    visible: text !== ""
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }

            // Keyboard page: the key combinations the input relay has now.
            Kirigami.Heading {
                level: 3
                text: "Your key combinations"
                visible: (root.page.keys || []).length > 0
            }
            Table {
                visible: (root.page.keys || []).length > 0
                rows: (root.page.keys || []).map(k => [k.combo, k.action])
            }
            Kirigami.Heading {
                level: 3
                text: "Desktop shortcuts"
                visible: (root.page.keys || []).length > 0
            }

            Table {
                visible: rows.length > 0
                rows: root.page.rows || []
            }

            // Something to click with your eyes or your hands. A click is a click whatever
            // made it, so it counts all of them.
            Kirigami.AbstractCard {
                id: practice
                visible: (root.page.practice || "") !== ""
                property int hits: 0
                Layout.fillWidth: true
                Connections {
                    target: root
                    function onIndexChanged() { practice.hits = 0 }
                }
                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.largeSpacing
                    Kirigami.Heading {
                        level: 3
                        text: "Try it"
                    }
                    Controls.Label {
                        text: root.page.practice === "gaze"
                              ? "Look at the button below and tap Meta+J. Then look away, hold Meta+J, turn "
                                + "your head back onto it, and let go."
                              : "Poke the button below with your index finger. With gaze mode on, look at it "
                                + "and pinch instead."
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Controls.Button {
                        text: practice.hits === 0 ? "Click me" : practice.hits === 1 ? "Got it: 1 click"
                                                                                     : "Got it: " + practice.hits + " clicks"
                        icon.name: practice.hits === 0 ? "crosshairs" : "checkmark"
                        implicitWidth: Kirigami.Units.gridUnit * 14
                        implicitHeight: Kirigami.Units.gridUnit * 4
                        Layout.alignment: Qt.AlignHCenter
                        onClicked: practice.hits++
                    }
                }
            }

            // Floating windows page: the Display Settings choice, right here.
            ColumnLayout {
                visible: root.page.toggle === "dashboardApps"
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Heading {
                    level: 3
                    text: "Apps from the SteamVR dashboard"
                }
                Controls.RadioButton {
                    text: "Open as SteamVR windows"
                    checked: backend.dashboardApps !== "frametop"
                    onToggled: if (checked) backend.setDashboardApps("steamvr")
                }
                Controls.RadioButton {
                    text: "Open as Frametop floating windows (SteamVR windows when every one is in use)"
                    checked: backend.dashboardApps === "frametop"
                    onToggled: if (checked) backend.setDashboardApps("frametop")
                }
            }

            Controls.Label {
                text: root.page.note || ""
                visible: text !== ""
                wrapMode: Text.Wrap
                opacity: 0.8
                Layout.fillWidth: true
            }

            Flow {
                spacing: Kirigami.Units.smallSpacing
                visible: (root.page.actions || []).length > 0
                Layout.fillWidth: true
                Repeater {
                    model: root.page.actions || []
                    Controls.Button {
                        required property var modelData
                        text: modelData.text
                        icon.name: modelData.icon
                        onClicked: backend.act(modelData.id)
                    }
                }
            }

            Controls.CheckBox {
                visible: root.last
                text: "Show this tour when the desktop starts"
                checked: backend.showAtStart
                onToggled: backend.setShowAtStart(checked)
            }
        }

        footer: Controls.ToolBar {
            contentItem: RowLayout {
                Controls.Button {
                    text: "Skip the tour"
                    flat: true
                    visible: !root.last
                    onClicked: { root.how = "skipped"; root.close() }
                }
                Item { Layout.fillWidth: true }
                Controls.Button {
                    text: "Back"
                    icon.name: "go-previous"
                    enabled: root.index > 0
                    onClicked: root.index--
                }
                Controls.Button {
                    text: root.last ? "Finish" : "Next"
                    icon.name: root.last ? "checkmark" : "go-next"
                    highlighted: true
                    onClicked: {
                        if (root.last) {
                            root.how = "finished"
                            root.close()
                        } else {
                            root.index++
                        }
                    }
                }
            }
        }
    }
}
