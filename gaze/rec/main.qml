// Frametop Eye Recorder (Kirigami). Backend: ft_eyerec.py ("backend"); eye frames come from its
// image provider (image://eyes/...). Copied from the hand recorder's window (hands/rec/main.qml)
// and changed for rounds picked from the bounty board, the fit card and the eye cameras.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "../../recorder/qml"

Kirigami.ApplicationWindow {
    id: root
    title: "Frametop Eye Recorder"
    width: Kirigami.Units.gridUnit * 50
    height: Kirigami.Units.gridUnit * 36

    // The bounty picked on the board, for the checklist's Start.
    property string chosenBounty: ""
    // The round chosen on the Export page, for the Upload page.
    property string chosenSession: ""

    globalDrawer: Kirigami.GlobalDrawer {
        isMenu: false
        modal: false
        collapsible: true
        collapsed: root.width < Kirigami.Units.gridUnit * 34
        actions: [
            Kirigami.Action { text: "Welcome"; icon.name: "help-about"; onTriggered: root.show(welcomePage) },
            Kirigami.Action {
                text: "Bounties"; icon.name: "view-list-details"; enabled: !backend.needsConsent
                onTriggered: root.show(boardPage)
            },
            Kirigami.Action {
                text: "Before you start"; icon.name: "view-task"; enabled: !backend.needsConsent
                onTriggered: root.show(checklistPage)
            },
            Kirigami.Action {
                text: "Round"; icon.name: "media-record"; enabled: !backend.needsConsent
                onTriggered: root.show(sessionPage)
            },
            Kirigami.Action { text: "Review"; icon.name: "view-preview"; onTriggered: root.show(reviewPage) },
            Kirigami.Action {
                text: "Export"; icon.name: "document-export"; enabled: !backend.needsConsent
                onTriggered: root.show(exportPage)
            },
            Kirigami.Action {
                text: "Upload"; icon.name: "cloud-upload"; enabled: !backend.needsConsent
                onTriggered: root.show(uploadPage)
            }
        ]
    }

    // The texts wait for a legal review; until then nothing goes to the dataset (hub.py's gate). DraftBanner shows it.
    readonly property string draftText: "Contributions aren't open yet. The consent and upload texts are drafts waiting for a legal "
                                        + "review: you can record and review, but uploads stay closed until this message is gone."

    // A rounded label in a colour: a priority, a fit state, an eye's state.
    component Chip: Controls.Label {
        property color tint: Kirigami.Theme.neutralTextColor
        padding: Kirigami.Units.smallSpacing
        leftPadding: Kirigami.Units.largeSpacing
        rightPadding: Kirigami.Units.largeSpacing
        color: "white"
        background: Rectangle {
            radius: height / 2
            color: parent.tint
        }
    }

    function plural(n, word) {
        return n + " " + word + (n === 1 ? "" : "s")
    }

    // A round's label, from its id (the Export page's queue).
    function sessionLabel(id) {
        for (const s of backend.sessions)
            if (s.id === id)
                return s.label
        return id
    }
    // The round's export can upload by itself once done: logged in, and its dataset takes
    // contributions (or a dry run).
    function canUploadAfter(id) {
        return backend.canUploadAfter && (backend.hubDryRun || backend.uploadAllowedFor(id))
    }
    // Waiting to export, or exporting now.
    function exportPending(id) {
        return backend.exportSessionId === id && backend.exporting
               || backend.exportQueue.some(q => q.session === id)
    }

    function show(page) {
        pageStack.clear()
        pageStack.push(page)
    }

    // --page welcome|bounties|checklist|session|review|export|upload opens the app on that page;
    // the Welcome page comes first until the consent is agreed to.
    pageStack.initialPage: backend.needsConsent && startPage !== "review" ? welcomePage
        : (({ welcome: welcomePage, bounties: boardPage, checklist: checklistPage, session: sessionPage,
              review: reviewPage, export: exportPage, upload: uploadPage })[startPage] || boardPage)

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    // Asks before something is deleted for good: ask(title, text, function).
    Kirigami.PromptDialog {
        id: confirm
        property var action: null
        standardButtons: Controls.Dialog.Ok | Controls.Dialog.Cancel
        onAccepted: if (action) action()
        function ask(title, text, fn) {
            confirm.title = title
            confirm.subtitle = text
            confirm.action = fn
            confirm.open()
        }
    }

    // Restart SteamVR (the eye check's fix; fast: turn on its 90 fps eye cameras first). What it
    // does to Frametop, as the hand recorder says: ft-screens quits when SteamVR does, which ends
    // the desktop, and its unit doesn't restart.
    function askRestartSteamVR(fast) {
        confirm.ask(fast ? "Turn on 90 fps and restart SteamVR?" : "Restart SteamVR?",
                    (fast ? "SteamVR then runs the eye cameras at 90 frames a second, from its next start: Frametop "
                            + "turns on its setting for that (gaze/eye-rate.py). " : "SteamVR starts its eye cameras again. ")
                    + "Restarting closes every VR app. It also closes the Frametop desktop with all its windows, "
                    + "this one too: Frametop's screens don't come back by themselves. Save your work first.\n\n"
                    + "Once SteamVR is back, start the desktop again (Desktop in the library) and open the Eye "
                    + "recorder: it checks the eye cameras again.",
                    () => backend.restartSteamVR(fast))
    }

    // The eye camera check's problem, with its fix (a header message on the checklist and round pages).
    component EyeMessage: Kirigami.InlineMessage {
        position: Kirigami.InlineMessage.Position.Header
        type: backend.restartingSteamVR ? Kirigami.MessageType.Information : Kirigami.MessageType.Error
        text: backend.restartingSteamVR ? "Restarting SteamVR, then checking the eye cameras again…" : backend.eyeText
        actions: [
            Kirigami.Action {
                text: backend.eyeState === "slow" ? "Turn on 90 fps…" : "Restart SteamVR…"
                icon.name: "system-reboot"
                enabled: !backend.restartingSteamVR && !backend.sessionActive
                onTriggered: root.askRestartSteamVR(backend.eyeState === "slow")
            },
            Kirigami.Action {
                text: "Check again"
                icon.name: "view-refresh"
                enabled: !backend.eyeBusy
                onTriggered: backend.checkEyes()
            }
        ]
    }

    // Closing during a round asks first. Quitting closes the window again (Qt 6), so the
    // answer is remembered; the backend stops the round as the app quits.
    property bool quitting: false
    onClosing: close => {
        if (backend.sessionActive && !quitting) {
            close.accepted = false
            confirm.ask("Stop the round?", "A round is recording. Closing stops it; what's recorded so far is kept.",
                        () => { root.quitting = true; Qt.quit() })
        } else if ((backend.uploading || backend.exporting || backend.uploadQueue.length > 0) && !quitting) {
            close.accepted = false
            confirm.ask("Cancel the exports and uploads?",
                        "An export or upload is running or waiting (the Export page lists them). Closing cancels "
                        + "them all; an export cancelled keeps nothing. You can start them again later.",
                        () => { root.quitting = true; Qt.quit() })
        }
    }

    // ---------------------------------------------------------------- Welcome
    Component {
        id: welcomePage
        WelcomePage {
            draftText: root.draftText
            changedSinceAgreed: backend.contributor !== ""
            accept: (adult, region, agree) => backend.acceptConsent(adult, region, agree)
            next: () => root.show(boardPage)
        }
    }

    // ---------------------------------------------------------------- Bounties
    Component {
        id: boardPage
        Kirigami.ScrollablePage {
            id: board
            title: "Bounties"
            readonly property var priorityTint: ({
                high: Kirigami.Theme.negativeTextColor, normal: Kirigami.Theme.highlightColor,
                low: Kirigami.Theme.disabledTextColor
            })
            actions: [
                Kirigami.Action {
                    text: "Get the newest board"
                    icon.name: "view-refresh"
                    enabled: backend.boardFetch.state !== "fetching" && !backend.hubDryRun
                    onTriggered: backend.refreshBoard()
                }
            ]

            header: ColumnLayout {
                spacing: 0
                DraftBanner { Layout.fillWidth: true; text: root.draftText }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.runnerError !== ""
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Error
                    text: backend.runnerError
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.boardFetch.state === "error" || backend.boardFetch.state === "ok"
                    position: Kirigami.InlineMessage.Position.Header
                    type: backend.boardFetch.state === "ok" ? Kirigami.MessageType.Positive : Kirigami.MessageType.Information
                    text: backend.boardFetch.text || ""
                }
            }

            ListView {
                model: backend.bounties
                spacing: Kirigami.Units.smallSpacing

                header: Controls.Label {
                    width: ListView.view.width
                    padding: Kirigami.Units.largeSpacing
                    wrapMode: Text.Wrap
                    text: "Each bounty is a short round of about 5 minutes for one case where eye tracking struggles. "
                          + "Pick one; the most wanted come first. Every round starts with putting the headset on and "
                          + "nine dots. You can do a bounty again." + (backend.boardUpdated ? " (Board of "
                          + backend.boardUpdated + ".)" : "")
                }

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    visible: parent.count === 0
                    text: "No bounties"
                    explanation: "The board that came with the recorder is missing (gaze/rec/bounties.json)."
                }

                delegate: Controls.ItemDelegate {
                    id: bountyRow
                    required property var modelData
                    width: ListView.view.width
                    enabled: modelData.available === ""
                    onClicked: {
                        root.chosenBounty = modelData.id
                        root.show(checklistPage)
                    }

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing
                            RowLayout {
                                Controls.Label { text: bountyRow.modelData.title; font.bold: true }
                                Chip {
                                    visible: bountyRow.modelData.priority !== "normal"
                                    text: bountyRow.modelData.priority === "high" ? "Wanted most" : "Low priority"
                                    tint: board.priorityTint[bountyRow.modelData.priority] || Kirigami.Theme.neutralTextColor
                                    font: Kirigami.Theme.smallFont
                                }
                                Chip {
                                    visible: bountyRow.modelData.done > 0
                                    text: bountyRow.modelData.done === 1 ? "Done" : "Done " + bountyRow.modelData.done + " times"
                                    tint: Kirigami.Theme.positiveTextColor
                                    font: Kirigami.Theme.smallFont
                                }
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: bountyRow.modelData.blurb
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                text: [bountyRow.modelData.minutesText,
                                       bountyRow.modelData.lastText ? "last done " + bountyRow.modelData.lastText : "",
                                       bountyRow.modelData.available].filter(s => s !== "").join(" · ")
                            }
                        }
                        Controls.Button {
                            text: bountyRow.modelData.done > 0 ? "Record again" : "Record"
                            icon.name: "media-record"
                            enabled: bountyRow.modelData.available === "" && !backend.sessionActive
                            onClicked: bountyRow.clicked()
                        }
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- Before you start
    Component {
        id: checklistPage
        Kirigami.ScrollablePage {
            id: checklist
            title: "Before you start"
            readonly property var bounty: {
                for (const b of backend.bounties)
                    if (b.id === root.chosenBounty)
                        return b
                return null
            }
            readonly property bool ready: bounty !== null && bounty.available === "" && backend.diskOk
                                          && backend.runnerError === "" && !backend.sessionActive
            readonly property var saved: backend.checklist

            function answers() {
                const makeup = []
                for (let i = 0; i < makeupBoxes.count; ++i) {
                    const box = makeupBoxes.itemAt(i)
                    if (box.checked)
                        makeup.push(box.value)
                }
                return {
                    glasses: glasses.currentValue,
                    makeup: makeup,
                    pad: pad.currentValue,
                    strap: strap.currentValue,
                    ipd_mm: ipd.text === "" ? 0 : Number(ipd.text)
                }
            }
            function health() {
                const ticked = []
                for (let i = 0; i < healthBoxes.count; ++i) {
                    const box = healthBoxes.itemAt(i)
                    if (box.checked)
                        ticked.push(box.value)
                }
                return ticked
            }
            function save() {
                backend.saveChecklist(answers())
                backend.saveHealth(healthOptIn.checked, health())
            }

            header: ColumnLayout {
                spacing: 0
                DraftBanner { Layout.fillWidth: true; text: root.draftText }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.runnerError !== ""
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Error
                    text: backend.runnerError
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.sessionActive
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Information
                    text: "A round is running."
                    actions: [ Kirigami.Action { text: "Go to it"; onTriggered: root.show(sessionPage) } ]
                }
                EyeMessage {
                    Layout.fillWidth: true
                    visible: !backend.sessionActive && !backend.eyesIgnored
                             && ((backend.eyeState !== "ok" && backend.eyeState !== "" && backend.eyeState !== "checking")
                                 || backend.restartingSteamVR)
                }
            }
            Component.onCompleted: backend.checkEyes()

            Kirigami.FormLayout {
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "The bounty" }
                RowLayout {
                    Kirigami.FormData.label: "Recording:"
                    Controls.Label {
                        text: checklist.bounty ? checklist.bounty.title : "None picked yet"
                        font.bold: checklist.bounty !== null
                    }
                    Controls.Button {
                        text: checklist.bounty ? "Pick another…" : "Pick one…"
                        icon.name: "view-list-details"
                        flat: checklist.bounty !== null
                        onClicked: root.show(boardPage)
                    }
                }
                Controls.Label {
                    visible: checklist.bounty !== null
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: checklist.bounty ? checklist.bounty.blurb + "\n" + backend.planText(checklist.bounty.id) + "." : ""
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Your eyes today" }
                Controls.ComboBox {
                    id: glasses
                    Kirigami.FormData.label: "Glasses:"
                    model: backend.glassesChoices
                    textRole: "text"
                    valueRole: "value"
                    Component.onCompleted: currentIndex = Math.max(0, indexOfValue(checklist.saved.glasses))
                    onActivated: checklist.save()
                }
                Repeater {
                    id: makeupBoxes
                    model: backend.makeupChoices
                    Controls.CheckBox {
                        required property var modelData
                        required property int index
                        readonly property string value: modelData.value
                        Kirigami.FormData.label: index === 0 ? "Eye makeup:" : ""
                        text: modelData.text
                        checked: checklist.saved.makeup.indexOf(modelData.value) >= 0
                        onToggled: checklist.save()
                    }
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "Come as you normally are: the dataset needs eyes with and without glasses and makeup. This only notes it."
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "The headset" }
                Controls.ComboBox {
                    id: pad
                    Kirigami.FormData.label: "Face pad:"
                    model: backend.padChoices
                    textRole: "text"
                    valueRole: "value"
                    Component.onCompleted: currentIndex = Math.max(0, indexOfValue(checklist.saved.pad))
                    onActivated: checklist.save()
                }
                Controls.ComboBox {
                    id: strap
                    Kirigami.FormData.label: "Strap:"
                    model: backend.strapChoices
                    textRole: "text"
                    valueRole: "value"
                    Component.onCompleted: currentIndex = Math.max(0, indexOfValue(checklist.saved.strap))
                    onActivated: checklist.save()
                }
                RowLayout {
                    Kirigami.FormData.label: "Your IPD (optional):"
                    Controls.TextField {
                        id: ipd
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                        placeholderText: "e.g. 63.5"
                        text: checklist.saved.ipd_mm > 0 ? String(checklist.saved.ipd_mm) : ""
                        validator: DoubleValidator { bottom: 50; top: 80; decimals: 1; notation: DoubleValidator.StandardNotation }
                        onEditingFinished: checklist.save()
                    }
                    Controls.Label { text: "mm" }
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "The distance between your pupils, if an optician measured it (it's on many glasses "
                          + "prescriptions as PD). Leave it empty if you don't know it: the headset's IPD setting is recorded anyway."
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Eye health (optional)" }
                Controls.CheckBox {
                    id: healthOptIn
                    Kirigami.FormData.label: ""
                    text: "I want to add answers about my eyes' health"
                    checked: backend.healthOptIn
                    onToggled: checklist.save()
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "Eye trackers often fail for people with these, so they help the dataset. They're health "
                          + "data: they go into a separate file, encrypted like the eye images, that only the "
                          + "maintainers can read. Untick this to stop adding them; rounds already exported keep them."
                }
                Repeater {
                    id: healthBoxes
                    model: healthOptIn.checked ? backend.healthChoices : []
                    Controls.CheckBox {
                        required property var modelData
                        required property int index
                        readonly property string value: modelData.value
                        Kirigami.FormData.label: index === 0 ? "I have, or had:" : ""
                        text: modelData.text
                        checked: backend.healthAnswers.indexOf(modelData.value) >= 0
                        onToggled: checklist.save()
                    }
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Space" }
                Controls.Label {
                    Kirigami.FormData.label: "Free space:"
                    text: backend.freeText + " (a round and its export take about 12 GB)"
                }
                Kirigami.InlineMessage {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    Layout.fillWidth: true
                    visible: !backend.diskOk
                    type: Kirigami.MessageType.Error
                    text: "Not enough free space for a round. Export, upload and delete earlier rounds (Review), "
                          + "or free up space, first."
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Eye cameras" }
                RowLayout {
                    Kirigami.FormData.label: "Eye cameras:"
                    Controls.Label {
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                        wrapMode: Text.Wrap
                        text: backend.eyesIgnored ? "Not checked (--ignore-eyes or a dry run)"
                              : backend.eyeState === "ok" ? "Both running at 90 fps (" + backend.eyeFpsText + ")."
                              : backend.eyeState === "slow" ? "Too slow (" + backend.eyeFpsText + "): see the message at the top."
                              : backend.eyeState === "none" ? "No frames: see the message at the top."
                              : "Checking…"
                    }
                    Controls.ToolButton {
                        icon.name: "view-refresh"
                        text: "Check again"
                        display: Controls.AbstractButton.IconOnly
                        enabled: !backend.eyeBusy
                        onClicked: backend.checkEyes()
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered
                    }
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "What will happen" }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    text: "Press Start, then put the headset on so it's comfortable and the picture is sharp, "
                          + "and look at the dot on the panel in the headset. It shows the headset's IPD setting: "
                          + "it can't be changed with the headset on, so set it before you start. Then nine dots, "
                          + "then the bounty's steps. Look at each dot "
                          + "until its ring fills, and keep your head still unless a step says otherwise.\n\n"
                          + "Each step waits until you're ready: press the button on the right side of the headset, "
                          + "or Space or Next in this window. A 3-2-1 countdown follows, then the step. Nothing is "
                          + "recorded while a step waits. The headset button also pauses and resumes; press it twice "
                          + "to record a step again, or hold it to stop. In this window P pauses, R records the last "
                          + "step again, S skips the rest of a take and Esc stops.\n\n"
                          + "Nothing leaves the headset. Afterwards you look through the takes in Review, delete "
                          + "anything you don't want to share, and only then export."
                }
                Controls.Button {
                    text: "Start"
                    icon.name: "media-record"
                    enabled: checklist.ready && !backend.eyesBlockStart && !backend.restartingSteamVR
                    onClicked: {
                        checklist.save()
                        if (backend.startSession(root.chosenBounty))
                            root.show(sessionPage)
                    }
                }
                Controls.Label {
                    visible: (!checklist.ready || backend.eyesBlockStart) && !backend.sessionActive
                    opacity: 0.7
                    text: checklist.bounty === null ? "Pick a bounty to start."
                          : backend.eyesBlockStart ? (backend.eyeState === "checking" || backend.eyeState === ""
                                                      ? "Checking the eye cameras…"
                                                      : "The eye cameras aren't ready: see the message at the top.") : ""
                }
            }
        }
    }

    // ---------------------------------------------------------------- Round
    Component {
        id: sessionPage
        Kirigami.Page {
            id: sessionView
            title: "Round"
            readonly property var st: backend.status
            readonly property string state: st.state || ""
            readonly property bool waiting: !!st.waiting
            readonly property var card: st.fit_card || null
            readonly property var stateText: ({
                starting: "Starting…", fitment: "Fitting the headset", ready: "Get ready", countdown: "Starting",
                running: "Recording", paused: "Paused", done: "Done", stopped: "Stopped", error: "Error"
            })

            // The keys while the window has focus (the panel in the headset lists them too).
            Shortcut {
                sequence: "Space"
                enabled: backend.sessionActive
                onActivated: backend.nextStep()
            }
            Shortcut {
                sequence: "P"
                enabled: backend.sessionActive
                onActivated: backend.togglePause()
            }
            Shortcut {
                sequence: "R"
                enabled: backend.sessionActive
                onActivated: backend.redo()
            }
            Shortcut {
                sequence: "S"
                enabled: backend.sessionActive
                onActivated: backend.skipSection()
            }
            Shortcut {
                sequence: "Esc"
                enabled: backend.sessionActive
                onActivated: backend.stopSession()
            }

            // The fit coach's reading while the headset is fitted (gaze/fitcoach.py): the state,
            // each eye, and what to do.
            component FitCard: Kirigami.Card {
                id: fitCard
                property var card
                readonly property var fitTint: ({
                    ok: Kirigami.Theme.positiveTextColor, adjust: Kirigami.Theme.neutralTextColor,
                    lost: Kirigami.Theme.negativeTextColor, waiting: Kirigami.Theme.disabledTextColor
                })
                readonly property var eyeText: ({ found: "found", lost: "lost", waiting: "waiting" })
                header: RowLayout {
                    Kirigami.Heading { level: 3; text: "Headset fit" }
                    Item { Layout.fillWidth: true }
                    Chip {
                        text: !fitCard.card ? "Starting the eye tracker"
                              : ({ ok: "Good", adjust: "Adjust it", lost: "An eye is lost", waiting: "Waiting for both eyes" })[fitCard.card.state] || ""
                        tint: fitCard.card ? fitCard.fitTint[fitCard.card.state] || Kirigami.Theme.neutralTextColor
                                           : Kirigami.Theme.disabledTextColor
                    }
                }
                contentItem: ColumnLayout {
                    // (Without a width of its own the card grows with its longest line.)
                    implicitWidth: Kirigami.Units.gridUnit * 16
                    spacing: Kirigami.Units.smallSpacing
                    Repeater {
                        model: ["right", "left"]
                        Controls.Label {
                            required property string modelData
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            readonly property var eye: fitCard.card && fitCard.card.eyes ? fitCard.card.eyes[modelData] : null
                            visible: eye !== null && eye !== undefined
                            text: (modelData === "right" ? "Right eye: " : "Left eye: ")
                                  + (eye ? (fitCard.eyeText[eye.state] || eye.state) + ", seen in " + Math.round(100 * eye.found) + " % of frames"
                                           + (eye.offset_deg ? ", " + eye.offset_deg[0].toFixed(1) + "° sideways, "
                                              + eye.offset_deg[1].toFixed(1) + "° up" : "") : "")
                        }
                    }
                    Repeater {
                        model: fitCard.card ? fitCard.card.hints : []
                        Controls.Label {
                            required property string modelData
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: "• " + modelData
                        }
                    }
                    Controls.Label {
                        visible: !!fitCard.card && fitCard.card.provisional && fitCard.card.against === "reference"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        text: "A first guess: the coach has been measured on one face so far."
                    }
                }
            }

            header: ColumnLayout {
                spacing: 0
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.runnerError !== "" || sessionView.state === "error"
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Error
                    text: backend.runnerError || ("The round stopped with an error: " + (sessionView.st.error || "unknown"))
                }
            }

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                visible: sessionView.state === ""
                icon.name: "media-record"
                text: "No round yet"
                explanation: "Pick a bounty, go through Before you start, then press Start there."
                helpfulAction: Kirigami.Action {
                    text: "Bounties"; icon.name: "view-list-details"
                    onTriggered: root.show(boardPage)
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Kirigami.Units.gridUnit
                visible: sessionView.state !== ""
                spacing: Kirigami.Units.largeSpacing

                RowLayout {
                    Layout.fillWidth: true
                    Kirigami.Heading {
                        level: 1
                        text: sessionView.stateText[sessionView.state] || sessionView.state
                    }
                    Item { Layout.fillWidth: true }
                    Controls.Label {
                        visible: !!sessionView.st.take && sessionView.state !== "paused"
                                 && (sessionView.state === "countdown" || sessionView.state === "running")
                        text: "● Recording"
                        color: Kirigami.Theme.negativeTextColor
                    }
                }
                Controls.Label {
                    visible: (sessionView.st.step_index || 0) > 0 || !!sessionView.st.title
                    text: (sessionView.st.title || "")
                          + ((sessionView.st.step_index || 0) > 0
                             ? (sessionView.st.title ? " · " : "") + "step " + sessionView.st.step_index + " of "
                               + sessionView.st.step_count : "")
                    font.bold: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.gridUnit * 1.5

                    FitCard {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 18
                        visible: (sessionView.state === "fitment" && sessionView.st.fit_coach !== false)
                                 || (sessionView.state === "paused" && sessionView.card !== null)
                        card: sessionView.card
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: Kirigami.Units.largeSpacing
                        Controls.Label {
                            Layout.fillWidth: true
                            visible: !!(sessionView.state === "fitment" ? sessionView.st.fit_words : sessionView.st.prompt)
                            wrapMode: Text.Wrap
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.4
                            text: sessionView.state === "fitment" ? "Headset position: " + (sessionView.st.fit_words || "")
                                                                  : (sessionView.st.prompt || "")
                        }
                        // The IPD setting (SteamOS's, through SteamVR), shown while the headset goes on
                        Controls.Label {
                            Layout.fillWidth: true
                            visible: sessionView.state === "fitment"
                            wrapMode: Text.Wrap
                            text: sessionView.st.ipd === undefined || sessionView.st.ipd === null
                                  ? "Couldn't read the IPD setting from SteamVR."
                                  : "Your IPD setting: " + (sessionView.st.ipd * 1000).toFixed(1) + " mm. If that's not "
                                    + "right for you, stop (Esc), take the headset off, set it, and start again."
                        }
                        // The countdown's 3, 2, 1, then a cue's word, big
                        Controls.Label {
                            visible: !!sessionView.st.big && sessionView.state !== "paused"
                            text: sessionView.st.big || ""
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 4
                            font.bold: true
                            color: Kirigami.Theme.highlightColor
                        }
                        Controls.Label {
                            visible: !sessionView.waiting && sessionView.state === "running"
                                     && (sessionView.st.seconds_left || 0) > 0
                            text: Math.ceil(sessionView.st.seconds_left || 0) + " s left"
                            opacity: 0.7
                        }
                        Controls.Button {
                            visible: sessionView.waiting && sessionView.state !== "paused"
                            focusPolicy: Qt.NoFocus
                            text: "Next"
                            icon.name: "go-next"
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.5
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                            Layout.preferredHeight: Kirigami.Units.gridUnit * 3
                            onClicked: backend.nextStep()
                        }
                        Controls.Label {
                            visible: sessionView.waiting && sessionView.state !== "paused"
                            opacity: 0.7
                            text: sessionView.state === "fitment"
                                  ? (sessionView.st.fit_coach === false
                                     ? "Put the headset on comfortably, with a sharp picture, look at the dot, then press Next."
                                     : "Press Next once the fit says good. Pressing it before warns first; a second press starts anyway.")
                                  : (sessionView.st.ready_text || "Ready? Press Space or click Next")
                                    + ". A 3-2-1 countdown starts the recording."
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !!sessionView.st.note
                    // (A hidden InlineMessage keeps its height in a layout.)
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    type: Kirigami.MessageType.Warning
                    text: sessionView.st.note || ""
                }
                Controls.Label {
                    visible: !!sessionView.st.take
                    text: "Take " + (sessionView.st.take || "")
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                }

                Item { Layout.fillHeight: true }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "The dots and instructions appear in the headset. "
                          + (sessionView.st.button ? "The button on the right side of the headset: next, pause or "
                             + "resume; twice: redo; hold: stop. " : "")
                          + "While this window has focus: Space: next · P: pause or resume · R: redo · S: skip the "
                          + "rest of the take · Esc: stop. Redo during a step starts it again; before a step "
                          + "starts, it goes back to the one before. Stopping keeps what's recorded so far."
                }
                RowLayout {
                    // No keyboard focus on the buttons, so Space always reaches the shortcut.
                    Controls.Button {
                        visible: !backend.sessionActive
                        focusPolicy: Qt.NoFocus
                        text: "Another bounty…"
                        icon.name: "view-list-details"
                        onClicked: root.show(boardPage)
                    }
                    Controls.Button {
                        visible: backend.sessionActive
                        focusPolicy: Qt.NoFocus
                        text: sessionView.state === "paused" ? "Resume" : "Pause"
                        icon.name: sessionView.state === "paused" ? "media-playback-start" : "media-playback-pause"
                        onClicked: backend.togglePause()
                    }
                    Controls.Button {
                        visible: backend.sessionActive
                        enabled: !!sessionView.st.can_redo
                        focusPolicy: Qt.NoFocus
                        // At a ready screen it goes back to the step before; else this one again.
                        text: sessionView.state === "ready" ? "Back to the last step" : "Redo this step"
                        icon.name: "edit-undo"
                        onClicked: backend.redo()
                    }
                    Controls.Button {
                        visible: backend.sessionActive
                        focusPolicy: Qt.NoFocus
                        text: "Skip"
                        icon.name: "media-skip-forward"
                        onClicked: backend.skipSection()
                    }
                    Controls.Button {
                        visible: backend.sessionActive
                        focusPolicy: Qt.NoFocus
                        text: "Stop"
                        icon.name: "media-playback-stop"
                        onClicked: backend.stopSession()
                    }
                    Controls.Button {
                        visible: !backend.sessionActive && backend.sessionId !== ""
                        focusPolicy: Qt.NoFocus
                        text: "Review this round"
                        icon.name: "view-preview"
                        onClicked: {
                            root.show(reviewPage)
                            pageStack.push(takesPage, { session: backend.sessionId })
                        }
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- Review
    Component {
        id: reviewPage
        Kirigami.ScrollablePage {
            title: "Review"
            actions: [
                Kirigami.Action { text: "Refresh"; icon.name: "view-refresh"; onTriggered: backend.refreshSessions() }
            ]

            ListView {
                model: backend.sessions
                spacing: Kirigami.Units.smallSpacing

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    visible: parent.count === 0
                    text: "No rounds yet"
                    explanation: "Recorded rounds appear here, to look through and trim before you export them."
                }

                delegate: Controls.ItemDelegate {
                    id: roundRow
                    required property var modelData
                    width: ListView.view.width
                    onClicked: pageStack.push(takesPage, { session: modelData.id })

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        Kirigami.Icon {
                            source: roundRow.modelData.active ? "media-record" : "folder-videos"
                            implicitWidth: Kirigami.Units.iconSizes.medium
                            implicitHeight: implicitWidth
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Controls.Label {
                                text: roundRow.modelData.label + (roundRow.modelData.active ? " (recording)" : "")
                                      + (roundRow.modelData.dry_run ? " (dry run)" : "")
                                font.bold: true
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                text: root.plural(roundRow.modelData.takes, "take") + " · " + roundRow.modelData.sizeText
                                      + (roundRow.modelData.statusText ? " · " + roundRow.modelData.statusText : "")
                                      + (roundRow.modelData.export_stale ? " · changed since its export"
                                         : roundRow.modelData.exported ? " · exported (" + roundRow.modelData.exportText + ")" : "")
                            }
                        }
                        Controls.ToolButton {
                            icon.name: "edit-delete"
                            text: "Delete round"
                            display: Controls.AbstractButton.IconOnly
                            enabled: !roundRow.modelData.active
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: {
                                const rid = roundRow.modelData.id
                                confirm.ask("Delete this round?",
                                            "The round of " + roundRow.modelData.label + " (" + root.plural(roundRow.modelData.takes, "take")
                                            + ", " + roundRow.modelData.sizeText + ") and its export are deleted from the "
                                            + "headset for good.",
                                            () => { pageStack.pop(pageStack.items[0]); backend.deleteSession(rid) })
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: takesPage
        TakesPage {
            recorder: root
            viewerComponent: viewerPage
            itemNoun: "round"
            thumbWidth: 5.5
            imageProvider: "eyes"
            countKey: "frameCount"
            countNoun: "frames"
            deletedKey: "deletedCount"
        }
    }

    Component {
        id: viewerPage
        ViewerPage {
            frameNoun: "frame"
            pageStep: 90
            imagePath: "eyes/pair"
            frameTime: (session, take, index) => backend.frameTime(session, take, index)
            noFramesText: "This take has no frames"
            helpText: "← → step one frame, Page Up/Down a second, Home/End. [ and ] mark a range's start and end, "
                      + "Delete deletes it, for both eyes. Deleted frames stay on the headset until export, which leaves them out."
        }
    }

    // ---------------------------------------------------------------- Export
    Component {
        id: exportPage
        ExportPage {
            recorder: root
            uploadComponent: uploadPage
            itemNoun: "round"
            exportProblem: backend.exportProblem
            busyTime: "a minute or so"
            busyFirst: true
            aboutText: "Export leaves out the ranges you deleted, encrypts the eye images and any health answers to "
                       + "the dataset's keys (only its maintainers can read them), and adds a manifest and checksums, "
                       + "in " + backend.exportsDir + ". It runs at the lowest priority, one at a time; the headset "
                       + "stays awake until they're done. Nothing is uploaded unless you press Export and upload here, "
                       + "or Upload on the Upload page."
        }
    }

    // ---------------------------------------------------------------- Upload
    Component {
        id: uploadPage
        UploadPage {
            recorder: root
            exportComponent: exportPage
            itemNoun: "round"
            recorderName: "Eye Recorder"
            datasetFor: session => backend.dataset
            datasetUrlFor: session => backend.datasetUrl
            uploadAllowedFor: session => backend.uploadAllowed
            allowUploadEnv: "FT_EYEREC_ALLOW_UPLOAD"
            rehearsalEnd: "while the texts wait for their review."
            reviewText: "The eye images and health answers in it are encrypted: only the dataset's maintainers "
                        + "can read them. Nothing joins the dataset until the maintainer has checked it."
            loginTextWhileWaiting: true
        }
    }
}
