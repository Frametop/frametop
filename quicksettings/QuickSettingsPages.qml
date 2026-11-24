// Frametop Settings' Quick Settings page: which toggles the palm menu's Quick Settings shows
// (settings/main.qml loads this file the first time it opens). Backend: ft_quick_settings.py
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
            title: "Quick Settings"

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Hold your open palm toward your face in VR to open the palm Quick Access menu, then tap Quick Settings "
                          + "to reach these toggles. Choose which shortcuts appear there — changes apply the next time you open it."
                }

                Kirigami.FormLayout {
                    Layout.fillWidth: true
                    Repeater {
                        model: backend.toggles
                        delegate: Controls.CheckBox {
                            required property var modelData
                            Kirigami.FormData.label: modelData.label
                            text: "Show in Quick Settings"
                            checked: modelData.on
                            Accessible.name: modelData.label
                            onToggled: backend.setShown(modelData.name, checked)
                        }
                    }
                }
            }
        }
    }
}
