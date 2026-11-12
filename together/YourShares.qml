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
                text: "Let them have it…"
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
            text: "Off until you turn it on, and for this room only. Only a direction leaves this headset, never a picture of your eyes. The others see your avatar's eyes turn, and a faint mark on the panel you look at."
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
                  ? "The desktop's screens show here while the Frametop desktop runs."
                  : "Up to three at a time. The others see a shared screen where it stands in your room; "
                    + "they can click and type on it only once you give it to them. Windows of your PC "
                    + "that stand here as panels can be shared as well, up to four: your PC sends them itself."
        }
        Controls.ComboBox {
            objectName: "qualityBox"
            Kirigami.FormData.label: "Their video:"
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
            text: shares.quality.text + ". That is the most it uses: it gets less by itself on a slow link, and while nobody looks straight at the screen."
        }
    }
}
