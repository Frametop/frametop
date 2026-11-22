// Remote Displays' "Monitors": a PC's own monitors turned on or off and put in another mode while
// Frametop streams from it, then back as they were. Needs Frametop's build of Vibepollo.
// RemoteDisplayPages.qml makes one; its backend is host_monitors.py (backend.hostMonitors).
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.PromptDialog {
    id: dialog
    objectName: "monitorsDialog"
    title: "Monitors of " + host
    standardButtons: Kirigami.Dialog.NoButton
    property QtObject monitors  // backend.hostMonitors
    property string host: ""
    property var listed: []     // the PC's monitors, as it answered
    property var wishes: ({})   // id -> {on, width, height, refresh_mhz}; width 0: its own mode
    property string error: ""
    property string note: ""    // the PC's own word on its last change
    property int grace: 120
    property bool loading: false
    readonly property bool anyOn: listed.some(m => wishes[m.id] && wishes[m.id].on)

    function modeText(m) {
        return m.width + " × " + m.height + " at " + (m.refresh_mhz / 1000).toFixed(m.refresh_mhz % 1000 ? 2 : 0) + " Hz"
    }
    // As the PC matches them: Hyprland reports 239.761 Hz for a mode it lists as 239.76, and
    // 59.94 and 60 Hz are two modes.
    function sameMode(a, b) {
        return !!a && !!b && a.width === b.width && a.height === b.height && Math.abs(a.refresh_mhz - b.refresh_mhz) <= 10
    }
    // A monitor's mode choices: first its own (no mode in the plan: the one it has without
    // Frametop), then each mode it lists.
    function choices(m) {
        const own = m.mode && !m.changed ? "Its own mode (" + modeText(m.mode) + ")" : "Its own mode"
        return [{ text: own, mode: null }].concat(m.modes.map(x => ({ text: modeText(x), mode: x })))
    }
    function choiceIndex(m, w) {
        if (!w || !(w.width > 0)) return 0
        return Math.max(0, choices(m).findIndex(c => c.mode && sameMode(c.mode, w)))
    }
    function pick(id, mode) {
        wish(id, mode ? { width: mode.width, height: mode.height, refresh_mhz: mode.refresh_mhz }
                      : { width: 0, height: 0, refresh_mhz: 0 })
    }
    function openFor(h) {
        host = h.name; listed = []; wishes = ({}); error = ""; note = ""
        open()
        loading = true
        monitors.refresh(host)
    }
    function wish(id, change) {
        const w = Object.assign({}, wishes)
        w[id] = Object.assign({}, w[id], change)
        wishes = w
    }
    function send() {
        loading = true
        monitors.apply(host, wishes)
    }

    Connections {
        target: dialog.monitors
        function onMonitorsReady(host, state, error) {
            if (host !== dialog.host) return
            dialog.loading = false
            dialog.error = error
            if (error) return
            dialog.listed = state.monitors || []
            dialog.note = state.error || ""
            dialog.grace = state.grace_seconds || 120
            const w = {}
            // Only a mode picked here goes in the plan: the others stay as the PC has them.
            for (const m of dialog.listed) {
                const wish = m.wish || { on: m.on, mode: null }
                const mode = wish.mode
                w[m.id] = { on: wish.on, width: mode ? mode.width : 0, height: mode ? mode.height : 0,
                            refresh_mhz: mode ? mode.refresh_mhz : 0 }
            }
            dialog.wishes = w
        }
    }

    ColumnLayout {
        Controls.Label {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 28
            wrapMode: Text.Wrap
            opacity: 0.7
            text: "This applies while Frametop streams from " + dialog.host + ", then everything goes back as it was "
                  + Math.round(dialog.grace / 60) + " minutes after the last stream ends (or when it restarts). "
                  + "A monitor's display streams at its mode."
        }
        Controls.BusyIndicator { visible: dialog.loading; running: visible; Layout.alignment: Qt.AlignHCenter }
        Repeater {
            model: dialog.listed
            delegate: RowLayout {
                id: row
                required property var modelData
                readonly property var wish: dialog.wishes[modelData.id] || { on: modelData.on }
                Layout.fillWidth: true
                Controls.Switch {
                    objectName: "monitorOn-" + row.modelData.id
                    checked: row.wish.on
                    onToggled: dialog.wish(row.modelData.id, { on: checked })
                }
                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true
                    Controls.Label { text: row.modelData.name + (row.modelData.primary ? "  (main)" : ""); textFormat: Text.PlainText }
                    Controls.Label {
                        text: row.modelData.id + "  ·  " + (row.modelData.on ? "on now" : "off now")
                              + (row.modelData.changed ? ", changed by Frametop" : "")
                        textFormat: Text.PlainText
                        opacity: 0.7
                    }
                }
                Controls.ComboBox {
                    objectName: "monitorMode-" + row.modelData.id
                    enabled: row.wish.on && row.modelData.modes.length > 0
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                    readonly property var entries: dialog.choices(row.modelData)
                    model: entries
                    textRole: "text"
                    currentIndex: dialog.choiceIndex(row.modelData, row.wish)
                    onActivated: (i) => dialog.pick(row.modelData.id, entries[i].mode)
                }
            }
        }
        Controls.Label {
            visible: !dialog.loading && dialog.error === "" && dialog.listed.length === 0
            text: "It has no monitors to change."
            opacity: 0.7
        }
        Controls.Label {
            visible: dialog.listed.length > 0 && !dialog.anyOn
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Kirigami.Theme.negativeTextColor
            text: "Leave at least one monitor on."
        }
        Controls.Label {
            visible: dialog.note !== "" || dialog.error !== ""
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 28
            wrapMode: Text.Wrap
            color: Kirigami.Theme.negativeTextColor
            text: dialog.error || ("Its last change didn't take: " + dialog.note)
            textFormat: Text.PlainText
        }
    }
    customFooterActions: [
        Kirigami.Action {
            text: "Apply"
            icon.name: "dialog-ok-apply"
            enabled: !dialog.loading && dialog.listed.length > 0 && dialog.anyOn
            onTriggered: dialog.send()
        },
        Kirigami.Action {
            text: "Put them all back"
            icon.name: "edit-undo"
            enabled: !dialog.loading && dialog.listed.length > 0
            onTriggered: { dialog.loading = true; dialog.monitors.putBack(dialog.host) }
        },
        Kirigami.Action { text: "Close"; icon.name: "dialog-close"; onTriggered: dialog.close() }
    ]
}
