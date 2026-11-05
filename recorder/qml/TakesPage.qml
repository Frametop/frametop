// The recorders' Takes page: a session's or round's takes, each opening in the Viewer page, with a
// thumbnail, its counts and a delete button. Both windows use it; what differs comes in as properties.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: takesView
    // The window (its pageStack), and its Viewer page, which a take opens in.
    property var recorder
    property var viewerComponent
    // What a recording is called (session, round); the take thumbnails' image provider; the
    // row's count of its frame sets or frames, its name, and its count of deleted ones.
    property string itemNoun
    property real thumbWidth: 5
    property string imageProvider
    property string countKey
    property string countNoun
    property string deletedKey
    property string session: ""
    property var rows: backend.takeList(session)
    title: "Takes"

    Connections {
        target: backend
        function onSessionsChanged() { takesView.rows = backend.takeList(takesView.session) }
    }

    ListView {
        model: takesView.rows
        spacing: Kirigami.Units.smallSpacing

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: parent.count === 0
            text: "No takes in this " + takesView.itemNoun
        }

        delegate: Controls.ItemDelegate {
            id: takeRow
            required property var modelData
            width: ListView.view.width
            onClicked: takesView.recorder.pageStack.push(takesView.viewerComponent,
                                                     { session: takesView.session, take: modelData.id })

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing
                Image {
                    Layout.preferredWidth: Kirigami.Units.gridUnit * takesView.thumbWidth
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 4.5
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    source: takeRow.modelData[takesView.countKey] > 0
                            ? "image://" + takesView.imageProvider + "/thumb/" + takesView.session + "/"
                              + takeRow.modelData.id : ""
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Controls.Label { text: takeRow.modelData.title; font.bold: true }
                    Controls.Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        text: takeRow.modelData.status + " · " + takeRow.modelData.durationText + " · "
                              + takeRow.modelData[takesView.countKey] + " " + takesView.countNoun
                              + (takeRow.modelData[takesView.deletedKey]
                                 ? " (" + takeRow.modelData[takesView.deletedKey] + " deleted)" : "")
                              + " · " + takeRow.modelData.sizeText
                    }
                }
                Controls.ToolButton {
                    icon.name: "edit-delete"
                    text: "Delete take"
                    display: Controls.AbstractButton.IconOnly
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered
                    onClicked: {
                        const take = takeRow.modelData.id
                        confirm.ask("Delete this take?",
                                    "\"" + takeRow.modelData.title + "\" (" + takeRow.modelData.durationText
                                    + ") is deleted from the headset for good.",
                                    () => { takesView.recorder.pageStack.pop(takesView); backend.deleteTake(takesView.session, take) })
                    }
                }
            }
        }
    }
}
