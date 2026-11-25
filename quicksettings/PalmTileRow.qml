// One of the palm menu's tiles in Frametop Settings (QuickSettingsPages.qml): shown or not, its words,
// what it runs, its place, and taking it out.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: row
    required property int index
    required property QtObject backend
    required property var choices   // backend.actions
    required property bool fixed    // the eye row: it only hides
    required property string page
    required property var tile      // one of backend.groups' tiles
    // The tile of a row about to go (its list is already shorter).
    readonly property var blank: ({ label: "", action: "", on: false, first: true, last: true })
    readonly property int chosen: choices.findIndex(c => c.value === tile.action)
    spacing: Kirigami.Units.smallSpacing

    Controls.CheckBox {
        checked: row.tile.on
        Accessible.name: "Show " + row.tile.label
        Controls.ToolTip.text: "Show on the palm menu"
        Controls.ToolTip.visible: hovered
        onToggled: {
            row.backend.setShown(row.page, row.index, checked)
            checked = Qt.binding(() => row.tile.on)
        }
    }
    Controls.TextField {
        objectName: "palmLabel"
        Layout.fillWidth: true
        Layout.minimumWidth: Kirigami.Units.gridUnit * 5
        Layout.preferredWidth: Kirigami.Units.gridUnit * 12
        text: row.tile.label
        placeholderText: "Text on the tile"
        maximumLength: row.backend.labelMax
        opacity: row.tile.on ? 1 : 0.6
        Accessible.name: "Text on the tile"
        onEditingFinished: {
            if (text !== row.tile.label)
                row.backend.setLabel(row.page, row.index, text)
            text = Qt.binding(() => row.tile.label)
        }
    }
    Controls.ComboBox {
        objectName: "palmAction"
        Layout.fillWidth: true
        Layout.minimumWidth: Kirigami.Units.gridUnit * 5
        Layout.preferredWidth: Kirigami.Units.gridUnit * 14
        model: row.choices
        textRole: "text"
        valueRole: "value"
        currentIndex: row.chosen
        // A new list puts a ComboBox on its first entry: back to this tile's.
        onModelChanged: currentIndex = Qt.binding(() => row.chosen)
        wheelEnabled: false   // (scrolling the page mustn't change what a tile runs)
        opacity: row.tile.on ? 1 : 0.6
        Accessible.name: "What " + row.tile.label + " runs"
        onActivated: {
            row.backend.setAction(row.page, row.index, currentValue)
            currentIndex = Qt.binding(() => row.chosen)
        }
    }
    Controls.ToolButton {
        visible: !row.fixed
        enabled: !row.tile.first
        icon.name: "go-up"
        display: Controls.AbstractButton.IconOnly
        text: "Move up"
        Controls.ToolTip.text: text
        Controls.ToolTip.visible: hovered
        onClicked: row.backend.move(row.page, row.index, -1)
    }
    Controls.ToolButton {
        visible: !row.fixed
        enabled: !row.tile.last
        icon.name: "go-down"
        display: Controls.AbstractButton.IconOnly
        text: "Move down"
        Controls.ToolTip.text: text
        Controls.ToolTip.visible: hovered
        onClicked: row.backend.move(row.page, row.index, 1)
    }
    Controls.ToolButton {
        visible: !row.fixed
        icon.name: "edit-delete"
        display: Controls.AbstractButton.IconOnly
        text: "Remove"
        Controls.ToolTip.text: text
        Controls.ToolTip.visible: hovered
        onClicked: row.backend.remove(row.page, row.index)
    }
}
