// Frametop Tour (Kirigami). Backend: ft_tour.py ("backend"). One page at a time from
// backend.pages: a title and intro, the page's videos (one after another, or picked by number),
// the key table on the keyboard page, rows of what to do and what it does under headings, a
// practice target on the gaze and hands pages, and buttons that try things or open their
// settings. The footer's page list jumps anywhere. Closing the window, Skip, and Finish all mark
// the tour done.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root
    title: "Frametop Tour"
    width: Kirigami.Units.gridUnit * 42
    height: Kirigami.Units.gridUnit * 40

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

    // Two columns: what to do, and what it does. A string is a heading over the rows after it,
    // and a row's third item an action its Open button runs.
    component Table: ColumnLayout {
        id: table
        property var rows: []
        spacing: Kirigami.Units.smallSpacing
        Layout.fillWidth: true
        Repeater {
            model: table.rows
            ColumnLayout {
                id: row
                required property var modelData
                required property int index
                readonly property bool heading: typeof modelData === "string"
                spacing: 0
                Layout.fillWidth: true
                Kirigami.Heading {
                    visible: row.heading
                    level: 3
                    text: row.heading ? row.modelData : ""
                    Layout.topMargin: row.index > 0 ? Kirigami.Units.largeSpacing : 0
                }
                RowLayout {
                    visible: !row.heading
                    spacing: Kirigami.Units.largeSpacing
                    Layout.fillWidth: true
                    Controls.Label {
                        text: row.heading ? "" : row.modelData[0]
                        font.bold: true
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        Layout.preferredWidth: 2  // with fillWidth, the share of the width
                        Layout.alignment: Qt.AlignTop
                    }
                    Controls.Label {
                        text: row.heading ? "" : row.modelData[1]
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        Layout.preferredWidth: 3
                        Layout.alignment: Qt.AlignTop
                    }
                    Controls.Button {
                        visible: !row.heading && row.modelData.length > 2
                        text: "Open"
                        icon.name: "go-next"
                        Layout.alignment: Qt.AlignTop
                        onClicked: backend.act(row.modelData[2])
                    }
                }
            }
        }
    }

    // The page's videos: each loops once, then the next one plays. A click pauses; a number picks.
    component Clips: ColumnLayout {
        id: clips
        property var list: []
        property int shown: 0
        property bool paused: false
        readonly property var current: list[Math.min(shown, list.length - 1)] || ({})
        visible: list.length > 0
        spacing: Kirigami.Units.smallSpacing
        Layout.fillWidth: true
        onListChanged: { shown = 0; paused = false }

        Rectangle {
            color: "black"
            radius: Kirigami.Units.smallSpacing
            clip: true
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 34
            Layout.preferredHeight: width * 9 / 16
            Layout.alignment: Qt.AlignHCenter
            AnimatedImage {
                id: video
                anchors.fill: parent
                source: clips.current.file ? "file://" + clips.current.file : ""
                fillMode: Image.PreserveAspectFit
                cache: false
                playing: !clips.paused && root.visible
                property int last: 0
                onCurrentFrameChanged: {
                    // back to its first frame: it played through once, so the next one's turn
                    if (currentFrame < last && clips.list.length > 1)
                        clips.shown = (clips.shown + 1) % clips.list.length
                    last = currentFrame
                }
                onSourceChanged: last = 0
            }
            Kirigami.Icon {
                anchors.centerIn: parent
                visible: clips.paused
                source: "media-playback-start"
                implicitWidth: Kirigami.Units.iconSizes.huge
                implicitHeight: implicitWidth
                opacity: 0.8
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: clips.paused = !clips.paused
            }
        }
        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 34
            Layout.alignment: Qt.AlignHCenter
            Repeater {
                model: clips.list.length > 1 ? clips.list.length : 0
                Controls.Button {
                    required property int index
                    text: index + 1
                    highlighted: index === clips.shown
                    implicitWidth: implicitHeight
                    Controls.ToolTip.text: clips.list[index].caption
                    Controls.ToolTip.visible: hovered
                    onClicked: { clips.shown = index; clips.paused = false }
                }
            }
            Controls.Label {
                text: clips.list.length > 1 ? "Videos " + (clips.shown + 1) + " of " + clips.list.length
                                              + ": they play in turn" : ""
                opacity: 0.7
                font: Kirigami.Theme.smallFont
                Layout.fillWidth: true
            }
        }
    }

    pageStack.initialPage: Kirigami.ScrollablePage {
        id: tourPage
        title: root.page.title || ""
        Connections {
            target: root
            function onIndexChanged() { tourPage.flickable.contentY = 0 }
        }
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

            Clips {
                list: root.page.clips || []
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
                              : "Poke the button below with your index finger. From a distance, with gaze mode "
                                + "on, look at it and pinch; with it off, point a raised hand at it and pinch."
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
                // Every page by name: the tour as a reference
                Controls.ComboBox {
                    id: jump
                    model: root.pages.map((p, i) => (i + 1) + ". " + p.title)
                    implicitWidth: Kirigami.Units.gridUnit * 14
                    Component.onCompleted: currentIndex = root.index
                    onActivated: (i) => root.index = i
                    Connections {
                        target: root
                        function onIndexChanged() { jump.currentIndex = root.index }
                    }
                }
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
