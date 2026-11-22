// A display panel's monitor mode, on a host whose monitors follow its panels ("Turn off and set
// its own monitors"): its own mode, or one the monitor lists. RemoteDisplayPages.qml puts one in
// each such display's settings; the pick goes to host_monitors.py (pickMode).
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: picker
    property var monitor: null  // the PC's monitor as it answered: {id, on, mode, modes, changed, ...}
    property var picked: ({})   // the panel's mode: {width, height, refresh_mhz}, or {} for its own
    signal pick(var mode)       // a mode, or null for its own

    function modeText(m) {
        return m.width + " × " + m.height + " at " + (m.refresh_mhz / 1000).toFixed(m.refresh_mhz % 1000 ? 2 : 0) + " Hz"
    }
    // As the PC matches them: Hyprland reports 239.761 Hz for a mode it lists as 239.76, and
    // 59.94 and 60 Hz are two modes.
    function sameMode(a, b) {
        return !!a && !!b && a.width === b.width && a.height === b.height && Math.abs(a.refresh_mhz - b.refresh_mhz) <= 10
    }
    // Its own mode first (the one it has without Frametop), then each mode it lists, and the
    // picked one if it doesn't list that (a Windows PC lists none for a monitor that's off).
    function choices() {
        const m = monitor
        const own = m && m.mode && m.on && !(picked && picked.width) ? "Its own mode (" + modeText(m.mode) + ")" : "Its own mode"
        const modes = m ? m.modes.slice() : []
        if (picked && picked.width && !modes.some(x => sameMode(x, picked))) modes.push(picked)
        return [{ text: own, mode: null }].concat(modes.map(x => ({ text: modeText(x), mode: x })))
    }
    function choiceIndex(entries) {
        if (!picked || !(picked.width > 0)) return 0
        return Math.max(0, entries.findIndex(c => c.mode && sameMode(c.mode, picked)))
    }

    // Entry i of the box picked: sent unless it's the one picked already.
    function choose(i) {
        const mode = box.entries[i].mode
        if (!(mode ? sameMode(mode, picked) : !(picked && picked.width))) pick(mode)
    }

    Controls.ComboBox {
        id: box
        objectName: "monitorMode"
        readonly property var entries: picker.choices()
        enabled: picker.monitor !== null && entries.length > 1
        Layout.preferredWidth: Kirigami.Units.gridUnit * 14
        model: entries
        textRole: "text"
        currentIndex: picker.choiceIndex(entries)
        onActivated: (i) => picker.choose(i)
    }
    Controls.Label {
        visible: picker.monitor === null
        text: "Not one of its monitors now (unplugged, or it didn't answer)"
        opacity: 0.7
    }
}
