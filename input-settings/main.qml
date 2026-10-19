// Frametop Input Settings (Kirigami). Backend: ft_input_settings.py ("backend").
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root
    title: "Frametop Input Settings"
    width: Kirigami.Units.gridUnit * 44
    height: Kirigami.Units.gridUnit * 34

    globalDrawer: Kirigami.GlobalDrawer {
        isMenu: false
        modal: false
        collapsible: true
        collapsed: root.width < Kirigami.Units.gridUnit * 30
        actions: [
            Kirigami.Action { text: "Devices"; icon.name: "input-mouse"; onTriggered: root.show(devicesPage) },
            Kirigami.Action { text: "Buttons"; icon.name: "input-keyboard"; onTriggered: root.show(buttonsPage) },
            Kirigami.Action { text: "Controllers"; icon.name: "input-gamepad"; onTriggered: root.show(controllersPage) },
            Kirigami.Action { text: "Game optimization"; icon.name: "applications-games"; onTriggered: root.show(gamesPage) },
            Kirigami.Action { text: "Keyboard"; icon.name: "input-keyboard-virtual"; onTriggered: root.show(keyboardPage) },
            Kirigami.Action { text: "Voice"; icon.name: "audio-input-microphone"; onTriggered: root.show(voicePage) },
            Kirigami.Action { text: "Pointer"; icon.name: "transform-move"; onTriggered: root.show(pointerPage) },
            Kirigami.Action { text: "Ignored panels"; icon.name: "view-hidden"; onTriggered: root.show(ignorePage) },
            Kirigami.Action { text: "Gaze"; icon.name: "view-visible"; onTriggered: root.show(gazePage) },
            Kirigami.Action { text: "Hands"; icon.name: "transform-browse"; onTriggered: root.show(handsPage) },
            Kirigami.Action { text: "Bluetooth"; icon.name: "preferences-system-bluetooth"; onTriggered: root.show(bluetoothPage) }
        ]
    }

    // Why SteamVR has no ft_pointer driver, and how to get it back. Clicks, scrolling and mapped
    // actions all go through that driver, so every page shows this as (part of) its header.
    component DriverWarning: Kirigami.InlineMessage {
        readonly property string fix: "SteamVR Settings > Startup / Shutdown > Manage Add-Ons"
        readonly property string restart: ", then restart SteamVR (or reboot the headset)."
        visible: backend.driverBlock !== ""
        position: Kirigami.InlineMessage.Position.Header
        type: backend.driverBlock === "unloaded" ? Kirigami.MessageType.Warning : Kirigami.MessageType.Error
        text: ({
            blocked: "SteamVR blocked the Frametop pointer driver (ft_pointer) after a crash, so mouse clicks "
                     + "and scrolling do nothing (the cursor still moves). To fix it, open " + fix
                     + ", press Unblock next to ft_pointer" + restart,
            disabled: "The Frametop pointer driver (ft_pointer) is turned off in SteamVR, so mouse clicks and "
                      + "scrolling do nothing (the cursor still moves). To fix it, open " + fix
                      + ", turn ft_pointer on" + restart,
            safemode: "SteamVR is in safe mode, so it loads no add-ons, the Frametop pointer driver (ft_pointer) "
                      + "included: mouse clicks and scrolling do nothing (the cursor still moves). To fix it, turn "
                      + "safe mode off in SteamVR and check " + fix + restart,
            unloaded: "SteamVR is running without the Frametop pointer driver (ft_pointer), so mouse clicks and "
                      + "scrolling do nothing. If you just unblocked it, restart SteamVR (or reboot the headset). "
                      + "Otherwise check " + fix + ", or reinstall it with pointer/driver/install.sh."
        })[backend.driverBlock] || ""
    }

    // A model picker (Hands and Gaze pages): the catalog's versions (models/catalog.py) with their notes
    // and benchmarks, set against the one running (viewing that one: against the baseline, stock or
    // SteamVR). Choosing a version only shows it; "Use" downloads it if needed and switches to it.
    // The catalog's texts come from GitHub, so they're shown as plain text.
    component ModelPicker: ColumnLayout {
        id: picker
        property string kind
        property var view: ({})
        readonly property string busy: (backend.modelBusy || {})[kind] || ""
        property string viewed: ""
        readonly property var options: view.options || []
        readonly property var opt: options.find(o => o.name === (viewed || view.choice)) || options[0] || ({})
        readonly property string running: view.running || ""
        readonly property string otherName: opt.target !== running ? running
                                            : opt.target !== view.baseline ? (view.baseline || "") : ""
        readonly property var other: otherName ? ((view.byName || {})[otherName] || null) : null
        readonly property bool inUse: opt.name === view.choice && !view.pinned
        spacing: Kirigami.Units.smallSpacing
        Layout.maximumWidth: Kirigami.Units.gridUnit * 30

        function fmt(v, unit) {
            if (v === undefined || v === null) return "–"
            const n = Math.abs(v) >= 100 ? Math.round(v) : Math.round(v * 100) / 100
            return n + (unit === "" || unit === "%" || unit === "°" ? unit : " " + unit)
        }
        function better(m, v, o) {
            if (v === undefined || v === null || o === undefined || o === null || v === o) return 0
            return (m.better === "lower" ? v < o : v > o) ? 1 : -1
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !!picker.view.error
            type: Kirigami.MessageType.Error
            text: picker.view.error || ""
        }
        Repeater {
            model: picker.options
            delegate: Controls.RadioButton {
                required property var modelData
                text: modelData.title + (modelData.tag ? "  ·  " + modelData.tag : "")
                      + (modelData.name === picker.view.choice ? (picker.view.pinned ? "  ·  in use, not following updates"
                                                                                       : "  ·  in use") : "")
                      + (!modelData.usable ? "  ·  needs a newer Frametop"
                         : !modelData.installed ? "  ·  not downloaded" : "")
                font.bold: modelData.name === picker.view.choice
                checked: picker.opt.name === modelData.name
                onToggled: if (checked) picker.viewed = modelData.name
            }
        }
        Controls.Label {
            visible: !!picker.view.note
            text: picker.view.note || ""
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Kirigami.Theme.neutralTextColor
            font: Kirigami.Theme.smallFont
        }

        // The one being looked at: what it is, what to expect, what it costs, and its numbers.
        Kirigami.Separator { Layout.fillWidth: true; visible: !!picker.opt.name }
        Controls.Label {
            text: (picker.opt.title || "") + (picker.opt.date ? " (" + picker.opt.date + ")" : "")
            textFormat: Text.PlainText
            font.bold: true
        }
        Controls.Label {
            visible: picker.opt.follows === true
            text: "The newest version. Picked, it follows updates: when a newer version ships, Frametop moves to it without asking. Any other version stays put when picked."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
        }
        Controls.Label {
            text: picker.opt.summary || ""
            visible: text !== ""
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
        Controls.Label {
            text: picker.opt.notes || ""
            visible: text !== ""
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.85
        }
        Controls.Label {
            text: picker.opt.cost ? "Cost: " + picker.opt.cost : ""
            visible: text !== ""
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
        }
        GridLayout {
            visible: (picker.view.metrics || []).some(m => picker.opt.bench && picker.opt.bench[m.key] !== undefined)
            columns: 3
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: 2
            Controls.Label { text: "Benchmark"; font.bold: true; Layout.fillWidth: true }
            Controls.Label { text: picker.opt.target || ""; textFormat: Text.PlainText; font.bold: true }
            Controls.Label {
                text: picker.other ? picker.otherName + (picker.otherName === picker.running ? " (in use)" : "") : ""
                textFormat: Text.PlainText
                font.bold: true
                opacity: 0.7
            }
            Repeater {
                model: picker.view.metrics || []
                delegate: Controls.Label {
                    required property var modelData
                    required property int index
                    text: modelData.label
                    textFormat: Text.PlainText
                    Layout.row: index + 1
                    Layout.column: 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font: Kirigami.Theme.smallFont
                    Controls.ToolTip.visible: !!modelData.note && mhover.hovered
                    Controls.ToolTip.text: modelData.note || ""
                    HoverHandler { id: mhover }
                }
            }
            Repeater {
                model: picker.view.metrics || []
                delegate: Controls.Label {
                    required property var modelData
                    required property int index
                    readonly property var v: (picker.opt.bench || {})[modelData.key]
                    readonly property int b: picker.other ? picker.better(modelData, v, (picker.other.bench || {})[modelData.key]) : 0
                    text: picker.fmt(v, modelData.unit)
                    Layout.row: index + 1
                    Layout.column: 1
                    color: b > 0 ? Kirigami.Theme.positiveTextColor : b < 0 ? Kirigami.Theme.negativeTextColor
                                                                       : Kirigami.Theme.textColor
                }
            }
            Repeater {
                model: picker.view.metrics || []
                delegate: Controls.Label {
                    required property var modelData
                    required property int index
                    text: picker.other ? picker.fmt((picker.other.bench || {})[modelData.key], modelData.unit) : ""
                    Layout.row: index + 1
                    Layout.column: 2
                    opacity: 0.7
                }
            }
        }
        Controls.Label {
            visible: (picker.view.metrics || []).some(m => picker.opt.bench && picker.opt.bench[m.key] !== undefined)
            text: "Green: better than " + (picker.otherName || "the other") + ", red: worse. Hover a row for how it was measured."
            textFormat: Text.PlainText
            opacity: 0.7
            font: Kirigami.Theme.smallFont
        }
        RowLayout {
            Controls.Button {
                text: picker.inUse ? "In use"
                      : !picker.opt.installed ? "Download and use"
                      : "Use this"
                icon.name: picker.inUse ? "checkmark" : !picker.opt.installed ? "download" : "dialog-ok-apply"
                enabled: !picker.inUse && picker.opt.usable !== false && picker.busy === "" && !!picker.opt.name
                onClicked: backend.useModel(picker.kind, picker.opt.name)
            }
            Controls.Button {
                text: "Remove download"
                icon.name: "edit-delete"
                visible: !picker.opt.builtin && !picker.opt.follows && picker.opt.installed === true
                         && picker.opt.target !== picker.running && picker.opt.name !== picker.view.choice
                enabled: picker.busy === ""
                onClicked: backend.removeModel(picker.kind, picker.opt.name)
            }
            Controls.Button {
                text: "Check for new versions"
                icon.name: "view-refresh"
                enabled: picker.busy === ""
                onClicked: backend.refreshModels(picker.kind)
            }
            Controls.BusyIndicator {
                visible: picker.busy !== ""
                running: visible
                Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                Layout.preferredWidth: Layout.preferredHeight
            }
            Controls.Label {
                visible: picker.busy !== ""
                text: picker.busy
                opacity: 0.7
                font: Kirigami.Theme.smallFont
            }
        }
        Controls.Label {
            text: "List from " + (picker.view.source || "this install")
                  + (picker.view.checked ? ", checked " + picker.view.checked : "")
            opacity: 0.6
            font: Kirigami.Theme.smallFont
        }
    }

    function show(page) {
        pageStack.clear()
        pageStack.push(page)
    }

    // FT_INPUT_PAGE=buttons|controllers|games (Game optimization)|keyboard|voice|pointer|ignore|gaze|hands|bluetooth opens the app on that page.
    pageStack.initialPage: ({ buttons: buttonsPage, controllers: controllersPage, games: gamesPage, keyboard: keyboardPage,
                              voice: voicePage, pointer: pointerPage, ignore: ignorePage, gaze: gazePage, hands: handsPage,
                              bluetooth: bluetoothPage })[startPage] || devicesPage

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    // ---------------------------------------------------------------- Devices
    Component {
        id: devicesPage
        Kirigami.ScrollablePage {
            title: "Devices"

            header: ColumnLayout {
                spacing: 0
                DriverWarning { Layout.fillWidth: true }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !backend.relayRunning || !backend.pointerMode
                    position: Kirigami.InlineMessage.Position.Header
                    type: backend.relayRunning ? Kirigami.MessageType.Information : Kirigami.MessageType.Error
                    text: !backend.relayRunning
                          ? "The input relay isn't running (frametop-input-relay.service)."
                          : "Pointer mode is off (POINTER=0): pointer devices act as a plain mouse."
                }
            }

            ListView {
                model: backend.devices
                spacing: Kirigami.Units.smallSpacing

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    visible: parent.count === 0
                    text: "No USB or Bluetooth mice or keyboards connected"
                    explanation: "Connect or wake one; it appears here within a second."
                }

                delegate: Controls.ItemDelegate {
                    id: row
                    required property var modelData
                    width: ListView.view.width
                    hoverEnabled: false
                    down: false

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing

                        // Activity light: flashes when the device sends input.
                        Rectangle {
                            id: light
                            implicitWidth: Kirigami.Units.gridUnit * 0.8
                            implicitHeight: implicitWidth
                            radius: width / 2
                            color: Kirigami.Theme.disabledTextColor
                            opacity: 0.35
                            Connections {
                                target: backend
                                function onActivity(id) {
                                    if (id === row.modelData.id) {
                                        light.color = Kirigami.Theme.positiveTextColor
                                        light.opacity = 1
                                        fade.restart()
                                    }
                                }
                            }
                            Timer {
                                id: fade
                                interval: 350
                                onTriggered: { light.color = Kirigami.Theme.disabledTextColor; light.opacity = 0.35 }
                            }
                        }

                        Kirigami.Icon {
                            source: row.modelData.kinds.indexOf("mouse") >= 0 ? "input-mouse" : "input-keyboard"
                            implicitWidth: Kirigami.Units.iconSizes.medium
                            implicitHeight: implicitWidth
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Controls.Label {
                                text: row.modelData.name
                                font.bold: true
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            Controls.Label {
                                text: row.modelData.connected
                                      ? row.modelData.bus + " · " + row.modelData.kinds.join(" + ") + " · " + row.modelData.id
                                        + (row.modelData.grabbed ? " · grabbed" : "")
                                      : "not connected · saved settings · " + row.modelData.id
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                        }

                        Controls.ComboBox {
                            // Keyboard + 3D pointer only where it splits something: a mouse node and a keyboard
                            // node. A device that isn't connected has no nodes to tell, so it's offered there too.
                            model: backend.roles.filter(r => r.value !== "combo" || row.modelData.role === "combo"
                                                        || !row.modelData.connected
                                                        || (row.modelData.kinds.indexOf("mouse") >= 0
                                                            && row.modelData.kinds.indexOf("keyboard") >= 0))
                            textRole: "text"
                            valueRole: "value"
                            Component.onCompleted: currentIndex = indexOfValue(row.modelData.role)
                            onActivated: backend.setRole(row.modelData.id, currentValue, row.modelData.name)
                        }
                        Controls.Button {
                            visible: !row.modelData.connected
                            text: "Forget"
                            icon.name: "edit-delete-remove"
                            onClicked: backend.forgetDevice(row.modelData.id)
                        }
                        Controls.ToolButton {
                            icon.name: "edit-undo"
                            visible: row.modelData.explicit
                            display: Controls.AbstractButton.IconOnly
                            text: "Use the default role"
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: backend.resetRole(row.modelData.id)
                        }
                    }
                }
            }

            footer: Controls.Label {
                padding: Kirigami.Units.largeSpacing
                wrapMode: Text.Wrap
                opacity: 0.7
                text: "Move or press a device to see which row it is. 3D pointer: grabbed, drives the SteamVR pointer. "
                      + "Pass through: left alone, but its key combinations (Keyboard page) work. Ignore: left alone."
            }
        }
    }

    // ---------------------------------------------------------------- Buttons
    Component {
        id: buttonsPage
        Kirigami.ScrollablePage {
            id: bpage
            title: "Buttons"
            header: DriverWarning {}
            actions: [
                Kirigami.Action {
                    text: "Remove all"
                    icon.name: "edit-clear-all"
                    tooltip: "Remove every binding you added for this device; built-in buttons go back to their defaults"
                    enabled: bpage.rows.some(r => r.custom)
                    onTriggered: backend.clearMappings(bpage.deviceId)
                }
            ]
            property string deviceId: pointerDevices.length > 0 ? pointerDevices[Math.max(0, deviceBox.currentIndex)].id : ""
            property var pointerDevices: backend.devices.filter(d => d.role === "pointer" || d.role === "combo")
            property int capturedCode: -1
            property string capturedName: ""
            property bool capturing: false
            property var rows: deviceId ? backend.mappings(deviceId) : []

            Connections {
                target: backend
                function onCaptured(code, name) { bpage.capturedCode = code; bpage.capturedName = name; bpage.capturing = false }
                function onMappingsChanged() { bpage.rows = bpage.deviceId ? backend.mappings(bpage.deviceId) : [] }
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.PlaceholderMessage {
                    Layout.fillWidth: true
                    visible: bpage.pointerDevices.length === 0
                    text: "No 3D pointer devices"
                    explanation: "Set a device's role to 3D pointer on the Devices page."
                }

                Kirigami.FormLayout {
                    Layout.fillWidth: true
                    visible: bpage.pointerDevices.length > 0

                    Controls.ComboBox {
                        id: deviceBox
                        Kirigami.FormData.label: "Device:"
                        model: bpage.pointerDevices.map(d => ({ name: d.name + (d.connected ? "" : "  (not connected)"), id: d.id }))
                        textRole: "name"
                        onActivated: { bpage.capturedCode = -1; bpage.rows = backend.mappings(bpage.deviceId) }
                    }

                    RowLayout {
                        Kirigami.FormData.label: "New mapping:"
                        enabled: bpage.pointerDevices.length > 0 && bpage.pointerDevices[Math.max(0, deviceBox.currentIndex)].connected
                        Controls.Button {
                            text: bpage.capturing ? "Press a button or key on the device…" : "Capture a button"
                            icon.name: "input-mouse-click-left"
                            highlighted: bpage.capturing
                            onClicked: {
                                if (bpage.capturing) { backend.cancelCapture(); bpage.capturing = false }
                                else { bpage.capturedCode = -1; bpage.capturing = true; backend.startCapture(bpage.deviceId) }
                            }
                        }
                        Controls.Label {
                            visible: bpage.capturedCode >= 0
                            text: bpage.capturedName
                            font.bold: true
                        }
                        Controls.ComboBox {
                            id: newAction
                            visible: bpage.capturedCode >= 0
                            model: backend.actions
                            textRole: "text"
                            valueRole: "value"
                        }
                        Controls.Button {
                            visible: bpage.capturedCode >= 0
                            text: "Map"
                            icon.name: "dialog-ok-apply"
                            onClicked: { backend.setMapping(bpage.deviceId, bpage.capturedCode, newAction.currentValue); bpage.capturedCode = -1 }
                        }
                    }

                    Kirigami.Separator {
                        visible: bpage.rows.length > 0
                        Kirigami.FormData.isSection: true
                        Kirigami.FormData.label: "Current mappings"
                    }

                    Repeater {
                        model: bpage.rows
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.largeSpacing
                            Controls.Label {
                                text: modelData.name
                                font.family: "monospace"
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                            }
                            Controls.ComboBox {
                                model: backend.actions
                                textRole: "text"
                                valueRole: "value"
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 13
                                Component.onCompleted: currentIndex = indexOfValue(modelData.action)
                                onActivated: backend.setMapping(bpage.deviceId, modelData.code, currentValue)
                            }
                            Controls.Label {
                                text: modelData.custom ? (modelData.isDefault ? "changed" : "custom") : "default"
                                opacity: 0.6
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 4
                            }
                            // Remove: your own binding goes away (the button passes through again).
                            // Reset: a changed built-in binding goes back to its default.
                            // Unbind: an untouched built-in binding is set to "Do nothing".
                            Controls.Button {
                                text: modelData.custom ? (modelData.isDefault ? "Reset" : "Remove") : "Unbind"
                                icon.name: modelData.custom ? (modelData.isDefault ? "edit-undo" : "edit-delete-remove")
                                                            : "list-remove"
                                onClicked: modelData.custom ? backend.removeMapping(bpage.deviceId, modelData.code)
                                                            : backend.unbind(bpage.deviceId, modelData.code)
                            }
                            Item { Layout.fillWidth: true }
                        }
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "Buttons without a mapping pass through (mouse buttons as clicks, keys as keys). "
                          + "The Z3's extra buttons show up as keys from its keyboard node. "
                          + "The Frame controllers' buttons are on the Controllers page."
                }
            }
        }
    }

    // ---------------------------------------------------------------- Controllers
    Component {
        id: controllersPage
        Kirigami.ScrollablePage {
            id: cpage
            title: "Controllers"
            header: DriverWarning {}
            actions: [
                Kirigami.Action {
                    text: "Remove all"
                    icon.name: "edit-clear-all"
                    tooltip: "Give every controller button back to games"
                    enabled: backend.controllerMappings.length > 0
                    onTriggered: backend.clearControllerMappings()
                }
            ]
            property string capturedButton: ""
            property string capturedLabel: ""
            property bool capturing: false
            property var status: backend.controllerStatus
            Component.onDestruction: backend.cancelControllerCapture()

            Connections {
                target: backend
                function onCapturedController(button, label) {
                    cpage.capturedButton = button; cpage.capturedLabel = label; cpage.capturing = false
                    buttonBox.currentIndex = buttonBox.indexOfValue(button)
                }
            }
            Timer {
                // The relay takes every button for 30 s while capturing.
                running: cpage.capturing
                interval: 30000
                onTriggered: { backend.cancelControllerCapture(); cpage.capturing = false }
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !cpage.status.helper
                    type: Kirigami.MessageType.Error
                    text: "The pointer helper isn't answering (frametop-pointer.service, needs SteamVR). "
                          + "It reads the controller buttons."
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: cpage.status.helper && !cpage.status.manifest
                    type: Kirigami.MessageType.Error
                    text: "The pointer helper couldn't set up SteamVR input (pointer/helper/actions). See its log."
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: cpage.status.helper && !cpage.status.global && backend.controllerMappings.length > 0
                    type: Kirigami.MessageType.Warning
                    text: "Global input is off, so the mapped buttons only reach Frametop when nothing else has "
                          + "focus, if at all."
                }

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    Controls.Switch {
                        Kirigami.FormData.label: "Global input:"
                        text: "SteamVR's \"Enable global input from overlays (Experimental)\", which mapped buttons need"
                        checked: cpage.status.global
                        enabled: cpage.status.helper
                        onToggled: backend.setGlobalInput(checked)
                    }
                    Controls.Switch {
                        Kirigami.FormData.label: "In games:"
                        text: "Mapped buttons work while a game is running too (the game loses them)"
                        checked: backend.controllerInGames
                        onToggled: backend.setControllerInGames(checked)
                    }
                    RowLayout {
                        Kirigami.FormData.label: "New mapping:"
                        Controls.Button {
                            text: cpage.capturing ? "Press a button on a controller…" : "Capture a button"
                            icon.name: "input-gamepad"
                            highlighted: cpage.capturing
                            enabled: cpage.status.helper
                            onClicked: {
                                if (cpage.capturing) { backend.cancelControllerCapture(); cpage.capturing = false }
                                else { cpage.capturedButton = ""; cpage.capturing = true; backend.startControllerCapture() }
                            }
                        }
                        Controls.Label { text: "or" ; opacity: 0.7 }
                        Controls.ComboBox {
                            id: buttonBox
                            model: backend.controllerButtons
                            textRole: "text"
                            valueRole: "value"
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                        }
                        Controls.ComboBox {
                            id: newControllerAction
                            model: backend.controllerActions
                            textRole: "text"
                            valueRole: "value"
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 13
                        }
                        Controls.Button {
                            text: "Map"
                            icon.name: "dialog-ok-apply"
                            onClicked: { backend.setControllerMapping(buttonBox.currentValue, newControllerAction.currentValue); cpage.capturedButton = "" }
                        }
                    }
                }

                Kirigami.Heading {
                    visible: backend.controllerMappings.length > 0
                    level: 3
                    text: "Current mappings"
                }

                Repeater {
                    model: backend.controllerMappings
                    delegate: RowLayout {
                        id: crow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.largeSpacing
                        Controls.Label {
                            text: crow.modelData.label
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                        }
                        Controls.ComboBox {
                            model: backend.controllerActions
                            textRole: "text"
                            valueRole: "value"
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 13
                            Component.onCompleted: currentIndex = indexOfValue(crow.modelData.action)
                            onActivated: backend.setControllerMapping(crow.modelData.button, currentValue)
                        }
                        Controls.Label {
                            // Active: SteamVR delivers it to Frametop now (a controller is on).
                            text: cpage.status.active.indexOf(crow.modelData.button) >= 0 ? "active"
                                  : cpage.status.inGame && !backend.controllerInGames ? "game's" : "waiting"
                            opacity: 0.6
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 4
                            Controls.ToolTip.text: text === "active" ? "SteamVR gives this button to Frametop"
                                                   : text === "game's" ? "A game is running: the button is the game's until it quits"
                                                   : "No controller with this button is on, or SteamVR doesn't give it to Frametop"
                            Controls.ToolTip.visible: hover.hovered
                            HoverHandler { id: hover }
                        }
                        Controls.Button {
                            text: "Remove"
                            icon.name: "edit-delete-remove"
                            onClicked: backend.removeControllerMapping(crow.modelData.button)
                        }
                        Item { Layout.fillWidth: true }
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "Outside games, a mapped button is Frametop's; while a game runs it's the game's, unless "
                          + "\"In games\" is on. Unmapped buttons are left alone, and the system button stays SteamVR's. "
                          + "Clicks and scrolling go to the 3D pointer. The trigger and grip also move SteamVR's laser "
                          + "to that controller, so they're better left unmapped."
                }
            }
        }
    }

    // ---------------------------------------------------------------- Keyboard
    Component {
        id: keyboardPage
        Kirigami.ScrollablePage {
            id: kpage
            title: "Keyboard"
            // Pass-through keyboards connected now: with "no_keyboard", the keyboard waits for none.
            // A program's uinput keyboard (a remote desktop's, say) doesn't count.
            property var keyboards: backend.devices.filter(d => d.connected && (d.role === "passthrough" || d.role === "combo")
                                                                && d.kinds.indexOf("keyboard") >= 0 && !d.uinput)

            Kirigami.FormLayout {
                Controls.ComboBox {
                    Kirigami.FormData.label: "Show the keyboard:"
                    model: backend.vrKeyboardModes
                    textRole: "text"
                    valueRole: "value"
                    Component.onCompleted: currentIndex = indexOfValue(backend.vrKeyboard)
                    onActivated: backend.setVrKeyboard(currentValue)
                }
                Controls.Switch {
                    Kirigami.FormData.label: "Keep it open:"
                    text: "Until you press its Close key or your keyboard button, not only while the text field has focus"
                    checked: backend.vrKeyboardPersist
                    enabled: backend.vrKeyboard !== "never"
                    onToggled: backend.setVrKeyboardPersist(checked)
                }
                Controls.Label {
                    Kirigami.FormData.label: "Keyboards connected:"
                    text: kpage.keyboards.length ? kpage.keyboards.map(d => d.name).join(", ") : "none"
                }
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Key combinations" }

                Repeater {
                    model: backend.keyShortcuts
                    delegate: RowLayout {
                        required property var modelData
                        Kirigami.FormData.label: modelData.label + ":"
                        Controls.Label {
                            text: modelData.actionLabel
                            elide: Text.ElideRight
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                        }
                        Controls.ToolButton {
                            icon.name: "edit-delete"
                            display: Controls.AbstractButton.IconOnly
                            text: "Remove"
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: backend.removeShortcut(modelData.combo)
                        }
                    }
                }
                RowLayout {
                    Kirigami.FormData.label: "New:"
                    Controls.ComboBox {
                        id: shortcutAction
                        model: backend.shortcutActions
                        textRole: "text"
                        valueRole: "value"
                        Component.onCompleted: currentIndex = indexOfValue("float_toggle")
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 16
                    }
                    Controls.Button {
                        text: backend.capturingShortcut ? "Press the keys… (Cancel)" : "Set keys…"
                        enabled: backend.capturingShortcut || shortcutAction.currentValue !== "command:"
                                 || shortcutCommand.text.trim() !== ""
                        onClicked: backend.capturingShortcut ? backend.cancelShortcutCapture()
                                                             : backend.startShortcutCapture(
                                                                   shortcutAction.currentValue === "command:"
                                                                   ? "command:" + shortcutCommand.text.trim()
                                                                   : shortcutAction.currentValue)
                    }
                }
                Controls.TextField {
                    id: shortcutCommand
                    Kirigami.FormData.label: "Command:"
                    visible: shortcutAction.currentValue === "command:"
                    placeholderText: "e.g. ft-layout use Work"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 24
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    wrapMode: Text.WordWrap
                    text: "Hold the modifiers (Ctrl, Alt, Shift, Meta), then press the key, on any keyboard. Or tap "
                          + "one modifier on its own: a left one taps either side, a right one only itself (Right Ctrl). "
                          + "The combination's last key isn't typed; the modifiers still reach "
                          + "the app. While you're in Steam or a game, they see the keys too. Until you remove or change "
                          + "them, a Meta tap opens the Steam menu (or closes the dashboard) instead of the desktop's "
                          + "launcher, and Meta+Shift+F floats the desktop window under the pointer in VR, or puts it "
                          + "back. A command runs with sh as the input relay's service, outside the desktop's session, "
                          + "with Frametop's ft-layout, ft-float and ft-steam on its path; its output goes to the "
                          + "relay's log (journalctl --user -u frametop-input-relay)."
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                }
            }

            footer: Controls.Label {
                padding: Kirigami.Units.largeSpacing
                wrapMode: Text.Wrap
                opacity: 0.7
                text: "Frametop's keyboard opens in front of you, below your eyes, and closes with its Close key or a layout reset "
                      + "(or when the text field loses focus, with Keep it open off). It steps aside while the Steam "
                      + "menu or Steam's own keyboard is up. Type on it with a laser or the 3D mouse. It types into any "
                      + "app, but only Qt, GTK and Firefox apps say when a text field is selected; for the rest "
                      + "(Chromium, Electron and X11 apps), map Open/close keyboard to a button on the Buttons or "
                      + "Controllers page. Keyboards set to Ignore, and keyboards other programs make, don't "
                      + "count as connected."
            }
        }
    }

    // ---------------------------------------------------------------- Voice
    Component {
        id: voicePage
        Kirigami.ScrollablePage {
            id: vpage
            title: "Voice"
            property var st: backend.voiceStatus
            property var vs: backend.voiceSettings
            property bool capturing: false
            Component.onCompleted: backend.refreshVoice()
            Component.onDestruction: if (capturing) backend.cancelControllerCapture()

            Connections {
                target: backend
                function onCapturedController(button, label) {
                    if (vpage.capturing) { vpage.capturing = false; backend.addVoiceButton(button) }
                }
            }
            Timer {
                // The relay takes every button for 30 s while capturing.
                running: vpage.capturing
                interval: 30000
                onTriggered: { backend.cancelControllerCapture(); vpage.capturing = false }
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !backend.voiceInstalled
                    type: Kirigami.MessageType.Information
                    text: "Voice dictation isn't installed. Run voice/run.sh install in Frametop's folder: it "
                          + "downloads about 320 MB of speech models, which stay on the headset."
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.voiceInstalled && backend.voiceEnabled && !vpage.st.running
                    type: Kirigami.MessageType.Warning
                    text: "Voice dictation is on but not running. See journalctl --user -u frametop-voice."
                }

                Kirigami.FormLayout {
                    Layout.fillWidth: true
                    enabled: backend.voiceInstalled

                    Controls.Switch {
                        Kirigami.FormData.label: "Voice dictation:"
                        text: !vpage.st.running ? "Off" : vpage.st.dictating ? "On, listening now" : "On"
                        checked: backend.voiceEnabled
                        onToggled: backend.setVoiceOn(checked)
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Push to talk" }
                    Controls.Label {
                        Kirigami.FormData.label: "Keys:"
                        text: backend.dictationKeys.length
                              ? backend.dictationKeys.map(k => k.label + " (" + k.actionLabel.toLowerCase() + ")").join(", ")
                              : "none"
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                    }
                    Controls.Button {
                        text: "Change on the Keyboard page"
                        icon.name: "input-keyboard"
                        onClicked: root.show(keyboardPage)
                    }
                    Repeater {
                        model: backend.voiceButtons
                        delegate: RowLayout {
                            required property var modelData
                            Kirigami.FormData.label: "Controller:"
                            Controls.Label { text: modelData.label }
                            Controls.ToolButton {
                                icon.name: "edit-delete"
                                display: Controls.AbstractButton.IconOnly
                                text: "Remove"
                                Controls.ToolTip.text: text
                                Controls.ToolTip.visible: hovered
                                onClicked: backend.removeVoiceButton(modelData.button)
                            }
                        }
                    }
                    RowLayout {
                        Kirigami.FormData.label: backend.voiceButtons.length ? "Another:" : "Controller:"
                        Controls.Button {
                            text: vpage.capturing ? "Press a button on a controller…" : "Capture a button"
                            icon.name: "input-gamepad"
                            highlighted: vpage.capturing
                            enabled: backend.controllerStatus.helper
                            onClicked: {
                                if (vpage.capturing) { backend.cancelControllerCapture(); vpage.capturing = false }
                                else { vpage.capturing = true; backend.startControllerCapture() }
                            }
                        }
                        Controls.Label { text: "or"; opacity: 0.7 }
                        Controls.ComboBox {
                            id: voiceButtonBox
                            model: backend.controllerButtons
                            textRole: "text"
                            valueRole: "value"
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                        }
                        Controls.Button {
                            text: "Add"
                            icon.name: "list-add"
                            onClicked: backend.addVoiceButton(voiceButtonBox.currentValue)
                        }
                    }
                    Controls.ComboBox {
                        Kirigami.FormData.label: "Controller buttons:"
                        model: backend.voiceModes
                        textRole: "text"
                        valueRole: "value"
                        Component.onCompleted: currentIndex = indexOfValue(vpage.vs.controller_mode)
                        onActivated: backend.setVoiceControllerMode(currentValue)
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Typing" }
                    Controls.Switch {
                        Kirigami.FormData.label: "As you talk:"
                        text: "Type words while you're still talking (off: all at once when you stop)"
                        checked: vpage.vs.live_typing
                        onToggled: backend.setVoiceOption("live_typing", checked)
                    }
                    Controls.Switch {
                        Kirigami.FormData.label: "Sounds:"
                        text: "A short tone when dictation starts and stops"
                        checked: vpage.vs.sounds
                        onToggled: backend.setVoiceOption("sounds", checked)
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Wake word" }
                    Controls.Switch {
                        Kirigami.FormData.label: "Wake word:"
                        text: "Listen for it, then for a command (the microphone stays open)"
                        checked: vpage.vs.wake_word
                        onToggled: backend.setVoiceOption("wake_word", checked)
                    }
                    Controls.TextField {
                        Kirigami.FormData.label: "Say:"
                        text: vpage.vs.wake_phrases.map(p => p.toLowerCase().replace(/\b\w/g, c => c.toUpperCase())).join(", ")
                        placeholderText: "Hey Computer"
                        enabled: vpage.vs.wake_word
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 16
                        onEditingFinished: backend.setWakePhrases(text)
                    }
                    Repeater {
                        model: backend.voiceCommands
                        delegate: Controls.Label {
                            required property var modelData
                            Kirigami.FormData.label: "Command:"
                            text: "\u201c" + modelData.say + "\u201d runs " + modelData.run
                            elide: Text.ElideRight
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                        }
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "Speech is recognised on the headset; nothing leaves it. Text is typed where you type: the "
                          + "desktop window you clicked last, or Steam and games. It waits while you hold Ctrl, Alt, "
                          + "Shift or Meta, so letters don't turn into shortcuts. Recordings under 2 seconds are "
                          + "dropped. A key works only on its own: with another modifier held it's a shortcut, not "
                          + "dictation. Controller buttons still do what they do in games. After the wake word, a "
                          + "command goes to voice/hooks/on-command, or runs a program you set in "
                          + "~/.config/frametop-voice.json (\"commands\"; see voice/README.md)."
                }
            }
        }
    }

    // ---------------------------------------------------------------- Pointer
    Component {
        id: pointerPage
        Kirigami.ScrollablePage {
            title: "Pointer"
            header: DriverWarning {}
            actions: [
                Kirigami.Action {
                    text: "Recenter"
                    icon.name: "zoom-fit-best"
                    onTriggered: backend.recenter()
                }
            ]

            Kirigami.FormLayout {
                Controls.Switch {
                    Kirigami.FormData.label: "Head follow:"
                    text: "Pointer follows your head (experimental; leash below, 0° locks it to your view)"
                    checked: backend.pointerFollow
                    onToggled: backend.setPointerFollow(checked)
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    visible: true
                    type: Kirigami.MessageType.Warning
                    text: "Head follow is experimental. It's only lightly tested and not finished: the head "
                          + "follow settings below are a starting point, and polishing how it feels is left open "
                          + "for anyone who wants to take it further."
                }
                Repeater {
                    model: backend.pointerSettings
                    delegate: RowLayout {
                        required property var modelData
                        Kirigami.FormData.label: modelData.label + ":"
                        Controls.Slider {
                            id: slider
                            from: modelData.min
                            to: modelData.max
                            stepSize: modelData.step
                            value: modelData.value
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                            onMoved: backend.setPointerSetting(modelData.key, value)
                        }
                        Controls.Label {
                            text: (modelData.step < 1 ? slider.value.toFixed(modelData.step < 0.01 ? 3 : 2) : Math.round(slider.value))
                                  + (modelData.unit ? " " + modelData.unit : "")
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                        }
                        Controls.ToolButton {
                            icon.name: "edit-undo"
                            display: Controls.AbstractButton.IconOnly
                            text: "Default (" + modelData.default + ")"
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: { slider.value = modelData.default; backend.setPointerSetting(modelData.key, modelData.default) }
                        }
                    }
                }
            }

            footer: Controls.Label {
                padding: Kirigami.Units.largeSpacing
                wrapMode: Text.Wrap
                opacity: 0.7
                text: "Changes apply live. SteamVR dot shrink: how close to the target the pointer's laser starts; "
                      + "higher makes SteamVR's own blue dot smaller (max 0.98)."
            }
        }
    }

    // ---------------------------------------------------------------- Ignored panels
    Component {
        id: ignorePage
        Kirigami.ScrollablePage {
            id: ipage
            title: "Ignored panels"
            header: DriverWarning {}

            Timer {
                // The helper lists SteamVR's panels again for each request.
                running: true
                repeat: true
                triggeredOnStart: true
                interval: 3000
                onTriggered: backend.refreshPanels()
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !backend.controllerStatus.helper
                    type: Kirigami.MessageType.Error
                    text: "The pointer helper isn't answering (frametop-pointer.service, needs SteamVR). "
                          + "It lists SteamVR's panels and does the ignoring."
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "The mouse pointer passes through the panels ticked here to whatever is behind them, "
                          + "as if they weren't there. Use it for panels you only look at, like a performance "
                          + "overlay that follows your view. Ignore a whole app, or only some of its panels. "
                          + "Controllers aren't affected."
                }
                Controls.Switch {
                    id: showHidden
                    text: "Also list panels that aren't showing now"
                }
                Controls.Label {
                    visible: backend.controllerStatus.helper && !backend.panelsLoaded
                    text: "Asking the pointer helper for SteamVR's panels…"
                    opacity: 0.7
                }

                Repeater {
                    model: backend.panelGroups
                    delegate: ColumnLayout {
                        id: grp
                        required property var modelData
                        Layout.fillWidth: true
                        visible: showHidden.checked || modelData.anyVisible || modelData.anyIgnored
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            Kirigami.Heading {
                                level: 4
                                text: grp.modelData.title
                            }
                            Controls.Label {
                                text: grp.modelData.app
                                opacity: 0.6
                            }
                            Item { Layout.fillWidth: true }
                            Controls.CheckBox {
                                text: "Ignore the whole app"
                                checked: grp.modelData.appIgnored
                                onToggled: backend.setAppIgnored(grp.modelData.app, checked)
                            }
                        }
                        Repeater {
                            model: grp.modelData.panels
                            delegate: RowLayout {
                                id: prow
                                required property var modelData
                                // Ignored by the whole app, or by a pattern written in frametop.conf.
                                readonly property bool byOther: modelData.ignoredBy !== "" && modelData.ignoredBy !== modelData.key
                                visible: showHidden.checked || modelData.visible || modelData.ignoredBy !== ""
                                Layout.leftMargin: Kirigami.Units.gridUnit
                                Controls.CheckBox {
                                    text: prow.modelData.name
                                    checked: prow.modelData.ignoredBy !== ""
                                    enabled: !prow.byOther
                                    onToggled: backend.setPanelIgnored(prow.modelData.key, checked)
                                }
                                Controls.Label {
                                    text: prow.modelData.key
                                    opacity: 0.6
                                }
                                Controls.Label {
                                    text: (prow.modelData.visible ? "showing" : "hidden")
                                          + (!prow.byOther ? ""
                                             : prow.modelData.ignoredBy === grp.modelData.app + "*" ? ", whole app ignored"
                                             : ", ignored by " + prow.modelData.ignoredBy)
                                    opacity: 0.6
                                }
                            }
                        }
                    }
                }

                Kirigami.Heading {
                    visible: backend.ignoreOrphans.length > 0
                    level: 3
                    text: "Ignored, not open now"
                }
                Repeater {
                    model: backend.ignoreOrphans
                    delegate: RowLayout {
                        id: orow
                        required property string modelData
                        Controls.Label {
                            text: orow.modelData
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 16
                        }
                        Controls.Button {
                            text: "Remove"
                            icon.name: "edit-delete-remove"
                            onClicked: backend.removeIgnore(orow.modelData)
                        }
                    }
                }
            }

            footer: Controls.Label {
                padding: Kirigami.Units.largeSpacing
                wrapMode: Text.Wrap
                opacity: 0.7
                text: "Saved as POINTER_IGNORE in ~/.config/frametop.conf, and applied at once. A whole app is "
                      + "its key followed by *, which also covers panels it opens later. Frametop's own screens "
                      + "aren't listed."
            }
        }
    }

    // ---------------------------------------------------------------- Games
    Component {
        id: gamesPage
        Kirigami.ScrollablePage {
            id: gmpage
            title: "Game optimization"
            property var st: backend.pauseStatus

            // The gesture's boxes follow the saved gesture (which may tidy what was picked).
            function syncGesture() {
                const g = backend.pauseGesture
                firstBox.currentIndex = firstBox.indexOfValue(g.first)
                secondBox.currentIndex = secondBox.indexOfValue(g.second)
                pressBox.currentIndex = pressBox.indexOfValue(g.presses)
            }
            function saveGesture() {
                backend.setPauseGesture(firstBox.currentValue, secondBox.currentValue, pressBox.currentValue)
            }
            Component.onCompleted: syncGesture()
            Connections {
                target: backend
                function onMappingsChanged() { gmpage.syncGesture() }
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !gmpage.st.relay
                    type: Kirigami.MessageType.Error
                    text: gmpage.st.old
                          ? "The input relay that runs is older and can't pause. Restart it: "
                            + "systemctl --user restart frametop-input-relay"
                          : "The input relay isn't answering (frametop-input-relay.service). It does the pausing."
                }

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    RowLayout {
                        Kirigami.FormData.label: "Frametop:"
                        Controls.Label { text: gmpage.st.summary }
                        Controls.Button {
                            text: gmpage.st.paused ? "Resume" : "Pause now"
                            icon.name: gmpage.st.paused ? "media-playback-start" : "media-playback-pause"
                            enabled: gmpage.st.relay
                            onClicked: backend.setPaused(!gmpage.st.paused)
                        }
                    }
                    Controls.Switch {
                        Kirigami.FormData.label: "VR games:"
                        text: "Pause while a VR game runs, and resume when it ends"
                        checked: backend.pauseAuto
                        onToggled: backend.setPauseAuto(checked)
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Controller gesture" }
                    Controls.ComboBox {
                        id: firstBox
                        Kirigami.FormData.label: "Button:"
                        model: backend.gestureButtons
                        textRole: "text"
                        valueRole: "value"
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                        onActivated: gmpage.saveGesture()
                    }
                    Controls.ComboBox {
                        id: secondBox
                        Kirigami.FormData.label: "Together with:"
                        model: backend.gestureButtons
                        textRole: "text"
                        valueRole: "value"
                        enabled: firstBox.currentValue !== ""
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                        onActivated: gmpage.saveGesture()
                    }
                    Controls.ComboBox {
                        id: pressBox
                        Kirigami.FormData.label: "Pressed:"
                        model: [{ value: 2, text: "Twice" }, { value: 1, text: "Once" }]
                        textRole: "text"
                        valueRole: "value"
                        // One button pressed once would go off far too easily in games.
                        enabled: firstBox.currentValue !== "" && secondBox.currentValue !== ""
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                        onActivated: gmpage.saveGesture()
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        text: (backend.pauseGesture.first === "" ? "No gesture: a mapped button, a key combination, or ft-pause still work."
                               : "Pauses and resumes Frametop, in games too. The game still gets the presses, so pick buttons it "
                                 + "doesn't use this way. Two buttons count as together when both go down within a third of a "
                                 + "second, and twice means within 0.7 s. A gesture with a button on each controller works with "
                                 + "one controller too: while only one is connected, its button alone, clicked twice, does it.")
                              + (gmpage.st.relay && backend.pauseGesture.first !== "" && !gmpage.st.gestureReader
                                 ? " Not reading the controllers right now (SteamVR isn't running?)." : "")
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "While paused" }
                    Controls.ComboBox {
                        Kirigami.FormData.label: "The desktop:"
                        model: backend.pauseDesktopModes
                        textRole: "text"
                        valueRole: "value"
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                        Component.onCompleted: currentIndex = indexOfValue(backend.pauseDesktop)
                        onActivated: backend.setPauseDesktop(currentValue)
                    }
                    Controls.Switch {
                        Kirigami.FormData.label: "Sound:"
                        text: "Play a sound on pause and resume"
                        checked: backend.pauseSound
                        onToggled: backend.setPauseSound(checked)
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "Paused, Frametop leaves the headset's CPU and GPU to the game. The gaze service and our eye "
                          + "tracker stop, and nothing reads SteamVR's eye tracking. Hand tracking and remote desktop stop "
                          + "if they run. The desktop hides and slows down, or closes, as chosen above. The 3D mouse lets go, "
                          + "so the mouse is a plain one for SteamVR. Mapped buttons and key combinations do nothing but "
                          + "pausing, the Steam menu, and your own commands. Resuming brings back what pausing stopped. "
                          + "Map \"Pause/resume Frametop\" to a button or key combination on the other pages, or run "
                          + "input/ft-pause."
                }
            }
        }
    }

    // ---------------------------------------------------------------- Gaze
    Component {
        id: gazePage
        Kirigami.ScrollablePage {
            id: gpage
            title: "Gaze"
            header: DriverWarning {}
            property var status: backend.gazeStatus
            // The gaze service idles while the gaze isn't used; open, this page keeps it going.
            Timer {
                running: true
                repeat: true
                interval: 10000
                triggeredOnStart: true
                onTriggered: backend.keepGazeAwake()
            }
            actions: [
                Kirigami.Action {
                    text: "Quick check"
                    icon.name: "crosshairs"
                    enabled: backend.gazeServiceRunning
                    tooltip: "One dot in front of you in the headset: look at it, and the gaze tracker relearns where it sits"
                    onTriggered: backend.gazeQuickCheck()
                },
                Kirigami.Action {
                    text: "Calibrate"
                    icon.name: "crosshairs"
                    enabled: backend.gazeServiceRunning
                    tooltip: "The full calibration in the headset: dots in three rounds, dark to bright"
                    onTriggered: backend.gazeCalibrate()
                },
                Kirigami.Action {
                    text: "Check headset fit"
                    icon.name: "view-visible"
                    enabled: backend.gazeServiceRunning
                    tooltip: "In the headset: how well the eye tracker sees each eye, live, while you adjust it"
                    onTriggered: backend.gazeFitCheck()
                },
                Kirigami.Action {
                    text: "Reload calibration"
                    icon.name: "view-refresh"
                    enabled: backend.gazeServiceRunning
                    onTriggered: backend.reloadGazeCalibration()
                },
                Kirigami.Action {
                    text: "Forget nudges"
                    icon.name: "edit-clear-history"
                    enabled: backend.gazeServiceRunning
                    tooltip: "Drop what your mouse nudges taught; the calibration stays"
                    onTriggered: backend.forgetGazeLessons()
                },
                Kirigami.Action {
                    // A development tool: the checks and the calibration run in the headset panel.
                    text: "Gaze probe (development)…"
                    icon.name: "tools"
                    displayHint: Kirigami.DisplayHint.AlwaysHide
                    tooltip: "The gaze tracking's lab tool, for developing it: experiments, accuracy tests, practice"
                    onTriggered: backend.openGazeProbe()
                }
            ]

            Kirigami.FormLayout {
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    visible: true
                    type: Kirigami.MessageType.Warning
                    text: "Gaze mode is experimental. The pointer goes where you look and the mouse does the last bit; "
                          + "moving the mouse takes over, looking well away hands it back. A small nudge and a click "
                          + "teach the gaze service where it was off."
                }
                Controls.Switch {
                    Kirigami.FormData.label: "Gaze pointer:"
                    text: backend.gazeMode < 0 ? "Pointer helper not running" : "Pointer goes where you look"
                    enabled: backend.gazeMode >= 0
                    checked: backend.gazeMode > 0
                    onToggled: backend.setGazeMode(checked)
                }
                Controls.Label {
                    visible: backend.gazeMode >= 0 && (backend.gazeMode > 0) !== backend.gazeDefault
                    text: "Toggled by a button; it starts " + (backend.gazeDefault ? "on" : "off") + " after a restart."
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                }
                Controls.Label {
                    // Why the gaze pointer, on, can't follow your eyes yet, so it isn't left looking
                    // like a plain mouse. The calibration part is the gaze service's (gaze/gazecheck.py).
                    readonly property var checks: gpage.status.checks || {}
                    readonly property string why: !backend.gazeServiceInstalled
                        ? "The gaze service isn't installed: run gaze/run.sh install in the Frametop folder, in a terminal"
                        : !backend.gazeServiceRunning ? "The gaze service isn't running (it starts with SteamVR)"
                        : backend.gazeTracker === "own" && !gpage.status.eyegrab
                        ? "Our eye tracker needs its frame grabber: run gaze/tracker/install.sh (asks for sudo)"
                        : checks.problem || ""
                    visible: backend.gazeMode > 0 && why !== ""
                    text: why
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    wrapMode: Text.WordWrap
                    color: checks.check ? Kirigami.Theme.neutralTextColor : Kirigami.Theme.negativeTextColor
                    font: Kirigami.Theme.smallFont
                }
                ColumnLayout {
                    Kirigami.FormData.label: "Mouse left button:"
                    Repeater {
                        model: backend.gazeMouseChoices
                        delegate: Controls.RadioButton {
                            required property var modelData
                            text: modelData.text
                            checked: backend.gazeMouse === modelData.value
                            onToggled: if (checked) backend.setGazeMouse(modelData.value)
                        }
                    }
                }
                Controls.Switch {
                    Kirigami.FormData.label: "Mouse movement:"
                    text: "Only while a button is held (recommended)"
                    checked: backend.gazeMouseHeld
                    onToggled: backend.setGazeMouseHeld(checked)
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    Controls.ToolTip.text: checked
                        ? "Your eyes move the pointer, so moving the mouse on its own does nothing. Hold a button "
                          + "and the pointer stops where you look: move onto the target and let go to click there "
                          + "(left or right). To drag after moving, press right while holding left. It's on because a "
                          + "bumped mouse can't pull the pointer away, and every mouse move is a real correction the "
                          + "eye tracker learns from. If the eye tracker stops, the mouse works as usual."
                        : "The mouse moves the pointer any time, as without gaze. Moves that weren't corrections can "
                          + "teach the eye tracker the wrong thing."
                }
                Controls.Switch {
                    Kirigami.FormData.label: "Gaze dot:"
                    text: "Always shown (off: only while the mouse moves it)"
                    checked: backend.gazeDotAlways
                    onToggled: backend.setGazeDotAlways(checked)
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    wrapMode: Text.WordWrap
                    text: "Gaze works with the mouse and the keyboard. Keyboard clicks (Meta+J left, Meta+K right by "
                          + "default): tap to click where you look, or hold, turn your head until the dot sits on the "
                          + "target, and let go; hold still to drag. The Frame controllers don't take part, and moving "
                          + "one hands the pointer back to them. Gaze pointer on/off, Gaze precision, and Gaze drag can "
                          + "also go on a mouse button (Buttons page) or another key combination (Keyboard page)."
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                }
                // Whose eye tracking: SteamVR's (GAZE_TRACKER=steam), or ours (own) with the classic
                // pupil finder or an eye net (EYES_MODEL: latest or a version).
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Eye tracker" }
                ModelPicker {
                    Kirigami.FormData.label: "Tracker:"
                    kind: "eyes"
                    view: backend.eyeModels
                    Component.onCompleted: backend.reloadModels()
                }
                Controls.Label {
                    // Ours is the default once it's installed (GAZE_TRACKER=auto, gaze/ft-gazed).
                    visible: backend.gazeTracker === "steam" && !backend.gazeOwnInstalled
                    text: "Our own tracker (the classic one and the eye nets) needs its frame grabber. Install it "
                          + "with gaze/tracker/install.sh in the Frametop folder, in a terminal (asks for sudo)"
                          + (backend.gazeTrackerAuto ? "; gaze then uses it, and you calibrate it once."
                                                     : ", then pick it here and calibrate it once.")
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    wrapMode: Text.WordWrap
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                }
                Controls.Label {
                    // The gaze service runs ours (gaze/tracker/ft-eyes); it needs the frame grabber
                    // (gaze/tracker/install.sh, root) and keeps its own calibration.
                    visible: backend.gazeTracker === "own" && backend.gazeServiceRunning
                             && (!gpage.status.own_running || gpage.status.own_reseat || !gpage.status.calibration_samples)
                    text: !gpage.status.eyegrab ? "Needs its frame grabber: run gaze/tracker/install.sh (asks for sudo)"
                          : !gpage.status.own_running ? "Starting…"
                          : !gpage.status.calibration_samples ? "Not calibrated: use Calibrate"
                          : "The headset was off: your first nudge and click resets where it sits"
                    color: gpage.status.own_running ? Kirigami.Theme.neutralTextColor : Kirigami.Theme.negativeTextColor
                    font: Kirigami.Theme.smallFont
                }
                RowLayout {
                    Kirigami.FormData.label: "Eye bias:"
                    Controls.RadioButton {
                        text: "Auto"
                        checked: backend.gazeEye === "auto"
                        onToggled: if (checked) backend.setGazeEye("auto")
                    }
                    Controls.RadioButton {
                        text: "Left"
                        checked: backend.gazeEye === "left"
                        onToggled: if (checked) backend.setGazeEye("left")
                    }
                    Controls.RadioButton {
                        text: "Right"
                        checked: backend.gazeEye === "right"
                        onToggled: if (checked) backend.setGazeEye("right")
                    }
                }
                Controls.Label {
                    // What the bias comes to now; on auto, from how far off each eye was at the last nudges.
                    property var w: gpage.status.eye_weights
                    property var n: gpage.status.eye_misses || [0, 0]
                    property var rms: gpage.status.eye_rms || [null, null]
                    visible: backend.gazeServiceRunning && w !== undefined
                    text: w === undefined ? "" : "Left " + Math.round(w[0] * 100) + "% · right " + Math.round(w[1] * 100) + "%"
                          + (backend.gazeEye !== "auto" ? ""
                             : n[0] >= 5 && n[1] >= 5
                               ? " (off by " + rms[0].toFixed(1) + "° and " + rms[1].toFixed(1) + "° at your last nudges)"
                               : " (even until each eye has 5 nudges: " + n[0] + " and " + n[1] + " so far)")
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                }
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Gaze pointer tuning" }
                Repeater {
                    model: backend.gazeSettings
                    delegate: RowLayout {
                        required property var modelData
                        Kirigami.FormData.label: modelData.label + ":"
                        Controls.Slider {
                            id: gslider
                            from: modelData.min
                            to: modelData.max
                            stepSize: modelData.step
                            value: modelData.value
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                            onMoved: backend.setPointerSetting(modelData.key, value)
                        }
                        Controls.Label {
                            text: gslider.value.toFixed(modelData.step < 0.1 ? 2 : 1) + " " + modelData.unit
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                        }
                        Controls.ToolButton {
                            icon.name: "edit-undo"
                            display: Controls.AbstractButton.IconOnly
                            text: "Default (" + modelData.default + ")"
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: { gslider.value = modelData.default; backend.setPointerSetting(modelData.key, modelData.default) }
                        }
                    }
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Gaze service" }

                Controls.Label {
                    Kirigami.FormData.label: "Service:"
                    text: backend.gazeServiceRunning
                          ? (gpage.status.awake === false ? "idle: " + gpage.status.idle + " (it starts when the gaze is used)"
                             : gpage.status.ft_gaze ? "running" : "running, eye tracker reader starting")
                          : backend.gazeServiceInstalled ? "not running (frametop-gaze.service, starts with SteamVR)"
                          : "not installed: run gaze/run.sh install in the Frametop folder, in a terminal"
                    color: backend.gazeServiceRunning ? Kirigami.Theme.textColor : Kirigami.Theme.negativeTextColor
                }
                Controls.Label {
                    visible: backend.gazeServiceRunning
                    Kirigami.FormData.label: "Headset:"
                    text: gpage.status.headset_on ? "on" : "off"
                }
                Controls.Label {
                    visible: backend.gazeServiceRunning
                    Kirigami.FormData.label: "Tracking:"
                    text: gpage.status.rate === undefined || gpage.status.rate === null ? "…"
                          : Math.round(gpage.status.rate) + " samples/s"
                            + (gpage.status.one_eye_share > 0.5 ? " · only one eye tracked" : "")
                    color: gpage.status.one_eye_share > 0.5 ? Kirigami.Theme.neutralTextColor : Kirigami.Theme.textColor
                    Controls.ToolTip.text: "Only one eye tracked: the gaze comes from the other eye, a little less "
                                           + "precisely. Check headset fit shows where the tracker loses it."
                    Controls.ToolTip.visible: gpage.status.one_eye_share > 0.5 && ghover.hovered
                    HoverHandler { id: ghover }
                }
                Controls.Label {
                    // Share of the last second's samples where the tracker had lost each eye.
                    property real lostL: gpage.status.lost_left_share || 0
                    property real lostR: gpage.status.lost_right_share || 0
                    visible: backend.gazeServiceRunning && gpage.status.lost_left !== undefined
                    Kirigami.FormData.label: "Eyes:"
                    text: lostL < 0.05 && lostR < 0.05 ? "both tracked"
                          : (lostL >= 0.05 ? "left eye lost " + Math.round(lostL * 100) + "%" : "")
                            + (lostL >= 0.05 && lostR >= 0.05 ? " · " : "")
                            + (lostR >= 0.05 ? "right eye lost " + Math.round(lostR * 100) + "%" : "")
                            + ((lostL >= 0.05) !== (lostR >= 0.05) ? " (the other eye stands in)" : "")
                    color: lostL >= 0.05 || lostR >= 0.05 ? Kirigami.Theme.neutralTextColor : Kirigami.Theme.textColor
                }
                Controls.Label {
                    visible: backend.gazeServiceRunning
                    Kirigami.FormData.label: "Calibration:"
                    text: gpage.status.calibration_samples > 0
                          ? gpage.status.calibration_samples + " points ("
                            + (gpage.status.tracker === "own" ? "Own tracker, " + gpage.status.calibration_made
                               + (gpage.status.own_mapped && gpage.status.own_mapped.length === 1
                                  ? ", " + gpage.status.own_mapped[0] + " eye only" : "")
                               : gpage.status.kind === "eyes" ? gpage.status.model + ", each eye" : gpage.status.model) + ")"
                          : "none yet: use Calibrate"
                }
                Controls.Label {
                    visible: backend.gazeServiceRunning
                    Kirigami.FormData.label: "Learned from nudges:"
                    text: gpage.status.lessons + (gpage.status.lessons === 1 ? " nudge" : " nudges")
                          + (gpage.status.refused > 0 ? " (" + gpage.status.refused + " too far off, ignored)" : "")
                }
            }

            footer: Controls.Label {
                padding: Kirigami.Units.largeSpacing
                wrapMode: Text.Wrap
                opacity: 0.7
                text: "Map a mouse button (Buttons) or a key combination (Keyboard) to \"Gaze pointer on/off\" "
                      + "to switch it on the fly. A click waits for the release: if the gaze is off, drag onto the target "
                      + "with the button held and let go there. Hold still to drag: hold a press this long without moving "
                      + "to drag something instead. Look away to hand back: how far from the pointer you look before the "
                      + "gaze takes it back from the mouse. Largest correction to learn: a click corrected further than "
                      + "this isn't learned; the tracker is that far off, so the quick check runs instead. With hand tracking, "
                      + "a pinch clicks where you look; pinch, hold and move the hand to nudge the pointer first. Pinch nudge "
                      + "starts after: how far the hand moves before the pointer does. Pinch nudge speed: how far the pointer "
                      + "moves for each degree of hand movement after that. Eye tracker: SteamVR's, or our own "
                      + "(gaze/tracker), the classic pupil finder or an eye net; each keeps its own calibration. Eye bias: the gaze "
                      + "combines both eyes, since their errors partly cancel; Left or Right counts that eye twice as "
                      + "much, and Auto weights each by how far off it was at your recent nudges."
            }
        }
    }

    // ---------------------------------------------------------------- Hands
    Component {
        id: handsPage
        Kirigami.ScrollablePage {
            id: hpage
            title: "Hands"
            header: DriverWarning {}
            property var st: backend.handsStatus
            readonly property var range: backend.handsMarginRange
            // What runs, and why it can't: asked on the host, while this page is open.
            Timer {
                running: true
                repeat: true
                interval: 5000
                triggeredOnStart: true
                onTriggered: backend.refreshHands()
            }

            Kirigami.FormLayout {
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    visible: !!hpage.st.problem
                    type: backend.handsMode === "off" ? Kirigami.MessageType.Information : Kirigami.MessageType.Warning
                    text: hpage.st.problem || ""
                }
                ColumnLayout {
                    Kirigami.FormData.label: "Hand tracking:"
                    Repeater {
                        model: backend.handsModes
                        delegate: Controls.RadioButton {
                            required property var modelData
                            text: modelData.text
                            checked: backend.handsMode === modelData.value
                            onToggled: if (checked) backend.setHandsMode(modelData.value)
                        }
                    }
                }
                Controls.Label {
                    text: hpage.st.summary === undefined ? "…" : hpage.st.summary
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                }
                RowLayout {
                    Kirigami.FormData.label: "Hand cutout margin:"
                    Controls.Label { text: "smaller"; opacity: 0.7 }
                    Controls.Slider {
                        id: marginSlider
                        from: hpage.range.min
                        to: hpage.range.max
                        stepSize: hpage.range.step
                        snapMode: Controls.Slider.SnapAlways
                        value: backend.handsMargin
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                        onMoved: backend.setHandsMargin(value)
                    }
                    Controls.Label { text: "bigger"; opacity: 0.7 }
                    Controls.Label {
                        text: marginSlider.value.toFixed(2) + "°"
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 3
                    }
                    Controls.ToolButton {
                        icon.name: "edit-undo"
                        display: Controls.AbstractButton.IconOnly
                        text: "Default (" + hpage.range.default + "°)"
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered
                        onClicked: { marginSlider.value = hpage.range.default; backend.setHandsMargin(hpage.range.default) }
                    }
                }

                // Which hand models ft-hands runs (HANDS_MODEL): latest, a version, or stock MediaPipe.
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Hand model" }
                ModelPicker {
                    Kirigami.FormData.label: "Model:"
                    kind: "hands"
                    view: backend.handModels
                    Component.onCompleted: backend.reloadModels()
                }

                // Three ways the hands work the desktop, each on its own (handsmode.METHODS).
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Hand input" }
                Controls.Switch {
                    Kirigami.FormData.label: "With gaze on:"
                    text: "Pinch to click where you look"
                    checked: !!backend.handMethods.gaze_clicks
                    onToggled: backend.setHandMethod("gaze_clicks", checked)
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    Controls.ToolTip.text: "Your eyes aim and your hands are the mouse's buttons. Index pinch: left click. "
                        + "Middle finger to thumb: right click. Pinch and hold still to press and drag. Hold a pinch "
                        + "with one hand and pinch with the other: both buttons held, each until its pinch opens."
                }
                Controls.Switch {
                    Kirigami.FormData.label: "With gaze off:"
                    text: "Point a raised hand for a ray"
                    checked: !!backend.handMethods.ray
                    onToggled: backend.setHandMethod("ray", checked)
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    Controls.ToolTip.text: "A hand raised in front and pointing shows a ray that works like a controller's "
                        + "laser: pinch to click, middle pinch to right-click or scroll. A pinch with the hand held low "
                        + "presses where the pointer is."
                }
                Controls.Switch {
                    Kirigami.FormData.label: "Direct touch:"
                    text: "Poke and grab Frametop's screens, gaze on or off"
                    checked: !!backend.handMethods.touch
                    onToggled: backend.setHandMethod("touch", checked)
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    Controls.ToolTip.text: "Touch a screen with your index finger to click it. Pinch its bar, an edge or "
                        + "a corner to move or resize it."
                }
                Controls.Label {
                    visible: backend.handsMode === "off"
                    text: "These need hand tracking on (above)."
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                }

                // Every interaction on Frametop's screens, each on its own (ft_input_settings.HAND_INTERACTIONS);
                // they apply at once. Direct touch above switches all of them.
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Hand interactions" }
                Repeater {
                    model: backend.handInteractions
                    delegate: Controls.Switch {
                        required property var modelData
                        Kirigami.FormData.label: modelData.group ? modelData.group + ":" : ""
                        text: modelData.label
                        checked: modelData.on
                        enabled: !!backend.handMethods.touch
                        onToggled: backend.setHandInteraction(modelData.key, checked)
                        Controls.ToolTip.visible: hovered
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                        Controls.ToolTip.text: modelData.tip
                    }
                }
            }

            footer: Controls.Label {
                padding: Kirigami.Units.largeSpacing
                wrapMode: Text.Wrap
                opacity: 0.7
                text: "Your hands show through Frametop's screens where they're in front of them. Hand tracking "
                      + "uses a lot of the headset's CPU. The margin is how far past your hand the screen opens up; "
                      + "it applies right away. The hand inputs are separate: any of them can be on alone, and they "
                      + "apply within a second. A hand holding a controller, or whose controller you just used, "
                      + "does none of them."
            }
        }
    }

    // ---------------------------------------------------------------- Bluetooth
    Component {
        id: bluetoothPage
        Kirigami.ScrollablePage {
            title: "Bluetooth"
            header: DriverWarning {}
            actions: [
                Kirigami.Action { text: "Refresh"; icon.name: "view-refresh"; onTriggered: backend.refreshBluetooth() },
                Kirigami.Action {
                    text: "Apply Bluetooth fixes"
                    icon.name: "tools-wizard"
                    tooltip: "Privacy off, and address resolution on bonded LE devices (asks for your password)"
                    onTriggered: backend.applyBluetoothFixes()
                }
            ]

            ListView {
                model: backend.bluetooth
                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    visible: parent.count === 0
                    text: "No paired Bluetooth devices"
                    explanation: "Pair in Steam: Settings → Bluetooth."
                }
                delegate: Controls.ItemDelegate {
                    required property var modelData
                    width: ListView.view.width
                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        Kirigami.Icon {
                            source: "preferences-system-bluetooth"
                            implicitWidth: Kirigami.Units.iconSizes.medium
                            implicitHeight: implicitWidth
                            opacity: modelData.connected ? 1 : 0.4
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Controls.Label { text: modelData.name; font.bold: true }
                            Controls.Label {
                                text: modelData.address + " · " + (modelData.connected ? "connected" : "not connected")
                                      + (modelData.battery >= 0 ? " · battery " + modelData.battery + "%" : "")
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                            }
                        }
                    }
                }
            }

            footer: Controls.Label {
                padding: Kirigami.Units.largeSpacing
                wrapMode: Text.Wrap
                opacity: 0.7
                text: "Pair new devices in Steam (Settings → Bluetooth). After pairing an LE mouse or keyboard, "
                      + "use Apply Bluetooth fixes once so it reconnects on its own."
            }
        }
    }
}
