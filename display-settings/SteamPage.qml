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
        Controls.Label { text: "Applies when the desktop starts again." }
        Controls.Button {
            text: "Restart desktop"
            icon.name: "system-reboot"
            onClicked: page.restartAsked()
        }
    }

    Kirigami.FormLayout {
        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Notifications" }
        Ft.RadioChoices {
            Kirigami.FormData.label: "Where they show:"
            Kirigami.FormData.labelAlignment: Qt.AlignTop
            choices: [
                { value: "auto", text: "Show them in the Steam session", help: "The desktop's notifications go to the Steam session's notification server, such as one that shows them in VR, with their buttons. Plasma's own popups stay off meanwhile; its history keeps them. Not to SteamOS's own server, which can't show them on the Frame." },
                // "on" can lose them (SteamOS's server): set in frametop.conf, shown only then
                { value: "on", text: "Always send them to the Steam session", hidden: backend.notifyForward !== "on", help: "Whatever serves notifications there, SteamOS's own server too, which can't show them on the Frame: they're lost then." },
                { value: "off", text: "Keep them in the desktop", help: "Plasma shows them on the desktop's screens." }
            ]
            value: backend.notifyForward
            onChose: backend.setNotifyForward(picked)
        }
        Controls.Label {
            Kirigami.FormData.label: "Now:"
            visible: backend.desktopRunning
            text: backend.notifyForwardState === "forwarding" ? "In the Steam session"
                : backend.notifyForwardState === "waiting" ? "In the desktop: the Steam session has no notification server that can show them"
                : "In the desktop"
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 26
        }
        RestartRow { visible: backend.notifyRestartNeeded }

        // Steam's PIN lock (issue 66): Frametop hides with it. Experimental and opt-in;
        // what it turns off and what's still untested: docs/steam-pin-lock.md.
        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Steam's lock screen" }
        Controls.Switch {
            objectName: "steamPinLock"
            Kirigami.FormData.label: "While Steam is locked:"
            text: "Hide Frametop (experimental)"
            enabled: backend.backend === "screens" || backend.steamPinLock
            checked: backend.steamPinLock
            onToggled: backend.setSteamPinLock(checked)
        }
        Ft.Hint {
            text: "While Steam asks for its PIN (a PIN set in Steam's Security settings), every Frametop "
                  + "screen, window and keyboard hides and takes no input; apps keep running and keep "
                  + "your work. Remote access to this desktop and sending its notifications to the Steam "
                  + "session stay off while this is on. Turned on in an unlocked session, Frametop stays "
                  + "hidden until Steam locks and unlocks once. Needs the screens backend."
        }
        RestartRow { visible: backend.steamPinLockRestartNeeded }

        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Apps from the SteamVR dashboard" }
        Ft.RadioChoices {
            Kirigami.FormData.label: "Launch a program:"
            Kirigami.FormData.labelAlignment: Qt.AlignTop
            enabled: backend.floatSlots > 0
            choices: [
                { value: "steamvr", text: "Open as SteamVR windows", help: "Apps picked in the dashboard's Launch a program list open as SteamVR's own windows, as without Frametop." },
                { value: "frametop", text: "Open as Frametop floating windows", help: "They open in the Frametop desktop instead, floating in front of you (where that app last floated), so drag and drop, the clipboard, frunner and profiles work with them. When the desktop isn't running, Frametop is paused, or all floating windows are in use, they open as SteamVR windows. Steam, SteamVR and Frametop's own layout entries always stay SteamVR's." }
            ]
            value: backend.dashboardApps
            onChose: backend.setDashboardApps(picked)
        }
        Ft.Hint {
            visible: backend.floatSlots === 0
            text: "Floating windows are off (FLOAT_SLOTS=0 in ~/.config/frametop.conf), so these open in SteamVR."
        }
    }
}
