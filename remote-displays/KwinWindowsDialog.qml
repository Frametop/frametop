// Remote Displays' "Add windows" (issue 81): a KDE Plasma PC's open windows, from utrack's
// KWin Window Stream on it, each one picked a panel of its own that the window sizes.
// RemoteDisplayPages.qml makes one; its backend is kwin_windows.py (backend.kwinWindows).
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.PromptDialog {
    id: dialog
    objectName: "kwinDialog"
    title: "Add windows from " + host
    standardButtons: Kirigami.Dialog.NoButton
    property QtObject windows   // backend.kwinWindows
    property string host: ""
    property string client: ""  // the one the PC took, to connect with
    property var listed: []
    property var chosen: ({})   // id -> ticked
    property string error: ""
    property bool loading: false
    readonly property int count: listed.filter(w => chosen[w.id] && !w.added).length

    function openFor(h) {
        host = h.name; client = ""; listed = []; chosen = ({}); error = ""
        open()
        refresh()
    }
    function refresh() {
        loading = true
        windows.listWindows(host)
    }
    function tick(id, on) {
        const c = Object.assign({}, chosen)
        c[id] = on
        chosen = c
    }
    function addTicked() {
        for (const w of listed)
            if (chosen[w.id] && !w.added) windows.addWindow(host, w.id, w.title, client, w.width, w.height)
        close()
    }

    Connections {
        target: dialog.windows
        function onWindowsReady(host, listed, client, error) {
            if (host !== dialog.host) return
            dialog.loading = false
            dialog.listed = listed
            dialog.client = client
            dialog.error = error
            const c = {}
            for (const w of listed) c[w.id] = false
            dialog.chosen = c
        }
    }

    ColumnLayout {
        Controls.BusyIndicator { visible: dialog.loading; running: visible; Layout.alignment: Qt.AlignHCenter }
        Controls.Label {
            visible: dialog.error !== ""
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 28
            wrapMode: Text.Wrap
            textFormat: Text.PlainText  // the error can carry what the PC said
            text: "Couldn't list its windows: " + dialog.error
                  + ". It needs KWin Window Stream running on the PC (github.com/utrack/kwin-window-stream)."
        }
        Controls.Label {
            visible: !dialog.loading && dialog.error === "" && dialog.listed.length === 0
            text: "No windows open on it."
            opacity: 0.7
        }
        Repeater {
            model: dialog.listed
            // A window's title and app are the PC's: shown as plain text (a CheckBox's own text
            // would show markup in them as rich text).
            RowLayout {
                id: row
                required property var modelData
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 28
                Controls.CheckBox {
                    id: box
                    enabled: !row.modelData.added
                    checked: dialog.chosen[row.modelData.id] === true
                    onToggled: dialog.tick(row.modelData.id, checked)
                    Accessible.name: label.text
                }
                Controls.Label {
                    id: label
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    enabled: box.enabled
                    text: row.modelData.title + " (" + (row.modelData.app ? row.modelData.app + ", " : "")
                          + row.modelData.width + " × " + row.modelData.height
                          + (row.modelData.minimized ? ", minimized" : "") + ")" + (row.modelData.added ? ": added" : "")
                    TapHandler {
                        enabled: box.enabled
                        onTapped: dialog.tick(row.modelData.id, !box.checked)
                    }
                }
            }
        }
    }
    customFooterActions: [
        Kirigami.Action {
            text: dialog.count > 1 ? "Add " + dialog.count : "Add"
            icon.name: "list-add"
            enabled: dialog.count > 0
            onTriggered: dialog.addTicked()
        },
        Kirigami.Action { text: "Refresh"; icon.name: "view-refresh"; enabled: !dialog.loading; onTriggered: dialog.refresh() },
        Kirigami.Action { text: "Cancel"; icon.name: "dialog-cancel"; onTriggered: dialog.close() }
    ]
}
