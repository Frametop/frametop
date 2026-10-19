// Frametop Gaze Bench's window (gazebench.py), one page at a time: "menu" picks a scenario,
// "run" follows it (its targets show on the headset's panel, not here), "done" shows how far the
// trackers were off. The keys work as the headset button does.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: win
    title: "Frametop Gaze Bench"
    width: 960
    height: 720
    visible: true
    color: "#262626"

    readonly property color ink: "#eeeeee"
    readonly property color dim: "#9a9a9a"

    Shortcut { sequence: "Esc"; onActivated: bench.stop() }
    Shortcut { sequence: "Backspace"; onActivated: bench.undo() }
    Shortcut { sequence: "S"; enabled: bench.page === "run"; onActivated: bench.skip() }
    Shortcut { sequences: ["Space", "Return", "Enter"]; enabled: bench.page !== "menu"; onActivated: bench.markKey() }

    // --- menu

    Item {
        anchors.fill: parent
        visible: bench.page === "menu"

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 60, 900)
            spacing: 12

            Label {
                text: "Gaze Bench"
                font.pixelSize: 30
                color: win.ink
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: win.dim
                font.pixelSize: 15
                text: "Each scenario shows dots one at a time on a panel in front of you in the headset. "
                    + "It moves with your head, so your eyes do all the looking. Look at the dot "
                    + "and press the button on the headset's right side. Every look is recorded, so different "
                    + "gaze techniques can be compared on the same looks afterwards."
            }
            Label {
                Layout.fillWidth: true
                visible: bench.notice !== ""
                wrapMode: Text.Wrap
                color: "#ffb347"
                font.pixelSize: 15
                text: bench.notice
            }
            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                columns: 2
                columnSpacing: 12
                rowSpacing: 8
                Repeater {
                    model: bench.scenarios
                    delegate: Button {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        implicitHeight: 64
                        focusPolicy: Qt.NoFocus
                        onClicked: bench.start(modelData.id)
                        contentItem: ColumnLayout {
                            spacing: 2
                            Label {
                                text: modelData.name + "  (" + modelData.count + ")"
                                font.pixelSize: 16
                                font.bold: true
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData.about
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                font.pixelSize: 12
                                opacity: 0.8
                            }
                        }
                    }
                }
            }
            CheckBox {
                text: "Record the eye cameras too (about 1.2 GB a minute; they stay on this headset)"
                checked: bench.images
                focusPolicy: Qt.NoFocus
                onToggled: bench.images = checked
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: win.dim
                font.pixelSize: 13
                text: bench.status
            }
            RowLayout {
                Layout.topMargin: 4
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: win.dim
                    font.pixelSize: 13
                    text: "The headset button: press = marked, two presses = back one, hold = end. A left click, "
                        + "Enter or an index pinch marks too; a right click ends. "
                        + "Here: Space marks, Backspace goes back one, S skips, Esc ends."
                }
                Button { text: "Quit"; focusPolicy: Qt.NoFocus; onClicked: Qt.quit() }
            }
        }
    }

    // --- a run (the dots are on the headset's panel)

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 60, 760)
        visible: bench.page === "run"
        spacing: 14
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: bench.progress
            color: win.ink
            font.pixelSize: 22
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: bench.message
            color: win.dim
            font.pixelSize: 16
        }
        Label {
            Layout.fillWidth: true
            text: bench.last
            color: win.ink
            font.pixelSize: 16
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: win.dim
            font.pixelSize: 13
            text: "The headset button: press = marked, two presses = back one, hold = end. A left click, "
                + "Enter or an index pinch marks too; a right click ends. "
                + "Here: Space marks, Backspace goes back one, S skips, Esc ends."
        }
        RowLayout {
            spacing: 10
            Button { text: "Back one"; focusPolicy: Qt.NoFocus; onClicked: bench.undo() }
            Button { text: "Skip"; focusPolicy: Qt.NoFocus; onClicked: bench.skip() }
            Button { text: "End"; focusPolicy: Qt.NoFocus; onClicked: bench.stop() }
        }
    }

    // --- done

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 60, 860)
        visible: bench.page === "done"
        spacing: 16
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: bench.summary
            color: win.ink
            font.pixelSize: 18
            lineHeight: 1.3
        }
        RowLayout {
            spacing: 12
            Button { text: "Again"; focusPolicy: Qt.NoFocus; onClicked: bench.again() }
            Button { text: "Scenarios"; focusPolicy: Qt.NoFocus; onClicked: bench.stop() }
        }
    }
}
