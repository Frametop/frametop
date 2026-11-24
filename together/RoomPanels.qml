// The room's panels on Frametop Settings' Together page (TogetherPages.qml uses it): who asks to
// use which of your screens, who can, and every panel in the room, to ask for or to go to.
// Backend: the page's own, for its `control` (ft_together_control.py) and its goTo.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: rows
    required property QtObject backend
    readonly property QtObject control: backend.control
    readonly property bool any: control.asks.length + control.given.length + control.panels.length > 0
    spacing: Kirigami.Units.smallSpacing

    Kirigami.Heading {
        level: 3
        text: "Panels in the room"
    }
    Repeater {
        objectName: "controlAsks"
        model: rows.control.asks
        delegate: ColumnLayout {
            id: askRow
            required property var modelData
            spacing: Kirigami.Units.smallSpacing
            RowLayout {
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Icon {
                    source: "dialog-question"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                }
                Controls.Label {
                    objectName: "askLabel"
                    text: askRow.modelData.text
                    textFormat: Text.PlainText
                }
                Controls.Button {
                    objectName: "giveButton"
                    // (a window of your PC is given at the second press: askSecond, below)
                    text: askRow.modelData.second !== "" ? "Yes, give it" : "Give it"
                    icon.name: "dialog-ok"
                    onClicked: rows.control.give(askRow.modelData.number, askRow.modelData.person)
                }
                Controls.Button {
                    objectName: "refuseButton"
                    text: "No"
                    icon.name: "dialog-cancel"
                    onClicked: rows.control.take(askRow.modelData.number, askRow.modelData.person)
                }
            }
            Controls.Label {
                objectName: "askSecond"
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                visible: text !== ""
                wrapMode: Text.Wrap
                text: askRow.modelData.second
                textFormat: Text.PlainText
            }
        }
    }
    Controls.Label {
        objectName: "controlNote"
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        opacity: 0.7
        text: rows.control.note
    }
    Repeater {
        objectName: "controlGiven"
        model: rows.control.given
        delegate: RowLayout {
            id: givenRow
            required property var modelData
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Icon {
                source: "input-mouse"
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
            }
            Controls.Label {
                objectName: "givenLabel"
                text: givenRow.modelData.text
                textFormat: Text.PlainText
            }
            Controls.Button {
                objectName: "takeButton"
                text: "Revoke control"
                icon.name: "edit-undo"
                onClicked: rows.control.take(givenRow.modelData.number, givenRow.modelData.person)
            }
        }
    }
    Repeater {
        objectName: "roomPanels"
        model: rows.control.panels
        delegate: RowLayout {
            id: panelRow
            required property var modelData
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Icon {
                source: "video-display"
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
            }
            Controls.Label {
                objectName: "panelLabel"
                text: panelRow.modelData.title
                textFormat: Text.PlainText
            }
            Controls.Label {
                visible: panelRow.modelData.what !== ""
                text: panelRow.modelData.what
                opacity: 0.7
            }
            Controls.Button {
                objectName: "askButton"
                visible: panelRow.modelData.ask
                text: "Request control"
                onClicked: rows.control.ask(panelRow.modelData.id)
            }
            Controls.ToolButton {
                objectName: "goPanel"
                text: "Focus"
                icon.name: "go-jump"
                onClicked: rows.backend.goTo("panel", panelRow.modelData.id)
            }
        }
    }
    Controls.Label {
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        wrapMode: Text.Wrap
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        text: "Anyone you grant control to can click, type, and move that screen while you wear the headset "
              + "(or control that remote PC for a shared PC window). The room host can reposition any panel."
    }
}
