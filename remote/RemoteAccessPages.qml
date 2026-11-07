// Frametop Settings' Remote access page: VNC access to the Frametop desktop over the tailnet
// (settings/main.qml loads this file the first time it opens). Backend: remote_access.py
// ("backend", made then: settings/ft_settings.py).
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

    function page(name) { return name === "remote-access" ? accessPage : null }

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    // Checks only while the page shows (each one runs remote-ctl.sh on the host).
    Connections {
        target: window
        function onCurrentChanged() { backend.setActive(window.current === "remote-access") }
    }

    Kirigami.PromptDialog {
        id: changeDialog
        title: "Change the VNC password?"
        subtitle: "Viewers connected now are disconnected and need the new password."
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: "Change"
                icon.name: "view-refresh"
                onTriggered: { changeDialog.close(); backend.changePassword() }
            },
            Kirigami.Action {
                text: "Cancel"
                icon.name: "dialog-cancel"
                onTriggered: changeDialog.close()
            }
        ]
    }

    Component {
        id: accessPage
        Kirigami.ScrollablePage {
            title: "Remote access"

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "See and use the Frametop desktop from another computer on your tailnet, with any VNC "
                          + "viewer (RealVNC Viewer, TigerVNC, or macOS Screen Sharing). It shows the primary "
                          + "screen, the one with the taskbar, and follows it when you change the layout."
                }

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    Controls.Switch {
                        objectName: "remoteSwitch"
                        Kirigami.FormData.label: "Remote access over VNC:"
                        text: backend.state
                        checked: backend.enabled
                        enabled: !backend.busy
                        onToggled: backend.setEnabled(checked)
                    }
                    RowLayout {
                        Kirigami.FormData.label: "Address:"
                        Controls.Label {
                            text: backend.address
                            textFormat: Text.PlainText
                        }
                        Controls.ToolButton {
                            icon.name: "edit-copy"
                            display: Controls.AbstractButton.IconOnly
                            text: "Copy the address"
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: backend.copyAddress()
                        }
                    }

                    Kirigami.Separator {
                        Kirigami.FormData.isSection: true
                        Kirigami.FormData.label: "Password"
                    }
                    RowLayout {
                        Kirigami.FormData.label: "VNC password:"
                        Controls.Label {
                            text: backend.password
                            textFormat: Text.PlainText
                            font.family: backend.revealed ? "monospace" : Kirigami.Theme.defaultFont.family
                        }
                        Controls.ToolButton {
                            visible: backend.hasPassword
                            icon.name: backend.revealed ? "view-hidden" : "view-visible"
                            display: Controls.AbstractButton.IconOnly
                            text: backend.revealed ? "Hide" : "Show"
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: backend.setRevealed(!backend.revealed)
                        }
                        Controls.ToolButton {
                            icon.name: "edit-copy"
                            display: Controls.AbstractButton.IconOnly
                            text: "Copy the password"
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: backend.copyPassword()
                        }
                    }
                    Controls.Button {
                        Kirigami.FormData.label: "New password:"
                        text: "Change…"
                        enabled: !backend.busy
                        onClicked: changeDialog.open()
                    }
                    Controls.Label {
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        text: "VNC takes at most 8 characters. The connection itself is encrypted by the tailnet, "
                              + "and only devices on it can reach the Frame."
                    }
                }

                Kirigami.Heading {
                    level: 3
                    text: "How it works"
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.8
                    text: "KDE's krdp captures the desktop on 127.0.0.1 only, and a VNC server on the tailnet "
                          + "address shows its primary screen. Turning it on in a desktop that started with it off "
                          + "takes a desktop restart (KWin allows the capture only from its start). Logs: "
                          + "/tmp/frametop-remote.log and /tmp/frametop-vnc.log."
                }
            }
        }
    }
}
