// Frametop Settings' Remote PC shortcuts page: the key combinations a remote display's computer
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
    title: "Remote PC shortcuts"

    actions: [
        Kirigami.Action {
            text: "Restore defaults"
            icon.name: "edit-undo"
            tooltip: "Send Frametop's Meta shortcuts to the remote computer (except eye-tracking clicks)"
            enabled: !page.keys.isDefault
            onTriggered: page.keys.reset()
        }
    ]

    Kirigami.FormLayout {
        Ft.Hint {
            objectName: "remoteKeysIntro"
            text: "When you type on a remote PC or Mac display, Frametop shortcuts normally still control "
                  + "the headset. Any shortcut listed here is forwarded to the remote computer instead "
                  + "(either as-is, or remapped to a different key combo)."
        }
        Repeater {
            model: page.keys.rows
            delegate: RowLayout {
                required property var modelData
                Kirigami.FormData.label: modelData.pressedLabel + ":"
                ColumnLayout {
                    spacing: 0
                    Controls.Label {
                        text: modelData.same ? "Sent to remote computer as-is"
                                             : "Sends " + modelData.sentLabel + " to remote computer"
                        elide: Text.ElideRight
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                    }
                    Controls.Label {
                        visible: modelData.instead !== ""
                        text: "On headset: " + modelData.instead
                        elide: Text.ElideRight
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                    }
                }
                Controls.ToolButton {
                    icon.name: "edit-delete"
                    display: Controls.AbstractButton.IconOnly
                    text: "Remove (keep as a headset shortcut)"
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered
                    onClicked: page.keys.remove(modelData.pressed)
                }
            }
        }
        Controls.Label {
            objectName: "remoteKeysNone"
            visible: page.keys.rows.length === 0
            Kirigami.FormData.label: "Forwarded shortcuts:"
            text: "None — all Frametop shortcuts stay on the headset"
            opacity: 0.7
        }

        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Add shortcut rule" }
        RowLayout {
            Kirigami.FormData.label: "When you press:"
            Controls.Button {
                objectName: "remoteKeysPress"
                text: page.keys.capturing === "pressed" ? "Press keys now… (Cancel)" : "Record keys…"
                enabled: page.keys.capturing !== "sent"
                onClicked: page.keys.capturing === "pressed" ? page.keys.cancelCapture() : page.keys.capture("pressed")
            }
            Controls.Label { text: page.keys.pressedLabel }
        }
        RowLayout {
            Kirigami.FormData.label: "Send to remote PC:"
            enabled: page.keys.pressed !== ""
            Controls.Label { text: page.keys.sentLabel === "" ? "Same keys" : page.keys.sentLabel }
            Controls.Button {
                objectName: "remoteKeysSend"
                text: page.keys.capturing === "sent" ? "Press keys now… (Cancel)" : "Remap keys…"
                enabled: page.keys.capturing !== "pressed"
                onClicked: page.keys.capturing === "sent" ? page.keys.cancelCapture() : page.keys.capture("sent")
            }
            Controls.ToolButton {
                visible: page.keys.sentLabel !== ""
                icon.name: "edit-clear"
                display: Controls.AbstractButton.IconOnly
                text: "Reset to same keys"
                Controls.ToolTip.text: text
                Controls.ToolTip.visible: hovered
                onClicked: page.keys.sendSame()
            }
        }
        Controls.Button {
            objectName: "remoteKeysAdd"
            text: "Add rule"
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
        text: "Applies whenever your pointer is on a remote PC or Mac display. Shortcuts that Frametop doesn't "
              + "use already go straight to the remote computer; you only need to add rules here to override a "
              + "Frametop shortcut or remap one (for example, sending Meta+Space when you press Meta+Shift+Space). "
              + "Ctrl+Alt+Shift+Esc always stays on the headset to release stuck keys. Manage headset shortcuts "
              + "in Keyboard & shortcuts."
    }
}
