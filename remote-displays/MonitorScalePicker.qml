// A display panel's monitor scale, on a host whose monitors follow its panels: its own scale,
// or one the PC says the monitor takes in its mode now (Hyprland lists the ones that give a
// whole-pixel desktop; Windows lists none, so this isn't shown there). RemoteDisplayPages.qml
// puts one under each such display's mode picker; the pick goes to host_monitors.py (pickScale).
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
    // Its own scale first (the one it has without Frametop), then each one it takes, and the
    // picked one if it isn't listed (another mode takes another set).
    function choices() {
        const m = monitor
        const own = m && m.on && m.scale && !(picked > 0) ? "Its own scale (" + percent(m.scale) + ")" : "Its own scale"
        const scales = m && m.scales ? m.scales.slice() : []
        if (picked > 0 && !scales.some(x => sameScale(x, picked))) scales.push(picked)
        scales.sort((a, b) => a - b)
        return [{ text: own, scale: null }].concat(scales.map(x => ({ text: percent(x), scale: x })))
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
