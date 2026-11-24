// Frametop Settings' Remote display keys page: the key combinations a remote display's computer
// gets while you type on it (the input rules' "remote_key_bindings", conf/ft_remote_keys.py), as
// they are or as other keys. Its object is remote_keys.py's RemoteKeys ("backend.remoteKeys").
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "../settings/controls" as Ft

Kirigami.ScrollablePage {
    id: page
    property QtObject backend   // InputPages.qml sets it: ft_input_settings.py
    readonly property QtObject keys: backend.remoteKeys
    title: "Remote display keys"

    actions: [
        Kirigami.Action {
            text: "Defaults"
            icon.name: "edit-undo"
            tooltip: "Frametop's own Meta combinations go to the computer, but the gaze clicks"
            enabled: !page.keys.isDefault
            onTriggered: page.keys.reset()
        }
    ]

    Kirigami.FormLayout {
        Ft.Hint {
            objectName: "remoteKeysIntro"
            text: "While you type on a remote display, Frametop's own key combinations still work: "
                  + "Meta+J clicks where you look. The ones here go to the computer instead, as they "
                  + "are or as other keys."
        }
        Repeater {
            model: page.keys.rows
            delegate: RowLayout {
                required property var modelData
                Kirigami.FormData.label: modelData.pressedLabel + ":"
                ColumnLayout {
                    spacing: 0
                    Controls.Label {
                        text: modelData.same ? "The computer gets it as it is"
                                             : "The computer gets " + modelData.sentLabel
                        elide: Text.ElideRight
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                    }
                    Controls.Label {
                        visible: modelData.instead !== ""
                        text: "Elsewhere: " + modelData.instead
                        elide: Text.ElideRight
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                    }
                }
                Controls.ToolButton {
                    icon.name: "edit-delete"
                    display: Controls.AbstractButton.IconOnly
                    text: "Remove: it stays Frametop's on a remote display"
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered
                    onClicked: page.keys.remove(modelData.pressed)
                }
            }
        }
        Controls.Label {
            objectName: "remoteKeysNone"
            visible: page.keys.rows.length === 0
            Kirigami.FormData.label: "To the computer:"
            text: "nothing: every key combination of Frametop's works on a remote display"
            opacity: 0.7
        }

        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "New" }
        RowLayout {
            Kirigami.FormData.label: "You press:"
            Controls.Button {
                objectName: "remoteKeysPress"
                text: page.keys.capturing === "pressed" ? "Press the keys… (Cancel)" : "Set keys…"
                enabled: page.keys.capturing !== "sent"
                onClicked: page.keys.capturing === "pressed" ? page.keys.cancelCapture() : page.keys.capture("pressed")
            }
            Controls.Label { text: page.keys.pressedLabel }
        }
        RowLayout {
            Kirigami.FormData.label: "The computer gets:"
            enabled: page.keys.pressed !== ""
            Controls.Label { text: page.keys.sentLabel === "" ? "The same keys" : page.keys.sentLabel }
            Controls.Button {
                objectName: "remoteKeysSend"
                text: page.keys.capturing === "sent" ? "Press the keys… (Cancel)" : "Other keys…"
                enabled: page.keys.capturing !== "pressed"
                onClicked: page.keys.capturing === "sent" ? page.keys.cancelCapture() : page.keys.capture("sent")
            }
            Controls.ToolButton {
                visible: page.keys.sentLabel !== ""
                icon.name: "edit-clear"
                display: Controls.AbstractButton.IconOnly
                text: "The same keys"
                Controls.ToolTip.text: text
                Controls.ToolTip.visible: hovered
                onClicked: page.keys.sendSame()
            }
        }
        Controls.Button {
            objectName: "remoteKeysAdd"
            text: "Add"
            icon.name: "list-add"
            enabled: page.keys.pressed !== "" && page.keys.capturing === "" && page.keys.problem === ""
            onClicked: page.keys.add()
        }
        Controls.Label {
            objectName: "remoteKeysProblem"
            visible: page.keys.problem !== ""
            text: page.keys.problem
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
            Layout.maximumWidth: Kirigami.Units.gridUnit * 26
        }
    }

    footer: Ft.HelpFooter {
        text: "This applies while the 3D mouse is on a remote display and you type there. Keys the computer gets as "
              + "they are stay down as long as you hold them. Other keys are tapped once when you press yours (a "
              + "modifier's tap: when you let it go), so Meta+Shift+Space can open the computer's launcher "
              + "(Meta+Space there) while Meta+Space stays frunner's. A combination Frametop does nothing with reaches the computer anyway; list it only to "
              + "send other keys. Hold the modifiers (Ctrl, Alt, Shift, Meta), then press the key, or tap one "
              + "modifier on its own. Ctrl+Alt+Shift+Esc always stays Frametop's: it lets go of every key, for one "
              + "stuck on the computer too. Frametop's own combinations are on the Keyboard page."
    }
}
