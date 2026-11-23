// A display panel's monitor scale, on a host whose monitors follow its panels: its own scale,
// 125%, 150%, 175% and 200% always, and any other the PC says the monitor takes in its mode
// now. Hyprland takes only scales that give a whole-pixel desktop, so one of the four it
// doesn't list says the nearest one the monitor gets instead ("150% (160% on this monitor)":
// the PC fits it). Windows lists none, so this isn't shown there. RemoteDisplayPages.qml puts
// one in each such display's settings; the pick goes to host_monitors.py (pickScale).
// The stream keeps its size: the scale changes the PC's desktop, not its pixels.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: picker
    property var monitor: null  // the PC's monitor as it answered: {id, on, scale, scales, ...}
    property real picked: 0     // the panel's scale (1.25: 125%), or 0 for its own
    signal pick(var scale)      // a scale, or null for its own

    function percent(s) {
        return Math.round(s * 100) + "%"
    }
    function sameScale(a, b) {
        return Math.abs(a - b) < 0.001
    }
    readonly property var always: [1.25, 1.5, 1.75, 2.0]
    // The listed scale nearest to one (the PC's fit), or the scale itself when none is listed.
    function nearest(listed, s) {
        return listed.reduce((best, x) => Math.abs(x - s) < Math.abs(best - s) ? x : best, listed.length ? listed[0] : s)
    }
    // Its own scale first (the one it has without Frametop), then the four it always has, each
    // other one it takes, and the picked one if it isn't among them (another mode takes
    // another set). A listed scale that only stands in for one of the four isn't shown twice.
    function choices() {
        const m = monitor
        // While a picked scale applies, the PC says the one it goes back to (own_scale).
        const ownScale = m && m.on ? (m.changed ? m.own_scale : m.scale) : 0
        const own = ownScale ? "Its own scale (" + percent(ownScale) + ")" : "Its own scale"
        const listed = m && m.scales ? m.scales : []
        const has = (list, s) => list.some(x => sameScale(x, s))
        const entries = always.map(s => ({ scale: s, gives: has(listed, s) ? s : nearest(listed, s) }))
        const standIns = entries.filter(e => !sameScale(e.gives, e.scale)).map(e => e.gives)
        for (const s of listed) {
            if (!has(always, s) && (!has(standIns, s) || sameScale(s, picked))) entries.push({ scale: s, gives: s })
        }
        if (picked > 0 && !entries.some(e => sameScale(e.scale, picked)))
            entries.push({ scale: picked, gives: nearest(listed, picked) })
        entries.sort((a, b) => a.scale - b.scale)
        return [{ text: own, scale: null }].concat(entries.map(e => ({
            text: percent(e.scale) + (sameScale(e.gives, e.scale) ? "" : " (" + percent(e.gives) + " on this monitor)"),
            scale: e.scale })))
    }
    function choiceIndex(entries) {
        if (!(picked > 0)) return 0
        return Math.max(0, entries.findIndex(c => c.scale && sameScale(c.scale, picked)))
    }

    // Entry i of the box picked: sent unless it's the one picked already.
    function choose(i) {
        const scale = box.entries[i].scale
        if (!(scale ? sameScale(scale, picked) : !(picked > 0))) pick(scale)
    }

    Controls.ComboBox {
        id: box
        objectName: "monitorScale"
        readonly property var entries: picker.choices()
        enabled: picker.monitor !== null && entries.length > 1
        Layout.preferredWidth: Kirigami.Units.gridUnit * 14
        model: entries
        textRole: "text"
        currentIndex: picker.choiceIndex(entries)
        onActivated: (i) => picker.choose(i)
    }
}
