// Frametop Settings' Remote displays page: other computers' monitors as Frametop screens
// (settings/main.qml loads this file the first time it opens). Backend: ft_remote_displays.py
// ("backend", made then: settings/ft_settings.py).
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    id: root
    // settings/main.qml sets these two.
    property QtObject backend
    property var window
    property var openDisplays: ({})  // display id -> its settings are open (each display's expanded)
    property var customSizes: ({})   // display id -> its "Custom size" switch, once the user set it
    function showPassiveNotification(text, duration) { window.showPassiveNotification(text, duration) }

    function page(name) { return name === "remote-displays" ? hostsPage : null }

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    // What a stream is doing, in words (ft-screens' state, or ours for a disconnected one).
    function stateText(state) {
        const words = { live: "streaming", connecting: "connecting", starting: "starting", queued: "waiting its turn",
                        lost: "lost, trying again", disconnected: "disconnected",
                        "desktop off": "connects when the Frametop desktop starts, or Stream without the desktop",
                        "not running": "not running" }
        if (state in words) return words[state]
        if (state.startsWith("live via ")) return "streaming over the " + state.slice(9)
        if (state.startsWith("lost ")) return "lost (" + state.slice(5) + "), trying again"
        return state
    }
    function stateColor(state) {
        if (state.startsWith("live")) return Kirigami.Theme.positiveTextColor
        if (state.startsWith("lost")) return Kirigami.Theme.negativeTextColor
        if (state === "connecting" || state === "starting" || state === "queued") return Kirigami.Theme.neutralTextColor
        return Kirigami.Theme.disabledTextColor
    }

    // ---------------------------------------------------------------- dialogs
    // A host: picked from the computers found on the network (or typed in), then a sign-in
    // with its Web UI login: Vibepollo gives Frametop its own API token; Sunshine (a Mac) has
    // none, so the login pairs the displays picked next and is then forgotten. A host with no
    // Web UI login to give (any Moonlight host: an unmodified Sunshine, a USB KVM) is added with
    // "Pair with a PIN": each display then pairs with a PIN shown here, typed in on the host
    // (pinDialog). Also signs in again to one already added (openFor).
    Kirigami.PromptDialog {
        id: hostDialog
        objectName: "hostDialog"
        title: existing ? "Sign in to " + fixedName : "Add a computer"
        standardButtons: Kirigami.Dialog.NoButton
        property bool existing: false
        property string fixedName: ""
        property string fixedAddress: ""
        property string fixedDongle: ""
        property var found: []
        property bool searching: false
        property bool working: false
        property string error: ""
        property bool byPin: false  // no sign-in: its displays pair with a PIN (issue 107)
        property int picked: -1  // index in found; found.length: typed in
        readonly property bool typed: picked === found.length
        readonly property string pickedName: existing ? fixedName
            : typed ? (otherName.text.trim() || otherAddress.text.trim()) : picked >= 0 ? found[picked].name : ""
        readonly property string pickedAddress: existing ? fixedAddress
            : typed ? otherAddress.text.trim() : picked >= 0 ? found[picked].address : ""
        readonly property string pickedDongle: existing ? fixedDongle : !typed && picked >= 0 ? found[picked].dongle : ""
        readonly property bool ok: pickedName !== "" && pickedAddress !== "" && !working
                                   && (byPin || (user.text !== "" && password.text !== ""))
        function look() {
            searching = true
            backend.discoverHosts()
        }
        function openNew() {
            existing = false; found = []; picked = -1; error = ""; working = false; byPin = false
            otherName.text = ""; otherAddress.text = ""; password.text = ""
            open()
            look()
        }
        function openFor(h) {
            existing = true; fixedName = h.name; fixedAddress = h.address; fixedDongle = h.direct.split(",")[0].trim()
            error = ""; working = false; password.text = ""; byPin = false
            open()
            user.forceActiveFocus()
        }
        function accept() {
            if (!ok) return
            working = true; error = ""
            if (byPin) backend.addPinHost(pickedName, pickedAddress, pickedDongle)
            else backend.signIn(pickedName, pickedAddress, pickedDongle, user.text, password.text)
        }
        onClosed: password.text = ""
        Connections {
            target: backend
            function onHostsFound(list) {
                hostDialog.searching = false
                hostDialog.found = list
                if (hostDialog.picked < 0 || hostDialog.picked > list.length) {
                    const free = list.findIndex(h => !h.added)
                    hostDialog.picked = free >= 0 ? free : list.length
                }
            }
            function onSignedIn(name, ok, why) {
                if (!hostDialog.working) return
                hostDialog.working = false
                if (!ok) {
                    hostDialog.error = why
                    return
                }
                password.text = ""
                hostDialog.close()
                root.showPassiveNotification((hostDialog.byPin ? "Added " : "Signed in to ") + name)
                const h = backend.hosts.find(x => x.name === name)
                if (h && (!hostDialog.existing || h.kind === "sunshine")) displayDialog.openFor(h)
            }
        }
        ColumnLayout {
            RowLayout {
                visible: !hostDialog.existing
                Controls.Label { text: "Computers running Vibepollo, Sunshine or macOS Screen Sharing on your network:"; Layout.fillWidth: true }
                Controls.ToolButton {
                    icon.name: "view-refresh"
                    text: "Look again"
                    display: Controls.AbstractButton.IconOnly
                    enabled: !hostDialog.searching
                    onClicked: hostDialog.look()
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered
                }
            }
            Controls.BusyIndicator { visible: hostDialog.searching; running: visible; Layout.alignment: Qt.AlignHCenter }
            Repeater {
                model: hostDialog.existing ? [] : hostDialog.found
                Controls.RadioButton {
                    required property var modelData
                    required property int index
                    enabled: !modelData.added
                    text: modelData.name + " (" + modelData.address + (modelData.dongle ? ", and on the dongle" : "") + ")"
                          + (modelData.added ? ": added" : "")
                    checked: hostDialog.picked === index
                    onToggled: if (checked) hostDialog.picked = index
                }
            }
            Controls.RadioButton {
                visible: !hostDialog.existing && !hostDialog.searching
                text: hostDialog.found.length ? "Another one, by its address" : "None found: type its address"
                checked: hostDialog.typed
                onToggled: if (checked) hostDialog.picked = hostDialog.found.length
            }
            Kirigami.FormLayout {
                Layout.fillWidth: true
                visible: hostDialog.typed && !hostDialog.existing
                Controls.TextField { id: otherAddress; Kirigami.FormData.label: "Address:"; placeholderText: "192.168.1.20" }
                Controls.TextField { id: otherName; Kirigami.FormData.label: "Name:"; placeholderText: "My PC"; maximumLength: 40 }
            }
            Controls.CheckBox {
                objectName: "pinPairing"
                visible: !hostDialog.existing
                Layout.topMargin: Kirigami.Units.largeSpacing
                text: "Pair with a PIN instead (no sign-in)"
                checked: hostDialog.byPin
                onToggled: hostDialog.byPin = checked
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: hostDialog.byPin
                      ? "Use PIN pairing for standard Sunshine or Moonlight servers without Web UI sign-in. Each display you add shows a 4-digit PIN to enter on your PC (monitor auto-detection isn't available in PIN mode)."
                      : "Sign in with your Vibepollo/Sunshine Web UI login, or your Mac login (with macOS Screen Sharing enabled). Vibepollo saves a limited pairing token instead of your password; Mac logins are saved privately for reconnecting; Sunshine logins are only used once to pair."
            }
            Kirigami.FormLayout {
                Layout.fillWidth: true
                visible: !hostDialog.byPin
                Controls.TextField { id: user; Kirigami.FormData.label: "User name:" }
                Controls.TextField {
                    id: password
                    Kirigami.FormData.label: "Password:"
                    echoMode: TextInput.Password
                    onAccepted: hostDialog.accept()
                }
            }
            Controls.BusyIndicator { visible: hostDialog.working; running: visible; Layout.alignment: Qt.AlignHCenter }
            Controls.Label {
                visible: hostDialog.error !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Kirigami.Theme.negativeTextColor
                text: hostDialog.error
            }
        }
        customFooterActions: [
            Kirigami.Action { text: hostDialog.byPin ? "Add" : "Sign in"; icon.name: "go-next"; enabled: hostDialog.ok; onTriggered: hostDialog.accept() },
            Kirigami.Action { text: "Cancel"; icon.name: "dialog-cancel"; onTriggered: { password.text = ""; hostDialog.close() } }
        ]
    }

    // A host's app windows: a sign-in to the Vibepollo on it that shares them, which is its
    // own on Windows and a second one in the Hyprland session on Linux. Frametop keeps that
    // one's token; the host's apps and windows then show in the launcher.
    Kirigami.PromptDialog {
        id: windowsDialog
        objectName: "windowsDialog"
        title: "App windows on " + hostName
        standardButtons: Kirigami.Dialog.NoButton
        property string hostName: ""
        property bool working: false
        property string error: ""
        readonly property bool ok: windowsUser.text !== "" && windowsPassword.text !== "" && !working
        function openFor(h) {
            hostName = h.name; error = ""; working = false; windowsPassword.text = ""
            open()
            windowsUser.forceActiveFocus()
        }
        function accept() {
            if (!ok) return
            working = true; error = ""
            backend.appWindowsOn(hostName, windowsUser.text, windowsPassword.text)
        }
        onClosed: windowsPassword.text = ""
        Connections {
            target: backend
            function onAppWindowsSet(name, ok, why) {
                if (!windowsDialog.working || name !== windowsDialog.hostName) return
                windowsDialog.working = false
                if (!ok) {
                    windowsDialog.error = why
                    return
                }
                windowsPassword.text = ""
                windowsDialog.close()
                root.showPassiveNotification(name + "'s apps and windows are in the launcher (Meta+Space)")
            }
        }
        ColumnLayout {
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Lists this computer's apps and open windows in Frunner (Meta+Space) so each window opens as its own VR panel. Sign in with your Vibepollo Web UI credentials (on Linux/Hyprland, the login created with vibepollo-frametop-windows setup). Only a pairing token is saved — your password is not stored."
            }
            Kirigami.FormLayout {
                Layout.fillWidth: true
                Controls.TextField { id: windowsUser; objectName: "windowsUser"; Kirigami.FormData.label: "User name:" }
                Controls.TextField {
                    id: windowsPassword
                    objectName: "windowsPassword"
                    Kirigami.FormData.label: "Password:"
                    echoMode: TextInput.Password
                    onAccepted: windowsDialog.accept()
                }
            }
            Controls.BusyIndicator { visible: windowsDialog.working; running: visible; Layout.alignment: Qt.AlignHCenter }
            Controls.Label {
                visible: windowsDialog.error !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Kirigami.Theme.negativeTextColor
                text: windowsDialog.error
            }
        }
        customFooterActions: [
            Kirigami.Action { text: "Turn on"; icon.name: "go-next"; enabled: windowsDialog.ok; onTriggered: windowsDialog.accept() },
            Kirigami.Action { text: "Cancel"; icon.name: "dialog-cancel"; onTriggered: { windowsPassword.text = ""; windowsDialog.close() } }
        ]
    }

    // A host's displays to add: its monitors as they are (all of them ticked, but the ones
    // already added), and on Vibepollo, a virtual one at any size. A monitor is known by its
    // key: what it streams, and a Sunshine instance's port.
    Kirigami.PromptDialog {
        id: displayDialog
        objectName: "displayDialog"
        title: "Add displays from " + host
        standardButtons: Kirigami.Dialog.NoButton
        property string host: ""
        property string address: ""
        property string kind: ""
        onKindChanged: {
            const wantW = kind === "apple" ? 3840 : 2560, wantH = kind === "apple" ? 2160 : 1440
            displayRes.currentIndex = Math.max(0, backend.resolutions.findIndex(r => r.width === wantW && r.height === wantH))
        }
        property var added: []     // its displays' keys, so a monitor isn't added twice
        property var monitors: []
        property var chosen: ({})  // app -> ticked
        property bool virtualChosen: false
        property string error: ""
        property bool loading: false
        readonly property bool virtualAlready: kind === "apple" && added.some(k => k.startsWith("apple:virtual"))
        readonly property int count: monitors.filter(m => chosen[m.key]).length + (virtualChosen ? 1 : 0)
        function openFor(h) {
            host = h.name; address = h.address; kind = h.kind; added = h.displays.map(d => d.key); pairing = false
            monitors = []; chosen = ({}); virtualChosen = false; error = ""; loading = true
            const wantW = h.kind === "apple" ? 3840 : 2560, wantH = h.kind === "apple" ? 2160 : 1440
            displayRes.currentIndex = Math.max(0, backend.resolutions.findIndex(r => r.width === wantW && r.height === wantH))
            open()
            backend.listMonitors(address)
        }
        function tick(key, on) {
            const c = Object.assign({}, chosen)
            c[key] = on
            chosen = c
            if (on && kind === "apple") virtualChosen = false
        }
        function accept() {
            if (!count) return
            pairing = true
            close()
            const fps = displayRate.currentValue, kbps = Math.round(displayBitrate.value * 1000)
            const r = backend.resolutions[displayRes.currentIndex]
            for (const m of monitors) {
                if (!chosen[m.key]) continue
                if (kind === "pin") {  // its app by the name typed, at the size picked
                    const app = pinApp.text.trim() || m.app
                    backend.addDisplay(host, app, app, r.width, r.height, fps, kbps, 0)
                } else {
                    backend.addDisplay(host, m.app, m.name, m.width, m.height, fps, kbps, m.port)
                }
            }
            if (virtualChosen)  // a Mac makes it with its own Screen Sharing (ft-appless)
                backend.addDisplay(host, kind === "apple" ? "apple:virtual" : "monitor", "Virtual", r.width, r.height, fps, kbps, 0)
            if (kind === "sunshine") backend.forgetLogin(address)  // queued with each display
        }
        function later() {
            close()
        }
        // Escape and a click outside close it without later(): a Sunshine login is forgotten then
        // too. accept() forgets it itself, once each display has it queued.
        property bool pairing: false
        onClosed: if (!pairing && kind === "sunshine") backend.forgetLogin(address)
        Connections {
            target: backend
            function onMonitorsReady(address, monitors, error) {
                if (address !== displayDialog.address) return
                displayDialog.loading = false
                displayDialog.monitors = monitors.filter(m => !m.virtual && m.active)
                const c = {}
                for (const m of displayDialog.monitors)
                    c[m.key] = !displayDialog.added.includes(m.key) && !(displayDialog.kind === "apple" && displayDialog.added.length > 0)
                displayDialog.chosen = c
                displayDialog.error = error
            }
        }
        ColumnLayout {
            Controls.BusyIndicator { visible: displayDialog.loading; running: visible; Layout.alignment: Qt.AlignHCenter }
            Controls.Label {
                visible: displayDialog.error !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Couldn't list its monitors: " + displayDialog.error
            }
            Repeater {
                model: displayDialog.monitors
                Controls.CheckBox {
                    required property var modelData
                    readonly property bool already: displayDialog.added.includes(modelData.key)
                    enabled: !already
                    text: modelData.name + " (" + modelData.width + " × " + modelData.height + (modelData.primary ? ", main" : "") + ")"
                          + (already ? ": added" : "")
                    checked: displayDialog.chosen[modelData.key] === true
                    onToggled: displayDialog.tick(modelData.key, checked)
                }
            }
            Kirigami.FormLayout {
                Layout.fillWidth: true
                visible: displayDialog.kind === "pin" && displayDialog.count > 0
                Controls.TextField {
                    id: pinApp
                    objectName: "pinApp"
                    Kirigami.FormData.label: "Its app:"
                    text: "Desktop"
                    // ft-layout's rule for an app's name (no spaces).
                    validator: RegularExpressionValidator { regularExpression: /[A-Za-z0-9._:-]{1,40}/ }
                }
            }
            Controls.CheckBox {
                objectName: "virtualDisplayCheck"
                // Sunshine can't make them on a Mac, nor a host paired by PIN without its API.
                visible: !displayDialog.loading && displayDialog.kind !== "sunshine" && displayDialog.kind !== "pin"
                enabled: !displayDialog.virtualAlready
                text: "Create a virtual monitor (at the resolution you choose below)"
                      + (displayDialog.virtualAlready ? ": added" : "")
                checked: displayDialog.virtualChosen
                onToggled: {
                    displayDialog.virtualChosen = checked
                    if (checked && displayDialog.kind === "apple") displayDialog.chosen = ({})
                }
            }
            Kirigami.FormLayout {
                Layout.fillWidth: true
                visible: displayDialog.count > 0
                Controls.ComboBox {
                    id: displayRes
                    Kirigami.FormData.label: displayDialog.kind === "pin" ? "Stream size:" : "Virtual display:"
                    visible: displayDialog.virtualChosen || displayDialog.kind === "pin"
                    model: backend.resolutions
                    textRole: "text"
                    Component.onCompleted: currentIndex = Math.max(0, backend.resolutions.findIndex(r => r.width === 2560 && r.height === 1440))
                }
                Controls.ComboBox {
                    id: displayRate
                    Kirigami.FormData.label: "Frame rate:"
                    model: backend.streamRates.map(r => ({ text: r + " fps", value: r }))
                    textRole: "text"
                    valueRole: "value"
                    Component.onCompleted: currentIndex = Math.max(0, indexOfValue(60))
                }
                RowLayout {
                    Kirigami.FormData.label: "Bitrate:"
                    Controls.SpinBox { id: displayBitrate; from: 0; to: 150; stepSize: 5; value: 0; editable: true }
                    Controls.Label { text: displayBitrate.value === 0 ? "Mbps (0 = automatic)" : "Mbps" }
                }
            }
        }
        customFooterActions: [
            Kirigami.Action {
                text: displayDialog.count > 1 ? "Add " + displayDialog.count : "Add"
                icon.name: "list-add"
                enabled: displayDialog.count > 0
                onTriggered: displayDialog.accept()
            },
            Kirigami.Action { text: "Later"; icon.name: "dialog-cancel"; onTriggered: displayDialog.later() }
        ]
    }

    // A KDE PC's open windows to add as panels (issue 81; its own file).
    KwinWindowsDialog { id: kwinDialog; windows: root.backend.kwinWindows }

    // A PIN pairing (a host added with "Pair with a PIN"): the PIN to type in on the host, shown
    // while ft-layout waits for it (3 minutes at most). It closes once the pairing ends either way;
    // the page's message says how it went.
    Kirigami.PromptDialog {
        id: pinDialog
        objectName: "pinDialog"
        title: what
        standardButtons: Kirigami.Dialog.NoButton
        property string what: ""
        property string pin: ""
        Connections {
            target: backend
            function onPairingPin(label, pin) {
                pinDialog.what = label; pinDialog.pin = pin
                pinDialog.open()
            }
            function onBusyChanged() {
                if (pinDialog.opened && backend.busy !== pinDialog.what) pinDialog.close()
            }
        }
        ColumnLayout {
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Enter this PIN on your computer's streaming pairing page (in Sunshine or Vibepollo's Web UI → PIN)."
            }
            Kirigami.Heading {
                objectName: "pinText"
                level: 1
                text: pinDialog.pin
                Layout.alignment: Qt.AlignHCenter
            }
            Controls.BusyIndicator { running: pinDialog.opened; Layout.alignment: Qt.AlignHCenter }
        }
        customFooterActions: [
            Kirigami.Action { text: "Cancel"; icon.name: "dialog-cancel"; onTriggered: backend.cancelPairing() }
        ]
    }

    // ---------------------------------------------------------------- hosts and their displays
    Component {
        id: hostsPage
        Kirigami.ScrollablePage {
            title: "Remote displays"
            // What ft-layout is doing now (signing in, pairing, connecting).
            footer: Controls.ToolBar {
                visible: backend.busy !== ""
                RowLayout {
                    anchors.fill: parent
                    Controls.BusyIndicator { running: backend.busy !== ""; Layout.preferredHeight: Kirigami.Units.iconSizes.medium }
                    Controls.Label { text: backend.busy + "…"; Layout.fillWidth: true }
                }
            }
            actions: [
                Kirigami.Action {
                    text: "Add computer"
                    icon.name: "list-add"
                    onTriggered: hostDialog.openNew()
                }
            ]
            header: Kirigami.InlineMessage {
                position: Kirigami.InlineMessage.Position.Header
                visible: true
                type: backend.desktopRunning ? Kirigami.MessageType.Information : Kirigami.MessageType.Warning
                // Without the desktop, ft-screens can run alone and show just these displays.
                actions: [
                    Kirigami.Action {
                        text: "Stream without the desktop"
                        icon.name: "media-playback-start"
                        visible: !backend.desktopRunning
                        enabled: backend.busy === ""
                        onTriggered: backend.startStreams()
                    },
                    Kirigami.Action {
                        text: "Stop streaming"
                        icon.name: "media-playback-stop"
                        visible: backend.streamsOnly
                        enabled: backend.busy === ""
                        onTriggered: backend.stopStreams()
                    }
                ]
                text: !backend.desktopRunning
                      ? "The Frametop desktop isn't running. Connected displays will start automatically with the desktop, or you can stream them on their own now."
                      : backend.streamsOnly
                        ? "Streaming remote displays standalone (without the Frametop desktop). Click Stop streaming before starting the full desktop."
                        : backend.hosts.length === 0
                        ? "Bring your PC or Mac monitors into VR alongside your Frametop screens. Install Vibepollo or Sunshine on your PC (or turn on Screen Sharing on a Mac), then click Add computer above."
                        : "Each remote monitor behaves like a VR screen — move, resize, curve, or pin it, and save its position in Layout & profiles. Disconnected displays keep their saved settings."
            }
            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing
                Repeater {
                    model: backend.hosts
                    delegate: Kirigami.AbstractCard {
                        id: hostCard
                        required property var modelData
                        readonly property bool anyConnected: modelData.displays.some(d => d.connected)
                        // Sunshine keeps no token: adding displays signs in first, every time.
                        readonly property bool sunshine: modelData.kind === "sunshine"
                        readonly property bool apple: modelData.kind === "apple"
                        readonly property bool vibepollo: !sunshine && !apple
                        readonly property string kind: modelData.kind
                        // Its Vibepollo can turn off and set its own monitors (Frametop's build).
                        readonly property bool canOwnMonitors: vibepollo && modelData.hasToken && kind !== "pin"
                        readonly property var monitorsListed: backend.hostMonitors.listed[modelData.name] || ({ monitors: [], note: "", stale: false })
                        Component.onCompleted: if (canOwnMonitors && (modelData.monitors || !(modelData.name in backend.hostMonitors.supported)))
                                                   backend.hostMonitors.refresh(modelData.name)
                        Layout.fillWidth: true
                        contentItem: ColumnLayout {
                            RowLayout {
                                Kirigami.Icon {
                                    source: "computer"
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                                }
                                ColumnLayout {
                                    spacing: 0
                                    Kirigami.Heading { level: 3; text: hostCard.modelData.name }
                                    Controls.Label {
                                        text: hostCard.modelData.address + "  ·  "
                                              + (hostCard.modelData.online === true ? "online"
                                                 : hostCard.modelData.online === false ? "not answering" : "checking…")
                                              + (hostCard.modelData.hasToken || hostCard.sunshine ? "" : "  ·  not signed in")
                                        color: hostCard.modelData.online === false ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                                        opacity: hostCard.modelData.online === false ? 1 : 0.7
                                    }
                                }
                                Item { Layout.fillWidth: true }
                                Controls.Switch {
                                    text: "Connected"
                                    visible: hostCard.modelData.displays.length > 0
                                    checked: hostCard.anyConnected
                                    onToggled: backend.setHostConnected(hostCard.modelData.name, checked)
                                    Controls.ToolTip.text: "Connect or disconnect all of its displays"
                                    Controls.ToolTip.visible: hovered
                                }
                                Controls.Button {
                                    visible: hostCard.modelData.hasToken || hostCard.sunshine
                                    text: "Add displays"
                                    icon.name: "list-add"
                                    onClicked: hostCard.sunshine && !hostCard.modelData.hasToken
                                               ? hostDialog.openFor(hostCard.modelData) : displayDialog.openFor(hostCard.modelData)
                                }
                                Controls.Button {
                                    // A KDE PC with KWin Window Stream: its windows, each a panel.
                                    visible: hostCard.modelData.kwinWindows
                                    text: "Add windows"
                                    icon.name: "window-new"
                                    onClicked: kwinDialog.openFor(hostCard.modelData)
                                    Controls.ToolTip.text: "Open windows on this PC, each as a panel of its own. Needs KDE "
                                                           + "Plasma with KWin Window Stream (github.com/utrack/kwin-window-stream); "
                                                           + "they connect with this computer's displays' pairing."
                                    Controls.ToolTip.visible: hovered
                                }
                                Controls.Button {
                                    visible: !hostCard.modelData.hasToken && !hostCard.sunshine
                                    text: "Sign in"
                                    icon.name: "unlock"
                                    onClicked: hostDialog.openFor(hostCard.modelData)
                                }
                                Controls.ToolButton {
                                    visible: hostCard.modelData.hasToken && !hostCard.sunshine && hostCard.modelData.kind !== "pin"
                                    icon.name: "unlock"
                                    display: Controls.AbstractButton.IconOnly
                                    text: "Sign in again"
                                    Controls.ToolTip.text: hostCard.apple ? "Sign in again to update the saved Mac login"
                                                           : "Sign in again: a new token replaces the one kept here (revoke the old one in its Web UI, API Tokens)"
                                    Controls.ToolTip.visible: hovered
                                    onClicked: hostDialog.openFor(hostCard.modelData)
                                }
                                Controls.ToolButton {
                                    icon.name: "edit-delete-remove"
                                    display: Controls.AbstractButton.IconOnly
                                    enabled: hostCard.modelData.displays.length === 0
                                    text: "Remove this host"
                                    Controls.ToolTip.text: enabled ? text : "Remove its displays first"
                                    Controls.ToolTip.visible: hovered
                                    onClicked: backend.removeHost(hostCard.modelData.name)
                                }
                            }
                            // How its streams reach it: its Steam Link dongle on the Frame's hotspot
                            // (no router in the way) or the home network. Without a dongle, the
                            // network, and Find to look for one.
                            RowLayout {
                                id: route
                                property string finding: ""
                                readonly property bool hasDongle: hostCard.modelData.direct !== ""
                                Controls.Label { text: route.hasDongle ? "Connection:" : "Connection: the network" }
                                Controls.ComboBox {
                                    visible: route.hasDongle
                                    model: [{ text: "Auto (dongle when it's up)", value: "auto" },
                                            { text: "Network only", value: "network" },
                                            { text: "Dongle only", value: "dongle" }]
                                    textRole: "text"
                                    valueRole: "value"
                                    currentIndex: Math.max(0, indexOfValue(hostCard.modelData.route))
                                    onActivated: if (currentValue !== hostCard.modelData.route) backend.setRoute(hostCard.modelData.name, currentValue)
                                    Controls.ToolTip.text: "Switching connection restarts this computer's active display streams (virtual displays reconnect fresh)"
                                    Controls.ToolTip.visible: hovered
                                }
                                Controls.Label { visible: route.hasDongle; text: "Dongle:" }
                                Controls.TextField {
                                    id: dongle
                                    visible: route.hasDongle
                                    text: hostCard.modelData.direct
                                    placeholderText: "not found yet"
                                    Layout.preferredWidth: Kirigami.Units.gridUnit * 7
                                    onEditingFinished: if (text !== hostCard.modelData.direct) backend.setDirect(hostCard.modelData.name, text)
                                }
                                Controls.Button {
                                    text: route.finding === "…" ? "Looking…" : route.hasDongle ? "Find" : "Find a dongle"
                                    icon.name: "edit-find"
                                    flat: !route.hasDongle
                                    enabled: route.finding !== "…"
                                    onClicked: { route.finding = "…"; backend.findDongle(hostCard.modelData.name) }
                                    Controls.ToolTip.text: "Look for this computer on the Frame's hotspot (Steam Link's dongle)"
                                    Controls.ToolTip.visible: hovered
                                }
                                Controls.Label {
                                    visible: route.finding !== "" && route.finding !== "…"
                                    text: route.finding
                                    opacity: 0.7
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Connections {
                                    target: backend
                                    function onDongleFound(name, address, why) {
                                        if (name !== hostCard.modelData.name) return
                                        route.finding = address ? "found " + address : "not found: " + why
                                        if (address && address !== hostCard.modelData.direct) backend.setDirect(name, address)
                                    }
                                }
                            }
                            // The Frame's microphone rides along with its streams. Only
                            // Frametop's build of Vibepollo receives it.
                            Controls.Switch {
                                objectName: "sendMic"
                                visible: hostCard.vibepollo
                                text: "Send my microphone"
                                checked: hostCard.modelData.mic
                                onToggled: backend.setMic(hostCard.modelData.name, checked)
                                Controls.ToolTip.text: "Apps on it can record the Frame's microphone while its displays stream: "
                                                       + "pick \"Microphone (Steam Streaming Microphone)\" on Windows (it needs Steam installed), "
                                                       + "\"Frametop Microphone\" on Linux. Needs Frametop's build of Vibepollo."
                                Controls.ToolTip.visible: hovered
                            }
                            // Its own keyboard and mouse, each lent on its own while the headset is
                            // worn: with only the keyboard lent, its mouse keeps moving its desktop.
                            // Only Frametop's build of Vibepollo lends them.
                            ColumnLayout {
                                visible: hostCard.vibepollo
                                spacing: 0
                                Controls.Label { text: "Use this computer's own" }
                                RowLayout {
                                    spacing: Kirigami.Units.largeSpacing
                                    Repeater {
                                        model: [{ kind: "keyboard", label: "Keyboard", flag: "inputKeyboard" },
                                                { kind: "mouse", label: "Mouse", flag: "inputMouse" }]
                                        delegate: Controls.Switch {
                                            required property var modelData
                                            objectName: "input-" + modelData.kind
                                            text: modelData.label
                                            checked: hostCard.modelData[modelData.flag]
                                            onToggled: backend.setInput(hostCard.modelData.name, modelData.kind, checked)
                                            Controls.ToolTip.text: "While you wear the headset and its displays stream, its "
                                                                   + modelData.kind + " works in Frametop and stops working on "
                                                                   + "the computer. Take the headset off, or press both Shifts "
                                                                   + "and Escape on it, to give it back. Works on Windows and "
                                                                   + "Linux with Frametop's build of Vibepollo, with host input "
                                                                   + "turned on there. Lending only one of the two needs a "
                                                                   + "Linux host for now: Windows lends both or neither."
                                            Controls.ToolTip.visible: hovered
                                        }
                                    }
                                }
                            }
                            // Its apps' windows as panels of their own, and in the launcher.
                            // Turning it on signs in to the Vibepollo that shares them, so
                            // the switch stays as it was until that works.
                            Controls.Switch {
                                id: appWindows
                                objectName: "appWindows"
                                visible: hostCard.vibepollo
                                text: "Show apps and windows in the launcher"
                                checked: hostCard.modelData.appWindows
                                onToggled: {
                                    if (checked) {
                                        checked = Qt.binding(() => hostCard.modelData.appWindows)
                                        windowsDialog.openFor(hostCard.modelData)
                                    } else {
                                        backend.appWindowsOff(hostCard.modelData.name)
                                    }
                                }
                                Controls.ToolTip.text: "The launcher (Meta+Space) lists this computer's apps and open windows, "
                                                       + "and each one opens as a panel of its own while the window leaves "
                                                       + "its monitors. Needs Frametop's build of Vibepollo: on Windows as it "
                                                       + "is, on Linux with Hyprland and vibepollo-frametop-windows setup."
                                Controls.ToolTip.visible: hovered
                            }
                            // Copied text and images both ways, at the moments typing changes
                            // sides. Only Vibepollo has the request.
                            Controls.Switch {
                                objectName: "shareClipboard"
                                visible: hostCard.vibepollo
                                text: "Share the clipboard"
                                checked: hostCard.modelData.clipboard
                                onToggled: backend.setClipboard(hostCard.modelData.name, checked)
                                Controls.ToolTip.text: "Text or an image you copy on the desktop can be pasted on this "
                                                       + "computer once you click one of its displays, and what you copy "
                                                       + "there can be pasted on the desktop once you click back. Needs "
                                                       + "Vibepollo (images and Linux: Frametop's build). Displays added "
                                                       + "before this switch need Clipboard allowed for their client in "
                                                       + "Vibepollo's Web UI (Clients), or adding again."
                                Controls.ToolTip.visible: hovered
                            }
                            // Its own monitors follow its display panels (Frametop's build of
                            // Vibepollo; host_monitors.py): this changes the PC itself. Right above
                            // the displays it acts on.
                            Kirigami.Separator {
                                visible: ownMonitorsBox.visible
                                Layout.fillWidth: true
                            }
                            ColumnLayout {
                                id: ownMonitorsBox
                                visible: hostCard.canOwnMonitors && (backend.hostMonitors.supported[hostCard.modelData.name] === true
                                                                     || hostCard.modelData.monitors)
                                spacing: 0
                                Layout.fillWidth: true
                                Controls.Switch {
                                    objectName: "ownMonitors"
                                    text: "Let Frametop control " + hostCard.modelData.name + "'s displays"
                                    checked: hostCard.modelData.monitors
                                    onToggled: backend.hostMonitors.setControl(hostCard.modelData.name, checked)
                                    Controls.ToolTip.text: "This setting lets Frametop turn off or resize "
                                                           + hostCard.modelData.name + "'s displays while connected."
                                    Controls.ToolTip.visible: hovered
                                }
                                Controls.Label {
                                    objectName: "ownMonitorsHelp"
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    opacity: 0.7
                                    text: "Lets Frametop turn off or resize " + hostCard.modelData.name + "'s displays while "
                                          + "connected: each display below switches to its stream size and the scale picked "
                                          + "in its settings, and the ones that aren't below, or are disconnected, turn off. "
                                          + "They go back when the last one is disconnected, or 15 seconds after its stream is lost."
                                }
                                Controls.Label {
                                    objectName: "ownMonitorsNote"
                                    visible: hostCard.modelData.monitors && hostCard.monitorsListed.note !== ""
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    color: Kirigami.Theme.negativeTextColor
                                    text: hostCard.monitorsListed.note
                                    textFormat: Text.PlainText
                                }
                            }
                            // Its sign-in is from before Frametop could ask for its monitors: the
                            // switch can't show until it signs in again (with the switch on, the
                            // note above says so).
                            Controls.Label {
                                objectName: "ownMonitorsSignIn"
                                visible: hostCard.canOwnMonitors && !hostCard.modelData.monitors
                                         && hostCard.monitorsListed.stale === true
                                         && backend.hostMonitors.supported[hostCard.modelData.name] !== true
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                opacity: 0.7
                                text: "To let Frametop control " + hostCard.modelData.name + "'s displays, sign in "
                                      + "again: its sign-in is from before Frametop could. (It needs Frametop's build of Vibepollo.)"
                            }
                            Controls.Label {
                                visible: hostCard.modelData.displays.length === 0
                                text: "No displays yet."
                                opacity: 0.7
                            }
                            Repeater {
                                model: hostCard.modelData.displays
                                delegate: ColumnLayout {
                                    id: disp
                                    required property var modelData
                                    readonly property bool appleMain: modelData.app === "apple:physical"
                                    // A monitor of a PC whose monitors follow its panels: the PC puts
                                    // it at the stream size set here. (From its own entry: the card's
                                    // properties aren't there yet while the Repeater makes it.)
                                    readonly property bool followsMonitor: modelData.followsMonitor
                                                                           && backend.hostMonitors.supported[modelData.host] !== false
                                    readonly property bool ownSize: !appleMain
                                    // Its PC could set it, with the switch on (Frametop's build of Vibepollo)
                                    readonly property bool couldFollow: !followsMonitor && !modelData.virtual && !modelData.kwin
                                                                        && backend.hostMonitors.supported[modelData.host] === true
                                    readonly property var monitor: followsMonitor
                                        ? ((backend.hostMonitors.listed[modelData.host] || {}).monitors || [])
                                              .find(m => "display:" + m.id === modelData.app) || null : null
                                    // The biggest size its monitor takes ([width, height], or null: not
                                    // known): its size boxes stop there.
                                    readonly property var largest: monitor && monitor.largest ? monitor.largest : null
                                    // "Custom size", as the user left it (StreamSizePicker's custom); kept
                                    // by the page like expanded.
                                    property var customSize: root.customSizes[modelData.id]
                                    // Its monitor can't be its stream size (the PC's driver refused it,
                                    // or it lists no such mode): the stream crops or fits it.
                                    readonly property bool cantBeSize: monitor !== null && backend.hostMonitors.cantBe(
                                        modelData.host, monitor.id, modelData.width, modelData.height)
                                    readonly property string sizesItHas: {
                                        const sizes = monitor && monitor.sizes ? monitor.sizes : []
                                        if (sizes.length === 0)
                                            return ""
                                        return " Sizes it has: " + sizes.slice(0, 4).map(s => s[0] + " × " + s[1]).join(", ")
                                               + (sizes.length > 4 ? " and smaller." : ".")
                                    }
                                    // Kept by the page: the list is made again when the layout changes
                                    // (Apply, a scale pick), and its settings stay open through that.
                                    property bool expanded: root.openDisplays[modelData.id] === true
                                    onExpandedChanged: root.openDisplays[modelData.id] = expanded
                                    readonly property bool showRoutes: !modelData.kwin && (hostCard.modelData.direct !== "" || modelData.hasRoutes)
                                    property bool directBitrateEdited: false
                                    property bool networkBitrateEdited: false
                                    function routeRateModel(currentFps) {
                                        const list = [{ text: "Default (" + modelData.fps + " fps)", value: 0 }]
                                        const rates = backend.streamRates.slice()
                                        if (currentFps > 0 && !rates.includes(currentFps)) {
                                            rates.push(currentFps)
                                            rates.sort((a, b) => a - b)
                                        }
                                        for (const r of rates) list.push({ text: r + " fps", value: r })
                                        return list
                                    }
                                    function routeBitrateChanged(mbps, savedKbps, edited) {
                                        if (mbps === 0) return savedKbps > 0 || (savedKbps === 0 && edited)
                                        return mbps * 1000 !== savedKbps
                                    }
                                    function routeBitrateArg(mbps, savedKbps, edited) {
                                        if (!routeBitrateChanged(mbps, savedKbps, edited)) return -2
                                        return mbps === 0 ? -1 : mbps * 1000
                                    }
                                    Layout.fillWidth: true
                                    Kirigami.Separator { Layout.fillWidth: true }
                                    RowLayout {
                                        Kirigami.Icon {
                                            source: disp.modelData.virtual ? "video-display-symbolic" : "video-display"
                                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                                        }
                                        ColumnLayout {
                                            spacing: 0
                                            Kirigami.Heading { level: 4; text: disp.modelData.label }
                                            Controls.Label {
                                                text: (disp.modelData.kwin ? "window" : disp.modelData.virtual ? "virtual display" : "monitor") + ", "
                                                      + disp.modelData.width + " × " + disp.modelData.height + " at " + disp.modelData.fps
                                                      + " fps, screen " + disp.modelData.number
                                                opacity: 0.7
                                            }
                                            Controls.Label {
                                                text: root.stateText(disp.modelData.state)
                                                color: root.stateColor(disp.modelData.state)
                                            }
                                        }
                                        Item { Layout.fillWidth: true }
                                        Controls.Switch {
                                            text: "Connected"
                                            checked: disp.modelData.connected
                                            onToggled: backend.setConnected(disp.modelData.id, checked)
                                        }
                                        Controls.Switch {
                                            text: "Shown"
                                            enabled: disp.modelData.connected
                                            checked: disp.modelData.shown
                                            onToggled: backend.setShown(disp.modelData.id, checked)
                                            Controls.ToolTip.text: "Hide its panel in VR; the stream keeps going, slowly"
                                            Controls.ToolTip.visible: hovered
                                        }
                                        Controls.ToolButton {
                                            icon.name: disp.expanded ? "go-up" : "configure"
                                            display: Controls.AbstractButton.IconOnly
                                            text: disp.expanded ? "Hide display settings" : "Stream & size settings"
                                            Controls.ToolTip.text: text
                                            Controls.ToolTip.visible: hovered
                                            onClicked: disp.expanded = !disp.expanded
                                        }
                                        Controls.ToolButton {
                                            icon.name: "edit-delete-remove"
                                            display: Controls.AbstractButton.IconOnly
                                            text: "Remove this display"
                                            Controls.ToolTip.text: text
                                            Controls.ToolTip.visible: hovered
                                            onClicked: backend.removeDisplay(disp.modelData.id)
                                        }
                                    }
                                    Kirigami.FormLayout {
                                        Layout.fillWidth: true
                                        visible: disp.expanded
                                        RowLayout {
                                            visible: !disp.modelData.kwin  // the window sets its size
                                            Kirigami.FormData.label: "Stream:"
                                            StreamSizePicker {
                                                id: ss
                                                visible: disp.ownSize
                                                sizes: disp.monitor && disp.monitor.sizes ? disp.monitor.sizes : []
                                                largest: disp.largest
                                                currentWidth: disp.modelData.width
                                                currentHeight: disp.modelData.height
                                                custom: disp.customSize
                                                onCustomPicked: (on) => {
                                                    root.customSizes[disp.modelData.id] = on
                                                    disp.customSize = on
                                                }
                                            }
                                            Controls.ComboBox {
                                                id: sr
                                                model: backend.streamRates.map(r => ({ text: r + " fps", value: r }))
                                                textRole: "text"
                                                valueRole: "value"
                                                Component.onCompleted: currentIndex = Math.max(0, indexOfValue(disp.modelData.fps))
                                            }
                                            Controls.SpinBox {
                                                id: sb
                                                from: 0; to: 150; stepSize: 5; editable: true
                                                value: Math.round(disp.modelData.bitrate / 1000)
                                            }
                                            Controls.Label { text: sb.value === 0 ? "Mbps (auto)" : "Mbps" }
                                            Controls.Button {
                                                text: "Apply"
                                                enabled: (disp.ownSize && ss.changed)
                                                         || sr.currentValue !== disp.modelData.fps || sb.value * 1000 !== disp.modelData.bitrate
                                                onClicked: backend.setStream(disp.modelData.id,
                                                                             disp.ownSize ? ss.pickedWidth : disp.modelData.width,
                                                                             disp.ownSize ? ss.pickedHeight : disp.modelData.height,
                                                                             sr.currentValue, sb.value * 1000)
                                            }
                                        }
                                        RowLayout {
                                            objectName: "routeRow-direct"
                                            visible: disp.showRoutes
                                            Kirigami.FormData.label: "Dongle / direct:"
                                            Controls.ComboBox {
                                                id: directRate
                                                objectName: "routeFps-direct"
                                                model: disp.routeRateModel(disp.modelData.directFps)
                                                textRole: "text"
                                                valueRole: "value"
                                                Component.onCompleted: currentIndex = Math.max(0, indexOfValue(disp.modelData.directFps))
                                            }
                                            Controls.SpinBox {
                                                id: directBitrate
                                                objectName: "routeBitrate-direct"
                                                visible: !hostCard.apple
                                                from: 0; to: 150; stepSize: 5; editable: true
                                                value: disp.modelData.directBitrate >= 0 ? Math.round(disp.modelData.directBitrate / 1000) : 0
                                                onValueModified: disp.directBitrateEdited = true
                                            }
                                            Controls.Label {
                                                visible: !hostCard.apple
                                                text: directBitrate.value === 0
                                                      ? (disp.modelData.directBitrate === 0 && !disp.directBitrateEdited ? "Mbps (auto)" : "Mbps (default)")
                                                      : "Mbps"
                                            }
                                        }
                                        RowLayout {
                                            objectName: "routeRow-network"
                                            visible: disp.showRoutes
                                            Kirigami.FormData.label: "Network:"
                                            Controls.ComboBox {
                                                id: networkRate
                                                objectName: "routeFps-network"
                                                model: disp.routeRateModel(disp.modelData.networkFps)
                                                textRole: "text"
                                                valueRole: "value"
                                                Component.onCompleted: currentIndex = Math.max(0, indexOfValue(disp.modelData.networkFps))
                                            }
                                            Controls.SpinBox {
                                                id: networkBitrate
                                                objectName: "routeBitrate-network"
                                                visible: !hostCard.apple
                                                from: 0; to: 150; stepSize: 5; editable: true
                                                value: disp.modelData.networkBitrate >= 0 ? Math.round(disp.modelData.networkBitrate / 1000) : 0
                                                onValueModified: disp.networkBitrateEdited = true
                                            }
                                            Controls.Label {
                                                visible: !hostCard.apple
                                                text: networkBitrate.value === 0
                                                      ? (disp.modelData.networkBitrate === 0 && !disp.networkBitrateEdited ? "Mbps (auto)" : "Mbps (default)")
                                                      : "Mbps"
                                            }
                                            Controls.Button {
                                                objectName: "routeApply"
                                                text: "Apply"
                                                enabled: directRate.currentValue !== disp.modelData.directFps
                                                         || networkRate.currentValue !== disp.modelData.networkFps
                                                         || (!hostCard.apple && disp.routeBitrateChanged(directBitrate.value, disp.modelData.directBitrate, disp.directBitrateEdited))
                                                         || (!hostCard.apple && disp.routeBitrateChanged(networkBitrate.value, disp.modelData.networkBitrate, disp.networkBitrateEdited))
                                                onClicked: backend.setRouteStream(
                                                    disp.modelData.id,
                                                    directRate.currentValue !== disp.modelData.directFps ? directRate.currentValue : -1,
                                                    hostCard.apple ? -2 : disp.routeBitrateArg(directBitrate.value, disp.modelData.directBitrate, disp.directBitrateEdited),
                                                    networkRate.currentValue !== disp.modelData.networkFps ? networkRate.currentValue : -1,
                                                    hostCard.apple ? -2 : disp.routeBitrateArg(networkBitrate.value, disp.modelData.networkBitrate, disp.networkBitrateEdited))
                                            }
                                            Controls.ToolButton {
                                                objectName: "routeReset"
                                                icon.name: "edit-clear"
                                                display: Controls.AbstractButton.IconOnly
                                                visible: disp.modelData.hasRoutes
                                                text: "Clear per-route settings"
                                                Controls.ToolTip.text: text
                                                Controls.ToolTip.visible: hovered
                                                onClicked: backend.setRouteStream(disp.modelData.id, 0, -1, 0, -1)
                                            }
                                        }
                                        Controls.Label {
                                            objectName: "routeHelp"
                                            visible: disp.showRoutes
                                            Layout.fillWidth: true
                                            wrapMode: Text.Wrap
                                            opacity: 0.7
                                            text: "Optional frame rate" + (hostCard.apple ? "" : " and bitrate")
                                                  + " overrides for the direct/dongle link versus the network. With Connection set to Auto, a stream on the network switches to the direct link automatically once it answers."
                                        }
                                        MonitorScalePicker {
                                            // A PC that can set it (Hyprland, KDE; not Windows), or one picked before.
                                            // It then always has 125%, 150%, 175% and 200%.
                                            visible: disp.followsMonitor && disp.monitor !== null
                                                     && ((disp.monitor.scales || []).length > 0 || disp.modelData.scale > 0)
                                            Kirigami.FormData.label: "Display scale:"
                                            monitor: disp.monitor
                                            picked: disp.modelData.scale
                                            onPick: (scale) => backend.hostMonitors.pickScale(disp.modelData.host, disp.modelData.id, scale)
                                        }
                                        Controls.Label {
                                            objectName: "streamCustomWarning"
                                            visible: ss.visible && ss.lists && ss.typed
                                            Layout.fillWidth: true
                                            wrapMode: Text.Wrap
                                            color: Kirigami.Theme.neutralTextColor
                                            text: "A custom size may not work: it depends on the sizes " + disp.modelData.host
                                                  + "'s monitor accepts. If the monitor doesn't take it, it stays as it is and "
                                                  + "the stream crops or fits it."
                                        }
                                        Controls.Label {
                                            objectName: "streamHelp"
                                            visible: !disp.modelData.virtual && !disp.modelData.kwin
                                            Layout.fillWidth: true
                                            wrapMode: Text.Wrap
                                            opacity: disp.cantBeSize ? 1 : 0.7
                                            color: disp.cantBeSize ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                                            text: disp.appleMain
                                                  ? "Streams the Mac's main display at its own resolution, keeping the Mac's keyboard and trackpad working."
                                                  : disp.cantBeSize
                                                  ? disp.modelData.host + "'s monitor can't be " + disp.modelData.width + " × "
                                                    + disp.modelData.height + ", so the stream crops or fits it as it is."
                                                    + disp.sizesItHas
                                                  : disp.followsMonitor
                                                  ? disp.modelData.host + "'s monitor switches to this size while it streams, pixel for "
                                                    + "pixel, and back when it stops. Disconnect it to turn the monitor off."
                                                    + (ss.typed && disp.largest ? " Its largest size is " + disp.largest[0] + " × "
                                                                                  + disp.largest[1] + "." : "")
                                                  : disp.couldFollow
                                                  ? "The stream crops or fits " + disp.modelData.host + "'s monitor as it is. To switch the "
                                                    + "monitor to this size, turn on \"Let Frametop control " + disp.modelData.host
                                                    + "'s displays\" above."
                                                  : "The monitor switches to the stream's resolution while it streams (or its closest mode with the same shape), and back when it stops."
                                        }
                                        RowLayout {
                                            Kirigami.FormData.label: "Width in VR:"
                                            Controls.Slider {
                                                id: rm
                                                from: 0.3; to: 6.0; stepSize: 0.05
                                                value: disp.modelData.metres
                                                Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                                onMoved: backend.setMetres(disp.modelData.id, value)
                                            }
                                            Controls.Label {
                                                text: rm.value.toFixed(2) + " m wide, "
                                                      + (rm.value * disp.modelData.height / disp.modelData.width).toFixed(2) + " m tall"
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
