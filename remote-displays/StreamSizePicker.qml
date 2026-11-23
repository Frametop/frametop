// A display panel's stream size. For a monitor of a PC whose monitors follow its panels, a list
// of the sizes that monitor lists (it takes each of them), with a "Custom size" switch that
// shows the width and height boxes instead: a size the monitor doesn't list is one its PC has
// to make, which some monitors and drivers refuse. Every other display has only the boxes: its
// stream crops or fits it, or it's made in that size. RemoteDisplayPages.qml puts one in each
// display's Stream row, reads width and height for Apply, and keeps `custom` across the list
// being made again.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

RowLayout {
    id: picker
    property var sizes: []            // the sizes its monitor lists, [[width, height]], largest first; none: boxes only
    property var largest: null        // the most the boxes go to, [width, height], or null: any size
    property int currentWidth: 0      // the panel's stream size now
    property int currentHeight: 0
    property var custom: undefined    // the switch as the user left it; undefined: on if its size isn't a listed one
    signal customPicked(bool on)

    readonly property bool lists: sizes.length > 0
    // Where the panel's size is in the list (-1: it isn't a listed one). A function, for the
    // list's handlers: they run before a property made from sizes has its new value.
    function listedIndex() {
        return sizes.findIndex(s => s[0] === currentWidth && s[1] === currentHeight)
    }
    readonly property int listedAt: listedIndex()
    // The boxes show, not the list
    readonly property bool typed: !lists || (custom === undefined ? listedAt < 0 : custom === true)
    // The size picked here (the panel's own until one is)
    readonly property int pickedWidth: typed ? widthBox.value : box.currentIndex >= 0 ? sizes[box.currentIndex][0] : currentWidth
    readonly property int pickedHeight: typed ? heightBox.value : box.currentIndex >= 0 ? sizes[box.currentIndex][1] : currentHeight
    readonly property bool changed: pickedWidth !== currentWidth || pickedHeight !== currentHeight

    Controls.ComboBox {
        id: box
        objectName: "streamSize"
        visible: picker.lists && !picker.typed
        model: picker.sizes.map(s => s[0] + " × " + s[1])
        // The list is made again when the PC answers: back to the panel's size then.
        onModelChanged: currentIndex = picker.listedIndex()
        Component.onCompleted: currentIndex = picker.listedIndex()
        displayText: currentIndex < 0 ? "Pick a size" : currentText
    }
    Controls.SpinBox {
        id: widthBox
        objectName: "streamWidth"
        visible: picker.typed
        from: 640; to: picker.largest ? Math.max(640, picker.largest[0]) : 7680
        stepSize: 8; editable: true
        value: picker.currentWidth
    }
    Controls.Label { visible: picker.typed; text: "×" }
    Controls.SpinBox {
        id: heightBox
        objectName: "streamHeight"
        visible: picker.typed
        from: 360; to: picker.largest ? Math.max(360, picker.largest[1]) : 4320
        stepSize: 8; editable: true
        value: picker.currentHeight
    }
    Controls.Switch {
        objectName: "streamCustom"
        visible: picker.lists
        text: "Custom size"
        checked: picker.typed
        onToggled: picker.customPicked(checked)
    }
}
