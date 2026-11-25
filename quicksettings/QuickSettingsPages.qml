// Frametop Settings' Quick Settings page: the palm menu's tiles, what each says and runs, and which
// show (settings/main.qml loads this file the first time it opens). Backend: ft_quick_settings.py
// ("backend", made then: settings/ft_settings.py). A row is PalmTileRow.qml.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "../settings/controls" as Ft

Item {
    id: root
    // settings/main.qml sets these two.
    property QtObject backend
    property var window
    function showPassiveNotification(text, duration) { window.showPassiveNotification(text, duration) }

    function page(name) { return name === "quick-settings" ? quickPage : null }

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    Component {
        id: quickPage
        Kirigami.ScrollablePage {
            id: qpage
            title: "Quick Settings"
            // Read once per change: the rows bind to their own part of these, so a change keeps the
            // rows (and the keyboard focus) it doesn't concern.
            readonly property var groups: backend.groups
            readonly property var choices: backend.actions
            readonly property var newChoices: backend.newActions
            onVisibleChanged: if (visible) backend.refresh()
            actions: [
                Kirigami.Action {
                    text: "Custom commands"
                    icon.name: "utilities-terminal"
                    tooltip: "Make a command of your own for a tile to run"
                    onTriggered: root.window.showSection("commands")
                },
                Kirigami.Action {
                    text: "Restore defaults"
                    icon.name: "edit-reset"
                    onTriggered: restore.open()
                }
            ]

            Kirigami.PromptDialog {
                id: restore
                title: "Restore the palm menu?"
                subtitle: "Every tile goes back to the one Frametop comes with. Your custom commands stay."
                standardButtons: Kirigami.Dialog.Cancel
                customFooterActions: [
                    Kirigami.Action {
                        text: "Restore defaults"
                        icon.name: "edit-reset"
                        onTriggered: {
                            restore.close()
                            backend.restoreDefaults()
                        }
                    }
                ]
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Hold your open palm toward your face in VR to open the palm menu. Here you choose its tiles: "
                          + "which ones show, what each one says, and the Frametop action, profile or custom command it runs. "
                          + "Changes apply the next time you open it."
                }

                Repeater {
                    model: qpage.groups.length
                    delegate: ColumnLayout {
                        id: group
                        required property int index
                        readonly property var info: qpage.groups[index]
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        Kirigami.Heading {
                            Layout.topMargin: Kirigami.Units.largeSpacing
                            level: 3
                            text: group.info.fixed ? group.info.title
                                                   : group.info.title + " (" + group.info.shown + " of " + group.info.max + " shown)"
                        }
                        Ft.Hint {
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 40
                            text: group.info.help
                        }
                        Repeater {
                            model: group.info.tiles.length
                            delegate: PalmTileRow {
                                Layout.fillWidth: true
                                backend: root.backend
                                choices: qpage.choices
                                fixed: group.info.fixed
                                page: group.info.page
                                tile: group.info.tiles[index] || blank
                            }
                        }
                        RowLayout {
                            visible: !group.info.fixed
                            Layout.fillWidth: true
                            Controls.ComboBox {
                                id: fresh
                                Layout.fillWidth: true
                                Layout.maximumWidth: Kirigami.Units.gridUnit * 18
                                model: qpage.newChoices
                                textRole: "text"
                                valueRole: "value"
                                wheelEnabled: false
                                Accessible.name: "New tile's action"
                            }
                            Controls.Button {
                                text: "Add tile"
                                icon.name: "list-add"
                                enabled: fresh.currentIndex >= 0
                                onClicked: backend.add(group.info.page, fresh.currentValue)
                            }
                        }
                    }
                }
            }

            footer: Ft.HelpFooter {
                text: "To run something of your own, save it under Custom commands, then pick \"Run command NAME\" here. "
                      + "A page shows up to six tiles; hidden ones stay in this list. Clear a tile's text to get the usual words back. "
                      + "A tile called Hide Screens says Show Screens while they're hidden."
            }
        }
    }
}
