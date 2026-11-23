// The heading of a group that opens and closes: the settings most people never change stay out
// of the way until asked for. The rows under it bind their visible to expanded.
//   Disclosure { id: fine; title: "Fine tuning"; summary: "7 settings" }
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Controls.ToolButton {
    id: button
    property string title: ""
    // What's inside, said short, beside the title.
    property string summary: ""
    property bool expanded: false
    text: summary !== "" ? title + "  ·  " + summary : title
    icon.name: expanded ? "arrow-down" : "arrow-right"
    display: Controls.AbstractButton.TextBesideIcon
    font.bold: true
    Accessible.name: title + (expanded ? ", open" : ", closed")
    onClicked: expanded = !expanded
}
