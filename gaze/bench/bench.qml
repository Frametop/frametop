// Frametop Gaze Bench's window (gazebench.py): full screen, one page at a time. "menu" picks a
// scenario, "run" shows its targets, "done" shows how far the trackers were off.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: win
    title: "Frametop Gaze Bench"
    visibility: Window.FullScreen
    visible: true
    color: bench.page === "run" ? bench.background : "#262626"

    function report() {
        bench.setScreen(win.width, win.height, win.screen ? win.screen.devicePixelRatio : 1,
                        win.screen ? win.screen.name : "", win.screen ? win.screen.virtualX : 0,
                        win.screen ? win.screen.virtualY : 0)
    }
    onWidthChanged: report()
    onHeightChanged: report()
    onScreenChanged: report()
    Component.onCompleted: report()

    // Light text on the dark pages, dark on a bright background
    readonly property bool bright: bench.bright
    readonly property color ink: bright ? "#202020" : "#eeeeee"
    readonly property color dim: bright ? "#606060" : "#9a9a9a"

    Shortcut { sequence: "Esc"; onActivated: bench.stop() }
    Shortcut { sequence: "Backspace"; onActivated: bench.undo() }
    Shortcut { sequence: "S"; enabled: bench.page === "run"; onActivated: bench.skip() }

    // --- menu

    Item {
        anchors.fill: parent
        visible: bench.page === "menu"

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 80, 900)
            spacing: 14

            Label {
                text: "Gaze Bench"
                font.pixelSize: 34
                color: win.ink
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: win.dim
                font.pixelSize: 16
                text: "Turn gaze mode on. Each scenario shows targets one at a time: look at the target, "
                    + "and click what you're really looking at (nudge the pointer onto the target first if "
                    + "it's off). Every click is recorded, so different gaze techniques can be compared "
                    + "on the same looks afterwards."
            }
            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: 10
                columns: 2
                columnSpacing: 14
                rowSpacing: 10
                Repeater {
                    model: bench.scenarios
                    delegate: Button {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        implicitHeight: 74
                        onClicked: bench.start(modelData.id)
                        contentItem: ColumnLayout {
                            spacing: 2
                            Label {
                                text: modelData.name + "  (" + modelData.count + ")"
                                font.pixelSize: 18
                                font.bold: true
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData.about
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                font.pixelSize: 13
                                opacity: 0.8
                            }
                        }
                    }
                }
            }
            CheckBox {
                text: "Record the eye cameras too (about 1.2 GB a minute; they stay on this headset)"
                checked: bench.images
                onToggled: bench.images = checked
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: win.dim
                font.pixelSize: 14
                text: bench.status
            }
            RowLayout {
                Layout.topMargin: 6
                Label {
                    Layout.fillWidth: true
                    color: win.dim
                    font.pixelSize: 13
                    text: "In a scenario: right click or S skips a target, Backspace takes the last one back, Esc ends it."
                }
                Button { text: "Quit"; onClicked: Qt.quit() }
            }
        }
    }

    // --- a run

    Item {
        id: run
        anchors.fill: parent
        visible: bench.page === "run"

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            // The press: in gaze mode the pointer helper sends it with the release, after a nudge
            onPressed: (mouse) => bench.click(mouse.x, mouse.y, mouse.button === Qt.RightButton ? "right" : "left")
        }

        // Where the head should face (eyes only), and where it does
        Item {
            visible: bench.markOn
            x: run.width / 2
            y: run.height / 2
            Rectangle { x: -18; y: -1; width: 36; height: 2; color: win.dim }
            Rectangle { x: -1; y: -18; width: 2; height: 36; color: win.dim }
        }
        Rectangle {
            visible: bench.markOn && bench.headOn
            x: bench.hx - width / 2
            y: bench.hy - height / 2
            width: 22; height: 22; radius: 11
            color: "transparent"
            border.color: "#4fa3ff"
            border.width: 2
            opacity: 0.8
        }

        // The decoy (small and close): a hollow ring
        Rectangle {
            visible: bench.targetOn && bench.decoyOn
            x: bench.dx - width / 2
            y: bench.dy - height / 2
            width: bench.radius * 2; height: width; radius: width / 2
            color: "transparent"
            border.color: win.bright ? "#303030" : "#d0d0d0"
            border.width: 3
        }
        // The target: a ring with a dot to look at; grey until it can be clicked (hold)
        Rectangle {
            id: target
            visible: bench.targetOn
            x: bench.tx - width / 2
            y: bench.ty - height / 2
            width: bench.radius * 2; height: width; radius: width / 2
            readonly property color tone: bench.ready ? (win.bright ? "#000000" : "#ffffff") : "#8a8a8a"
            color: bench.decoyOn ? tone : "transparent"
            border.color: tone
            border.width: 3
            Rectangle {
                anchors.centerIn: parent
                width: 6; height: 6; radius: 3
                color: bench.decoyOn ? (win.bright ? "#f2f2f2" : "#262626") : target.tone
            }
        }

        // Instructions, only while no target shows
        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 80, 760)
            visible: !bench.targetOn
            spacing: 10
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: bench.progress
                color: win.dim
                font.pixelSize: 16
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: bench.message
                color: win.ink
                font.pixelSize: 22
            }
        }
    }

    // --- done

    Item {
        anchors.fill: parent
        visible: bench.page === "done"
        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 80, 900)
            spacing: 16
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: bench.summary
                color: win.ink
                font.pixelSize: 20
                lineHeight: 1.3
            }
            RowLayout {
                spacing: 12
                Button { text: "Again"; onClicked: bench.again() }
                Button { text: "Scenarios"; onClicked: bench.stop() }
            }
        }
    }
}
