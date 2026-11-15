// Frametop Settings' Together page: a room shared with other Frames over your tailnet
// (settings/main.qml loads this file the first time it opens). Backend:
// ft_together_settings.py ("backend", made then: settings/ft_settings.py).
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    id: root
    // settings/main.qml sets these two.
    property QtObject backend
    property var window
    function showPassiveNotification(text, duration) { window.showPassiveNotification(text, duration) }

    function page(name) { return name === "together" ? togetherPage : null }

    readonly property bool inRoom: backend.mode === "hosting" || backend.mode === "joined"

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    // Asks only while the page shows.
    Connections {
        target: window
        function onCurrentChanged() { backend.setActive(window.current === "together") }
    }

    Kirigami.PromptDialog {
        id: closeDialog
        title: "Close the room?"
        subtitle: "It ends for everyone in it."
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: "Close the room"
                icon.name: "window-close"
                onTriggered: { closeDialog.close(); backend.leave() }
            },
            Kirigami.Action {
                text: "Cancel"
                icon.name: "dialog-cancel"
                onTriggered: closeDialog.close()
            }
        ]
    }

    // Asked once of a model that can't say itself whether it may be passed on (a VRM's licence
    // does): its file goes to the room only after a yes here.
    Kirigami.PromptDialog {
        id: allowDialog
        objectName: "allowDialog"
        title: "Let the others have your avatar?"
        subtitle: "The people you share a room with get a copy of its file, to see you in it. "
                  + "Say yes only for a model that is yours to pass on: one you made, or one whose licence allows it."
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                objectName: "allowAction"
                text: "Let them have it"
                icon.name: "dialog-ok"
                onTriggered: { allowDialog.close(); backend.allowAvatar() }
            },
            Kirigami.Action {
                text: "Cancel"
                icon.name: "dialog-cancel"
                onTriggered: allowDialog.close()
            }
        ]
    }

    Component {
        id: togetherPage
        Kirigami.ScrollablePage {
            title: "Frametop Together"

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Share a room with up to three other Steam Frames on your tailnet: you see each other, "
                          + "talk, and look at the screens each of you chooses to share. Nothing is shared until "
                          + "you say so, and nobody comes in until you let them."
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !backend.installed
                    type: Kirigami.MessageType.Information
                    text: "Frametop Together isn't installed on this headset. Install it with together/run.sh install."
                }

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    Controls.Switch {
                        objectName: "togetherSwitch"
                        Kirigami.FormData.label: "Frametop Together:"
                        text: backend.status
                        checked: backend.on
                        enabled: backend.installed && !backend.busy
                        onToggled: {
                            backend.setOn(checked)
                            checked = Qt.binding(() => backend.on)
                        }
                    }
                    RowLayout {
                        Kirigami.FormData.label: "This headset:"
                        visible: backend.here !== ""
                        Controls.Label {
                            objectName: "hereLabel"
                            text: backend.here
                            textFormat: Text.PlainText
                            font.family: "monospace"
                        }
                        Controls.ToolButton {
                            icon.name: "edit-copy"
                            display: Controls.AbstractButton.IconOnly
                            text: "Copy the address"
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: backend.copy(backend.here)
                        }
                    }
                }

                // --- the room ---
                Kirigami.Heading {
                    level: 3
                    visible: backend.ready
                    text: "The room"
                }
                Controls.Label {
                    objectName: "roomLabel"
                    Layout.fillWidth: true
                    visible: backend.ready
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    text: backend.roomText
                }
                Controls.Label {
                    objectName: "noteLabel"
                    Layout.fillWidth: true
                    visible: backend.ready && backend.note !== ""
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    opacity: 0.7
                    text: backend.note
                }

                // in no room: open one, or join someone's
                Kirigami.FormLayout {
                    Layout.fillWidth: true
                    visible: backend.ready && backend.mode === "idle"

                    Controls.Button {
                        objectName: "openButton"
                        Kirigami.FormData.label: "Your own room:"
                        text: "Open a room"
                        icon.name: "list-add"
                        onClicked: backend.openRoom()
                    }
                    Kirigami.Separator {
                        Kirigami.FormData.isSection: true
                        Kirigami.FormData.label: "Join someone's room"
                    }
                    Controls.TextField {
                        id: joinAddress
                        objectName: "joinAddress"
                        Kirigami.FormData.label: "Their headset:"
                        placeholderText: "its tailnet name or number"
                        maximumLength: 260
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                    }
                    Controls.TextField {
                        id: joinCode
                        objectName: "joinCode"
                        Kirigami.FormData.label: "Invite code:"
                        maximumLength: 40
                        font.family: "monospace"
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                        onAccepted: backend.join(joinAddress.text, joinCode.text)
                    }
                    Controls.Button {
                        objectName: "joinButton"
                        text: "Ask to come in"
                        icon.name: "go-next"
                        enabled: joinAddress.text !== "" && joinCode.text !== ""
                        onClicked: backend.join(joinAddress.text, joinCode.text)
                    }
                }

                Controls.Button {
                    objectName: "cancelButton"
                    visible: backend.ready && backend.mode === "calling"
                    text: "Stop asking"
                    icon.name: "dialog-cancel"
                    onClicked: backend.leave()
                }

                // hosting: invites, and those who ask to come in
                Kirigami.FormLayout {
                    Layout.fillWidth: true
                    visible: backend.ready && backend.mode === "hosting"

                    RowLayout {
                        Kirigami.FormData.label: "Invite someone:"
                        Controls.Button {
                            objectName: "inviteButton"
                            text: backend.code === "" ? "Make an invite code" : "Make another"
                            icon.name: "list-add-user"
                            onClicked: backend.invite()
                        }
                        Controls.Label {
                            objectName: "codeLabel"
                            visible: backend.code !== ""
                            text: backend.code
                            textFormat: Text.PlainText
                            font.family: "monospace"
                            font.bold: true
                        }
                        Controls.ToolButton {
                            visible: backend.code !== ""
                            icon.name: "edit-copy"
                            display: Controls.AbstractButton.IconOnly
                            text: "Copy the code"
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: backend.copy(backend.code)
                        }
                    }
                    Controls.Label {
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        text: "Give them this headset's address and the code. A code lets one person ask, once; "
                              + "you still say yes or no here."
                    }
                }
                Repeater {
                    objectName: "asks"
                    model: backend.ready ? backend.asks : []
                    // Not an InlineMessage: its label reads markup, and the name is one the
                    // asker chose (together/README.md: shown as plain text).
                    delegate: RowLayout {
                        id: askCard
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        Kirigami.Icon {
                            source: "dialog-warning"
                            implicitWidth: Kirigami.Units.iconSizes.small
                            implicitHeight: Kirigami.Units.iconSizes.small
                        }
                        Controls.Label {
                            objectName: "askText"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: (askCard.modelData.name || askCard.modelData.address) + " asks to come in"
                                  + (askCard.modelData.login ? " (" + askCard.modelData.login + ")" : "")
                        }
                        Controls.Button {
                            objectName: "askLetIn"
                            text: "Let in"
                            icon.name: "dialog-ok"
                            onClicked: backend.accept(askCard.modelData.id)
                        }
                        Controls.Button {
                            objectName: "askNo"
                            text: "No"
                            icon.name: "dialog-cancel"
                            onClicked: backend.decline(askCard.modelData.id)
                        }
                    }
                }

                // in a room: who is in it
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: backend.ready && root.inRoom
                    spacing: Kirigami.Units.smallSpacing
                    Repeater {
                        objectName: "people"
                        model: backend.people
                        delegate: RowLayout {
                            id: personRow
                            required property var modelData
                            spacing: Kirigami.Units.smallSpacing
                            Kirigami.Icon {
                                source: "user-identity"
                                implicitWidth: Kirigami.Units.iconSizes.small
                                implicitHeight: Kirigami.Units.iconSizes.small
                            }
                            Controls.Label {
                                text: personRow.modelData.name
                                textFormat: Text.PlainText
                            }
                            Controls.Label {
                                visible: personRow.modelData.what !== ""
                                text: personRow.modelData.what
                                textFormat: Text.PlainText
                                opacity: 0.7
                            }
                            Controls.ToolButton {
                                objectName: "goPerson"
                                visible: !personRow.modelData.you
                                text: "Go to them"
                                icon.name: "go-jump"
                                onClicked: backend.goTo("person", personRow.modelData.id)
                            }
                        }
                    }
                    Controls.Button {
                        objectName: "leaveButton"
                        Layout.topMargin: Kirigami.Units.smallSpacing
                        text: backend.mode === "hosting" ? "Close the room…" : "Leave the room"
                        icon.name: "window-close"
                        onClicked: backend.mode === "hosting" && backend.people.length > 1 ? closeDialog.open() : backend.leave()
                    }
                }

                RoomPanels {
                    objectName: "roomPanelRows"
                    Layout.fillWidth: true
                    visible: backend.ready && root.inRoom && any
                    backend: root.backend
                }

                // --- what the room gets of yours ---
                YourShares {
                    Layout.fillWidth: true
                    visible: backend.ready && root.inRoom
                    backend: root.backend
                    onAllowAvatar: allowDialog.open()
                }
            }
        }
    }
}
