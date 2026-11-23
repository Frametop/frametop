// A line or two of dim help under a control. Wraps at a readable width, so a long one doesn't
// make its form wider than the window (which stacks the form's labels and cuts the text off).
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Controls.Label {
    wrapMode: Text.Wrap
    opacity: 0.7
    font: Kirigami.Theme.smallFont
    Layout.fillWidth: true
    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
}
