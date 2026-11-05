// The recorders' Upload page: log in to Hugging Face, choose an export and send it as a pull request.
// Both windows use it; what differs between them comes in as properties.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: uploadView
    title: "Upload"
    // The window (its show(), plural(), exportPending(), chosenSession, draftText and, for offerFree, askFree()),
    // and its Export page, for "Nothing exported yet".
    property var recorder
    property var exportComponent
    // What a recording is called (session, round), and the recorder's name in its texts.
    property string itemNoun
    property string recorderName
    // The dataset an export goes to, its page and whether uploads are open: per export (the hand
    // recorder's testing sessions go to the test dataset) or the same for all.
    property var datasetFor: function (session) { return "" }
    property var datasetUrlFor: function (session) { return "" }
    property var uploadAllowedFor: function (session) { return false }
    // The rehearsal banner: the variable that allows uploads, and how it ends.
    property string allowUploadEnv
    property string rehearsalEnd
    // What follows "...as a pull request from your own Hugging Face account."
    property string reviewText
    // Show the login text while the login code waits (the eye recorder does, the hand recorder not).
    property bool loginTextWhileWaiting: false
    // After an upload, offer to delete the local copy (recorder.askFree).
    property bool offerFree: false
    readonly property var exported: backend.sessions.filter(s => s.exported)
    readonly property string session: exportBox.currentIndex >= 0 ? exportBox.currentValue || "" : ""
    readonly property var chosen: exportBox.currentIndex >= 0 && exportBox.currentIndex < exported.length
                                  ? exported[exportBox.currentIndex] : null
    readonly property var up: backend.upload
    readonly property var upError: up.error || ({})
    readonly property var upResult: up.result || ({})
    // The upload shown is this export's (the window keeps the last one's result).
    readonly property bool mine: up.session === session
    property var info: ({ previous: {}, uploads: 0 })
    readonly property bool loggedIn: backend.login.state === "ok"
    // Why Upload is off, or "" when it can go.
    readonly property string blocked: {
        if (session === "") return "Choose an export"
        if (chosen && chosen.export_stale) return "Export the " + itemNoun + " again first"
        if (uploadView.recorder.exportPending(session)) return "Wait for its export to finish"
        if (backend.uploading) return ""
        if (backend.hubDryRun) return ""
        if (!uploadAllowedFor(session)) return "Contributions aren't open yet"
        if (!loggedIn) return "Log in first (step 2)"
        return ""
    }

    function refreshInfo() { info = backend.uploadInfo(session) }
    onSessionChanged: refreshInfo()
    Connections {
        target: backend
        function onSessionsChanged() { uploadView.refreshInfo() }
        // An upload started by itself (queued on the Export page): show it.
        function onUploadChanged() {
            const i = backend.uploading ? exportBox.indexOfValue(backend.upload.session) : -1
            if (i >= 0 && i !== exportBox.currentIndex) {
                exportBox.currentIndex = i
                exportBox.keep = backend.upload.session
            }
        }
    }
    Component.onCompleted: {
        refreshInfo()
        if (backend.login.state === "unknown") backend.checkLogin()
    }

    function startUpload() {
        if (info.previous && info.previous.export_sha)
            confirm.ask("Upload this export again?",
                        "It was uploaded on " + info.previous.uploaded + " (" + (info.previous.pr_url || "no link")
                        + "). Uploading it again opens a second pull request.",
                        () => backend.startUpload(uploadView.session, true))
        else
            backend.startUpload(session, false)
    }

    header: ColumnLayout {
        spacing: 0
        DraftBanner { Layout.fillWidth: true; text: uploadView.recorder.draftText }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: backend.hubDryRun
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Information
            text: "Dry run (--hub-dry-run): Upload checks the export and lists what it would send. "
                  + "Nothing goes over the network."
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: backend.textsDraft && backend.allowUploadSet && backend.uploadAllowed && !backend.hubDryRun
            position: Kirigami.InlineMessage.Position.Header
            type: Kirigami.MessageType.Warning
            text: "Rehearsal: " + uploadView.allowUploadEnv + "=1 allows uploads to " + uploadView.datasetFor(uploadView.session)
                  + " " + uploadView.rehearsalEnd
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: uploadView.exported.length === 0
            icon.name: "document-export"
            text: "Nothing exported yet"
            explanation: "Review a " + uploadView.itemNoun + ", then export it. It can be uploaded from here."
            helpfulAction: Kirigami.Action {
                text: "Export"; icon.name: "document-export"
                onTriggered: uploadView.recorder.show(uploadView.exportComponent)
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: uploadView.exported.length > 0
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.StyledText
                text: "Your export goes to <a href=\"" + uploadView.datasetUrlFor(uploadView.session) + "\">" + uploadView.datasetFor(uploadView.session) + "</a> as a "
                      + "pull request from your own Hugging Face account. " + uploadView.reviewText
                onLinkActivated: link => Qt.openUrlExternally(link)
            }

            // 1. The export
            Kirigami.Heading { level: 3; text: "1. Choose the export" }
            RowLayout {
                Layout.fillWidth: true
                Controls.ComboBox {
                    id: exportBox
                    Layout.fillWidth: true
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                    model: uploadView.exported
                    textRole: "label"
                    valueRole: "id"
                    enabled: !backend.uploading
                    // An export that finishes meanwhile adds a row (newest first): stay on the
                    // same export, the uploading one while an upload runs.
                    property string keep: ""
                    Component.onCompleted: {
                        currentIndex = Math.max(0, indexOfValue(uploadView.recorder.chosenSession))
                        keep = currentValue || ""
                    }
                    onActivated: uploadView.recorder.chosenSession = keep = currentValue
                    onModelChanged: Qt.callLater(() => {
                        const i = indexOfValue(backend.uploading ? backend.upload.session : keep)
                        currentIndex = i >= 0 ? i : (count > 0 ? Math.max(0, currentIndex) : -1)
                        keep = currentValue || ""
                    })
                }
                Controls.Label {
                    opacity: 0.7
                    text: uploadView.chosen ? uploadView.chosen.exportText : ""
                }
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: uploadView.chosen !== null && uploadView.chosen.export_stale
                type: Kirigami.MessageType.Warning
                text: "This " + uploadView.itemNoun + " changed since it was exported. Export it again so the upload has your latest deletions."
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: uploadView.session !== "" && uploadView.info.previous.export_sha !== undefined
                type: Kirigami.MessageType.Information
                text: "This export was uploaded on " + (uploadView.info.previous.uploaded || "") + ": "
                      + (uploadView.info.previous.pr_url || "") + ". There's no need to upload it again."
            }

            // 2. The login: Log in opens Hugging Face in the browser, where the person approves
            // it. The token goes from there to hub.py, never into this window.
            Kirigami.Heading { level: 3; text: "2. Log in to Hugging Face" }
            RowLayout {
                Layout.fillWidth: true
                visible: loginState !== "waiting"
                readonly property string loginState: backend.login.state
                Kirigami.Icon {
                    visible: parent.loginState === "ok"
                    source: "checkmark"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: implicitWidth
                }
                Controls.BusyIndicator {
                    visible: ["checking", "starting", "unknown"].indexOf(parent.loginState) >= 0
                    running: visible
                    implicitWidth: Kirigami.Units.gridUnit * 1.5
                    implicitHeight: implicitWidth
                }
                Controls.Label {
                    Layout.fillWidth: true
                    visible: uploadView.loginTextWhileWaiting || parent.loginState !== "waiting"
                    wrapMode: Text.Wrap
                    text: parent.loginState === "dry" ? "Not needed for a dry run."
                          : parent.loginState === "unknown" ? "Checking your login"
                          : parent.loginState === "none"
                            ? "Press Log in. Hugging Face opens in your browser: sign in, or make a free "
                              + "account, then enter the code this page shows."
                          : backend.login.text || ""
                }
                Controls.Button {
                    visible: ["none", "read", "error", "ok"].indexOf(parent.loginState) >= 0
                    text: parent.loginState === "ok" ? "Use another account" : "Log in"
                    icon.name: parent.loginState === "ok" ? "system-switch-user" : "im-user-online"
                    flat: parent.loginState === "ok"
                    enabled: !backend.uploading
                    onClicked: backend.logIn()
                }
                Controls.Button {
                    visible: parent.loginState === "error"
                    text: "Check again"
                    icon.name: "view-refresh"
                    onClicked: backend.checkLogin()
                }
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: backend.login.state === "waiting"
                type: Kirigami.MessageType.Information
                text: "Hugging Face is open in your browser (" + (backend.login.url || "") + "). Sign in, or "
                      + "make a free account, enter the code below and approve the login. If it asks about "
                      + "organizations, leave them unticked. The code works for "
                      + Math.round((backend.login.expires_in || 300) / 60) + " minutes; this page carries on "
                      + "by itself once you've approved."
                actions: [openLogin, copyLogin, cancelLogin]
                Kirigami.Action {
                    id: openLogin
                    text: "Open again"
                    icon.name: "internet-services"
                    onTriggered: Qt.openUrlExternally(backend.login.url)
                }
                Kirigami.Action {
                    id: copyLogin
                    text: "Copy code"
                    icon.name: "edit-copy"
                    onTriggered: backend.copy(backend.login.code)
                }
                Kirigami.Action {
                    id: cancelLogin
                    text: "Cancel"
                    icon.name: "dialog-cancel"
                    onTriggered: backend.cancelLogin()
                }
            }
            Controls.Label {
                visible: backend.login.state === "waiting"
                Layout.leftMargin: Kirigami.Units.gridUnit
                text: backend.login.code || ""
                font.family: "monospace"
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 2.5
                font.letterSpacing: 4
            }

            // 3. The upload
            Kirigami.Heading { level: 3; text: "3. Upload" }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.StyledText
                text: "The first time, accept the dataset's terms on <a href=\"" + uploadView.datasetUrlFor(uploadView.session)
                      + "\">its page</a>. Upload checks the export, opens your pull request, then sends the files."
                onLinkActivated: link => Qt.openUrlExternally(link)
            }
            Controls.Label {
                Layout.fillWidth: true
                visible: backend.uploadQueue.length + backend.uploadAfter.length > 0
                wrapMode: Text.Wrap
                opacity: 0.7
                text: "Queued on the Export page to upload after this one, by themselves: "
                      + uploadView.recorder.plural(backend.uploadQueue.length + backend.uploadAfter.length, "export") + "."
            }
            RowLayout {
                Controls.Button {
                    text: backend.uploading ? "Uploading…" : backend.hubDryRun ? "Upload (dry run)" : "Upload"
                    icon.name: "cloud-upload"
                    enabled: uploadView.blocked === "" && !backend.uploading
                    onClicked: uploadView.startUpload()
                }
                Controls.BusyIndicator {
                    visible: backend.uploading
                    running: visible
                    implicitWidth: Kirigami.Units.gridUnit * 1.5
                    implicitHeight: implicitWidth
                }
                Controls.Button {
                    visible: backend.uploading
                    text: "Cancel"
                    icon.name: "dialog-cancel"
                    onClicked: backend.cancelUpload()
                }
                Controls.Label {
                    visible: !backend.uploading && uploadView.blocked !== ""
                    opacity: 0.7
                    text: uploadView.blocked
                }
            }

            // Progress: a share while the export is checked, a sweeping bar while it's sent
            // (huggingface_hub doesn't report progress). Drawn here, as on the Export page.
            Rectangle {
                id: track
                Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                implicitHeight: Kirigami.Units.smallSpacing * 2
                visible: backend.uploading
                radius: height / 2
                clip: true
                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.15)
                readonly property bool unknown: uploadView.up.fraction === undefined || uploadView.up.fraction < 0
                Rectangle {
                    id: fill
                    height: parent.height
                    radius: parent.radius
                    color: Kirigami.Theme.highlightColor
                    width: track.unknown ? parent.width / 4 : parent.width * Math.max(0, Math.min(1, uploadView.up.fraction))
                    x: 0
                    SequentialAnimation on x {
                        running: track.visible && track.unknown
                        loops: Animation.Infinite
                        onRunningChanged: if (!running) fill.x = 0
                        NumberAnimation { from: -fill.width; to: track.width; duration: 1600 }
                    }
                }
            }
            Controls.Label {
                Layout.fillWidth: true
                visible: uploadView.mine && (uploadView.up.text || "") !== ""
                         && (backend.uploading || uploadView.up.phase === "failed" && uploadView.upError.kind === "cancelled")
                wrapMode: Text.Wrap
                text: uploadView.up.text || ""
            }

            // The pull request is open and the files are on their way: time to plug in.
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: backend.uploading && uploadView.mine && (uploadView.up.pr_url || "") !== ""
                type: Kirigami.MessageType.Positive
                text: "Your pull request is open: " + (uploadView.up.pr_url || "") + ". The files are uploading "
                      + "to it now, which can take a while. Plug in the headset and leave it plugged in until "
                      + "this page says Uploaded. You can take the headset off: it stays awake until the upload "
                      + "is done. Keep the " + uploadView.recorderName + " open."
                actions: [openOpenedPr, copyOpenedPr]
                Kirigami.Action {
                    id: openOpenedPr
                    text: "Open"
                    icon.name: "internet-services"
                    onTriggered: Qt.openUrlExternally(uploadView.up.pr_url)
                }
                Kirigami.Action {
                    id: copyOpenedPr
                    text: "Copy link"
                    icon.name: "edit-copy"
                    onTriggered: backend.copy(uploadView.up.pr_url)
                }
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: !backend.uploading && uploadView.mine && uploadView.up.phase === "done"
                type: Kirigami.MessageType.Positive
                text: uploadView.upResult.dry_run
                      ? "Dry run: the export passed its checks. It would go to " + uploadView.upResult.repo + "/"
                        + uploadView.upResult.path_in_repo + " (" + uploadView.upResult.files + " files). Nothing was sent."
                      : "Uploaded. Your pull request: " + (uploadView.upResult.pr_url || "")
                        + ". The maintainer reviews it before it joins the dataset."
                actions: uploadView.upResult.pr_url ? [openPr, copyPr] : []
                Kirigami.Action {
                    id: openPr
                    text: "Open"
                    icon.name: "internet-services"
                    onTriggered: Qt.openUrlExternally(uploadView.upResult.pr_url)
                }
                Kirigami.Action {
                    id: copyPr
                    text: "Copy link"
                    icon.name: "edit-copy"
                    onTriggered: backend.copy(uploadView.upResult.pr_url)
                }
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: uploadView.offerFree && !backend.uploading && uploadView.mine && uploadView.up.phase === "done"
                         && !uploadView.upResult.dry_run && uploadView.chosen !== null && uploadView.chosen.freeable
                type: Kirigami.MessageType.Information
                text: "This round doesn't need to stay on the headset now. Deleting its local copy frees "
                      + (uploadView.chosen ? uploadView.chosen.freeText : "") + "; your record of the upload "
                      + "stays on the Review page."
                actions: [
                    Kirigami.Action {
                        text: "Delete local copy"
                        icon.name: "edit-clear-all"
                        onTriggered: uploadView.recorder.askFree(uploadView.chosen, null)
                    }
                ]
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: !backend.uploading && uploadView.mine && uploadView.up.phase === "failed"
                         && uploadView.upError.kind !== "cancelled"
                type: Kirigami.MessageType.Error
                text: (uploadView.upError.text || "")
                      + ((uploadView.upError.errors || []).length ? "\n\n• " + uploadView.upError.errors.join("\n• ") : "")
                actions: uploadView.upError.link ? [openErrorLink] : []
                Kirigami.Action {
                    id: openErrorLink
                    text: "Open"
                    icon.name: "internet-services"
                    onTriggered: Qt.openUrlExternally(uploadView.upError.link)
                }
            }
            Controls.TextArea {
                Layout.fillWidth: true
                visible: uploadView.mine && (uploadView.up.log || "") !== ""
                readOnly: true
                selectByMouse: true
                wrapMode: Text.WrapAnywhere
                font.family: "monospace"
                text: uploadView.up.log || ""
            }

            Kirigami.Separator { Layout.fillWidth: true; visible: uploadView.session !== "" }
            MarkdownText {
                Layout.fillWidth: true
                visible: uploadView.session !== ""
                markdown: backend.uploadText(uploadView.session)
            }
        }
    }
}
