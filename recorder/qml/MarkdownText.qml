// Read-only Markdown (CONSENT.md, UPLOAD.md), selectable for copying. Both recorder windows use it.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Controls.TextArea {
    property string markdown
    // Links get the application palette's colour when the text is parsed, and Kirigami's theme
    // doesn't set that palette: set it, then parse again (the theme resolves after creation).
    readonly property color linkColor: Kirigami.Theme.linkColor
    function parse() {
        backend.setLinkColor(linkColor)
        text = markdown
    }
    onLinkColorChanged: parse()
    onMarkdownChanged: parse()
    Component.onCompleted: parse()
    textFormat: TextEdit.MarkdownText
    readOnly: true
    selectByMouse: true
    wrapMode: Text.Wrap
    background: null
    padding: 0
    onLinkActivated: link => Qt.openUrlExternally(link)
}
