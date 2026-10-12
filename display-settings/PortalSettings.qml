// Native portal content, also testable without the surrounding Kirigami window.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

ColumnLayout {
    id: page
    required property var model
    property var portal: model.portal
    property var prefs: model.preferences
    spacing: 12

    Controls.GroupBox {
        title: "Desktop passthrough"
        Layout.fillWidth: true
        ColumnLayout {
            anchors.fill: parent
            spacing: 10
            Controls.Switch {
                id: portalSwitch
                objectName: "cameraPassthroughEnabled"
                text: "Desk passthrough"
                Binding { target: portalSwitch; property: "checked"; value: page.portal.requested === true }
                onToggled: page.model.setWanted(checked)
            }
            Controls.Label {
                objectName: "cameraPassthroughStatus"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: page.portal.summary
            }
            Controls.Switch {
                id: controlsSwitch
                objectName: "passthroughControls"
                text: "Show placement controls"
                Binding {
                    target: controlsSwitch; property: "checked"
                    value: page.prefs.controls.known ? page.prefs.controls.enabled : page.prefs.controls.saved
                }
                onToggled: page.model.setControls(checked)
            }
            Controls.Label {
                objectName: "passthroughControlsStatus"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: page.prefs.controls.summary
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Move and rotate the rectangle with its grab bar. Drag an edge handle to resize it. "
                    + "Showing placement controls does not turn passthrough on."
            }
            Controls.Button {
                objectName: "passthroughPlace"
                text: "Place at desk"
                onClicked: page.model.placeAtDesk()
            }
            Controls.Label {
                objectName: "passthroughGeometryStatus"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: page.model.geometrySummary
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Place at desk preserves the rectangle's size and tilt. Placement never turns passthrough on."
            }
        }
    }
    Controls.Switch {
        id: layerCheck
        objectName: "portalAboveSteam"
        text: "Passthrough above Steam menus"
        Binding {
            target: layerCheck; property: "checked"
            value: page.prefs.layer.known ? page.prefs.layer.enabled : page.prefs.layer.saved
        }
        onToggled: page.model.setAboveSteam(checked)
    }
    Controls.Label {
        objectName: "portalLayerStatus"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: page.prefs.layer.summary
    }
}
