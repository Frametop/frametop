// A colour: its swatch and a button that opens the picker.
//   ColorRow { title: "Drag zone fill color"; color: backend.x; onPicked: backend.setX(picked) }
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: row
    property color color
    property string title: "Color"
    // The colour just chosen, as "#rrggbb".
    property string picked: ""
    signal chose()

    Rectangle {
        Layout.preferredWidth: Kirigami.Units.gridUnit * 2
        Layout.preferredHeight: Kirigami.Units.gridUnit
        color: row.color
        border.color: Kirigami.Theme.textColor
    }
    Controls.Button { text: "Choose…"; onClicked: dialog.open() }
    ColorDialog {
        id: dialog
        title: row.title
        selectedColor: row.color
        onAccepted: { row.picked = selectedColor.toString(); row.chose() }
    }
}
