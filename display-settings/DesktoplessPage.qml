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
    property bool md: backend.backend === "screens"

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !dpage.md
            type: Kirigami.MessageType.Information
            text: "Desktopless mode needs the screens backend (ft-screens)."
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
                        text: "The desktops go, their apps stay: every app window from the sources below becomes a panel of its own, and their screens or displays hide. Turned off, each window goes back where it was. Meta+Shift+D or the launcher does the same."
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

        Kirigami.Heading { level: 3; text: "What's in it"; enabled: dpage.md }
        Controls.Label {
            Layout.fillWidth: true
            enabled: dpage.md
            text: "A source that's out keeps its desktop: the Frame its screens, a PC its display panels. PCs are out by default, since one with many windows open would make too many panels. While the mode is on, a switch applies right away."
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
                                  ? (source.modelData.on ? "In: its screens hide and its windows float as panels."
                                                         : "Out: its screens and windows stay as they are.")
                                  : !source.modelData.windows
                                  ? "Out: first turn on \"Show its apps and windows in the launcher\" on its card in Remote displays."
                                  : (source.modelData.on ? "In: its displays hide and each of its windows is a panel."
                                                         : "Out: its displays stay as display panels.")
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
        Controls.Label {
            visible: backend.desktoplessSources.length === 1
            Layout.fillWidth: true
            text: "No PCs yet: add one in Remote displays."
            opacity: 0.7
            wrapMode: Text.Wrap
        }
    }
}
