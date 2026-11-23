// A page's background notes, at its foot: two lines until More is pressed, so the notes don't
// take the room the settings need (a Frametop Settings window in VR is rarely tall).
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Controls.Control {
    id: help
    property string text: ""
    property bool expanded: false
    padding: Kirigami.Units.largeSpacing
    topPadding: Kirigami.Units.smallSpacing
    bottomPadding: Kirigami.Units.smallSpacing

    background: Item {
        Kirigami.Separator { anchors { left: parent.left; right: parent.right; top: parent.top } }
    }
    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing
        Controls.Label {
            id: notes
            Layout.fillWidth: true
            text: help.text
            wrapMode: Text.Wrap
            elide: Text.ElideRight
            maximumLineCount: help.expanded ? 1000 : 2
            opacity: 0.7
            font: Kirigami.Theme.smallFont
        }
        Controls.ToolButton {
            Layout.alignment: Qt.AlignTop
            visible: help.expanded || notes.truncated
            text: help.expanded ? "Less" : "More"
            icon.name: help.expanded ? "arrow-down" : "arrow-up"
            onClicked: help.expanded = !help.expanded
        }
    }
}
