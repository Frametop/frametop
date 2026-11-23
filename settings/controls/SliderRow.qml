// A slider with its value beside it, and a button back to the default when there is one.
//   SliderRow { from: 0; to: 50; stepSize: 0.1; value: backend.x; onMoved: backend.setX(value)
//               shown: value.toFixed(1) + " cm" }
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: row
    property alias from: slider.from
    property alias to: slider.to
    property alias stepSize: slider.stepSize
    property alias value: slider.value
    property alias snapMode: slider.snapMode
    // The value as text, with its unit.
    property string shown: ""
    // The default the reset button goes back to; no button without one.
    property var reset: undefined
    property string resetText: reset === undefined ? "" : "Default (" + reset + ")"
    property string tip: ""
    property real sliderWidth: Kirigami.Units.gridUnit * 12
    // The user moved it (or reset it): value is the new one.
    signal moved()

    Controls.Slider {
        id: slider
        Layout.preferredWidth: row.sliderWidth
        onMoved: row.moved()
        Controls.ToolTip.visible: row.tip !== "" && hovered
        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
        Controls.ToolTip.text: row.tip
    }
    Controls.Label {
        text: row.shown
        Layout.minimumWidth: Kirigami.Units.gridUnit * 4
    }
    Controls.ToolButton {
        visible: row.reset !== undefined
        icon.name: "edit-undo"
        display: Controls.AbstractButton.IconOnly
        text: row.resetText
        Controls.ToolTip.text: text
        Controls.ToolTip.visible: hovered
        onClicked: { slider.value = row.reset; row.moved() }
    }
}
