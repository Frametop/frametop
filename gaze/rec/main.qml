// Frametop Eye Recorder (Kirigami). Backend: ft_eyerec.py ("backend"); eye frames come from its
// image provider (image://eyes/...). Copied from the hand recorder's window (hands/rec/main.qml)
// and changed for rounds picked from the bounty board, the fit card and the eye cameras.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

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

    // The texts wait for a legal review; until then nothing goes to the dataset (hub.py's gate).
    component DraftBanner: Kirigami.InlineMessage {
        visible: backend.textsDraft
        position: Kirigami.InlineMessage.Position.Header
        type: Kirigami.MessageType.Warning
        text: "Contributions aren't open yet. The consent and upload texts are drafts waiting for a legal "
              + "review: you can record and review, but uploads stay closed until this message is gone."
    }

    // Read-only Markdown (CONSENT.md, UPLOAD.md), selectable for copying.
    component MarkdownText: Controls.TextArea {
        property string markdown
        // Links get the application palette's colour when the text is parsed, and Kirigami's theme
        // doesn't set that palette: set it, then parse again (the theme resolves after creation).
        readonly property color linkColor: Kirigami.Theme.linkColor
        function parse() {
            backend.setLinkColor(linkColor)
            text = markdown
        }
        onLinkColorChanged: parse()
        onMarkdownChanged: parse()
        Component.onCompleted: parse()
        textFormat: TextEdit.MarkdownText
        readOnly: true
        selectByMouse: true
        wrapMode: Text.Wrap
        background: null
        padding: 0
        onLinkActivated: link => Qt.openUrlExternally(link)
    }

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
        } else if (backend.uploading && !quitting) {
            close.accepted = false
            confirm.ask("Cancel the upload?", "An upload is running. Closing cancels it; you can start it again later.",
                        () => { root.quitting = true; Qt.quit() })
        }
    }

    // ---------------------------------------------------------------- Welcome
    Component {
        id: welcomePage
        Kirigami.ScrollablePage {
            title: "Welcome"
            header: DraftBanner { }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                MarkdownText {
                    Layout.fillWidth: true
                    markdown: backend.consentText
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !backend.needsConsent
                    type: Kirigami.MessageType.Positive
                    text: "You agreed to this text (" + backend.consentAccepted + "). Your contributor id is "
                          + backend.contributor + ": keep it if you might want to withdraw later."
                }
                Controls.Button {
                    visible: !backend.needsConsent
                    text: "Copy contributor id"
                    icon.name: "edit-copy"
                    onClicked: backend.copy(backend.contributor)
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.needsConsent && backend.contributor !== ""
                    type: Kirigami.MessageType.Information
                    text: "This text changed since you last agreed to it. Please read it again; your contributor id stays the same."
                }

                Controls.CheckBox {
                    id: adult
                    visible: backend.needsConsent
                    text: "I'm 18 or older"
                }
                Controls.CheckBox {
                    id: region
                    visible: backend.needsConsent
                    text: "I don't live in Illinois, Texas or Washington (USA)"
                }
                Controls.CheckBox {
                    id: agree
                    visible: backend.needsConsent
                    text: "I agree to the text above"
                }
                Controls.Button {
                    visible: backend.needsConsent
                    enabled: adult.checked && region.checked && agree.checked
                    text: "Agree and continue"
                    icon.name: "go-next"
                    onClicked: {
                        backend.acceptConsent(adult.checked, region.checked, agree.checked)
                        if (!backend.needsConsent)
                            root.show(boardPage)
                    }
                }
                Controls.Button {
                    visible: !backend.needsConsent
                    text: "Continue"
                    icon.name: "go-next"
                    onClicked: root.show(boardPage)
                }
            }
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
                DraftBanner { Layout.fillWidth: true }
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
                DraftBanner { Layout.fillWidth: true }
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
                             + "resume; twice: record the last step again; hold: stop. " : "")
                          + "While this window has focus: Space: next · P: pause or resume · R: record the last step "
                          + "again · S: skip the rest of the take · Esc: stop. Stopping keeps what's recorded so far."
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
                        text: "Redo step"
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
        Kirigami.ScrollablePage {
            id: takesView
            property string session: ""
            property var rows: backend.takeList(session)
            title: "Takes"

            Connections {
                target: backend
                function onSessionsChanged() { takesView.rows = backend.takeList(takesView.session) }
            }

            ListView {
                model: takesView.rows
                spacing: Kirigami.Units.smallSpacing

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    visible: parent.count === 0
                    text: "No takes in this round"
                }

                delegate: Controls.ItemDelegate {
                    id: takeRow
                    required property var modelData
                    width: ListView.view.width
                    onClicked: pageStack.push(viewerPage, { session: takesView.session, take: modelData.id })

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        Image {
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 5.5
                            Layout.preferredHeight: Kirigami.Units.gridUnit * 4.5
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            source: takeRow.modelData.frameCount > 0
                                    ? "image://eyes/thumb/" + takesView.session + "/" + takeRow.modelData.id : ""
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Controls.Label { text: takeRow.modelData.title; font.bold: true }
                            Controls.Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                text: takeRow.modelData.status + " · " + takeRow.modelData.durationText + " · "
                                      + takeRow.modelData.frameCount + " frames"
                                      + (takeRow.modelData.deletedCount ? " (" + takeRow.modelData.deletedCount + " deleted)" : "")
                                      + " · " + takeRow.modelData.sizeText
                            }
                        }
                        Controls.ToolButton {
                            icon.name: "edit-delete"
                            text: "Delete take"
                            display: Controls.AbstractButton.IconOnly
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: {
                                const take = takeRow.modelData.id
                                confirm.ask("Delete this take?",
                                            "\"" + takeRow.modelData.title + "\" (" + takeRow.modelData.durationText
                                            + ") is deleted from the headset for good.",
                                            () => { pageStack.pop(takesView); backend.deleteTake(takesView.session, take) })
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: viewerPage
        Kirigami.Page {
            id: viewer
            property string session: ""
            property string take: ""
            property var info: backend.takeInfo(session, take)
            property int index: 0
            property int markStart: -1
            property int markEnd: -1
            title: info.title
            readonly property bool deletedHere: isDeleted(index)

            function step(n) {
                index = Math.max(0, Math.min(info.count - 1, index + n))
            }
            function isDeleted(i) {
                for (const r of info.ranges)
                    if (i >= r[0] && i <= r[1] && r[0] >= 0)
                        return true
                return false
            }
            function deleteMarked() {
                backend.deleteRange(session, take, markStart, markEnd)
                markStart = markEnd = -1
                info = backend.takeInfo(session, take)
            }

            Connections {
                target: backend
                function onSessionsChanged() { viewer.info = backend.takeInfo(viewer.session, viewer.take) }
            }

            // Keyboard stepping: arrows one frame, Page Up/Down 90 (a second), Home/End, [ and ]
            // mark a range, Delete deletes it.
            Item {
                id: keys
                focus: true
                Component.onCompleted: forceActiveFocus()
                Keys.onPressed: event => {
                    const k = event.key
                    if (k === Qt.Key_Left) viewer.step(-1)
                    else if (k === Qt.Key_Right) viewer.step(1)
                    else if (k === Qt.Key_PageUp) viewer.step(-90)
                    else if (k === Qt.Key_PageDown) viewer.step(90)
                    else if (k === Qt.Key_Home) viewer.index = 0
                    else if (k === Qt.Key_End) viewer.index = Math.max(0, viewer.info.count - 1)
                    else if (k === Qt.Key_BracketLeft) viewer.markStart = viewer.index
                    else if (k === Qt.Key_BracketRight) viewer.markEnd = viewer.index
                    else if (k === Qt.Key_Delete && viewer.markStart >= 0 && viewer.markEnd >= 0) viewer.deleteMarked()
                    else return
                    event.accepted = true
                }
            }

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                visible: viewer.info.count === 0
                text: "This take has no frames"
            }

            ColumnLayout {
                anchors.fill: parent
                visible: viewer.info.count > 0
                spacing: Kirigami.Units.smallSpacing

                Image {
                    id: frame
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: false
                    retainWhileLoading: true
                    source: viewer.info.count > 0
                            ? "image://eyes/pair/" + viewer.session + "/" + viewer.take + "/" + viewer.index : ""
                    MouseArea { anchors.fill: parent; onClicked: keys.forceActiveFocus() }

                    Rectangle {
                        visible: viewer.deletedHere
                        x: (frame.width - frame.paintedWidth) / 2
                        y: (frame.height - frame.paintedHeight) / 2
                        width: frame.paintedWidth
                        height: frame.paintedHeight
                        color: Qt.rgba(0.8, 0, 0, 0.35)
                        Controls.Label {
                            anchors.centerIn: parent
                            text: "Deleted: not exported"
                            color: "white"
                            font.bold: true
                        }
                    }
                }

                // The slider, with the deleted ranges (red) and the marked range (blue) under it.
                Item {
                    Layout.fillWidth: true
                    implicitHeight: slider.implicitHeight
                    Repeater {
                        model: viewer.info.ranges
                        Rectangle {
                            required property var modelData
                            visible: modelData[0] >= 0
                            readonly property real unit: (slider.availableWidth) / Math.max(1, viewer.info.count - 1)
                            x: slider.leftPadding + modelData[0] * unit - 2
                            width: Math.max(4, (modelData[1] - modelData[0]) * unit + 4)
                            y: slider.topPadding + slider.availableHeight / 2 + 4
                            height: 4
                            color: Kirigami.Theme.negativeTextColor
                        }
                    }
                    Rectangle {
                        visible: viewer.markStart >= 0
                        readonly property int last: viewer.markEnd >= 0 ? viewer.markEnd : viewer.index
                        readonly property real unit: (slider.availableWidth) / Math.max(1, viewer.info.count - 1)
                        x: slider.leftPadding + Math.min(viewer.markStart, last) * unit - 2
                        width: Math.abs(last - viewer.markStart) * unit + 4
                        y: slider.topPadding + slider.availableHeight / 2 - 8
                        height: 4
                        color: Kirigami.Theme.highlightColor
                    }
                    Controls.Slider {
                        id: slider
                        anchors.left: parent.left
                        anchors.right: parent.right
                        focusPolicy: Qt.NoFocus
                        from: 0
                        to: Math.max(0, viewer.info.count - 1)
                        stepSize: 1
                        snapMode: Controls.Slider.SnapAlways
                        value: viewer.index
                        onMoved: viewer.index = Math.round(value)
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Controls.Label {
                        text: "Frame " + (viewer.index + 1) + " of " + viewer.info.count + " · "
                              + backend.frameTime(viewer.session, viewer.take, viewer.index)
                    }
                    Item { Layout.fillWidth: true }
                    Controls.Button {
                        focusPolicy: Qt.NoFocus
                        text: viewer.markStart >= 0 ? "Start: " + (viewer.markStart + 1) : "Mark start"
                        icon.name: "go-first"
                        onClicked: viewer.markStart = viewer.index
                    }
                    Controls.Button {
                        focusPolicy: Qt.NoFocus
                        text: viewer.markEnd >= 0 ? "End: " + (viewer.markEnd + 1) : "Mark end"
                        icon.name: "go-last"
                        onClicked: viewer.markEnd = viewer.index
                    }
                    Controls.Button {
                        focusPolicy: Qt.NoFocus
                        enabled: viewer.markStart >= 0 && viewer.markEnd >= 0
                        text: "Delete range"
                        icon.name: "edit-cut"
                        onClicked: viewer.deleteMarked()
                    }
                    Controls.Button {
                        focusPolicy: Qt.NoFocus
                        visible: viewer.markStart >= 0 || viewer.markEnd >= 0
                        text: "Clear marks"
                        icon.name: "edit-clear"
                        onClicked: viewer.markStart = viewer.markEnd = -1
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    visible: viewer.info.ranges.length > 0
                    Controls.Label { text: "Deleted:"; topPadding: Kirigami.Units.smallSpacing }
                    Repeater {
                        model: viewer.info.ranges
                        Controls.Button {
                            required property var modelData
                            required property int index
                            focusPolicy: Qt.NoFocus
                            flat: true
                            text: (modelData[0] < 0 ? "a range with no frames"
                                   : "frames " + (modelData[0] + 1) + " to " + (modelData[1] + 1)) + ": restore"
                            icon.name: "edit-undo"
                            onClicked: backend.restoreRange(viewer.session, viewer.take, index)
                        }
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                    text: "← → step one frame, Page Up/Down a second, Home/End. [ and ] mark a range's start and end, "
                          + "Delete deletes it, for both eyes. Deleted frames stay on the headset until export, which leaves them out."
                }
            }
        }
    }

    // ---------------------------------------------------------------- Export
    Component {
        id: exportPage
        Kirigami.ScrollablePage {
            id: exportView
            title: "Export"
            property bool worn: backend.headsetWorn()
            readonly property var chosen: {
                const list = backend.sessions
                for (const s of list)
                    if (s.id === roundBox.currentValue)
                        return s
                return null
            }

            Timer {
                interval: 5000
                running: true
                repeat: true
                onTriggered: exportView.worn = backend.headsetWorn()
            }

            header: ColumnLayout {
                spacing: 0
                DraftBanner { Layout.fillWidth: true }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: exportView.worn && backend.exporting
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Information
                    text: "Exporting keeps the processor busy for a minute or so, so VR may stutter a little until "
                          + "it's done. You can keep using the headset meanwhile."
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.exportProblem !== ""
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Error
                    text: backend.exportProblem
                }
            }

            Kirigami.FormLayout {
                Controls.ComboBox {
                    id: roundBox
                    Kirigami.FormData.label: "Round:"
                    model: backend.sessions
                    textRole: "label"
                    valueRole: "id"
                    enabled: !backend.exporting
                    Component.onCompleted: {
                        const i = indexOfValue(root.chosenSession)
                        currentIndex = i >= 0 ? i : (count > 0 ? 0 : -1)
                    }
                    onActivated: root.chosenSession = currentValue
                }
                Controls.Label {
                    Kirigami.FormData.label: "Recorded:"
                    visible: exportView.chosen !== null
                    text: exportView.chosen ? root.plural(exportView.chosen.takes, "take") + ", " + exportView.chosen.sizeText : ""
                }
                Controls.Label {
                    Kirigami.FormData.label: "Export:"
                    visible: exportView.chosen !== null
                    text: !exportView.chosen ? ""
                          : exportView.chosen.export_stale ? "changed since it was exported: export it again"
                          : exportView.chosen.exported ? "ready, " + exportView.chosen.exportText : "not exported yet"
                }
                RowLayout {
                    Controls.Button {
                        text: backend.exporting ? "Exporting…"
                              : exportView.chosen && exportView.chosen.exported ? "Export again" : "Export"
                        icon.name: "document-export"
                        enabled: exportView.chosen !== null && !backend.exporting && backend.exportProblem === ""
                                 && !exportView.chosen.active
                                 && !(backend.uploading && backend.upload.session === exportView.chosen.id)
                        onClicked: backend.exportSession(exportView.chosen.id, false)
                    }
                    // Shows the moment Export is pressed: the first progress can take a few seconds.
                    Controls.BusyIndicator {
                        visible: backend.exporting
                        running: visible
                        implicitWidth: Kirigami.Units.gridUnit * 1.5
                        implicitHeight: implicitWidth
                    }
                    Controls.Button {
                        visible: backend.exporting
                        text: "Cancel"
                        icon.name: "dialog-cancel"
                        onClicked: backend.cancelExport()
                    }
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
                    visible: backend.exportText !== "" && (backend.exporting || backend.exportSessionId === roundBox.currentValue)
                    wrapMode: Text.WrapAnywhere
                    text: backend.exportText
                }
                RowLayout {
                    visible: exportView.chosen !== null && exportView.chosen.exported && !backend.exporting
                    Controls.Button {
                        text: "Upload"
                        icon.name: "go-next"
                        onClicked: root.show(uploadPage)
                    }
                    Controls.Button {
                        text: "Delete export"
                        icon.name: "edit-delete"
                        onClicked: {
                            const rid = exportView.chosen.id
                            confirm.ask("Delete this export?", "The export is deleted; the round's recordings stay.",
                                        () => backend.deleteExport(rid))
                        }
                    }
                }
            }

            footer: Controls.Label {
                padding: Kirigami.Units.largeSpacing
                wrapMode: Text.Wrap
                opacity: 0.7
                text: "Export leaves out the ranges you deleted, encrypts the eye images and any health answers to "
                      + "the dataset's keys (only its maintainers can read them), and adds a manifest and checksums, "
                      + "in " + backend.exportsDir + ". It runs at the lowest priority; the headset stays awake "
                      + "until it's done. Nothing is uploaded until you press Upload on the Upload page."
            }
        }
    }

    // ---------------------------------------------------------------- Upload
    Component {
        id: uploadPage
        Kirigami.ScrollablePage {
            id: uploadView
            title: "Upload"
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
                if (chosen && chosen.export_stale) return "Export the round again first"
                if (backend.exporting && backend.exportSessionId === session) return "Wait for its export to finish"
                if (backend.uploading) return ""
                if (backend.hubDryRun) return ""
                if (!backend.uploadAllowed) return "Contributions aren't open yet"
                if (!loggedIn) return "Log in first (step 2)"
                return ""
            }

            function refreshInfo() { info = backend.uploadInfo(session) }
            onSessionChanged: refreshInfo()
            Connections {
                target: backend
                function onSessionsChanged() { uploadView.refreshInfo() }
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
                DraftBanner { Layout.fillWidth: true }
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
                    text: "Rehearsal: FT_EYEREC_ALLOW_UPLOAD=1 allows uploads to " + backend.dataset
                          + " while the texts wait for their review."
                }
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.PlaceholderMessage {
                    Layout.fillWidth: true
                    visible: uploadView.exported.length === 0
                    icon.name: "document-export"
                    text: "Nothing exported yet"
                    explanation: "Review a round, then export it. It can be uploaded from here."
                    helpfulAction: Kirigami.Action {
                        text: "Export"; icon.name: "document-export"
                        onTriggered: root.show(exportPage)
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
                        text: "Your export goes to <a href=\"" + backend.datasetUrl + "\">" + backend.dataset + "</a> as a "
                              + "pull request from your own Hugging Face account. The eye images and health answers in it "
                              + "are encrypted: only the dataset's maintainers can read them. Nothing joins the dataset "
                              + "until the maintainer has checked it."
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
                                currentIndex = Math.max(0, indexOfValue(root.chosenSession))
                                keep = currentValue || ""
                            }
                            onActivated: root.chosenSession = keep = currentValue
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
                        text: "This round changed since it was exported. Export it again so the upload has your latest deletions."
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
                        text: "The first time, accept the dataset's terms on <a href=\"" + backend.datasetUrl
                              + "\">its page</a>. Upload checks the export, opens your pull request, then sends the files."
                        onLinkActivated: link => Qt.openUrlExternally(link)
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
                              + "is done. Keep the Eye Recorder open."
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
    }
}
