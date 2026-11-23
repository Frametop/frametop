// The Hands page's interaction switches (ft_input_settings.py HAND_INTERACTIONS), one group at a
// time: each group's heading says how many of its switches are on and opens to show them, each
// with what it does under it. The lazy susan's distance (SCREEN_SPIN_DISTANCE) sits under its switch.
// Groups and rows are counted, not listed: a new list would rebuild them and close the open groups.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "../settings/controls" as Ft

ColumnLayout {
    id: list
    property QtObject backend   // InputPages.qml sets it: ft_input_settings.py
    readonly property var all: backend.handInteractions
    // Direct touch (Hand input) switches all of them.
    readonly property bool usable: !!backend.handMethods.touch
    readonly property var spinRange: backend.spinDistanceRange
    // The groups' names, in the list's order: set once, they never change.
    property var groups: []
    Component.onCompleted: groups = all.map(e => e.group).filter((g, i, every) => every.indexOf(g) === i)
    spacing: 0

    Ft.Hint {
        Layout.bottomMargin: Kirigami.Units.smallSpacing
        text: list.usable ? "What your hands do on Frametop's screens. Each applies at once."
                          : "These need Direct touch on (Hand input, above)."
    }
    Repeater {
        model: list.groups.length
        delegate: ColumnLayout {
            id: group
            required property int index
            readonly property string name: list.groups[index]
            readonly property var entries: list.all.filter(e => e.group === name)
            spacing: 0
            Ft.Disclosure {
                id: heading
                title: group.name
                summary: group.entries.filter(e => e.on).length + " of " + group.entries.length + " on"
            }
            Repeater {
                model: heading.expanded ? group.entries.length : 0
                delegate: ColumnLayout {
                    id: row
                    required property int index
                    readonly property var entry: group.entries[index]
                    spacing: 0
                    Layout.leftMargin: Kirigami.Units.gridUnit * 1.5
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    Controls.Switch {
                        text: row.entry.label
                        enabled: list.usable
                        checked: row.entry.on
                        onToggled: {
                            list.backend.setHandInteraction(row.entry.key, checked)
                            checked = Qt.binding(() => row.entry.on)
                        }
                    }
                    Ft.Hint {
                        text: row.entry.tip
                        leftPadding: Kirigami.Units.gridUnit * 2.4
                    }
                    Ft.SliderRow {
                        visible: row.entry.key === "SCREEN_SPIN_BRING"
                        enabled: list.usable && row.entry.on
                        Layout.leftMargin: Kirigami.Units.gridUnit * 2.4
                        from: list.spinRange.min
                        to: list.spinRange.max
                        stepSize: list.spinRange.step
                        snapMode: Controls.Slider.SnapAlways
                        value: list.backend.spinDistance
                        sliderWidth: Kirigami.Units.gridUnit * 10
                        shown: "comes " + value.toFixed(2) + " m out"
                        reset: list.spinRange.default
                        resetText: "Default (" + list.spinRange.default + " m)"
                        tip: "How far in front of you a brought screen comes (at least 0.9 m, behind "
                             + "frunner's search). One you push or pull there comes back that far next time, until the reset button."
                        onMoved: list.backend.setSpinDistance(value)
                    }
                }
            }
        }
    }
}
