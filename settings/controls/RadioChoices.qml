// One choice out of a few, each with a line of help under it.
//   RadioChoices { choices: [{ value: "a", text: "A", help: "What A does." }, ...]
//                  value: backend.x; onPicked: backend.setX(picked) }
// A choice with hidden: true isn't shown.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: group
    property var choices: []
    property string value: ""
    // The choice just made (its value).
    property string picked: ""
    signal chose()
    spacing: Kirigami.Units.smallSpacing

    Repeater {
        model: group.choices
        delegate: ColumnLayout {
            id: choice
            required property var modelData
            spacing: 0
            visible: !modelData.hidden
            Controls.RadioButton {
                text: choice.modelData.text
                checked: group.value === choice.modelData.value
                onToggled: {
                    if (checked) { group.picked = choice.modelData.value; group.chose() }
                    checked = Qt.binding(() => group.value === choice.modelData.value)  // what was saved
                }
            }
            Hint {
                visible: !!choice.modelData.help
                text: choice.modelData.help || ""
                leftPadding: Kirigami.Units.gridUnit * 1.6
            }
        }
    }
}
