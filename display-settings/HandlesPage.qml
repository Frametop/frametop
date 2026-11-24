// Frametop Settings' Grab bars & corners page: how easy the bar under each screen is to hit, and
// what its resize corners look like (docs/resize-corners.md). The two things most people change
// come first; the look of each opens on request. DisplayPages.qml makes it; its backend is
// ft_display_settings.py, and ft-screens takes every change at once.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "../settings/controls" as Ft

Kirigami.ScrollablePage {
    id: page
    property QtObject backend   // DisplayPages.qml sets it: ft_display_settings.py
    title: "Grab bars & corners"
    // The corner settings below the style only count for the outline styles.
    readonly property bool outline: backend.resizeCornerStyle !== "glow"

    Kirigami.FormLayout {
        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Grab bars" }
        Ft.Hint {
            text: "Use the bar underneath a screen or window to move it. Adding grab padding makes the "
                  + "bar easier to aim at without making the bar itself look larger. Changes apply immediately."
        }
        Ft.SliderRow {
            Kirigami.FormData.label: "Vertical grab padding:"
            from: 0; to: 50; stepSize: 0.1
            value: backend.grabBarPaddingY
            shown: value.toFixed(1) + " cm"
            onMoved: backend.setGrabBarPaddingY(value)
        }
        Ft.SliderRow {
            Kirigami.FormData.label: "Horizontal grab padding:"
            from: 0; to: 50; stepSize: 0.1
            value: backend.grabBarPaddingX
            shown: value.toFixed(1) + " cm"
            onMoved: backend.setGrabBarPaddingX(value)
        }
        Ft.Hint {
            text: "Added to each side (for example, 2 cm expands the target by 4 cm total; 0 cm matches "
                  + "the visible bar). Nearby bar buttons automatically shift aside to avoid overlapping."
        }
        Controls.Switch {
            Kirigami.FormData.label: "Haptic feedback:"
            text: "Vibrate controller when aiming at a grab bar"
            checked: backend.grabBarHaptics
            onToggled: backend.setGrabBarHaptics(checked)
        }
        Controls.Switch {
            Kirigami.FormData.label: "Highlight grab area:"
            text: "Show the expanded clickable area around the bar"
            checked: backend.grabBarPreview
            onToggled: backend.setGrabBarPreview(checked)
        }
        Ft.Disclosure {
            id: zoneLook
            visible: backend.grabBarPreview
            title: "Grab area appearance"
            summary: "colors, outline, visibility"
        }
        Controls.Switch {
            visible: zoneLook.visible && zoneLook.expanded
            Kirigami.FormData.label: "Always show:"
            text: "Keep visible whenever the window is shown"
            checked: backend.grabBarPreviewAlways
            onToggled: backend.setGrabBarPreviewAlways(checked)
        }
        Ft.ColorRow {
            visible: zoneLook.visible && zoneLook.expanded
            Kirigami.FormData.label: "Fill color:"
            title: "Grab area fill color"
            color: backend.grabBarPreviewColor
            onChose: backend.setGrabBarPreviewColor(picked)
        }
        Ft.SliderRow {
            visible: zoneLook.visible && zoneLook.expanded
            Kirigami.FormData.label: "Fill opacity:"
            from: 0; to: 1; stepSize: 0.01
            value: backend.grabBarPreviewOpacity
            shown: Math.round(value * 100) + "%"
            onMoved: backend.setGrabBarPreviewOpacity(value)
        }
        Ft.ColorRow {
            visible: zoneLook.visible && zoneLook.expanded
            Kirigami.FormData.label: "Outline color:"
            title: "Grab area outline color"
            color: backend.grabBarOutlineColor
            onChose: backend.setGrabBarOutlineColor(picked)
        }
        Ft.SliderRow {
            visible: zoneLook.visible && zoneLook.expanded
            Kirigami.FormData.label: "Outline opacity:"
            from: 0; to: 1; stepSize: 0.01
            value: backend.grabBarOutlineOpacity
            shown: Math.round(value * 100) + "%"
            onMoved: backend.setGrabBarOutlineOpacity(value)
        }
        Ft.SliderRow {
            visible: zoneLook.visible && zoneLook.expanded
            Kirigami.FormData.label: "Outline thickness:"
            from: 0; to: 8; stepSize: 1
            value: backend.grabBarOutlineWidth
            shown: Math.round(value) === 0 ? "None" : Math.round(value) + " px"
            onMoved: backend.setGrabBarOutlineWidth(value)
        }
        Ft.Hint {
            visible: zoneLook.visible && zoneLook.expanded
            text: "Unless Always show is on, the highlight fades in and out with the grab bar."
        }

        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Resize corners" }
        Ft.Hint {
            text: "Drag any corner of a screen to resize it. A corner handle appears when you point near it."
        }
        Controls.ComboBox {
            Kirigami.FormData.label: "Corner style:"
            model: ["Sharp outline", "Soft glow (classic)", "SteamOS style (gray to white)"]
            currentIndex: backend.resizeCornerStyle === "native" ? 2 : backend.resizeCornerStyle === "glow" ? 1 : 0
            onActivated: backend.setResizeCornerStyle(["sharp", "glow", "native"][currentIndex])
        }
        Ft.Hint {
            text: page.outline
                  ? "A crisp rounded corner bracket just outside the screen. When you point directly at "
                    + "it, it brightens and thickens to show it's ready to drag."
                  : "A soft glow at the corner that fades in as you point near it. The outline options "
                    + "below don't apply to this style, though your custom values are kept if you switch back."
        }
        Controls.Switch {
            Kirigami.FormData.label: "Haptic feedback:"
            text: "Vibrate controller when aiming at a corner"
            checked: backend.resizeCornerHaptics
            onToggled: backend.setResizeCornerHaptics(checked)
        }
        Ft.Disclosure {
            id: cornerLook
            visible: page.outline
            title: "Corner appearance & sensitivity"
            summary: "always-visible corners, size, colors, reach"
        }
        ColumnLayout {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Always show:"
            Kirigami.FormData.labelAlignment: Qt.AlignTop
            spacing: 0
            Repeater {
                model: ["Top left", "Top right", "Bottom left", "Bottom right"]
                delegate: Controls.CheckBox {
                    required property int index
                    required property string modelData
                    text: modelData
                    checked: backend.resizeCornerAlways[index]
                    onToggled: {
                        backend.setResizeCornerAlways(index, checked)
                        checked = Qt.binding(() => backend.resizeCornerAlways[index])
                    }
                }
            }
        }
        Controls.Switch {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Show hit areas:"
            text: "Highlight the clickable area around each corner"
            checked: backend.resizeCornerPreview
            onToggled: backend.setResizeCornerPreview(checked)
        }
        Ft.ColorRow {
            visible: cornerLook.visible && cornerLook.expanded
            enabled: backend.resizeCornerStyle === "sharp"
            Kirigami.FormData.label: "Color:"
            title: "Resize corner color"
            color: backend.resizeCornerColor
            onChose: backend.setResizeCornerColor(picked)
        }
        Ft.ColorRow {
            visible: cornerLook.visible && cornerLook.expanded
            enabled: backend.resizeCornerStyle === "sharp"
            Kirigami.FormData.label: "Hover color:"
            title: "Ready-to-drag corner color"
            color: backend.resizeCornerHoverColor
            onChose: backend.setResizeCornerHoverColor(picked)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Opacity:"
            from: 0; to: 1; stepSize: 0.01
            value: backend.resizeCornerOpacity
            shown: Math.round(value * 100) + "%"
            onMoved: backend.setResizeCornerOpacity(value)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Arm length:"
            from: 1; to: 10; stepSize: 0.1
            value: backend.resizeCornerLength
            shown: value.toFixed(1) + " cm"
            onMoved: backend.setResizeCornerLength(value)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Line thickness:"
            from: 0.5; to: 6; stepSize: 0.1
            value: backend.resizeCornerWidth
            shown: value.toFixed(1) + " mm"
            onMoved: backend.setResizeCornerWidth(value)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Corner roundness:"
            from: 0; to: 100; stepSize: 1
            value: backend.resizeCornerRoundness
            shown: Math.round(value) + "%"
            onMoved: backend.setResizeCornerRoundness(value)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Gap from screen:"
            from: 0; to: 10; stepSize: 0.1
            value: backend.resizeCornerGap
            shown: value.toFixed(1) + " cm"
            onMoved: backend.setResizeCornerGap(value)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Hit area padding:"
            from: 0.1; to: 10; stepSize: 0.1
            value: backend.resizeCornerPadding
            shown: value.toFixed(1) + " cm"
            onMoved: backend.setResizeCornerPadding(value)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Safe margin around screen:"
            from: 0; to: 5; stepSize: 0.1
            value: backend.resizeCornerExclusion
            shown: value.toFixed(1) + " cm"
            onMoved: backend.setResizeCornerExclusion(value)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Appear distance:"
            from: 1; to: 50; stepSize: 0.5
            value: backend.resizeCornerReach
            shown: value.toFixed(1) + " cm away"
            onMoved: backend.setResizeCornerReach(value)
        }
        Ft.Hint {
            visible: cornerLook.visible && cornerLook.expanded
            text: "Hit area padding makes the corner easier to grab with both controller lasers and the "
                  + "3D pointer. Safe margin pushes the handle outward so it never accidentally catches "
                  + "clicks near the edge of the screen. Appear distance only controls how close you "
                  + "point before the corner fades in. SteamOS style automatically shifts from gray to "
                  + "bright white and slightly enlarges on hover."
        }
    }
}
