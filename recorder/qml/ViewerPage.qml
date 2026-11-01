// The recorders' Viewer page: one take, a picture at a time, stepped by keys or buttons, with
// ranges to delete (left out on export) and restore. Both windows use it; what differs comes in as
// properties.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Page {
    id: viewer
    // What one picture of a take is called (set: the hand recorder's frame sets; frame: the eye
    // recorder's frame pairs), how far Page Up/Down steps, the picture's image source (provider
    // and kind), its time (the backend's setTime or frameTime) and the help line under it.
    property string frameNoun
    property int pageStep: 10
    property string imagePath
    property var frameTime: function (session, take, index) { return "" }
    property string noFramesText
    property string helpText
    property string session: ""
    property string take: ""
    property var info: backend.takeInfo(session, take)
    property int index: 0
    property int markStart: -1
    property int markEnd: -1
    title: info.title
    readonly property bool deletedHere: isDeleted(index)

    function step(n) {
        index = Math.max(0, Math.min(info.count - 1, index + n))
    }
    function isDeleted(i) {
        for (const r of info.ranges)
            if (i >= r[0] && i <= r[1] && r[0] >= 0)
                return true
        return false
    }
    function deleteMarked() {
        backend.deleteRange(session, take, markStart, markEnd)
        markStart = markEnd = -1
        info = backend.takeInfo(session, take)
    }

    Connections {
        target: backend
        function onSessionsChanged() { viewer.info = backend.takeInfo(viewer.session, viewer.take) }
    }

    // Keyboard stepping: arrows one frame, Page Up/Down pageStep, Home/End, [ and ] mark a
    // range, Delete deletes it.
    Item {
        id: keys
        focus: true
        Component.onCompleted: forceActiveFocus()
        Keys.onPressed: event => {
            const k = event.key
            if (k === Qt.Key_Left) viewer.step(-1)
            else if (k === Qt.Key_Right) viewer.step(1)
            else if (k === Qt.Key_PageUp) viewer.step(-viewer.pageStep)
            else if (k === Qt.Key_PageDown) viewer.step(viewer.pageStep)
            else if (k === Qt.Key_Home) viewer.index = 0
            else if (k === Qt.Key_End) viewer.index = Math.max(0, viewer.info.count - 1)
            else if (k === Qt.Key_BracketLeft) viewer.markStart = viewer.index
            else if (k === Qt.Key_BracketRight) viewer.markEnd = viewer.index
            else if (k === Qt.Key_Delete && viewer.markStart >= 0 && viewer.markEnd >= 0) viewer.deleteMarked()
            else return
            event.accepted = true
        }
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        visible: viewer.info.count === 0
        text: viewer.noFramesText
    }

    ColumnLayout {
        anchors.fill: parent
        visible: viewer.info.count > 0
        spacing: Kirigami.Units.smallSpacing

        Image {
            id: frame
            Layout.fillWidth: true
            Layout.fillHeight: true
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: false
            retainWhileLoading: true
            source: viewer.info.count > 0
                    ? "image://" + viewer.imagePath + "/" + viewer.session + "/" + viewer.take + "/" + viewer.index : ""
            MouseArea { anchors.fill: parent; onClicked: keys.forceActiveFocus() }

            Rectangle {
                visible: viewer.deletedHere
                x: (frame.width - frame.paintedWidth) / 2
                y: (frame.height - frame.paintedHeight) / 2
                width: frame.paintedWidth
                height: frame.paintedHeight
                color: Qt.rgba(0.8, 0, 0, 0.35)
                Controls.Label {
                    anchors.centerIn: parent
                    text: "Deleted: not exported"
                    color: "white"
                    font.bold: true
                }
            }
        }

        // The slider, with the deleted ranges (red) and the marked range (blue) under it.
        Item {
            Layout.fillWidth: true
            implicitHeight: slider.implicitHeight
            Repeater {
                model: viewer.info.ranges
                Rectangle {
                    required property var modelData
                    visible: modelData[0] >= 0
                    readonly property real unit: (slider.availableWidth) / Math.max(1, viewer.info.count - 1)
                    x: slider.leftPadding + modelData[0] * unit - 2
                    width: Math.max(4, (modelData[1] - modelData[0]) * unit + 4)
                    y: slider.topPadding + slider.availableHeight / 2 + 4
                    height: 4
                    color: Kirigami.Theme.negativeTextColor
                }
            }
            Rectangle {
                visible: viewer.markStart >= 0
                readonly property int last: viewer.markEnd >= 0 ? viewer.markEnd : viewer.index
                readonly property real unit: (slider.availableWidth) / Math.max(1, viewer.info.count - 1)
                x: slider.leftPadding + Math.min(viewer.markStart, last) * unit - 2
                width: Math.abs(last - viewer.markStart) * unit + 4
                y: slider.topPadding + slider.availableHeight / 2 - 8
                height: 4
                color: Kirigami.Theme.highlightColor
            }
            Controls.Slider {
                id: slider
                anchors.left: parent.left
                anchors.right: parent.right
                focusPolicy: Qt.NoFocus
                from: 0
                to: Math.max(0, viewer.info.count - 1)
                stepSize: 1
                snapMode: Controls.Slider.SnapAlways
                value: viewer.index
                onMoved: viewer.index = Math.round(value)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Controls.Label {
                text: viewer.frameNoun.charAt(0).toUpperCase() + viewer.frameNoun.slice(1) + " " + (viewer.index + 1)
                      + " of " + viewer.info.count + " · " + viewer.frameTime(viewer.session, viewer.take, viewer.index)
            }
            Item { Layout.fillWidth: true }
            Controls.Button {
                focusPolicy: Qt.NoFocus
                text: viewer.markStart >= 0 ? "Start: " + (viewer.markStart + 1) : "Mark start"
                icon.name: "go-first"
                onClicked: viewer.markStart = viewer.index
            }
            Controls.Button {
                focusPolicy: Qt.NoFocus
                text: viewer.markEnd >= 0 ? "End: " + (viewer.markEnd + 1) : "Mark end"
                icon.name: "go-last"
                onClicked: viewer.markEnd = viewer.index
            }
            Controls.Button {
                focusPolicy: Qt.NoFocus
                enabled: viewer.markStart >= 0 && viewer.markEnd >= 0
                text: "Delete range"
                icon.name: "edit-cut"
                onClicked: viewer.deleteMarked()
            }
            Controls.Button {
                focusPolicy: Qt.NoFocus
                visible: viewer.markStart >= 0 || viewer.markEnd >= 0
                text: "Clear marks"
                icon.name: "edit-clear"
                onClicked: viewer.markStart = viewer.markEnd = -1
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            visible: viewer.info.ranges.length > 0
            Controls.Label { text: "Deleted:"; topPadding: Kirigami.Units.smallSpacing }
            Repeater {
                model: viewer.info.ranges
                Controls.Button {
                    required property var modelData
                    required property int index
                    focusPolicy: Qt.NoFocus
                    flat: true
                    text: (modelData[0] < 0 ? "a range with no " + viewer.frameNoun + "s"
                           : viewer.frameNoun + "s " + (modelData[0] + 1) + " to " + (modelData[1] + 1)) + ": restore"
                    icon.name: "edit-undo"
                    onClicked: backend.restoreRange(viewer.session, viewer.take, index)
                }
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            text: viewer.helpText
        }
    }
}
