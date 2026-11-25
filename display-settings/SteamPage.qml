// Frametop Settings' Steam & notifications page: where the desktop's notifications show, what
// Frametop does while Steam is locked (docs/steam-pin-lock.md), and how apps picked in the
// SteamVR dashboard open. DisplayPages.qml makes it; its backend is ft_display_settings.py.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "../settings/controls" as Ft

Kirigami.ScrollablePage {
    id: page
    property QtObject backend   // DisplayPages.qml sets it: ft_display_settings.py
    title: "Steam & notifications"
    // The Restart desktop button was pressed (DisplayPages.qml asks first).
    signal restartAsked()

    component RestartRow: RowLayout {
        Controls.Label { text: "Takes effect after restarting the desktop." }
        Controls.Button {
            text: "Restart desktop"
            icon.name: "system-reboot"
            onClicked: page.restartAsked()
        }
    }

    Kirigami.FormLayout {
        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Desktop notifications" }
        Ft.RadioChoices {
            Kirigami.FormData.label: "Show notifications:"
            Kirigami.FormData.labelAlignment: Qt.AlignTop
            choices: [
                { value: "auto", text: "In SteamVR when supported (recommended)", help: "Forwards desktop notifications into VR whenever a VR notification viewer is active, so you can see and click them without looking at a desktop screen. Regular desktop popups stay quiet while forwarding, though notification history still keeps them." },
                // "on" can lose them (SteamOS's server): set in frametop.conf, shown only then
                { value: "on", text: "Always forward to Steam", help: "Forwards notifications even if no VR notification viewer is available, which may cause notifications to be missed." },
                { value: "off", text: "On desktop screens only", help: "Shows standard desktop notification popups directly on your virtual screens." }
            ]
            value: backend.notifyForward
            onChose: backend.setNotifyForward(picked)
        }
        Controls.Label {
            Kirigami.FormData.label: "Current status:"
            visible: backend.desktopRunning
            text: backend.notifyForwardState === "forwarding" ? "Showing in SteamVR"
                : backend.notifyForwardState === "waiting" ? "Showing on desktop screens (no VR notification viewer active)"
                : "Showing on desktop screens"
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 26
        }
        RestartRow { visible: backend.notifyRestartNeeded }

        // Steam's PIN lock (issue 66): Frametop hides with it. Experimental and opt-in;
        // what it turns off and what's still untested: docs/steam-pin-lock.md.
        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Steam lock screen (PIN)" }
        Controls.Switch {
            objectName: "steamPinLock"
            Kirigami.FormData.label: "When Steam is locked:"
            text: "Hide Frametop screens & windows (experimental)"
            enabled: backend.backend === "screens" || backend.steamPinLock
            checked: backend.steamPinLock
            onToggled: backend.setSteamPinLock(checked)
        }
        Ft.Hint {
            text: "Whenever Steam prompts for your Security PIN, all Frametop screens, windows, and the "
                  + "VR keyboard hide and stop accepting input, while your apps keep running safely in "
                  + "the background. Remote access and notification forwarding also stay off while this "
                  + "protection is enabled. Right after the headset boots, Frametop may show for about a "
                  + "second before Steam's PIN screen does."
        }
        RestartRow { visible: backend.steamPinLockRestartNeeded }

        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Apps launched from SteamVR" }
        Ft.RadioChoices {
            Kirigami.FormData.label: "Open dashboard apps:"
            Kirigami.FormData.labelAlignment: Qt.AlignTop
            enabled: backend.floatSlots > 0
            choices: [
                { value: "steamvr", text: "As standard SteamVR windows", help: "Apps you open from SteamVR's Launch a program list appear as regular SteamVR overlays, just like without Frametop." },
                { value: "frametop", text: "As Frametop floating windows", help: "Opens apps in the Frametop desktop floating in front of you, so copy/paste, drag-and-drop, Frunner, and saved profiles work seamlessly. Falls back to SteamVR windows if the desktop isn't running or is paused." }
            ]
            value: backend.dashboardApps
            onChose: backend.setDashboardApps(picked)
        }
        Ft.Hint {
            visible: backend.floatSlots === 0
            text: "Floating windows are currently disabled in your configuration (FLOAT_SLOTS=0), so dashboard apps will open in SteamVR."
        }
    }
}
