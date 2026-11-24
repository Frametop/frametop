// Frametop Settings' input pages (settings/main.qml loads this file the first time one of them
// opens). Backend: ft_input_settings.py ("backend", made then: settings/ft_settings.py).
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "../settings/controls" as Ft

Item {
    id: root
    // settings/main.qml sets these two.
    property QtObject backend
    property var window
    // Another page of the app, by its name (settings/ft_settings.py, SECTIONS).
    function show(name) { window.showSection(name) }
    function showPassiveNotification(text, duration) { window.showPassiveNotification(text, duration) }
    // Another section opened: the page left gives up the key combination it was waiting for (the
    // Keyboard page's, the Remote display keys page's), so the next keys pressed aren't taken for it.
    Connections {
        target: root.window
        function onCurrentChanged() {
            if (backend.capturingShortcut) backend.cancelShortcutCapture()
            backend.remoteKeys.cancelCapture()
        }
    }

    // The page for a section of the app, by name; null for a name that isn't one of these.
    function page(name) {
        return ({ devices: devicesPage, buttons: buttonsPage, controllers: controllersPage, games: gamesPage,
                  keyboard: keyboardPage, "remote-keys": remoteKeysPage, commands: commandsPage, voice: voicePage,
                  pointer: pointerPage, ignore: ignorePage, gaze: gazePage, hands: handsPage,
                  bluetooth: bluetoothPage })[name] || null
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
            text: "Automatically stays on the newest release as updates ship. Selecting a specific version locks to that version."
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
        RowLayout {
            Controls.Button {
                text: picker.inUse ? "Active"
                      : !picker.opt.installed ? "Download & use"
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
                text: "Check for updates"
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
        readonly property bool hasBench: (picker.view.metrics || []).some(m => picker.opt.bench && picker.opt.bench[m.key] !== undefined)
        Ft.Disclosure {
            id: techDetails
            Layout.fillWidth: true
            visible: !!picker.opt.notes || !!picker.opt.cost || picker.hasBench
            title: "Technical details & benchmarks"
            summary: "release notes, resource cost, scores"
        }
        Controls.Label {
            text: picker.opt.notes || ""
            visible: techDetails.expanded && text !== ""
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.85
            font: Kirigami.Theme.smallFont
        }
        Controls.Label {
            text: picker.opt.cost ? "Resource cost: " + picker.opt.cost : ""
            visible: techDetails.expanded && text !== ""
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
        }
        GridLayout {
            visible: techDetails.expanded && picker.hasBench
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
            visible: techDetails.expanded && picker.hasBench
            text: "Green: better than " + (picker.otherName || "baseline") + "; red: worse. Hover any metric for details."
            textFormat: Text.PlainText
            opacity: 0.7
            font: Kirigami.Theme.smallFont
        }
        Controls.Label {
            visible: techDetails.expanded
            text: "Catalog source: " + (picker.view.source || "built-in")
                  + (picker.view.checked ? " (checked " + picker.view.checked + ")" : "")
            opacity: 0.6
            font: Kirigami.Theme.smallFont
        }
    }

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    // One group of the Pointer page's sliders (ft_input_settings.py POINTER_SETTINGS). The rows
    // are counted, not listed: a new list would rebuild them, and FormLayout puts rebuilt rows last.
    component PointerSliders: Repeater {
        id: sliders
        property string group
        property bool shown: true
        readonly property var settings: backend.pointerSettings.filter(s => s.group === group)
        model: settings.length
        delegate: Ft.SliderRow {
            required property int index
            readonly property var setting: sliders.settings[index]
            visible: sliders.shown
            Kirigami.FormData.label: setting.label + ":"
            from: setting.min
            to: setting.max
            stepSize: setting.step
            value: setting.value
            sliderWidth: Kirigami.Units.gridUnit * 14
            shown: (setting.step < 1 ? value.toFixed(setting.step < 0.01 ? 3 : 2) : Math.round(value))
                   + (setting.unit ? " " + setting.unit : "")
            reset: setting.default
            tip: setting.help
            onMoved: backend.setPointerSetting(setting.key, value)
        }
    }

    // ---------------------------------------------------------------- Devices
    Component {
        id: devicesPage
        Kirigami.ScrollablePage {
            title: "Mice & keyboards"
            actions: [
                Kirigami.Action {
                    text: "Bluetooth"
                    icon.name: "preferences-system-bluetooth"
                    tooltip: "View paired Bluetooth devices and apply reconnect fixes"
                    onTriggered: root.show("bluetooth")
                }
            ]

            header: ColumnLayout {
                spacing: 0
                DriverWarning { Layout.fillWidth: true }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !backend.relayRunning || !backend.pointerMode
                    position: Kirigami.InlineMessage.Position.Header
                    type: backend.relayRunning ? Kirigami.MessageType.Information : Kirigami.MessageType.Error
                    text: !backend.relayRunning
                          ? "Frametop's input service isn't running right now, so devices and shortcuts won't respond."
                          : "3D pointer mode is turned off in your config, so mice behave as standard 2D mice."
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
                                      ? row.modelData.bus + " · " + row.modelData.kinds.join(" + ")
                                        + (row.modelData.grabbed ? " · active 3D pointer" : "")
                                      : "Disconnected · saved preference"
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

            footer: Ft.HelpFooter {
                text: "Move a mouse or press a key to flash its row's indicator light. "
                      + "3D pointer: controls the VR cursor. Pass through: types normally while supporting Frametop shortcuts. "
                      + "Keyboard + 3D pointer: best for keyboards with a built-in touchpad (keys type, touchpad steers the pointer). "
                      + "Ignore: Frametop leaves the device completely untouched."
            }
        }
    }

    // ---------------------------------------------------------------- Buttons
    Component {
        id: buttonsPage
        Kirigami.ScrollablePage {
            id: bpage
            title: "Mouse buttons"
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
                    explanation: "Assign a mouse to 3D pointer on the Mice & keyboards page first."
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
                    text: "Unmapped buttons work normally as regular clicks or keys. Extra programmable buttons on some "
                          + "mice may appear as keyboard keys when captured. To remap VR controller buttons, open the "
                          + "Controllers page."
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
                    text: "Frametop's pointer service isn't responding (requires SteamVR to be running). "
                          + "Controller button mappings need it to detect presses."
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: cpage.status.helper && !cpage.status.manifest
                    type: Kirigami.MessageType.Error
                    text: "Couldn't register controller actions with SteamVR. Try restarting SteamVR or checking the pointer log."
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: cpage.status.helper && !cpage.status.global && backend.controllerMappings.length > 0
                    type: Kirigami.MessageType.Warning
                    text: "Global input is currently off, so mapped controller buttons may not work while another app is active."
                }

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    Controls.Switch {
                        Kirigami.FormData.label: "Global input:"
                        text: "Mapped buttons work with any app in front"
                        checked: cpage.status.global
                        enabled: cpage.status.helper
                        onToggled: backend.setGlobalInput(checked)
                    }
                    Ft.Hint { text: "Required for controller mappings (controls SteamVR's \"Enable global input from overlays\" setting)." }
                    Controls.Switch {
                        Kirigami.FormData.label: "In games:"
                        text: "Keep mapped buttons active during VR games (overrides the game)"
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
                    }
                    RowLayout {
                        Kirigami.FormData.label: "Action:"
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
                    text: "Mapped buttons trigger Frametop actions outside of games, and return to the game while you play "
                          + "(unless \"In games\" is turned on above). The system button is always reserved by SteamVR. "
                          + "Tip: avoid remapping the trigger or grip since SteamVR uses them to aim its laser."
                }
            }
        }
    }

    // ---------------------------------------------------------------- Keyboard
    Component {
        id: keyboardPage
        Kirigami.ScrollablePage {
            id: kpage
            title: "Keyboard & shortcuts"
            // Pass-through keyboards connected now: with "no_keyboard", the keyboard waits for none.
            // A program's uinput keyboard (a remote desktop's, say) doesn't count.
            property var keyboards: backend.devices.filter(d => d.connected && (d.role === "passthrough" || d.role === "combo")
                                                                && d.kinds.indexOf("keyboard") >= 0 && !d.uinput)
            // 3D pointer devices with keys as well as a mouse: a keyboard with a touchpad among them
            // types nothing and isn't counted above until its role is Keyboard + 3D pointer (issue 68).
            // A mouse with keys for its extra buttons looks the same from here, so this only asks.
            property var keyedPointers: backend.devices.filter(d => d.connected && d.role === "pointer" && !d.uinput
                                                                    && d.kinds.indexOf("keyboard") >= 0
                                                                    && d.kinds.indexOf("mouse") >= 0)

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
                    text: "Until you close it, not only while a text field has focus"
                    checked: backend.vrKeyboardPersist
                    enabled: backend.vrKeyboard !== "never"
                    onToggled: backend.setVrKeyboardPersist(checked)
                }
                Ft.Hint { text: "Close it with its X or your keyboard button." }
                Controls.Button {
                    objectName: "toggleVrKeyboard"
                    Kirigami.FormData.label: "Now:"
                    text: "Open or close the keyboard"
                    icon.name: "input-keyboard-virtual"
                    enabled: backend.vrKeyboard !== "never" && backend.relayRunning
                    onClicked: backend.toggleVrKeyboard()
                }
                Ft.Hint { text: "Also reopens the keyboard if you previously closed it from its panel." }
                Controls.ComboBox {
                    objectName: "steamFieldsKeyboard"
                    Kirigami.FormData.label: "In Steam's text fields:"
                    model: backend.steamFieldsKeyboards
                    textRole: "text"
                    valueRole: "value"
                    Component.onCompleted: currentIndex = indexOfValue(backend.steamFieldsKeyboard)
                    onActivated: backend.setSteamFieldsKeyboard(currentValue)
                }
                Controls.Label {
                    visible: backend.steamFieldsKeyboard === "steam"
                    text: "Steam and its games open Steam's keyboard, which a Steam Controller or gamepad can type on. "
                          + "The desktop keeps the setting above."
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                }
                Controls.Label {
                    objectName: "keyboardsConnected"
                    Kirigami.FormData.label: "Keyboards connected:"
                    text: kpage.keyboards.length ? kpage.keyboards.map(d => d.name).join(", ") : "none"
                }
                Controls.Label {
                    objectName: "keyedPointers"
                    visible: kpage.keyedPointers.length > 0
                    text: "Not counted, because its role is 3D pointer: " + kpage.keyedPointers.map(d => d.name).join(", ")
                          + ". If that's a keyboard with a touchpad, change its role to Keyboard + 3D pointer in "
                          + "Mice & keyboards so its keys type while the touchpad steers the pointer."
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                }
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Desktop layout" }
                Controls.ComboBox {
                    Kirigami.FormData.label: "Language:"
                    model: backend.desktopLayouts
                    textRole: "text"
                    valueRole: "value"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 16
                    readonly property string want: backend.desktopLayout
                    onWantChanged: currentIndex = indexOfValue(want)
                    onModelChanged: currentIndex = indexOfValue(want)
                    Component.onCompleted: currentIndex = indexOfValue(want)
                    // the saved one again: a pick that didn't save doesn't stay showing
                    onActivated: { backend.setDesktopLayout(currentValue); currentIndex = indexOfValue(backend.desktopLayout) }
                }
                Controls.ComboBox {
                    Kirigami.FormData.label: "Variant:"
                    model: backend.desktopVariants
                    textRole: "text"
                    valueRole: "value"
                    visible: count > 1
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 16
                    readonly property string want: backend.desktopVariant
                    onWantChanged: currentIndex = indexOfValue(want)
                    onModelChanged: currentIndex = indexOfValue(want)
                    Component.onCompleted: currentIndex = indexOfValue(want)
                    // the saved one again: a pick that didn't save doesn't stay showing
                    onActivated: { backend.setDesktopVariant(currentValue); currentIndex = indexOfValue(backend.desktopVariant) }
                }
                Controls.Label {
                    text: "Sets the typing language for physical keyboards, the VR keyboard, and voice dictation on the desktop (Steam and VR games use their own language setting)."
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                }
                Controls.Button {
                    text: "More layouts and a switch shortcut…"
                    icon.name: "input-keyboard"
                    onClicked: backend.openKeyboardLayouts()
                }
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Keyboard shortcuts" }
                Controls.Switch {
                    Kirigami.FormData.label: "Meta tap:"
                    text: "Tap the Meta (Windows/Super) key alone to trigger its shortcut"
                    checked: backend.metaTap
                    onToggled: backend.setMetaTap(checked)
                }
                Ft.Hint { text: "Shortcuts that combine Meta with another key always work either way." }
                Controls.Switch {
                    Kirigami.FormData.label: "Carry floated windows:"
                    text: "Automatically attach a newly floated window to your pointer"
                    checked: backend.carryOnFloat
                    onToggled: backend.setCarryOnFloat(checked)
                }
                Ft.Hint { text: "Move your pointer to place the window, then click or press Enter to drop it." }
                Controls.ComboBox {
                    id: windowDragMod
                    Kirigami.FormData.label: "Window drag:"
                    model: backend.windowDragMods
                    textRole: "text"
                    valueRole: "value"
                    Component.onCompleted: currentIndex = indexOfValue(backend.windowDrag.mod)
                    onActivated: { backend.setWindowDrag("mod", currentValue); currentIndex = indexOfValue(backend.windowDrag.mod) }
                }
                Controls.Label {
                    text: "Hold this key and drag anywhere inside a window with the mouse to move or resize it without grabbing its title bar. While carrying a floating window, scroll forward into a screen to dock it there, or scroll back on a remote PC screen to pop a window out."
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                }
                Repeater {
                    model: [{ which: "left", label: "Left button" }, { which: "right", label: "Right button" },
                            { which: "middle", label: "Middle button" }]
                    delegate: Controls.ComboBox {
                        required property var modelData
                        Kirigami.FormData.label: modelData.label + ":"
                        enabled: windowDragMod.currentValue !== "off"
                        model: backend.windowDragActions
                        textRole: "text"
                        valueRole: "value"
                        Component.onCompleted: currentIndex = indexOfValue(backend.windowDrag[modelData.which])
                        onActivated: {
                            backend.setWindowDrag(modelData.which, currentValue)
                            currentIndex = indexOfValue(backend.windowDrag[modelData.which])
                        }
                    }
                }

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
                Controls.Button {
                    text: "Configure shortcuts for remote PCs…"
                    icon.name: "preferences-desktop-keyboard-shortcut"
                    onClicked: root.show("remote-keys")
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    wrapMode: Text.WordWrap
                    text: "To record a shortcut, hold modifier keys (Ctrl, Alt, Shift, Meta) and press a key — or tap a "
                          + "single modifier on its own. Built-in defaults: Meta tap opens the Steam menu, Meta+Shift+F "
                          + "floats/docks the window under your pointer, Meta+Shift+H hides/shows screens, Meta+W closes "
                          + "the focused app, and Ctrl+Alt+Shift+Esc releases any stuck keys."
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                }
            }

            footer: Ft.HelpFooter {
                text: "Frametop's VR keyboard opens in front of you and works with controller lasers, the 3D mouse, or direct "
                      + "fingertip taps. Some apps (such as Chrome or Electron apps) don't report when a text box is focused — "
                      + "for those, map \"Open/close keyboard\" in Mouse buttons or Controllers, or click the button above."
            }
        }
    }

    // ---------------------------------------------------------------- Voice
    Component {
        id: voicePage
        Kirigami.ScrollablePage {
            id: vpage
            title: "Voice dictation"
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
                    text: "Voice dictation isn't installed yet. Run voice/run.sh install from the Frametop folder in a "
                          + "terminal to download the offline speech models (~320 MB, runs completely on the headset)."
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.voiceInstalled && backend.voiceEnabled && !vpage.st.running
                    type: Kirigami.MessageType.Warning
                    text: "Voice dictation is enabled but the background service stopped. Check logs with: journalctl --user -u frametop-voice."
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
                        text: "Change in Keyboard & shortcuts"
                        icon.name: "input-keyboard"
                        onClicked: root.show("keyboard")
                    }
                    Controls.ComboBox {
                        Kirigami.FormData.label: "Keys do:"
                        model: backend.dictationKeyModes
                        textRole: "text"
                        valueRole: "value"
                        enabled: backend.dictationKeys.length > 0
                        displayText: backend.dictationKeyMode ? currentText : "Some tap, some are held"
                        readonly property string want: backend.dictationKeyMode
                        onWantChanged: currentIndex = indexOfValue(want)
                        Component.onCompleted: currentIndex = indexOfValue(want)
                        onActivated: backend.setDictationKeyMode(currentValue)
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

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Language" }
                    Controls.ComboBox {
                        Kirigami.FormData.label: "You speak:"
                        model: backend.voiceLanguages
                        textRole: "text"
                        valueRole: "value"
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                        readonly property string want: vpage.vs.language
                        onWantChanged: currentIndex = indexOfValue(want)
                        Component.onCompleted: currentIndex = indexOfValue(want)
                        onActivated: backend.setVoiceLanguage(currentValue)
                    }
                    Controls.ComboBox {
                        Kirigami.FormData.label: "Speech model:"
                        model: backend.voiceModels
                        textRole: "text"
                        valueRole: "value"
                        enabled: !backend.voiceModelFetching
                        displayText: backend.voiceModelFetching ? "Downloading " + backend.voiceModelFetching + "…" : currentText
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 18
                        readonly property string want: backend.voiceModel
                        onWantChanged: currentIndex = indexOfValue(want)
                        onModelChanged: currentIndex = indexOfValue(want)
                        Component.onCompleted: currentIndex = indexOfValue(want)
                        onActivated: backend.setVoiceModel(currentValue)
                    }
                    Controls.Label {
                        text: vpage.vs.language === "en"
                              ? "Fast live English dictation is used when \"Type as you speak\" is on. The speech model above is used for full-sentence dictation and voice commands."
                              : "Larger models improve accuracy for non-English languages but respond slower. The largest model (large-v3-turbo) may cause VR stutter."
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Typing" }
                    Controls.Switch {
                        Kirigami.FormData.label: "Live typing:"
                        text: "Type words as you speak (off: type after you finish)"
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
                        Kirigami.FormData.label: "Hands-free wake word:"
                        text: "Keep microphone listening for a wake phrase + voice command"
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
                    text: "All speech recognition runs privately on your headset — no audio is sent online. Dictated text "
                          + "types into whichever window or Steam field you last clicked. Push-to-talk keys pause while you hold "
                          + "other modifiers so they don't trigger shortcuts accidentally. Voice commands can run any saved "
                          + "entry from Custom commands."
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
                PointerSliders { group: "basic" }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Head follow" }
                Controls.Switch {
                    Kirigami.FormData.label: "Head follow:"
                    text: "The pointer follows your head (experimental)"
                    checked: backend.pointerFollow
                    onToggled: backend.setPointerFollow(checked)
                }
                Ft.Hint {
                    text: "Experimental feature: keeps the 3D pointer moving along with your view when you turn your head."
                }
                PointerSliders { group: "follow"; shown: backend.pointerFollow }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Travel" }
                Controls.Switch {
                    Kirigami.FormData.label: "Travel mode:"
                    text: "Track in a moving car, train or plane"
                    checked: backend.travelMode
                    enabled: backend.travelKnown && !backend.travelBusy
                    onToggled: backend.setTravelMode(checked)
                    Component.onCompleted: backend.refreshTravel()
                }
                Ft.Hint { text: "Prevents headset tracking from drifting in moving vehicles (controls SteamVR's Travel mode setting)." }
                RowLayout {
                    visible: backend.travelNote !== ""
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        text: backend.travelNote
                    }
                    Controls.Button {
                        text: "Check again"
                        enabled: !backend.travelBusy
                        onClicked: backend.refreshTravel()  // after starting SteamVR
                    }
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true }
                Ft.Disclosure {
                    id: pointerFine
                    title: "Fine tuning"
                    summary: "waking, handing back to controllers, reach"
                }
                PointerSliders { group: "fine"; shown: pointerFine.expanded }
            }

            footer: Ft.HelpFooter {
                text: "All pointer changes take effect immediately. Hover over any slider for details, or click its "
                      + "reset arrow to restore the default value."
            }
        }
    }

    // ---------------------------------------------------------------- Ignored panels
    Component {
        id: ignorePage
        Kirigami.ScrollablePage {
            id: ipage
            title: "Click-through panels"
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
                    text: "Frametop's pointer service isn't responding (requires SteamVR to be running). "
                          + "It is needed to list active VR overlays and let clicks pass through them."
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Checked panels become click-through for your 3D mouse pointer so clicks reach the screen "
                          + "behind them. Useful for view-only overlays like FPS counters or performance graphs. "
                          + "VR controllers are not affected."
                }
                Controls.Switch {
                    id: showHidden
                    text: "Show currently hidden overlays"
                }
                Controls.Label {
                    visible: backend.controllerStatus.helper && !backend.panelsLoaded
                    text: "Loading active SteamVR overlays…"
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
                                text: "Click through entire app"
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
                    text: "Saved click-through rules (not currently open)"
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

            footer: Ft.HelpFooter {
                text: "Changes apply immediately. Checking an entire app also makes any future overlays from that app "
                      + "click-through. Frametop's own screens are excluded from this list."
            }
        }
    }

    // ---------------------------------------------------------------- Games
    Component {
        id: gamesPage
        Kirigami.ScrollablePage {
            id: gmpage
            title: "VR games"
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
                          ? "Your running input service is from an older version and can't pause for games. "
                            + "Restart it in a terminal: systemctl --user restart frametop-input-relay"
                          : "Frametop's input service isn't responding right now, so automatic game pausing is unavailable."
                }

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    RowLayout {
                        Kirigami.FormData.label: "Status:"
                        Controls.Label { text: gmpage.st.summary }
                        Controls.Button {
                            text: gmpage.st.paused ? "Resume" : "Pause now"
                            icon.name: gmpage.st.paused ? "media-playback-start" : "media-playback-pause"
                            enabled: gmpage.st.relay
                            onClicked: backend.setPaused(!gmpage.st.paused)
                        }
                    }
                    Controls.Switch {
                        Kirigami.FormData.label: "Auto-pause:"
                        text: "Automatically pause Frametop when a VR game launches"
                        checked: backend.pauseAuto
                        onToggled: backend.setPauseAuto(checked)
                    }
                    Controls.ComboBox {
                        Kirigami.FormData.label: "PC games over Steam Link:"
                        model: backend.pcGamesModes
                        textRole: "text"
                        valueRole: "value"
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                        Component.onCompleted: currentIndex = indexOfValue(backend.pcGames)
                        onActivated: backend.setPcGames(currentValue)
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        text: "Frametop stays active while viewing SteamVR Home over Steam Link. Detecting PC VR games requires adding your PC in Remote displays with Vibepollo installed."
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Quick pause/resume shortcut" }
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
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        text: (backend.pauseGesture.first === "" ? "No controller gesture set (you can still map \"Pause/resume Frametop\" in Keyboard & shortcuts or Mouse buttons)."
                               : "Lets you pause or resume Frametop anytime, even mid-game. The game still receives the button presses, "
                                 + "so choose a combo you won't hit accidentally. If a combo uses one button on each controller, double-pressing "
                                 + "either button works when only one controller is turned on.")
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
                    text: "When paused, Frametop frees up CPU and GPU performance for your VR game by stopping eye tracking, "
                          + "hand tracking, and remote desktop streaming, and hiding or closing the desktop as chosen above. "
                          + "Everything resumes automatically when your game exits or when you unpause."
                }
            }
        }
    }

    // ---------------------------------------------------------------- Gaze
    Component {
        id: gazePage
        Kirigami.ScrollablePage {
            id: gpage
            title: "Eye tracking"
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
                    text: "Eye tracking pointer is experimental. Look at what you want to click, then nudge with your mouse "
                          + "or pinch to fine-tune if needed — each small correction teaches eye tracking to become more accurate."
                }
                Controls.Switch {
                    Kirigami.FormData.label: "Eye-tracking pointer:"
                    text: backend.gazeMode < 0 ? "Pointer service not running" : "Move pointer where you look"
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
                    Kirigami.FormData.label: "Eye cursor dot:"
                    text: "Always visible (off: only shows while adjusting)"
                    checked: backend.gazeDotAlways
                    onToggled: backend.setGazeDotAlways(checked)
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                    wrapMode: Text.WordWrap
                    text: "Works with your mouse, keyboard (Meta+J to left-click, Meta+K to right-click), or hand pinches. "
                          + "Hold a click key and turn your head slightly to fine-tune, or hold still to drag. "
                          + "You can also bind \"Gaze pointer on/off\" in Mouse buttons or Keyboard & shortcuts."
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                }
                // Whose eye tracking: SteamVR's (GAZE_TRACKER=steam), or ours (own) with the classic
                // pupil finder or an eye net (EYES_MODEL: latest or a version).
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Eye tracker" }
                ModelPicker {
                    Kirigami.FormData.label: "Tracker:"
                    Kirigami.FormData.labelAlignment: Qt.AlignTop
                    kind: "eyes"
                    view: backend.eyeModels
                    Component.onCompleted: backend.reloadModels()
                }
                Controls.Label {
                    // Ours is the default once it's installed (GAZE_TRACKER=auto, gaze/ft-gazed).
                    visible: backend.gazeTracker === "steam" && !backend.gazeOwnInstalled
                    text: "Frametop's built-in eye tracker needs a one-time setup step. Run gaze/tracker/install.sh in a "
                          + "terminal (asks for your password)"
                          + (backend.gazeTrackerAuto ? ", then run Calibrate above."
                                                     : ", then select it here and run Calibrate above.")
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
                    text: !gpage.status.eyegrab ? "Setup required: run gaze/tracker/install.sh in a terminal (asks for your password)"
                          : !gpage.status.own_running ? "Starting…"
                          : !gpage.status.calibration_samples ? "Not calibrated: use Calibrate"
                          : "The headset was off: your first nudge and click resets where it sits"
                    color: gpage.status.own_running ? Kirigami.Theme.neutralTextColor : Kirigami.Theme.negativeTextColor
                    font: Kirigami.Theme.smallFont
                }
                RowLayout {
                    Kirigami.FormData.label: "Dominant eye:"
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
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Status" }
                Controls.Label {
                    Kirigami.FormData.label: "Eye tracking:"
                    text: backend.gazeServiceRunning
                          ? (gpage.status.awake === false ? "Ready (idle until used)"
                             : gpage.status.ft_gaze ? "Running" : "Starting up…")
                          : backend.gazeServiceInstalled ? "Not running (starts automatically with SteamVR)"
                          : "Not installed — run gaze/run.sh install in a terminal"
                    color: backend.gazeServiceRunning ? Kirigami.Theme.textColor : Kirigami.Theme.negativeTextColor
                    wrapMode: Text.Wrap
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                }
                Controls.Label {
                    visible: backend.gazeServiceRunning
                    Kirigami.FormData.label: "Calibration:"
                    text: gpage.status.calibration_samples > 0
                          ? gpage.status.calibration_samples + " calibration points saved"
                          : "Not calibrated yet — click Calibrate above"
                    wrapMode: Text.Wrap
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                }

                Ft.Disclosure {
                    id: gazeTuning
                    title: "Pointer fine-tuning"
                    summary: "hand-back angle, drag hold, pinch speed"
                }
                Repeater {
                    model: backend.gazeSettings
                    delegate: RowLayout {
                        required property var modelData
                        visible: gazeTuning.expanded
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

                Ft.Disclosure {
                    id: gazeDiag
                    visible: backend.gazeServiceRunning
                    title: "Tracking diagnostics"
                    summary: "sample rate, eye visibility, learned corrections"
                }
                Controls.Label {
                    visible: backend.gazeServiceRunning && gazeDiag.expanded
                    Kirigami.FormData.label: "Headset worn:"
                    text: gpage.status.headset_on ? "Yes" : "No"
                }
                Controls.Label {
                    visible: backend.gazeServiceRunning && gazeDiag.expanded
                    Kirigami.FormData.label: "Tracking rate:"
                    text: gpage.status.rate === undefined || gpage.status.rate === null ? "…"
                          : Math.round(gpage.status.rate) + " Hz"
                            + (gpage.status.one_eye_share > 0.5 ? " · one eye only" : "")
                    color: gpage.status.one_eye_share > 0.5 ? Kirigami.Theme.neutralTextColor : Kirigami.Theme.textColor
                    Controls.ToolTip.text: "Only one eye is currently visible to the tracker, which slightly reduces accuracy. Use Check headset fit above to adjust."
                    Controls.ToolTip.visible: gpage.status.one_eye_share > 0.5 && ghover.hovered
                    HoverHandler { id: ghover }
                }
                Controls.Label {
                    // Share of the last second's samples where the tracker had lost each eye.
                    property real lostL: gpage.status.lost_left_share || 0
                    property real lostR: gpage.status.lost_right_share || 0
                    visible: backend.gazeServiceRunning && gazeDiag.expanded && gpage.status.lost_left !== undefined
                    Kirigami.FormData.label: "Eye visibility:"
                    text: lostL < 0.05 && lostR < 0.05 ? "Both eyes tracked"
                          : (lostL >= 0.05 ? "Left eye lost " + Math.round(lostL * 100) + "%" : "")
                            + (lostL >= 0.05 && lostR >= 0.05 ? " · " : "")
                            + (lostR >= 0.05 ? "Right eye lost " + Math.round(lostR * 100) + "%" : "")
                            + ((lostL >= 0.05) !== (lostR >= 0.05) ? " (using other eye)" : "")
                    color: lostL >= 0.05 || lostR >= 0.05 ? Kirigami.Theme.neutralTextColor : Kirigami.Theme.textColor
                }
                Controls.Label {
                    visible: backend.gazeServiceRunning && gazeDiag.expanded
                    Kirigami.FormData.label: "Learned nudges:"
                    text: gpage.status.lessons + (gpage.status.lessons === 1 ? " correction" : " corrections")
                          + (gpage.status.refused > 0 ? " (" + gpage.status.refused + " large jumps ignored)" : "")
                }
            }

            footer: Ft.HelpFooter {
                text: "Start with Calibrate above if this is your first time. Each eye tracker keeps its own calibration. "
                      + "Dominant eye lets you favor your steadier eye (Auto balances both automatically based on recent clicks). "
                      + "Use Quick check anytime if aim feels slightly off after adjusting the headset."
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
            property var rate: backend.handsRate   // read once a change: it reads two files
            // The 60 fps switch turns on only where hand tracking runs; on, it can always be turned off.
            readonly property bool rateUsable: !!st.installed && backend.handsMode !== "off"
            readonly property var range: backend.handsMarginRange
            // What runs, and why it can't: asked on the host, while this page is open.
            Timer {
                running: true
                repeat: true
                interval: 5000
                triggeredOnStart: true
                onTriggered: backend.refreshHands()
            }

            ColumnLayout {
                spacing: Kirigami.Units.smallSpacing
                Kirigami.FormLayout {
                    Layout.fillWidth: true
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                        visible: !!hpage.st.problem
                        type: backend.handsMode === "off" ? Kirigami.MessageType.Information : Kirigami.MessageType.Warning
                        text: hpage.st.problem || ""
                    }
                    ColumnLayout {
                        Kirigami.FormData.label: "Hand tracking:"
                        Kirigami.FormData.labelAlignment: Qt.AlignTop
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
                    Controls.Switch {
                        Kirigami.FormData.label: "Real-hand passthrough:"
                        text: "Show your real hands in front of screens"
                        checked: backend.handsCutouts
                        onToggled: backend.setHandsCutouts(checked)
                    }
                    Ft.Hint { text: "Reveals your real hands via camera passthrough whenever they pass in front of a virtual screen." }
                    RowLayout {
                        Kirigami.FormData.label: "Passthrough margin:"
                        enabled: backend.handsCutouts
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

                    // 60 tracked frames a second (hands/handsrate.py): the cameras' no-dark mode, which stops
                    // the controllers, and ft-hands tracking every frame. The warning always shows.
                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Frame rate" }
                    Controls.Switch {
                        objectName: "handsRateSwitch"
                        Kirigami.FormData.label: "60 fps hand tracking:"
                        text: "Smoother tracking (disables VR controller tracking)"
                        checked: !!hpage.rate.on
                        enabled: !hpage.rate.busy && (!!hpage.rate.on || hpage.rateUsable)
                        onToggled: backend.setHandsRate(checked)
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                        visible: true
                        type: Kirigami.MessageType.Warning
                        text: hpage.rate.warning
                    }
                    Controls.Label {
                        objectName: "handsRateNote"
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                        visible: text !== ""
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        // What it needs, once the host has said what's installed (st is {} until then).
                        text: hpage.rate.switching ? "Restarting head tracking: about 15 seconds."
                              : hpage.rate.note || (hpage.rate.on || hpage.rateUsable || hpage.st.installed === undefined ? ""
                              : hpage.st.installed ? "It needs hand tracking: turn that on above first."
                              : "It needs hand tracking, which isn't installed.")
                    }

                    // Which hand models ft-hands runs (HANDS_MODEL): latest, a version, or stock MediaPipe.
                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Hand model" }
                    ModelPicker {
                        Kirigami.FormData.label: "Model:"
                        Kirigami.FormData.labelAlignment: Qt.AlignTop
                        kind: "hands"
                        view: backend.handModels
                        Component.onCompleted: backend.reloadModels()
                    }

                    // Three ways the hands work the desktop, each on its own (handsmode.METHODS).
                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "How hands control the desktop" }
                    Controls.Switch {
                        Kirigami.FormData.label: "Eye + pinch:"
                        text: "Pinch to click wherever you're looking"
                        checked: !!backend.handMethods.gaze_clicks
                        onToggled: backend.setHandMethod("gaze_clicks", checked)
                    }
                    Ft.Hint {
                        text: "When eye tracking is on, look at a button and pinch thumb + index to left-click, or thumb + "
                            + "middle finger to right-click. Hold a pinch to drag."
                    }
                    Controls.Switch {
                        Kirigami.FormData.label: "Hand pointer ray:"
                        text: "Point a raised hand to aim a laser (when eye tracking is off)"
                        checked: !!backend.handMethods.ray
                        onToggled: backend.setHandMethod("ray", checked)
                    }
                    Ft.Hint {
                        text: "Raise your hand to point a ray at distant screens — index pinch to click, middle pinch to "
                            + "right-click or scroll."
                    }
                    Controls.Switch {
                        Kirigami.FormData.label: "Direct touch:"
                        text: "Tap, grab, and resize nearby screens directly with your hands"
                        checked: !!backend.handMethods.touch
                        onToggled: backend.setHandMethod("touch", checked)
                    }
                    Ft.Hint {
                        text: "Tap any nearby screen with your index finger to click, or pinch a screen's grab bar, edge, "
                            + "or corner to move and resize it."
                    }
                    Controls.Label {
                        visible: backend.handsMode === "off"
                        text: "Turn on Hand tracking above to use these controls."
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                    }

                    // Every interaction on Frametop's screens, each on its own: HandInteractions.qml, below.
                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Gestures & fine-tuning" }
                }

                HandInteractions {
                    backend: root.backend
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                }
            }

            footer: Ft.HelpFooter {
                text: "Hand tracking uses extra headset CPU while active. Each control method above can be toggled "
                      + "independently and takes effect within a second. A hand holding or recently using a VR controller "
                      + "automatically pauses hand input."
            }
        }
    }

    // ---------------------------------------------------------------- Commands
    Component {
        id: commandsPage
        Kirigami.ScrollablePage {
            id: cpage
            title: "Custom commands"
            header: DriverWarning {}
            property string problem: ""
            property bool adding: false

            Connections {
                target: backend
                function onCommandAdded(saved, problem) {
                    cpage.adding = false
                    cpage.problem = problem
                    if (saved) {
                        commandName.text = ""
                        commandRun.text = ""
                    }
                }
            }

            Kirigami.FormLayout {
                Repeater {
                    model: backend.commands
                    delegate: RowLayout {
                        required property var modelData
                        Kirigami.FormData.label: modelData.name + ":"
                        Controls.Label {
                            text: modelData.run
                            font.family: "monospace"
                            elide: Text.ElideRight
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                        }
                        Controls.ToolButton {
                            icon.name: "media-playback-start"
                            display: Controls.AbstractButton.IconOnly
                            text: "Run now"
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: backend.runCommand(modelData.name)
                        }
                        Controls.ToolButton {
                            icon.name: "edit-delete"
                            display: Controls.AbstractButton.IconOnly
                            text: "Remove (and the buttons mapped to it)"
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: backend.removeCommand(modelData.name)
                        }
                    }
                }
                Controls.Label {
                    visible: backend.commands.length === 0
                    Kirigami.FormData.label: "Your commands:"
                    text: "none yet"
                    opacity: 0.7
                }
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "New command" }
                Controls.TextField {
                    id: commandName
                    Kirigami.FormData.label: "Name:"
                    placeholderText: "e.g. stop-framecorder"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 16
                    onTextChanged: cpage.problem = ""
                }
                Controls.TextField {
                    id: commandRun
                    Kirigami.FormData.label: "Runs:"
                    placeholderText: "e.g. killall framecorder"
                    font.family: "monospace"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 24
                    onAccepted: addButton.clicked()
                }
                Controls.Button {
                    id: addButton
                    text: "Add"
                    icon.name: "list-add"
                    enabled: !cpage.adding && commandName.text.trim() !== "" && commandRun.text.trim() !== ""
                    onClicked: {
                        cpage.problem = ""
                        cpage.adding = true
                        backend.startAddCommand(commandName.text, commandRun.text)
                    }
                }
                Controls.Label {
                    visible: cpage.problem !== ""
                    text: cpage.problem
                    color: Kirigami.Theme.negativeTextColor
                    wrapMode: Text.WordWrap
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                }
            }

            footer: Ft.HelpFooter {
                text: "Saved shell commands run in the background on your headset. Trigger any saved command from Mouse "
                      + "buttons, Controllers, Keyboard & shortcuts (choose \"Run command NAME\"), Frunner (Meta+Space), "
                      + "Voice dictation (say your wake word, then \"run NAME\"), or directly by name in a terminal "
                      + "(use ft-cmd log NAME to view output)."
            }
        }
    }

    // ---------------------------------------------------------------- Remote display keys
    // (RemoteKeysPage.qml, a file of its own)
    Component {
        id: remoteKeysPage
        RemoteKeysPage { backend: root.backend }
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
                    tooltip: "Adjusts Bluetooth Low Energy settings so paired wireless mice and keyboards reconnect reliably (asks for your password)"
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

            footer: Ft.HelpFooter {
                text: "Pair new wireless devices in Steam (Settings → Bluetooth). If a Bluetooth mouse or keyboard doesn't "
                      + "reconnect automatically after sleeping, click Apply Bluetooth fixes once above."
            }
        }
    }
}
