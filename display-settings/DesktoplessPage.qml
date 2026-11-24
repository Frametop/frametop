// Frametop Settings' Desktopless mode page (docs/desktopless.md): the master switch, then each
// source's: this Frame (in by default) and each PC (out by default: its displays stay, so a PC with many
// windows doesn't flood the Frame). DisplayPages.qml makes it; its backend is ft_display_settings.py.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: dpage
    property QtObject backend   // DisplayPages.qml sets it: ft_display_settings.py
    title: "Desktopless mode"
    signal openSection(string name)
    property bool md: backend.backend === "screens"

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !dpage.md
            type: Kirigami.MessageType.Information
            text: "Desktopless mode requires the Frametop multi-screen desktop (not gamescope)."
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            enabled: dpage.md
            contentItem: RowLayout {
                ColumnLayout {
                    Layout.fillWidth: true
                    Kirigami.Heading { level: 2; text: "Desktopless mode" }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: "Float each open app window as its own VR panel and hide the virtual desktop screens. Turn this off to put every window back on its screen. You can also toggle it with Meta+Shift+D or from the launcher."
                        opacity: 0.7
                        wrapMode: Text.Wrap
                    }
                }
                Controls.Switch {
                    checked: backend.desktopless
                    enabled: backend.desktopRunning && !backend.busy
                    onToggled: {
                        backend.setDesktopless(checked)
                        checked = Qt.binding(() => backend.desktopless)
                    }
                }
            }
        }

        Kirigami.Heading { level: 3; text: "Included devices"; enabled: dpage.md }
        Controls.Label {
            Layout.fillWidth: true
            enabled: dpage.md
            text: "Choose which devices float individual app windows in Desktopless mode. Devices turned off keep their normal desktop screens. Remote PCs are off by default so a busy PC doesn't flood your space with panels. Changes apply right away while Desktopless mode is on."
            opacity: 0.7
            wrapMode: Text.Wrap
        }

        Repeater {
            model: backend.desktoplessSources
            delegate: Kirigami.AbstractCard {
                id: source
                required property var modelData
                Layout.fillWidth: true
                enabled: dpage.md
                contentItem: RowLayout {
                    Kirigami.Icon {
                        source: source.modelData.name ? "network-workgroup" : "video-display"
                        implicitWidth: Kirigami.Units.iconSizes.medium
                        implicitHeight: implicitWidth
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Controls.Label { text: source.modelData.title; font.bold: true }
                        Controls.Label {
                            Layout.fillWidth: true
                            text: !source.modelData.name
                                  ? (source.modelData.on ? "Included: virtual screens hide and each app floats as its own panel."
                                                         : "Not included: virtual screens and windows stay as normal.")
                                  : !source.modelData.windows
                                  ? "Not ready: enable \"Show apps and windows in the launcher\" for this computer in Remote displays first."
                                  : (source.modelData.on ? "Included: remote monitors hide and each app window floats as its own panel."
                                                         : "Not included: remote monitors stay as normal display panels.")
                            opacity: 0.7
                            wrapMode: Text.Wrap
                        }
                    }
                    Controls.Switch {
                        checked: source.modelData.on
                        enabled: source.modelData.windows && !backend.busy
                        onToggled: {
                            backend.setDesktoplessSource(source.modelData.name, checked)
                            checked = Qt.binding(() => source.modelData.on)
                        }
                    }
                }
            }
        }
        RowLayout {
            visible: backend.desktoplessSources.length === 1
            Layout.fillWidth: true
            Controls.Label {
                Layout.fillWidth: true
                text: "No remote PCs added yet."
                opacity: 0.7
                wrapMode: Text.Wrap
            }
            Controls.Button {
                text: "Open Remote displays"
                icon.name: "network-workgroup"
                onClicked: dpage.openSection("remote-displays")
            }
        }
    }
}
