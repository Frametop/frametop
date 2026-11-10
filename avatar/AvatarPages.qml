// Frametop Settings' Appearance page: the hand model your hands are drawn with, its look,
// importing a model of your own, and the avatar you wear and its mirror (settings/main.qml loads
// this file the first time it opens).
// Backend: ft_avatar_settings.py ("backend", made then: settings/ft_settings.py).
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    id: root
    // settings/main.qml sets these two.
    property QtObject backend
    property var window
    function showPassiveNotification(text, duration) { window.showPassiveNotification(text, duration) }

    function page(name) { return name === "appearance" ? appearancePage : null }

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    FileDialog {
        id: importDialog
        title: "A model to import"
        nameFilters: [backend.sourceFilter]
        // Whether it may be passed on is asked when it's first shared, not here: a VRM says itself.
        onAccepted: backend.importFile(selectedFile.toString(), false)
    }

    Kirigami.PromptDialog {
        id: removeDialog
        property string name: ""
        property string label: ""
        title: "Remove " + label + "?"
        subtitle: "Its file in your models folder is deleted. The file you imported it from stays."
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: "Remove"
                icon.name: "edit-delete"
                onTriggered: { removeDialog.close(); backend.remove(removeDialog.name) }
            },
            Kirigami.Action {
                text: "Cancel"
                icon.name: "dialog-cancel"
                onTriggered: removeDialog.close()
            }
        ]
    }

    function askRemove(model) {
        removeDialog.name = model.name
        removeDialog.label = model.title
        removeDialog.open()
    }

    // A model's card: its picture, its name and where it came from. One of yours can be removed.
    component ModelCard: Controls.ItemDelegate {
        id: card
        required property var modelData
        signal removeAsked()
        width: Kirigami.Units.gridUnit * 8
        height: Kirigami.Units.gridUnit * 9
        highlighted: modelData.current
        Accessible.name: modelData.title
        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Image {
                    anchors.fill: parent
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    source: card.modelData.thumb
                    visible: card.modelData.thumb !== ""
                }
                Kirigami.Icon {
                    anchors.centerIn: parent
                    width: Kirigami.Units.iconSizes.large
                    height: width
                    source: "transform-browse"
                    opacity: 0.4
                    visible: card.modelData.thumb === ""
                }
                Controls.ToolButton {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    visible: card.modelData.own
                    icon.name: "edit-delete"
                    display: Controls.AbstractButton.IconOnly
                    text: "Remove " + card.modelData.title
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered
                    onClicked: card.removeAsked()
                }
            }
            Controls.Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: card.modelData.title
                font.bold: card.modelData.current
            }
            Controls.Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: card.modelData.detail
                visible: text !== ""
                opacity: 0.7
                font: Kirigami.Theme.smallFont
            }
        }
    }

    Component {
        id: appearancePage
        Kirigami.ScrollablePage {
            title: "Appearance"
            // The models are read each time the page opens (ft-handview reads your folder again).
            Component.onCompleted: backend.refresh()

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "The hands Frametop draws where your tracked hands are: over games, over the void, and "
                          + "over your real hands in Room View. Pick a model, or import one of your own."
                }

                Kirigami.Heading {
                    level: 3
                    text: "Hand model"
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    Repeater {
                        objectName: "handModels"
                        model: backend.models
                        delegate: ModelCard {
                            objectName: "handModel-" + modelData.name
                            onClicked: backend.choose(modelData.name)
                            onRemoveAsked: root.askRemove(modelData)
                        }
                    }
                }
                RowLayout {
                    spacing: Kirigami.Units.largeSpacing
                    Controls.Button {
                        objectName: "importButton"
                        text: "Import a model…"
                        icon.name: "document-import"
                        enabled: backend.canImport && backend.busy === ""
                        onClicked: importDialog.open()
                    }
                    Controls.BusyIndicator {
                        visible: backend.busy !== ""
                        running: visible
                        implicitHeight: Kirigami.Units.iconSizes.smallMedium
                        implicitWidth: implicitHeight
                    }
                    Controls.Label {
                        text: backend.busy
                        textFormat: Text.PlainText
                        visible: text !== ""
                    }
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                    textFormat: Text.PlainText
                    text: "Takes a VRM avatar, or a glTF, GLB or FBX model with a hand skeleton: its hands become "
                          + "yours. A model with more detail than a hand needs is reduced, and one whose hand bones "
                          + "can't be told is refused with the reason. Your models are kept in " + backend.folder + "."
                }

                Kirigami.Heading {
                    level: 3
                    text: "Avatar"
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    text: backend.avatars.length > 0
                          ? "The body you wear. Its hands become yours, and the mirror shows all of it, moving as you do."
                          : "None yet. Import a model that is a whole person (a VRM avatar, or a glTF or FBX with a "
                            + "humanoid skeleton) to wear its body, not only its hands."
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    visible: backend.avatars.length > 0

                    Repeater {
                        objectName: "avatars"
                        model: backend.avatars
                        delegate: ModelCard {
                            objectName: "avatar-" + modelData.name
                            onClicked: backend.wear(modelData.name)
                            onRemoveAsked: root.askRemove(modelData)
                        }
                    }
                }
                Controls.Switch {
                    id: mirrorSwitch
                    objectName: "mirrorSwitch"
                    text: "Show a mirror"
                    visible: backend.avatars.length > 0
                    enabled: backend.canMirror && backend.avatar !== ""
                    checked: backend.mirror
                    onToggled: backend.setMirror(checked)
                    // a toggle ends the binding above, and the mirror can go on its own (it didn't come up)
                    Connections {
                        target: backend
                        function onChanged() { mirrorSwitch.checked = backend.mirror }
                    }
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                    visible: backend.avatars.length > 0
                    text: backend.avatar === ""
                          ? "Pick an avatar to see yourself in the mirror."
                          : "The mirror stands on the floor in front of where you look when you turn it on, and "
                            + "stays until you turn it off here. Turn it off and on to move it."
                }

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    Kirigami.Separator {
                        Kirigami.FormData.isSection: true
                        Kirigami.FormData.label: "Look"
                    }
                    Controls.ComboBox {
                        id: styleBox
                        objectName: "styleBox"
                        Kirigami.FormData.label: "Style:"
                        model: backend.styles
                        textRole: "text"
                        valueRole: "value"
                        implicitContentWidthPolicy: Controls.ComboBox.WidestText
                        Component.onCompleted: currentIndex = indexOfValue(backend.style)
                        onActivated: backend.setStyle(currentValue)
                        Connections {
                            target: backend
                            function onChanged() { styleBox.currentIndex = styleBox.indexOfValue(backend.style) }
                        }
                    }
                    Controls.Switch {
                        objectName: "inRoomSwitch"
                        Kirigami.FormData.label: "In Room View:"
                        text: "Draw them over your real hands too"
                        checked: backend.inRoom
                        onToggled: backend.setInRoom(checked)
                    }
                    Controls.Label {
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        text: "Off, the drawn hands show only where the room doesn't: in games and over the void."
                    }
                }
            }
        }
    }
}
