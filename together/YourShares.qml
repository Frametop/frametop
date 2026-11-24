// What the room gets of yours, on Frametop Settings' Together page (TogetherPages.qml uses it):
// your voice, your avatar, where you look and your screens. Backend: the page's own, its
// `gaze` (ft_together_gaze.py) for where you look, and its `quality` (ft_together_quality.py)
// for how good the video of your screens may get.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: shares
    required property QtObject backend
    readonly property QtObject gaze: backend.gaze
    readonly property QtObject quality: backend.quality
    // "Let them have it…" was pressed: the page asks whether its person means it.
    signal allowAvatar()
    spacing: Kirigami.Units.smallSpacing

    Kirigami.Heading {
        level: 3
        text: "What you share"
    }
    Kirigami.FormLayout {
        Layout.fillWidth: true

        Controls.Switch {
            objectName: "muteSwitch"
            Kirigami.FormData.label: "Your voice:"
            text: shares.backend.voiceText
            enabled: shares.backend.voiceUp
            checked: shares.backend.voiceUp && !shares.backend.muted
            onToggled: {
                shares.backend.setMuted(!checked)
                checked = Qt.binding(() => shares.backend.voiceUp && !shares.backend.muted)
            }
        }
        RowLayout {
            Kirigami.FormData.label: "Your avatar:"
            spacing: Kirigami.Units.largeSpacing
            Controls.Label {
                objectName: "avatarLabel"
                Layout.maximumWidth: Kirigami.Units.gridUnit * 18
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: shares.backend.avatarText
            }
            Controls.Button {
                objectName: "allowAvatarButton"
                visible: shares.backend.avatarAllow
                text: "Allow sharing…"
                onClicked: shares.allowAvatar()
            }
        }
        Controls.Switch {
            objectName: "gazeSwitch"
            Kirigami.FormData.label: "Where you look:"
            text: shares.gaze.text
            enabled: shares.gaze.can
            checked: shares.gaze.shared
            onToggled: {
                shares.gaze.setShared(checked)
                checked = Qt.binding(() => shares.gaze.shared)
            }
        }
        Controls.Label {
            objectName: "gazeNote"
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            wrapMode: Text.Wrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            text: "Off by default and applies only to this room. Only your gaze direction is sent, never a picture of your eyes. Others see your avatar's eyes move and a faint mark on the panel you look at."
        }
        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: "Your screens"
        }
        Repeater {
            objectName: "shares"
            model: shares.backend.shares
            delegate: Controls.Switch {
                id: shareSwitch
                required property var modelData
                text: shareSwitch.modelData.title + (shareSwitch.modelData.shared && !shareSwitch.modelData.live ? ": " + shareSwitch.modelData.wait : "")
                enabled: shareSwitch.modelData.can
                checked: shareSwitch.modelData.shared
                onToggled: {
                    shares.backend.setShared(shareSwitch.modelData.screen, checked)
                    checked = Qt.binding(() => shareSwitch.modelData.shared)
                }
            }
        }
        Controls.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            wrapMode: Text.Wrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            text: shares.backend.shares.length === 0
                  ? "Your desktop screens appear here while the Frametop desktop is running."
                  : "Share up to three desktop screens at a time. Others can see them, and can only click or type "
                    + "if you grant them control. You can also share up to four remote PC windows directly from your PC."
        }
        Controls.ComboBox {
            objectName: "qualityBox"
            Kirigami.FormData.label: "Stream quality:"
            model: shares.quality.names
            enabled: shares.quality.can
            currentIndex: shares.quality.index
            onActivated: index => {
                shares.quality.setIndex(index)
                currentIndex = Qt.binding(() => shares.quality.index)
            }
        }
        Controls.Label {
            objectName: "qualityNote"
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            wrapMode: Text.Wrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            text: shares.quality.text + " maximum. Quality lowers automatically — it gets less by itself on a slow link or when nobody is looking directly at the screen."
        }
    }
}
