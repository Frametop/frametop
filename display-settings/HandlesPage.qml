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
            text: "The bar under each screen and window moves it. Extra room makes the bar easier to "
                  + "hit without making it look bigger. Changes apply at once."
        }
        Ft.SliderRow {
            Kirigami.FormData.label: "Extra room above and below:"
            from: 0; to: 50; stepSize: 0.1
            value: backend.grabBarPaddingY
            shown: value.toFixed(1) + " cm"
            onMoved: backend.setGrabBarPaddingY(value)
        }
        Ft.SliderRow {
            Kirigami.FormData.label: "Extra room left and right:"
            from: 0; to: 50; stepSize: 0.1
            value: backend.grabBarPaddingX
            shown: value.toFixed(1) + " cm"
            onMoved: backend.setGrabBarPaddingX(value)
        }
        Ft.Hint {
            text: "Added on each side: 2 cm makes the target 4 cm bigger that way, and 0 is the bar "
                  + "alone. The bar's buttons move aside to stay outside the target."
        }
        Controls.Switch {
            Kirigami.FormData.label: "Buzz:"
            text: "The controller buzzes when its laser reaches a bar"
            checked: backend.grabBarHaptics
            onToggled: backend.setGrabBarHaptics(checked)
        }
        Controls.Switch {
            Kirigami.FormData.label: "Drag zone:"
            text: "Draw the area that counts as the bar"
            checked: backend.grabBarPreview
            onToggled: backend.setGrabBarPreview(checked)
        }
        Ft.Disclosure {
            id: zoneLook
            visible: backend.grabBarPreview
            title: "Drag zone look"
            summary: "colors, outline, when it shows"
        }
        Controls.Switch {
            visible: zoneLook.visible && zoneLook.expanded
            Kirigami.FormData.label: "Keep showing:"
            text: "While the window shows, not only near the bar"
            checked: backend.grabBarPreviewAlways
            onToggled: backend.setGrabBarPreviewAlways(checked)
        }
        Ft.ColorRow {
            visible: zoneLook.visible && zoneLook.expanded
            Kirigami.FormData.label: "Fill color:"
            title: "Drag zone fill color"
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
            title: "Drag zone outline color"
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
            shown: Math.round(value) === 0 ? "no outline" : Math.round(value) + " px"
            onMoved: backend.setGrabBarOutlineWidth(value)
        }
        Ft.Hint {
            visible: zoneLook.visible && zoneLook.expanded
            text: "The zone fades with the bar unless it keeps showing. Outline pixels are those of "
                  + "the bar's own 256×24 picture."
        }

        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Resize corners" }
        Ft.Hint {
            text: "Drag a screen's corner to resize it. The mark shows at the corner you come near."
        }
        Controls.ComboBox {
            Kirigami.FormData.label: "Corner mark:"
            model: ["Sharp outline", "Soft glow (the original)", "SteamOS-like (gray, then white)"]
            currentIndex: backend.resizeCornerStyle === "native" ? 2 : backend.resizeCornerStyle === "glow" ? 1 : 0
            onActivated: backend.setResizeCornerStyle(["sharp", "glow", "native"][currentIndex])
        }
        Ft.Hint {
            text: page.outline
                  ? "A crisp, rounded corner just outside the screen. On the corner itself it changes "
                    + "color, thickens and goes fully bright: ready to drag."
                  : "A soft glow in a square at the corner, fading in as you come near. The outline "
                    + "settings below aren't used, and stay saved for the other marks."
        }
        Controls.Switch {
            Kirigami.FormData.label: "Buzz:"
            text: "The controller buzzes when its laser reaches a corner"
            checked: backend.resizeCornerHaptics
            onToggled: backend.setResizeCornerHaptics(checked)
        }
        Ft.Disclosure {
            id: cornerLook
            visible: page.outline
            title: "Outline look and reach"
            summary: "which corners, size, colors"
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
            Kirigami.FormData.label: "Show targets:"
            text: "Draw the area each corner's laser target covers"
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
            Kirigami.FormData.label: "Color when ready to drag:"
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
            Kirigami.FormData.label: "Roundness:"
            from: 0; to: 100; stepSize: 1
            value: backend.resizeCornerRoundness
            shown: Math.round(value) + "%"
            onMoved: backend.setResizeCornerRoundness(value)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Gap from the screen:"
            from: 0; to: 10; stepSize: 0.1
            value: backend.resizeCornerGap
            shown: value.toFixed(1) + " cm"
            onMoved: backend.setResizeCornerGap(value)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Target room around the mark:"
            from: 0.1; to: 10; stepSize: 0.1
            value: backend.resizeCornerPadding
            shown: value.toFixed(1) + " cm"
            onMoved: backend.setResizeCornerPadding(value)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Kept clear round the screen:"
            from: 0; to: 5; stepSize: 0.1
            value: backend.resizeCornerExclusion
            shown: value.toFixed(1) + " cm"
            onMoved: backend.setResizeCornerExclusion(value)
        }
        Ft.SliderRow {
            visible: cornerLook.visible && cornerLook.expanded
            Kirigami.FormData.label: "Starts showing from:"
            from: 1; to: 50; stepSize: 0.5
            value: backend.resizeCornerReach
            shown: value.toFixed(1) + " cm away"
            onMoved: backend.setResizeCornerReach(value)
        }
        Ft.Hint {
            visible: cornerLook.visible && cornerLook.expanded
            text: "The laser's target follows the mark with the room round it, never a square, and the "
                  + "mark moves outward as needed to keep that target off the screen and the strip kept "
                  + "clear round it, which never resize. Starts showing from only sets when the mark "
                  + "appears. SteamOS-like goes from gray to almost white with 12% longer arms, 20% "
                  + "thicker lines and 3 mm more gap on the corner, without changing your colors. "
                  + "Roundness runs from a square bend to a full round one. Corners set to always show "
                  + "still hide with their screen. The target's size counts for the 3D mouse too; "
                  + "a hand's reach and SteamVR's own window controls are unchanged."
        }
    }
}
