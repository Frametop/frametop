// The recorders' Export page: the chosen session's or round's export, the export and upload queues,
// and how they went. Both windows use it; what differs comes in as properties.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: exportView
    // The window (its pageStack and helpers), and its Upload page.
    property var recorder
    property var uploadComponent
    // What a recording is called (session, round).
    property string itemNoun
    // Why nothing can be exported, or "".
    property string exportProblem: ""
    // The window sends the export on to the Upload page once it's done (Hand Recorder).
    property bool sendingAfterExport: false
    // How long an export keeps the processor busy, and whether that banner comes first (the Eye
    // Recorder) or after the stays-awake note (the Hand Recorder).
    property string busyTime
    property bool busyFirst: false
    property var cancelExport: () => backend.cancelExport()
    // After a failed export, offer to delete the uploaded rounds that free this much
    // (recorder.askFreeAll). The Eye Recorder leaves it at none.
    property int freeableCount: 0
    property string freeableText: ""
    property string aboutText
    readonly property string itemLabel: itemNoun.charAt(0).toUpperCase() + itemNoun.slice(1) + ":"
    title: "Export"
    property bool worn: backend.headsetWorn()
    readonly property var chosen: {
        const list = backend.sessions
        for (const s of list)
            if (s.id === sessionBox.currentValue)
                return s
        return null
    }
    // The chosen session or round is exporting, or waiting to.
    readonly property bool pending: chosen !== null && exportView.recorder.exportPending(chosen.id)
    readonly property bool canExport: chosen !== null && !pending && exportProblem === "" && !chosen.active
                                      && !(backend.uploading && backend.upload.session === chosen.id)
                                      && backend.uploadQueue.indexOf(chosen.id) < 0
    Component.onCompleted: if (backend.login.state === "unknown") backend.checkLogin()

    Timer {
        interval: 5000
        running: true
        repeat: true
        onTriggered: exportView.worn = backend.headsetWorn()
    }

    header: ColumnLayout {
        spacing: 0
        DraftBanner { Layout.fillWidth: true; text: exportView.recorder.draftText }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: exportView.busyFirst && exportView.worn && backend.exporting
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Information
            text: "Exporting keeps the processor busy for " + exportView.busyTime + ", so VR may stutter a little until "
                  + "it's done. You can keep using the headset meanwhile."
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: exportView.sendingAfterExport
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Information
            text: "Exporting your " + exportView.itemNoun + ": the Upload page follows by itself once it's done."
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: backend.uploadAfter.length + backend.uploadQueue.length > 0
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Positive
            text: "You can take the headset off and leave it plugged in: it stays awake until the "
                  + "exports and uploads below are done, and they show here how they went. Keep this "
                  + "window open: closing it cancels them."
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !exportView.busyFirst && exportView.worn && backend.exporting
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Information
            text: "Exporting keeps the processor busy for " + exportView.busyTime + ", so VR may stutter a little until "
                  + "it's done. You can keep using the headset meanwhile."
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: exportView.exportProblem !== ""
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Error
            text: exportView.exportProblem
        }
    }

    Kirigami.FormLayout {
        Controls.ComboBox {
            id: sessionBox
            Kirigami.FormData.label: exportView.itemLabel
            model: backend.sessions
            textRole: "label"
            valueRole: "id"
            Component.onCompleted: {
                const i = indexOfValue(exportView.recorder.chosenSession)
                currentIndex = i >= 0 ? i : (count > 0 ? 0 : -1)
            }
            onActivated: exportView.recorder.chosenSession = currentValue
        }
        Controls.Label {
            Kirigami.FormData.label: "Recorded:"
            visible: exportView.chosen !== null
            text: exportView.chosen ? exportView.recorder.plural(exportView.chosen.takes, "take") + ", " + exportView.chosen.sizeText : ""
        }
        Controls.Label {
            Kirigami.FormData.label: "Export:"
            visible: exportView.chosen !== null
            text: !exportView.chosen ? ""
                  : exportView.pending ? (backend.exportSessionId === exportView.chosen.id && backend.exporting
                                          ? "exporting now" : "waiting its turn, below")
                  : exportView.chosen.export_stale ? "changed since it was exported: export it again"
                  : exportView.chosen.exported ? "ready, " + exportView.chosen.exportText : "not exported yet"
        }
        // Exports run one at a time; one asked for meanwhile waits its turn. "Export and
        // upload" sends it by itself once it's exported.
        RowLayout {
            Controls.Button {
                text: exportView.chosen && exportView.chosen.exported ? "Export again" : "Export"
                icon.name: "document-export"
                enabled: exportView.canExport
                onClicked: backend.queueExport(exportView.chosen.id, false, false)
            }
            Controls.Button {
                text: "Export and upload"
                icon.name: "cloud-upload"
                enabled: exportView.canExport && exportView.recorder.canUploadAfter(exportView.chosen.id)
                onClicked: backend.queueExport(exportView.chosen.id, false, true)
            }
        }
        Controls.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 26
            visible: exportView.chosen !== null && !backend.hubDryRun
                     && (!backend.canUploadAfter || !backend.uploadAllowedFor(exportView.chosen.id))
            wrapMode: Text.Wrap
            opacity: 0.7
            text: !backend.canUploadAfter ? "To upload by itself once exported, log in on the Upload page first."
                  : "Contributions aren't open yet, so it can't upload by itself."
        }
        Controls.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 26
            visible: backend.exporting && exportView.chosen !== null && !exportView.pending
            wrapMode: Text.Wrap
            opacity: 0.7
            text: "Another export is running: this one waits its turn."
        }

        // The running export
        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: "Exporting now"
            visible: backend.exporting
        }
        RowLayout {
            Kirigami.FormData.label: exportView.itemLabel
            visible: backend.exporting
            Controls.Label { text: exportView.recorder.sessionLabel(backend.exportSessionId) }
            // Shows the moment Export is pressed: the first progress can take a few seconds.
            Controls.BusyIndicator {
                running: backend.exporting
                implicitWidth: Kirigami.Units.gridUnit * 1.5
                implicitHeight: implicitWidth
            }
            Controls.Button {
                text: "Cancel"
                icon.name: "dialog-cancel"
                onClicked: exportView.cancelExport()
            }
        }
        Controls.CheckBox {
            visible: backend.exporting
            text: "Upload it once exported"
            checked: backend.uploadAfter.indexOf(backend.exportSessionId) >= 0
            enabled: checked || exportView.recorder.canUploadAfter(backend.exportSessionId)
            onToggled: backend.setUploadAfter(backend.exportSessionId, checked)
        }
        // Drawn here: the desktop style's ProgressBar paints nothing without a QApplication.
        Rectangle {
            Kirigami.FormData.label: "Progress:"
            Layout.preferredWidth: Kirigami.Units.gridUnit * 20
            implicitHeight: Kirigami.Units.smallSpacing * 2
            visible: backend.exporting
            radius: height / 2
            color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.15)
            Rectangle {
                width: parent.width * Math.max(0, Math.min(1, backend.exportFraction))
                height: parent.height
                radius: parent.radius
                color: Kirigami.Theme.highlightColor
            }
        }
        Controls.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 26
            visible: backend.exportText !== "" && (backend.exporting || backend.exportSessionId === sessionBox.currentValue)
            wrapMode: Text.WrapAnywhere
            text: backend.exportText
        }
        Kirigami.InlineMessage {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 26
            Layout.fillWidth: true
            visible: !backend.exporting && backend.exportText.indexOf("Failed") === 0 && exportView.freeableCount > 0
            type: Kirigami.MessageType.Information
            text: exportView.recorder.plural(exportView.freeableCount, "uploaded round") + " can be deleted to free "
                  + exportView.freeableText + ". Your record of each upload stays."
            actions: [
                Kirigami.Action {
                    text: "Delete uploaded rounds"
                    icon.name: "edit-delete"
                    onTriggered: exportView.recorder.askFreeAll()
                }
            ]
        }
        RowLayout {
            visible: exportView.chosen !== null && exportView.chosen.exported && !exportView.pending
            Controls.Button {
                text: "Upload"
                icon.name: "go-next"
                onClicked: exportView.recorder.show(exportView.uploadComponent)
            }
            Controls.Button {
                text: "Delete export"
                icon.name: "edit-delete"
                enabled: !(backend.uploading && backend.upload.session === exportView.chosen.id)
                         && backend.uploadQueue.indexOf(exportView.chosen.id) < 0
                onClicked: {
                    const id = exportView.chosen.id
                    confirm.ask("Delete this export?", "The export is deleted; the " + exportView.itemNoun
                                + "'s recordings stay.", () => backend.deleteExport(id))
                }
            }
        }

        // Waiting: exports, then uploads
        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: "Waiting to export"
            visible: backend.exportQueue.length > 0
        }
        Repeater {
            model: backend.exportQueue
            delegate: RowLayout {
                required property var modelData
                required property int index
                Kirigami.FormData.label: (index + 1) + "."
                Controls.Label { text: exportView.recorder.sessionLabel(modelData.session) }
                Controls.CheckBox {
                    text: "then upload"
                    checked: modelData.upload
                    enabled: checked || exportView.recorder.canUploadAfter(modelData.session)
                    onToggled: backend.setUploadAfter(modelData.session, checked)
                }
                Controls.ToolButton {
                    icon.name: "list-remove"
                    text: "Remove"
                    display: Controls.AbstractButton.IconOnly
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered
                    onClicked: backend.unqueueExport(modelData.session)
                }
            }
        }
        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: "Uploading"
            visible: backend.uploading || backend.uploadQueue.length > 0
        }
        RowLayout {
            Kirigami.FormData.label: "Now:"
            visible: backend.uploading
            Controls.Label {
                text: exportView.recorder.sessionLabel(backend.upload.session || "") + (backend.upload.text ? ": " + backend.upload.text : "")
            }
            Controls.Button {
                text: "Show"
                icon.name: "go-next"
                flat: true
                onClicked: { exportView.recorder.chosenSession = backend.upload.session; exportView.recorder.show(exportView.uploadComponent) }
            }
        }
        Repeater {
            model: backend.uploadQueue
            delegate: RowLayout {
                required property var modelData
                required property int index
                Kirigami.FormData.label: "Then:"
                Controls.Label { text: exportView.recorder.sessionLabel(modelData) }
                Controls.ToolButton {
                    icon.name: "list-remove"
                    text: "Don't upload it"
                    display: Controls.AbstractButton.IconOnly
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered
                    onClicked: backend.unqueueUpload(modelData)
                }
            }
        }

        // How they went, for whoever comes back to the headset
        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: "Done"
            visible: backend.doneLog.length > 0
        }
        Repeater {
            model: backend.doneLog
            delegate: RowLayout {
                required property var modelData
                Kirigami.Icon {
                    source: modelData.error ? "dialog-error" : "checkmark"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: implicitWidth
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                    wrapMode: Text.Wrap
                    text: modelData.text
                }
                Controls.ToolButton {
                    visible: modelData.link !== ""
                    icon.name: "internet-services"
                    text: "Open"
                    onClicked: Qt.openUrlExternally(modelData.link)
                }
            }
        }
        Controls.Button {
            visible: backend.doneLog.length > 0
            text: "Clear"
            icon.name: "edit-clear-history"
            flat: true
            onClicked: backend.clearDoneLog()
        }
    }

    footer: Controls.Label {
        padding: Kirigami.Units.largeSpacing
        wrapMode: Text.Wrap
        opacity: 0.7
        text: exportView.aboutText
    }
}
