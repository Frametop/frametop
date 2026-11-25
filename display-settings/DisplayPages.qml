// Frametop Settings' display pages: Screens, Desktopless mode, Layout, Visibility & pins, Grab bars & corners,
// Steam & notifications and Power (settings/main.qml loads this file the first time one of them opens).
// Desktopless mode, Grab bars & corners and Steam & notifications are files of their own beside this one.
// Backend: ft_display_settings.py ("backend", made then: settings/ft_settings.py).
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "../settings/controls" as Ft

Item {
    id: root
    // settings/main.qml sets these two.
    property QtObject backend
    property var window
    function show(name) { window.showSection(name) }
    function showPassiveNotification(text, duration) { window.showPassiveNotification(text, duration) }

    // The page for a section of the app, by name; null for a name that isn't one of these.
    // Visibility & pins, Grab bars & corners and Steam & notifications are ft-screens' (the gamescope
    // backend has none; settings/sections.py SCREENS_ONLY).
    function page(name) {
        const screensOnly = ["visibility", "handles", "steam"]
        if (screensOnly.includes(name) && backend.backend !== "screens") return screensPage
        return ({ screens: screensPage, desktopless: desktoplessPage, layout: layoutPage, visibility: visibilityPage,
                  handles: handlesPage, steam: steamPage, power: powerPage })[name] || null
    }

    // "1 hour", "15 minutes", "30 seconds".
    function duration(seconds) {
        const unit = (n, word) => n + " " + word + (n === 1 ? "" : "s")
        if (seconds >= 3600 && seconds % 3600 === 0) return unit(seconds / 3600, "hour")
        if (seconds >= 60 && seconds % 60 === 0) return unit(seconds / 60, "minute")
        return unit(seconds, "second")
    }

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    Kirigami.PromptDialog {
        id: restartDialog
        title: "Restart the desktop?"
        subtitle: "All open desktop windows (including Frametop Settings) will close and restart with your updated settings."
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: "Restart"
                icon.name: "view-refresh"
                onTriggered: { restartDialog.close(); backend.restartDesktop() }
            },
            Kirigami.Action {
                text: "Cancel"
                icon.name: "dialog-cancel"
                onTriggered: restartDialog.close()
            }
        ]
    }

    // Save the arrangement under a name, or rename a saved layout.
    Kirigami.PromptDialog {
        id: nameDialog
        property string mode: "save"  // save | rename
        property string oldName: ""
        readonly property var names: backend.layoutNames
        readonly property string name: nameField.text.trim().split(/\s+/).join(" ")
        readonly property bool taken: name !== oldName && names.indexOf(name) >= 0
        readonly property bool ok: name !== "" && !(mode === "rename" && taken)
        title: mode === "save" ? "Save profile" : "Rename " + oldName
        standardButtons: Kirigami.Dialog.NoButton

        function openFor(m, text) {
            mode = m
            oldName = m === "rename" ? text : ""
            nameField.text = text
            open()
            nameField.forceActiveFocus()
            nameField.selectAll()
        }
        function accept() {
            if (!ok) return
            close()
            if (mode === "save") backend.saveLayout(name)
            else if (name !== oldName) backend.renameLayout(oldName, name)
        }

        ColumnLayout {
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: nameDialog.mode === "save"
                      ? "Save your current screen positions, sizes, curves, pins, and open apps under this name:"
                      : "New name:"
            }
            Controls.TextField {
                id: nameField
                Layout.fillWidth: true
                maximumLength: 40
                onAccepted: nameDialog.accept()
            }
            Controls.Label {
                visible: nameDialog.taken
                opacity: 0.7
                text: nameDialog.mode === "save" ? "This will overwrite the existing profile with that name."
                                                 : "A profile with that name already exists."
            }
        }
        customFooterActions: [
            Kirigami.Action {
                text: nameDialog.mode === "save" ? "Save" : "Rename"
                icon.name: nameDialog.mode === "save" ? "document-save" : "edit-rename"
                enabled: nameDialog.ok
                onTriggered: nameDialog.accept()
            },
            Kirigami.Action {
                text: "Cancel"
                icon.name: "dialog-cancel"
                onTriggered: nameDialog.close()
            }
        ]
    }

    Kirigami.PromptDialog {
        id: deleteDialog
        property string name: ""
        title: "Delete " + name + "?"
        subtitle: "Your current screens stay where they are — only the saved profile is removed."
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: "Delete"
                icon.name: "edit-delete"
                onTriggered: { deleteDialog.close(); backend.deleteLayout(deleteDialog.name) }
            },
            Kirigami.Action {
                text: "Cancel"
                icon.name: "dialog-cancel"
                onTriggered: deleteDialog.close()
            }
        ]
    }

    // ---------------------------------------------------------------- Screens
    Component {
        id: screensPage
        Kirigami.ScrollablePage {
            id: spage
            title: "Screens"
            property bool md: backend.backend === "screens"

            actions: [
                Kirigami.Action {
                    visible: spage.md
                    text: "Add screen"
                    icon.name: "list-add"
                    onTriggered: backend.addScreen()
                },
                Kirigami.Action {
                    visible: spage.md && backend.desktopRunning
                    text: "Hide/show screens"
                    icon.name: "view-visible"
                    onTriggered: backend.toggleScreens()
                },
                Kirigami.Action {
                    visible: spage.md
                    text: "Remote displays"
                    icon.name: "network-workgroup"
                    tooltip: "Stream screens from your PC or Mac"
                    onTriggered: root.show("remote-displays")
                },
                Kirigami.Action {
                    visible: backend.desktopRunning
                    text: "Restart desktop"
                    icon.name: "view-refresh"
                    tooltip: "Closes all desktop windows (including Settings) and restarts the desktop"
                    onTriggered: restartDialog.open()
                }
            ]

            header: Kirigami.InlineMessage {
                position: Kirigami.InlineMessage.Position.Header
                visible: backend.restartNeeded || !backend.desktopRunning
                type: Kirigami.MessageType.Information
                text: backend.restartNeeded
                      ? (spage.md ? "Screens were added or removed. Restart the desktop to apply (restarting closes open desktop windows and Settings)."
                                  : "Screen count, resolution, or panel width changed. Restart the desktop to apply (restarting closes open desktop windows and Settings).")
                      : "The desktop isn't running yet. Your changes will apply the next time it starts."
                actions: [
                    Kirigami.Action {
                        visible: backend.restartNeeded
                        text: "Restart desktop"
                        icon.name: "system-reboot"
                        onTriggered: backend.restartDesktop()
                    }
                ]
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                // Desktopless mode (docs/desktopless.md): the desktops hide, their apps stay.
                Kirigami.AbstractCard {
                    visible: spage.md
                    Layout.fillWidth: true
                    contentItem: RowLayout {
                        ColumnLayout {
                            Layout.fillWidth: true
                            Kirigami.Heading { level: 3; text: "Desktopless mode" }
                            Controls.Label {
                                Layout.fillWidth: true
                                text: "Float each open app as its own VR panel and hide virtual screens. Shortcut: Meta+Shift+D."
                                opacity: 0.7
                                wrapMode: Text.Wrap
                            }
                        }
                        Controls.Button {
                            text: "Configure…"
                            icon.name: "preferences-system-windows"
                            onClicked: root.show("desktopless")
                        }
                        Controls.Switch {
                            id: desktoplessSwitch
                            checked: backend.desktopless
                            enabled: backend.desktopRunning && !backend.busy
                            onToggled: {
                                backend.setDesktopless(checked)
                                checked = Qt.binding(() => backend.desktopless)
                            }
                        }
                    }
                }

                // ft-screens: every screen its own resolution and size.
                Repeater {
                    model: spage.md ? backend.screenList : []
                    delegate: Kirigami.AbstractCard {
                        id: card
                        required property var modelData
                        Layout.fillWidth: true
                        property int customIndex: backend.screenResolutions.length
                        contentItem: ColumnLayout {
                            RowLayout {
                                Kirigami.Heading { level: 3; text: "Screen " + (card.modelData.index + 1) }
                                Controls.Label {
                                    text: card.modelData.width + " × " + card.modelData.height
                                          + (card.modelData.scale !== 1 ? " (effective " + card.modelData.effective + ")" : "")
                                    opacity: 0.7
                                }
                                Item { Layout.fillWidth: true }
                                Controls.RadioButton {
                                    text: "Taskbar here"
                                    checked: card.modelData.primary
                                    onToggled: if (checked) backend.setPrimary(card.modelData.index)
                                }
                                Controls.ToolButton {
                                    icon.name: "edit-delete-remove"
                                    enabled: backend.screenList.length > 1
                                    display: Controls.AbstractButton.IconOnly
                                    text: "Remove this screen"
                                    Controls.ToolTip.text: text
                                    Controls.ToolTip.visible: hovered
                                    onClicked: backend.removeScreen(card.modelData.index)
                                }
                            }
                            Kirigami.FormLayout {
                                Layout.fillWidth: true
                                RowLayout {
                                    Kirigami.FormData.label: "Resolution:"
                                    Controls.ComboBox {
                                        id: res
                                        model: backend.screenResolutions.concat([{ text: "Custom…", width: 0, height: 0 }])
                                        textRole: "text"
                                        Component.onCompleted: {
                                            const i = backend.screenResolutions.findIndex(r => r.width === card.modelData.width && r.height === card.modelData.height)
                                            currentIndex = i >= 0 ? i : card.customIndex
                                        }
                                        onActivated: {
                                            const r = model[currentIndex]
                                            if (r.width > 0) backend.setScreenSize(card.modelData.index, r.width, r.height)
                                        }
                                    }
                                    Controls.SpinBox {
                                        id: cw
                                        visible: res.currentIndex === card.customIndex
                                        from: 320; to: 16384; stepSize: 8; editable: true
                                        value: card.modelData.width
                                    }
                                    Controls.Label { visible: cw.visible; text: "×" }
                                    Controls.SpinBox {
                                        id: ch
                                        visible: cw.visible
                                        from: 200; to: 16384; stepSize: 8; editable: true
                                        value: card.modelData.height
                                    }
                                    Controls.Button {
                                        visible: cw.visible
                                        text: "Set"
                                        onClicked: backend.setScreenSize(card.modelData.index, cw.value, ch.value)
                                    }
                                }
                                RowLayout {
                                    Kirigami.FormData.label: "Width in VR:"
                                    Controls.Slider {
                                        id: metres
                                        from: 0.5; to: 6.0; stepSize: 0.05
                                        value: card.modelData.metres
                                        Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                        onMoved: backend.setScreenMetres(card.modelData.index, value)
                                    }
                                    Controls.Label {
                                        text: metres.value.toFixed(2) + " m wide, "
                                              + (metres.value * card.modelData.height / card.modelData.width).toFixed(2) + " m tall"
                                    }
                                }
                                Controls.ComboBox {
                                    Kirigami.FormData.label: "Text & UI scale:"
                                    model: backend.scales
                                    textRole: "text"
                                    valueRole: "value"
                                    Component.onCompleted: currentIndex = Math.max(0, indexOfValue(card.modelData.scale))
                                    onActivated: backend.setScale(card.modelData.index, currentValue)
                                }
                                Controls.Switch {
                                    Kirigami.FormData.label: "Curve:"
                                    text: "Curve screen around you"
                                    checked: card.modelData.curved
                                    onToggled: backend.setCurved(card.modelData.index, checked)
                                }
                            }
                        }
                    }
                }

                // gamescope: one shared resolution.
                Kirigami.FormLayout {
                    visible: !spage.md
                    Layout.fillWidth: true

                    Controls.SpinBox {
                        Kirigami.FormData.label: "Screens:"
                        from: 1
                        to: 6
                        value: backend.screens
                        onValueModified: backend.setScreens(value)
                    }

                    RowLayout {
                        Kirigami.FormData.label: "Resolution:"
                        Controls.ComboBox {
                            id: resBox
                            property bool custom: currentIndex === count - 1
                            model: backend.resolutions.concat([{ text: "Custom…", width: 0, height: 0 }])
                            textRole: "text"
                            Component.onCompleted: {
                                const i = backend.resolutions.findIndex(r => r.width === backend.width && r.height === backend.height)
                                currentIndex = i >= 0 ? i : count - 1
                            }
                            onActivated: {
                                const r = model[currentIndex]
                                if (r.width > 0) backend.setResolution(r.width, r.height)
                            }
                        }
                        Controls.SpinBox {
                            id: customW
                            visible: resBox.custom
                            from: 640; to: 7680; stepSize: 8
                            editable: true
                            value: backend.width
                        }
                        Controls.Label { visible: resBox.custom; text: "×" }
                        Controls.SpinBox {
                            id: customH
                            visible: resBox.custom
                            from: 360; to: 4320; stepSize: 8
                            editable: true
                            value: backend.height
                        }
                        Controls.Button {
                            visible: resBox.custom
                            text: "Set"
                            onClicked: backend.setResolution(customW.value, customH.value)
                        }
                    }

                    RowLayout {
                        Kirigami.FormData.label: "Panel width:"
                        Controls.Slider {
                            id: physSlider
                            from: 0.8; to: 3.0; stepSize: 0.05
                            value: backend.physWidth
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            onMoved: backend.setPhysWidth(value)
                        }
                        Controls.Label { text: physSlider.value.toFixed(2) + " m" }
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Each screen" }

                    Repeater {
                        model: spage.md ? [] : backend.screenList
                        delegate: RowLayout {
                            required property var modelData
                            Kirigami.FormData.label: "Screen " + (modelData.index + 1) + ":"
                            Controls.ComboBox {
                                model: backend.rotations
                                textRole: "text"
                                valueRole: "value"
                                Component.onCompleted: currentIndex = Math.max(0, indexOfValue(modelData.rotation))
                                onActivated: backend.setRotation(modelData.index, currentValue)
                            }
                            Controls.ComboBox {
                                model: backend.scales
                                textRole: "text"
                                valueRole: "value"
                                Component.onCompleted: currentIndex = Math.max(0, indexOfValue(modelData.scale))
                                onActivated: backend.setScale(modelData.index, currentValue)
                            }
                            Controls.Label {
                                text: "looks like " + modelData.effective
                                opacity: 0.7
                            }
                        }
                    }

                    // DEE-317: Plasma's wallpaper becomes a key colour and ft-screens puts a picture
                    // per eye there (stereo_wallpaper.py). Docked screens only.
                    Kirigami.Separator {
                        visible: spage.md
                        Kirigami.FormData.isSection: true
                        Kirigami.FormData.label: "3D wallpaper"
                    }
                    Controls.Switch {
                        id: stereoSwitch
                        visible: spage.md
                        Kirigami.FormData.label: "3D depth wallpaper:"
                        text: checked ? "On" : "Off"
                        checked: backend.stereoWallpaper
                        enabled: backend.desktopRunning && backend.busy === "" && (checked || backend.stereoWallpaperReady)
                        onToggled: {
                            backend.setStereoWallpaper(checked)
                            checked = Qt.binding(() => backend.stereoWallpaper)
                        }
                    }
                    Ft.Hint {
                        visible: spage.md
                        text: "Displays a different image to each eye on docked screens for a stereoscopic 3D background. Choose an image below before turning this on."
                    }
                    RowLayout {
                        id: picsRow
                        visible: spage.md
                        Kirigami.FormData.label: "Wallpaper image:"
                        property string leftUrl: ""
                        Controls.Button {
                            text: "Side-by-side 3D image…"
                            enabled: backend.busy === ""
                            onClicked: sbsDialog.open()
                        }
                        Controls.Button {
                            text: "Separate left & right…"
                            enabled: backend.busy === ""
                            onClicked: leftDialog.open()
                        }
                        Controls.Label {
                            text: backend.stereoWallpaperReady ? "Ready" : "None chosen"
                            opacity: 0.7
                        }
                        FileDialog {
                            id: sbsDialog
                            title: "A side-by-side picture: the left eye's half on the left"
                            nameFilters: ["Pictures (*.png *.jpg *.jpeg *.webp)"]
                            onAccepted: backend.chooseStereoWallpaper(selectedFile.toString(), "")
                        }
                        FileDialog {
                            id: leftDialog
                            title: "The left eye's picture"
                            nameFilters: ["Pictures (*.png *.jpg *.jpeg *.webp)"]
                            onAccepted: { picsRow.leftUrl = selectedFile.toString(); rightDialog.open() }
                        }
                        FileDialog {
                            id: rightDialog
                            title: "The right eye's picture"
                            nameFilters: ["Pictures (*.png *.jpg *.jpeg *.webp)"]
                            onAccepted: backend.chooseStereoWallpaper(picsRow.leftUrl, selectedFile.toString())
                        }
                    }
                }
            }

            footer: Ft.HelpFooter {
                text: spage.md
                      ? "Each virtual screen can have its own resolution (pick a tall resolution for portrait). Resolution, width, "
                        + "and curve apply immediately. In VR, drag the bar underneath a screen to move it, or drag a corner to resize it. "
                        + "Save your setup anytime in Layout & profiles."
                      : "In gamescope mode, all screens share one resolution (up to 1920 × 1080 pixels each). Rotation and scale apply "
                        + "immediately; screen count and resolution apply after restarting the desktop."
            }
        }
    }

    // ---------------------------------------------------------------- Layout
    Component {
        id: layoutPage
        Kirigami.ScrollablePage {
            id: lpage
            title: "Layout & profiles"
            property var layout: backend.layout
            property var preset: layout.preset || {}
            property bool hasCustom: (layout.screens || []).some(s => s.pos !== undefined)
            // Named layouts: the arrangement is one of them (named) when it came from it, and
            // hasn't been placed by hand and saved without a name since.
            property var names: backend.layoutNames
            property bool fromNamed: names.indexOf(layout.active) >= 0
            property bool named: layout.mode === "custom" && fromNamed
            property bool unnamed: names.length === 0 || ((hasCustom || layout.mode === "custom") && !fromNamed)
            property var choices: [{ text: "Curved around you", value: "arc" }, { text: "Flat wall", value: "flat" }]
                .concat(names.map(n => ({ text: n, value: "layout:" + n })))
                .concat(unnamed ? [{ text: names.length ? "Unnamed arrangement" : "Saved arrangement", value: "custom" }] : [])

            actions: [
                Kirigami.Action {
                    text: lpage.named ? "Open profile" : "Arrange now"
                    icon.name: "view-restore"
                    tooltip: lpage.named ? "Put the screens in this profile's places, around where you're facing, and open its apps (windows already open move; nothing closes)"
                                         : "Float the screens out of the dashboard and put them in this layout, around where you're facing"
                    enabled: backend.desktopRunning && backend.busy === ""
                    onTriggered: backend.arrange()
                },
                Kirigami.Action {
                    text: "Save as profile…"
                    icon.name: "document-save"
                    tooltip: "Save where the screens are now, which ones are hidden, and the open apps and where their windows are, under a name"
                    enabled: backend.desktopRunning && backend.busy === ""
                    onTriggered: nameDialog.openFor("save", lpage.named ? lpage.layout.active
                                                                        : "Layout " + (lpage.names.length + 1))
                }
            ]

            header: Kirigami.InlineMessage {
                position: Kirigami.InlineMessage.Position.Header
                visible: backend.busy !== ""
                type: Kirigami.MessageType.Information
                text: backend.busy + "… (your pointer will pause for a few seconds)"
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    RowLayout {
                        Kirigami.FormData.label: "Arrangement:"
                        Controls.ComboBox {
                            model: lpage.choices
                            textRole: "text"
                            valueRole: "value"
                            currentIndex: lpage.layout.mode !== "custom" ? (lpage.preset.kind === "flat" ? 1 : 0)
                                        : lpage.named ? 2 + lpage.names.indexOf(lpage.layout.active)
                                        : lpage.choices.length - 1
                            onActivated: {
                                if (currentValue === "custom") backend.setMode("custom")
                                else if (currentValue.startsWith("layout:")) backend.useLayout(currentValue.slice(7))
                                else backend.setPreset("kind", currentValue)
                            }
                        }
                        Controls.ToolButton {
                            visible: lpage.named
                            icon.name: "edit-rename"
                            text: "Rename…"
                            display: Controls.AbstractButton.IconOnly
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: nameDialog.openFor("rename", lpage.layout.active)
                        }
                        Controls.ToolButton {
                            visible: lpage.named
                            icon.name: "edit-delete"
                            text: "Delete…"
                            display: Controls.AbstractButton.IconOnly
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: { deleteDialog.name = lpage.layout.active; deleteDialog.open() }
                        }
                    }

                    Controls.Label {
                        visible: lpage.layout.mode === "custom"
                        Kirigami.FormData.label: ""
                        text: lpage.named ? "Restores the saved screen positions and opens the profile's apps. Click Open profile to apply it, or Save as profile to update it."
                              : lpage.hasCustom ? "Uses your last saved screen positions. Click Save as profile to give it a name, or choose a preset."
                              : "No profile saved yet. Arrange your screens in VR, open the apps you want, then click Save as profile."
                        opacity: 0.7
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                    }

                    // The profile's apps (docs/profiles.md): each window and where it goes.
                    ColumnLayout {
                        id: profileApps
                        visible: lpage.named
                        Kirigami.FormData.label: "Apps:"
                        property var windows: lpage.named ? backend.profileWindows(lpage.layout.active) : []
                        property var hidden: lpage.named ? backend.profileHidden(lpage.layout.active) : []
                        property var remotes: lpage.named ? backend.profileRemotes(lpage.layout.active) : []
                        Connections {
                            target: backend
                            function onChanged() {
                                profileApps.windows = lpage.named ? backend.profileWindows(lpage.layout.active) : []
                                profileApps.hidden = lpage.named ? backend.profileHidden(lpage.layout.active) : []
                                profileApps.remotes = lpage.named ? backend.profileRemotes(lpage.layout.active) : []
                            }
                        }
                        Controls.Label {
                            visible: profileApps.windows.length === 0
                            text: "No apps saved in this profile yet. Open your apps, arrange their windows, then click Save as profile."
                            opacity: 0.7
                            wrapMode: Text.Wrap
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                        }
                        Repeater {
                            model: profileApps.windows
                            delegate: RowLayout {
                                required property var modelData
                                Controls.Label { text: modelData.app + " (" + modelData.where + ")" }
                                Controls.ToolButton {
                                    icon.name: "list-remove"
                                    text: "Leave out"
                                    display: Controls.AbstractButton.IconOnly
                                    Controls.ToolTip.text: "Leave this window out of the profile"
                                    Controls.ToolTip.visible: hovered
                                    onClicked: backend.removeProfileWindow(lpage.layout.active, modelData.index)
                                }
                            }
                        }
                        Controls.Label {
                            visible: profileApps.remotes.length > 0
                            text: "Remote display" + (profileApps.remotes.length > 1 ? "s: " : ": ") + profileApps.remotes.join(", ")
                            wrapMode: Text.Wrap
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                        }
                        Controls.Label {
                            visible: profileApps.hidden.length > 0
                            text: "Hides screen" + (profileApps.hidden.length > 1 ? "s " : " ") + profileApps.hidden.join(", ")
                            opacity: 0.7
                        }
                    }

                    Controls.SpinBox {
                        Kirigami.FormData.label: "Rows:"
                        visible: lpage.layout.mode !== "custom"
                        from: 1
                        to: Math.max(1, backend.screenCount)
                        value: lpage.preset.rows || 1
                        onValueModified: backend.setPreset("rows", value)
                    }

                    Repeater {
                        model: [
                            { key: "distance", label: "Distance", from: 0.6, to: 3.0, step: 0.05, unit: "m", def: 1.2 },
                            { key: "gap", label: "Gap", from: 0.0, to: 0.3, step: 0.01, unit: "m", def: 0.04 },
                            { key: "height", label: "Height", from: -0.8, to: 0.8, step: 0.05, unit: "m", def: 0.0 }
                        ]
                        delegate: RowLayout {
                            required property var modelData
                            visible: lpage.layout.mode !== "custom"
                            Kirigami.FormData.label: modelData.label + ":"
                            Controls.Slider {
                                id: s
                                from: modelData.from; to: modelData.to; stepSize: modelData.step
                                value: lpage.preset[modelData.key] !== undefined ? lpage.preset[modelData.key] : modelData.def
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                onMoved: backend.setPreset(modelData.key, value)
                            }
                            Controls.Label {
                                text: (modelData.key === "height" && s.value > 0 ? "+" : "") + s.value.toFixed(2) + " " + modelData.unit
                                      + (modelData.key === "height" ? " (from eye level)" : "")
                            }
                        }
                    }

                    Controls.Switch {
                        Kirigami.FormData.label: "When the desktop starts:"
                        text: "Float the screens and arrange them"
                        checked: lpage.layout.auto !== false
                        enabled: backend.defaultProfile === ""
                        onToggled: backend.setAuto(checked)
                    }
                    Controls.ComboBox {
                        Kirigami.FormData.label: "Start in profile:"
                        model: [{ text: "None", value: "" }].concat(lpage.names.map(n => ({ text: n, value: n })))
                        textRole: "text"
                        valueRole: "value"
                        // From the names, not indexOfValue: that ran before the model was set, so a saved
                        // default showed as None.
                        currentIndex: backend.defaultProfile === "" ? 0 : 1 + lpage.names.indexOf(backend.defaultProfile)
                        onActivated: backend.setDefaultProfile(currentValue)
                        Controls.ToolTip.text: "The desktop starts in this profile: its screens, and its apps open. Each profile also has its own entry in SteamVR's Launch a program list"
                        Controls.ToolTip.visible: hovered
                    }
                }

                // Preview (backend.preview): while the desktop runs, every screen, floating window and
                // remote display where it is now, and you, in the layout's frame (where you stood
                // when the screens were last arranged); otherwise the layout itself, you where it's
                // arranged from. From above, and as seen from your eyes.
                Kirigami.Heading { level: 3; text: "Preview" }
                Canvas {
                    id: preview
                    Layout.fillWidth: true
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 17
                    property var view: backend.preview
                    onViewChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    Component.onCompleted: backend.watchRoom(true)
                    Component.onDestruction: backend.watchRoom(false)

                    function colorOf(kind) {
                        return kind === "screen" ? Kirigami.Theme.highlightColor
                             : kind === "remote" ? Kirigami.Theme.positiveTextColor
                             : Kirigami.Theme.neutralTextColor  // a floating window
                    }
                    // A planned screen away from where that screen is now (more than 5 cm at an end
                    // or the middle, seen from above).
                    function moved(g, panels) {
                        const p = panels.find(q => q.kind === "screen" && q.label === g.label)
                        if (!p) return true
                        const at = (t, f) => t[Math.round((t.length - 1) * f)]
                        return [0, 0.5, 1].some(f => Math.hypot(at(p.top, f)[0] - at(g.top, f)[0],
                                                                at(p.top, f)[1] - at(g.top, f)[1]) > 0.05)
                    }
                    // t cut to fit w pixels ("" when not even a letter fits), "Frametop " first.
                    function fit(ctx, t, w) {
                        if (ctx.measureText(t).width <= w) return t
                        if (t.startsWith("Frametop ")) return fit(ctx, t.slice(9), w)
                        let n = t.length
                        while (n > 1 && ctx.measureText(t.slice(0, n - 1) + "…").width > w) n--
                        return n > 1 ? t.slice(0, n - 1) + "…" : ""
                    }
                    // Labels, centred on (x, y) inside box [x0, y0, x1, y1]: a screen's where
                    // another screen's already is joins it ("1·3": screens stacked in rows); one
                    // that would cover another tries the spots along (dx, dy), then stays out.
                    function label(ctx, labels, box, p, maxW, x, y, dx, dy) {
                        const screen = p.kind === "screen"
                        const near = screen && labels.find(l => l.screen && Math.abs(l.x - x) < 14 && Math.abs(l.y - y) < 10)
                        if (near) { near.t += "·" + p.label; return }
                        let t = p.label
                        t = fit(ctx, t, Math.min(maxW, box[2] - box[0] - 4))
                        if (!t) return
                        const w = ctx.measureText(t).width + 4, lh = Kirigami.Theme.smallFont.pixelSize + 3
                        for (let i = 0; i < 7; i++) {
                            const cx = Math.max(box[0] + w / 2, Math.min(box[2] - w / 2, x + dx * i))
                            const cy = Math.max(box[1] + lh / 2, Math.min(box[3] - lh / 2, y + dy * i))
                            if (!labels.some(l => Math.abs(l.x - cx) < (l.w + w) / 2 && Math.abs(l.y - cy) < lh)) {
                                labels.push({ x: cx, y: cy, w: w, t: t, screen: screen })
                                return
                            }
                        }
                    }
                    function drawLabels(ctx, labels) {
                        ctx.textBaseline = "middle"
                        for (const l of labels.filter(l => l.t)) {
                            const w = ctx.measureText(l.t).width
                            ctx.fillStyle = Qt.rgba(0, 0, 0, 0.35)  // legible over a panel's fill
                            ctx.fillRect(l.x - w / 2 - 2, l.y - Kirigami.Theme.smallFont.pixelSize / 2 - 1, w + 4, Kirigami.Theme.smallFont.pixelSize + 2)
                            ctx.fillStyle = Kirigami.Theme.textColor
                            ctx.fillText(l.t, l.x - w / 2, l.y)
                        }
                        ctx.textBaseline = "alphabetic"
                    }
                    function line(ctx, pts) {
                        ctx.beginPath()
                        pts.forEach((q, i) => i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]))
                    }

                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        const text = Kirigami.Theme.textColor
                        const half = width / 2, top = 24, legend = 20, boxH = height - top - legend - 8
                        ctx.font = Kirigami.Theme.smallFont.pixelSize + "px sans-serif"
                        ctx.fillStyle = text
                        ctx.fillText("From above", 4, 12)
                        ctx.fillText("From where you are", half + 8, 12)
                        if (!view || !view.planned) return
                        const live = view.live, head = view.head, you = head.tracked
                        const shown = live ? view.panels : view.planned
                        const ghosts = live ? view.planned.filter(g => moved(g, view.panels)) : []
                        // Remote displays at the back, floating windows in front.
                        const order = { remote: 0, screen: 1, window: 2 }
                        const panels = shown.slice().sort((a, b) => order[a.kind] - order[b.kind])

                        // From above: x right, forward up.
                        const xs = [head.x], zs = [head.z]
                        for (const p of panels.concat(ghosts)) for (const q of p.top) { xs.push(q[0]); zs.push(q[1]) }
                        const xmin = Math.min(...xs), xmax = Math.max(...xs), zmin = Math.min(...zs), zmax = Math.max(...zs)
                        const k = Math.max(1, Math.min((half - 48) / Math.max(xmax - xmin, 1), (boxH - 44) / Math.max(zmax - zmin, 1)))
                        const cx = 4 + half / 2 - (xmax + xmin) / 2 * k
                        const cz = top + 8 + boxH / 2 - (zmax + zmin) / 2 * k
                        const P = q => [cx + q[0] * k, cz + q[1] * k]
                        ctx.save()
                        ctx.beginPath(); ctx.rect(0, top, half - 2, boxH + 8); ctx.clip()
                        ctx.lineCap = "round"
                        ctx.setLineDash([4, 3])
                        ctx.lineWidth = 1.5
                        for (const g of ghosts) {
                            ctx.strokeStyle = colorOf(g.kind)
                            line(ctx, g.top.map(P)); ctx.stroke()
                        }
                        ctx.setLineDash([])
                        for (const p of panels) {
                            ctx.globalAlpha = p.hidden ? 0.35 : 1
                            ctx.strokeStyle = colorOf(p.kind)
                            ctx.lineWidth = p.kind === "window" ? 3 : 4
                            line(ctx, p.top.map(P)); ctx.stroke()
                        }
                        ctx.globalAlpha = 1
                        // You: a cone the way you face (about what the Frame shows), and an arrow.
                        const h = P([head.x, head.z]), yaw = head.yaw * Math.PI / 180
                        const ray = (a, len) => [h[0] - Math.sin(a) * len, h[1] - Math.cos(a) * len]
                        const fov = 50 * Math.PI / 180, reach = 44
                        ctx.fillStyle = Qt.rgba(text.r, text.g, text.b, 0.18)
                        ctx.beginPath(); ctx.moveTo(h[0], h[1])
                        ctx.arc(h[0], h[1], reach, -Math.PI / 2 - yaw - fov, -Math.PI / 2 - yaw + fov)
                        ctx.closePath(); ctx.fill()
                        ctx.strokeStyle = text; ctx.fillStyle = text
                        ctx.lineWidth = 2
                        line(ctx, [h, ray(yaw, 24)]); ctx.stroke()
                        line(ctx, [ray(yaw, 30), ray(yaw + 0.35, 20), ray(yaw - 0.35, 20)]); ctx.closePath(); ctx.fill()
                        ctx.beginPath(); ctx.arc(h[0], h[1], 4.5, 0, 2 * Math.PI); ctx.fill()
                        // Labels past each panel's middle, away from you, and off you (blank ones
                        // only hold your spot).
                        let labels = [h, ray(yaw, 30)].map(q => ({ x: q[0], y: q[1], w: 40, t: "" }))
                        const topBox = [0, top, half - 2, top + boxH + 8]
                        for (const p of panels) {
                            const m = P(p.top[Math.floor(p.top.length / 2)])
                            const d = Math.hypot(m[0] - h[0], m[1] - h[1]) || 1
                            const ux = (m[0] - h[0]) / d, uy = (m[1] - h[1]) / d
                            label(ctx, labels, topBox, p, half * 0.45, m[0] + ux * 12, m[1] + uy * 12, ux * 14, uy * 14)
                        }
                        drawLabels(ctx, labels)
                        ctx.restore()

                        // From where you are: across by direction (left on the left), up by height,
                        // in degrees from your eyes, so what's behind you shows at the sides.
                        const ys = [head.yaw], ps = [0]
                        for (const p of panels.concat(ghosts)) for (const q of p.view) { ys.push(q[0]); ps.push(q[1]) }
                        const ymin = Math.min(...ys), ymax = Math.max(...ys), pmin = Math.min(...ps), pmax = Math.max(...ps)
                        const fk = Math.max(0.1, Math.min((half - 24) / Math.max(ymax - ymin, 60), (boxH - 16) / Math.max(pmax - pmin, 40)))
                        const ox = half + 8 + (half - 16) / 2 + (ymax + ymin) / 2 * fk
                        const oy = top + 8 + boxH / 2 + (pmax + pmin) / 2 * fk
                        const V = q => [ox - q[0] * fk, oy - q[1] * fk]
                        ctx.save()
                        ctx.beginPath(); ctx.rect(half + 2, top - 6, half - 2, boxH + 14); ctx.clip()
                        ctx.strokeStyle = Kirigami.Theme.disabledTextColor
                        ctx.lineWidth = 1
                        ctx.setLineDash([4, 4])
                        ctx.beginPath(); ctx.moveTo(half + 8, oy); ctx.lineTo(width - 4, oy); ctx.stroke()  // eye level
                        const fx = ox - head.yaw * fk
                        ctx.strokeStyle = text
                        ctx.beginPath(); ctx.moveTo(fx, top + 14); ctx.lineTo(fx, top + boxH + 4); ctx.stroke()  // the way you face
                        // Your left, right and behind, where the view reaches them.
                        const guides = []
                        ctx.strokeStyle = Kirigami.Theme.disabledTextColor
                        for (const [a, t] of [[90, "left"], [-90, "right"], [180, "behind"], [-180, "behind"]]) {
                            const gx = fx - a * fk
                            if (gx < half + 8 || gx > width - 4) continue
                            ctx.beginPath(); ctx.moveTo(gx, top + 14); ctx.lineTo(gx, top + boxH + 4); ctx.stroke()
                            guides.push([gx, t])
                        }
                        ctx.setLineDash([4, 3])
                        ctx.lineWidth = 1.5
                        for (const g of ghosts) {
                            ctx.strokeStyle = colorOf(g.kind)
                            line(ctx, g.view.map(V)); ctx.closePath(); ctx.stroke()
                        }
                        ctx.setLineDash([])
                        labels = []
                        const viewBox = [half + 4, top + 14, width - 2, top + boxH + 8]
                        // Far ones first, so a near one covers what it's in front of; near ones
                        // label first, so theirs win a crowded spot.
                        const far = panels.slice().sort((a, b) => b.distance - a.distance)
                        const spots = []
                        for (const p of far) {
                            const c = colorOf(p.kind)
                            ctx.globalAlpha = p.hidden ? 0.35 : 1
                            const pts = p.view.map(V)
                            line(ctx, pts); ctx.closePath()
                            ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, 0.3)
                            ctx.fill()
                            ctx.strokeStyle = c
                            ctx.lineWidth = 2
                            ctx.stroke()
                            const px = pts.map(q => q[0]), py = pts.map(q => q[1])
                            const x0 = Math.min(...px), x1 = Math.max(...px), y1 = Math.max(...py)
                            spots.unshift([p, (x0 + x1) / 2, (Math.min(...py) + y1) / 2, x1 - x0, y1])
                        }
                        ctx.globalAlpha = 1
                        // Inside the panel when it's wide enough, else under it.
                        for (const [p, x, y, w, bottom] of spots) {
                            if (w >= 40) label(ctx, labels, viewBox, p, w - 6, x, y, 0, 13)
                            else label(ctx, labels, viewBox, p, half * 0.3, x, bottom + 9, 0, 13)
                        }
                        drawLabels(ctx, labels)
                        ctx.fillStyle = text
                        const facing = you ? "you face" : "ahead"
                        const fw = ctx.measureText(facing).width
                        ctx.fillText(facing, Math.max(half + 6, Math.min(width - 4 - fw, fx - fw / 2)), top + 10)
                        ctx.fillStyle = Kirigami.Theme.disabledTextColor
                        for (const [gx, t] of guides) {
                            const tw = ctx.measureText(t).width
                            ctx.fillText(t, Math.max(half + 6, Math.min(width - 4 - tw, gx - tw / 2)), top + 10)
                        }
                        ctx.restore()

                        // Legend.
                        let x = 4
                        const y = height - 6
                        const key = (draw, t) => {
                            draw(x, y - 4)
                            ctx.fillStyle = text
                            ctx.fillText(t, x + 16, y)
                            x += 16 + ctx.measureText(t).width + 14
                        }
                        const swatch = c => (x0, y0) => { ctx.fillStyle = c; ctx.fillRect(x0, y0 - 4, 12, 8) }
                        key(swatch(colorOf("screen")), "Screens")
                        if (panels.some(p => p.kind === "window")) key(swatch(colorOf("window")), "Windows")
                        if (panels.some(p => p.kind === "remote")) key(swatch(colorOf("remote")), "Remote displays")
                        key((x0, y0) => { ctx.fillStyle = text; ctx.beginPath(); ctx.arc(x0 + 6, y0, 4, 0, 2 * Math.PI); ctx.fill() },
                            you ? "You" : "You, when it's arranged")
                        if (ghosts.length) key((x0, y0) => {
                            ctx.strokeStyle = colorOf("screen"); ctx.lineWidth = 1.5; ctx.setLineDash([4, 3])
                            ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x0 + 12, y0); ctx.stroke(); ctx.setLineDash([])
                        }, "Where the layout puts them")
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "The diagram shows your current screens (dashed lines show where this layout would move them). "
                          + "Layouts always center around whichever direction you're facing when applied. You can move screens "
                          + "by hand anytime using their grab bars, snap them back with Arrange now above, and bring them "
                          + "all round to where you're looking, as they are, with Meta+Shift+R or a shortcut in Mouse buttons."
                }
            }
        }
    }

    // ---------------------------------------------------------------- Desktopless mode
    // (its own file: DesktoplessPage.qml)
    Component {
        id: desktoplessPage
        DesktoplessPage { backend: root.backend; onOpenSection: name => root.show(name) }
    }

    // ---------------------------------------------------------------- Visibility
    Component {
        id: visibilityPage
        Kirigami.ScrollablePage {
            id: vpage
            title: "Visibility & pins"
            property var v: backend.visibility

            actions: [
                Kirigami.Action {
                    text: "Hide/show now"
                    icon.name: "view-visible"
                    enabled: backend.desktopRunning
                    onTriggered: backend.toggleScreens()
                }
            ]

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "When the screens show" }

                    Ft.RadioChoices {
                        choices: [
                            { value: "always", text: "Always visible", help: "Press Meta+Shift+H (or a mapped button) to hide or show screens anytime. See During VR games below." },
                            { value: "dashboard", text: "Only when the SteamVR dashboard is open", help: "Screens appear and hide together with the SteamVR dashboard. Meta+Shift+H still toggles them anytime." },
                            { value: "gesture", text: "When I glance at my wrist", help: "Screens appear while you look toward your chosen controller. Meta+Shift+H still toggles them anytime." },
                            { value: "toggle", text: "Hidden until I toggle them", help: "Screens stay hidden until you press Meta+Shift+H or a mapped button." }
                        ]
                        value: vpage.v.mode
                        onChose: backend.setVisibility("mode", picked)
                    }

                    RowLayout {
                        Kirigami.FormData.label: "Wrist:"
                        visible: vpage.v.mode === "gesture"
                        Controls.ComboBox {
                            model: [{ text: "Left controller", value: "left" }, { text: "Right controller", value: "right" }]
                            textRole: "text"
                            valueRole: "value"
                            Component.onCompleted: currentIndex = indexOfValue(vpage.v.gesture_hand)
                            onActivated: backend.setVisibility("gesture_hand", currentValue)
                        }
                    }
                    RowLayout {
                        Kirigami.FormData.label: "Look within:"
                        visible: vpage.v.mode === "gesture"
                        Controls.Slider {
                            id: gesture
                            from: 5; to: 60; stepSize: 1
                            value: vpage.v.gesture_angle
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            onMoved: backend.setVisibility("gesture_angle", value)
                        }
                        Controls.Label { text: Math.round(gesture.value) + "° of it" }
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "During VR games" }

                    Ft.RadioChoices {
                        choices: [
                            { value: "hide", text: "Hide screens during VR games", help: "Keeps your game view clear; open the SteamVR dashboard (or press Meta+Shift+H) to view screens." },
                            { value: "visible", text: "Keep screens visible over VR games", help: "Keeps your screens floating in front of VR games. Note: also turn off automatic pausing in VR games settings so the desktop stays active." }
                        ]
                        value: vpage.v.in_games || "hide"
                        onChose: backend.setVisibility("in_games", picked)
                    }
                    Controls.Button {
                        visible: vpage.v.mode === "always" && (vpage.v.in_games || "hide") === "visible"
                        text: "Open VR games settings"
                        icon.name: "applications-games"
                        onClicked: root.show("games")
                    }
                    Controls.Label {
                        visible: vpage.v.mode !== "always"
                        text: "Screens in the room only follow this when they are set to Always visible above; dashboard tiles always do."
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Controller laser interaction" }

                    Ft.RadioChoices {
                        choices: [
                            { value: "outside_games", text: "Outside VR games only (recommended)", help: "During a VR game, your controllers stay dedicated to the game even if screens are visible. Use your 3D mouse or open the SteamVR dashboard to interact with screens." },
                            { value: "always", text: "Always", help: "Controller lasers can click screens whenever screens are visible, even over a VR game (which prevents the game from using the controllers while aiming at a screen)." },
                            { value: "dashboard", text: "Only when the SteamVR dashboard is open", help: "Outside the dashboard, only your 3D mouse interacts with screens. Helpful when playing flatscreen games." }
                        ]
                        value: vpage.v.controllers || "outside_games"
                        onChose: backend.setVisibility("controllers", picked)
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Screens shown" }

                    // Counts as models, here and in Pinned screens: a new list rebuilds the rows, and
                    // FormLayout puts rebuilt rows after the ones below (a report: pinning a screen
                    // moved the wrist slider and All screens up between Screen 2 and Screen 3).
                    Repeater {
                        model: backend.screensShown.length
                        delegate: Controls.Switch {
                            id: shownSwitch
                            required property int index
                            Kirigami.FormData.label: "Screen " + (index + 1) + ":"
                            text: checked ? "Shown" : "Hidden"
                            checked: backend.screensShown[index]
                            onToggled: {
                                backend.setScreenShown(shownSwitch.index, checked)
                                checked = Qt.binding(() => backend.screensShown[shownSwitch.index])
                            }
                        }
                    }
                    Controls.Label {
                        text: "Hidden screens stay off even when you press Meta+Shift+H. Existing windows remain on them, and new apps launched there will float into view instead."
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Pinned screens" }

                    Repeater {
                        model: backend.pins.length
                        delegate: Controls.ComboBox {
                            id: pinBox
                            required property int index
                            readonly property int pinIndex: Math.max(0, ["none", "left", "right", "head", "dashboard"].indexOf(backend.pins[index]))
                            Kirigami.FormData.label: "Screen " + (index + 1) + ":"
                            model: [
                                { text: "In the room", value: "none" },
                                { text: "On the left wrist", value: "left" },
                                { text: "On the right wrist", value: "right" },
                                { text: "On your head", value: "head" },
                                { text: "On the dashboard", value: "dashboard" }
                            ]
                            textRole: "text"
                            valueRole: "value"
                            currentIndex: pinIndex
                            // pinBox.index: activated's own argument is also called index (the
                            // chosen row), so a bare index sent "On your head" to screen 4.
                            onActivated: {
                                backend.pin(String(pinBox.index + 1), currentValue)
                                currentIndex = Qt.binding(() => pinBox.pinIndex)  // what ft-screens has, if the pin failed
                            }
                        }
                    }
                    Controls.ComboBox {
                        id: pinModeBox
                        Kirigami.FormData.label: "Head pins:"
                        // SCREEN_PIN_LEVEL: 0, 1, 2 (the row is the value)
                        model: [
                            "Locked to view (HUD)",
                            "Follow walking & turning, stay level",
                            "Follow walking only (look around freely)"
                        ]
                        currentIndex: backend.pinMode
                        onActivated: {
                            backend.setPinMode(currentIndex)
                            currentIndex = Qt.binding(() => backend.pinMode)
                        }
                    }
                    Controls.Label {
                        text: ["Moves directly with your headset every time you look or tilt your head, like a heads-up display.",
                               "Turns and walks with you while staying level — looking up/down, tilting, or crouching keeps it steady.",
                               "Follows you as you walk around the room, but stays facing the same direction so you can turn your head to look at side screens."][Math.max(0, Math.min(2, backend.pinMode))]
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    }
                    RowLayout {
                        Kirigami.FormData.label: "Wrist screens show within:"
                        Controls.Slider {
                            id: wrist
                            from: 20; to: 120; stepSize: 1
                            value: vpage.v.wrist_angle
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            onMoved: backend.setVisibility("wrist_angle", value)
                        }
                        Controls.Label { text: Math.round(wrist.value) + "°" }
                    }
                    Flow {
                        Kirigami.FormData.label: "Pin all screens to:"
                        Layout.fillWidth: true
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                        spacing: Kirigami.Units.smallSpacing
                        Controls.Button {
                            text: "Left wrist"
                            enabled: backend.desktopRunning
                            onClicked: backend.pin("all", "left")
                        }
                        Controls.Button {
                            text: "Right wrist"
                            enabled: backend.desktopRunning
                            onClicked: backend.pin("all", "right")
                        }
                        Controls.Button {
                            text: "Head"
                            enabled: backend.desktopRunning
                            onClicked: backend.pin("all", "head")
                        }
                        Controls.Button {
                            text: "Dashboard"
                            enabled: backend.desktopRunning
                            onClicked: backend.pin("all", "dashboard")
                        }
                        Controls.Button {
                            text: "Unpin all"
                            enabled: backend.desktopRunning
                            onClicked: backend.pin("all", "none")
                        }
                    }

                    // Dashboard tiles (dash_tiles.py; screens/tiling.h): ft-screens takes each live.
                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Dashboard tiles" }
                    Ft.Hint {
                        text: "Anchor screens, apps, or remote displays to the SteamVR dashboard so they snap neatly into a curved workspace."
                    }
                    Controls.ComboBox {
                        id: newWindowsBox
                        Kirigami.FormData.label: "New windows open:"
                        // NEW_WINDOWS_TILED: 1, 0 (row 0 is tiled)
                        model: ["Tiled on the dashboard", "Floating in the room"]
                        currentIndex: backend.dashTiles.newWindowsTiled ? 0 : 1
                        onActivated: {
                            backend.dashTiles.setNewWindowsTiled(currentIndex === 0)
                            currentIndex = Qt.binding(() => backend.dashTiles.newWindowsTiled ? 0 : 1)
                        }
                    }
                    Ft.Hint { text: "For apps that open floating. A window you float off the desktop yourself stays in the room. An app you tiled comes back tiled, and one you put In the Room stays there, whatever this says. Apps opened from Steam's dashboard always tile." }
                    Controls.Switch {
                        Kirigami.FormData.label: "Keep open:"
                        text: "Keep anchored panels visible after closing the dashboard"
                        checked: backend.dashTiles.stay
                        onToggled: backend.dashTiles.setStay(checked)
                    }
                    Ft.Hint { text: "Long-press a panel's anchor button to toggle this individually for one panel." }
                    Controls.Switch {
                        Kirigami.FormData.label: "In Theater mode:"
                        text: "Keep visible over flatscreen games in SteamVR Theater"
                        enabled: backend.dashTiles.stay
                        checked: backend.dashTiles.stayTheater
                        onToggled: backend.dashTiles.setStayTheater(checked)
                    }
                    Controls.Switch {
                        Kirigami.FormData.label: "Look to focus:"
                        text: "Move the tile you're looking at with Meta+arrows"
                        checked: backend.dashTiles.lookFocus
                        onToggled: backend.dashTiles.setLookFocus(checked)
                    }
                    Ft.Hint { text: "Applies when neither a mouse nor a controller laser is pointing at a panel." }
                    Controls.Switch {
                        Kirigami.FormData.label: "Dashboard apps:"
                        text: "Open flatscreen dashboard apps as Frametop panels"
                        checked: backend.dashTiles.gsApps
                        onToggled: backend.dashTiles.setGsApps(checked)
                    }
                    Controls.Switch {
                        Kirigami.FormData.label: "Steam windows:"
                        text: "Show Steam's store and menu as Frametop panels"
                        checked: backend.dashTiles.steamWindows
                        onToggled: backend.dashTiles.setSteamWindows(checked)
                    }
                    Controls.Switch {
                        Kirigami.FormData.label: "Align with Steam bar:"
                        text: "Place the center tile right above Steam's dashboard bar"
                        checked: backend.dashTiles.atBar
                        onToggled: backend.dashTiles.setAtBar(checked)
                    }
                    Ft.Hint { text: "Matches the Steam bar's position and distance. Turn off to set a custom height and distance below." }

                    Ft.Disclosure {
                        id: tileLayout
                        title: "Tile size, position & curve"
                        summary: "panel size, desk width, distance, curvature"
                    }
                    RowLayout {
                        visible: tileLayout.expanded
                        Kirigami.FormData.label: "Vertical position:"
                        enabled: !backend.dashTiles.atBar
                        Controls.Slider {
                            id: tileDesk
                            from: -40; to: 20; stepSize: 1
                            value: backend.dashTiles.deskDeg
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            onMoved: backend.dashTiles.setDeskDeg(value)
                        }
                        Controls.Label {
                            text: Math.abs(Math.round(tileDesk.value)) + "° " + (tileDesk.value < 0 ? "below" : "above") + " eye level"
                            visible: !backend.dashTiles.atBar   // (not where they are then)
                        }
                    }
                    Ft.Hint { visible: tileLayout.expanded && backend.dashTiles.atBar; text: "Locked to Steam's bar. Turn off Align with Steam bar above to adjust." }
                    RowLayout {
                        visible: tileLayout.expanded
                        Kirigami.FormData.label: "Panel scale:"
                        Controls.Slider {
                            id: tileDensity
                            // (left smaller, right bigger: fewer pixels a degree)
                            from: -120; to: -15; stepSize: 1
                            value: -backend.dashTiles.pxPerDeg
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            onMoved: backend.dashTiles.setPxPerDeg(-value)
                        }
                        Controls.Label { text: Math.round(-tileDensity.value) + " px/°" }
                    }
                    Ft.Hint { visible: tileLayout.expanded; text: "Drag right for larger panels and text, or left to fit more on screen." }
                    RowLayout {
                        visible: tileLayout.expanded
                        Kirigami.FormData.label: "Workspace width:"
                        Controls.Slider {
                            id: tileWall
                            from: 30; to: 300; stepSize: 5
                            value: backend.dashTiles.wallDeg
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            onMoved: backend.dashTiles.setWallDeg(value)
                        }
                        Controls.Label { text: Math.round(tileWall.value) + "° wide" }
                    }
                    Ft.Hint { visible: tileLayout.expanded; text: "Once tiles fill this horizontal span, new tiles stack into additional rows." }
                    RowLayout {
                        visible: tileLayout.expanded
                        Kirigami.FormData.label: "Distance:"
                        enabled: !backend.dashTiles.atBar
                        Controls.Slider {
                            id: tileRadius
                            from: 0.8; to: 6; stepSize: 0.05
                            value: backend.dashTiles.radius
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            onMoved: backend.dashTiles.setRadius(value)
                        }
                        Controls.Label {
                            text: tileRadius.value.toFixed(2) + " m"
                            visible: !backend.dashTiles.atBar
                        }
                    }
                    Ft.Hint { visible: tileLayout.expanded && backend.dashTiles.atBar; text: "Locked to Steam's bar. Turn off Align with Steam bar above to adjust." }
                    RowLayout {
                        visible: tileLayout.expanded
                        Kirigami.FormData.label: centreCurveOwn.checked ? "Side curve:" : "Curve:"
                        // the arc's radius, its centre to the wall (DASH_TILE_CURVE_M; not set: 2 m past the
                        // wall's distance). At the distance or less it's round your head. (At Steam's bar
                        // the wall is about 1.4 m away, 0.3 m past the bar; its real distance is ft-screens'.)
                        property real wallM: backend.dashTiles.atBar ? 1.4 : backend.dashTiles.radius
                        Controls.Slider {
                            id: tileCurve
                            from: 0.8; to: 12; stepSize: 0.1
                            value: backend.dashTiles.curveRadius > 0 ? backend.dashTiles.curveRadius : parent.wallM + 2
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            onMoved: backend.dashTiles.setCurveRadius(value)
                        }
                        Controls.Label {
                            text: tileCurve.value <= parent.wallM + 0.05 && !centreCurveOwn.checked
                                  ? "Full wrap (faces you)"
                                  : tileCurve.value.toFixed(1) + " m radius"
                        }
                    }
                    RowLayout {
                        visible: tileLayout.expanded
                        // the centre display's column on an arc of its own (DASH_TILE_CENTRE_CURVE_M; off: 0,
                        // Curve's all round). Turned on, it starts where Curve is.
                        Kirigami.FormData.label: "Center curve:"
                        property real wallM: backend.dashTiles.atBar ? 1.4 : backend.dashTiles.radius
                        Controls.Switch {
                            id: centreCurveOwn
                            text: "Custom"
                            checked: backend.dashTiles.centreCurveRadius > 0
                            onToggled: backend.dashTiles.setCentreCurveRadius(checked ? tileCurve.value : 0)
                        }
                        Controls.Slider {
                            id: centreCurve
                            from: 0.8; to: 12; stepSize: 0.1
                            visible: centreCurveOwn.checked
                            value: backend.dashTiles.centreCurveRadius > 0 ? backend.dashTiles.centreCurveRadius : tileCurve.value
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                            onMoved: backend.dashTiles.setCentreCurveRadius(value)
                        }
                        Controls.Label {
                            text: centreCurve.value <= parent.wallM + 0.05 ? "Faces you"
                                  : centreCurve.value.toFixed(1) + " m"
                            visible: centreCurveOwn.checked
                        }
                    }
                    Ft.Hint {
                        visible: tileLayout.expanded
                        text: centreCurveOwn.checked
                              ? "Sets a separate curve for the center column while side panels use Side curve above."
                              : "Turn on to give the center column a flatter or tighter curve than the side panels."
                    }
                    RowLayout {
                        visible: tileLayout.expanded
                        // the rows over and under the center display's (DASH_TILE_HEIGHT_CURVE_M; off: 0, as the
                        // center's curve). Turned on, it starts where that is.
                        Kirigami.FormData.label: "Vertical tilt:"
                        Controls.Switch {
                            id: heightCurveOwn
                            text: "Custom"
                            checked: backend.dashTiles.heightCurveRadius > 0
                            onToggled: backend.dashTiles.setHeightCurveRadius(
                                           !checked ? 0 : centreCurveOwn.checked ? centreCurve.value : tileCurve.value)
                        }
                        Controls.Slider {
                            id: heightCurve
                            from: 0.5; to: 12; stepSize: 0.1
                            visible: heightCurveOwn.checked
                            value: backend.dashTiles.heightCurveRadius > 0 ? backend.dashTiles.heightCurveRadius : 2.5
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                            onMoved: backend.dashTiles.setHeightCurveRadius(value)
                        }
                        Controls.Label {
                            text: heightCurve.value.toFixed(1) + " m"
                            visible: heightCurveOwn.checked
                        }
                    }
                    Ft.Hint {
                        visible: tileLayout.expanded
                        text: heightCurveOwn.checked
                              ? "Smaller values angle top and bottom rows more steeply toward you."
                              : "Turn on to angle stacked top and bottom rows separately from the horizontal curve."
                    }
                }

                Ft.Disclosure {
                    id: pinHelp
                    title: "How pinning and anchoring work"
                    summary: "wrist, head, dashboard"
                }
                Controls.Label {
                    visible: pinHelp.expanded
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "Pin to a wrist: Grab a screen by its bar and sweep your controller laser across your other "
                          + "controller's ring until the bar turns blue. Position the screen where you want it and release — "
                          + "it will stay attached to that wrist and appear whenever you turn it toward you. Sweep across "
                          + "the ring again to unpin.\n\n"
                          + "Pin to your head: Choose On your head above to keep a screen moving with you (use Head pins "
                          + "above to choose whether it locks to your view or stays level). Position the screen first, then "
                          + "pin it. Saved profiles remember your pins.\n\n"
                          + "Anchor to the dashboard: Choose On the dashboard above or click the anchor button underneath any "
                          + "screen, floating app, or remote display. Anchored panels arrange themselves into an arc around "
                          + "the SteamVR dashboard. Drag a tile to reorder it, or use Meta+arrows (and Meta+Ctrl+arrows to "
                          + "resize) on focused app tiles. Press Meta+/ anytime to see all shortcuts."
                }

            }
        }
    }

    // ---------------------------------------------------------------- Grab bars & corners, Steam & notifications
    // (their own files: HandlesPage.qml, SteamPage.qml)
    Component {
        id: handlesPage
        HandlesPage { backend: root.backend }
    }
    Component {
        id: steamPage
        SteamPage { backend: root.backend; onRestartAsked: restartDialog.open() }
    }

    // ---------------------------------------------------------------- Power
    Component {
        id: powerPage
        Kirigami.ScrollablePage {
            id: ppage
            title: "Power & startup"
            property var p: backend.power
            // The timeout choices, plus a value set by hand in frametop.conf.
            property var offChoices: {
                const list = [{ text: "Never", value: 0 }].concat([1, 2, 5, 10, 15, 30, 60].map(
                    m => ({ text: root.duration(m * 60), value: m })))
                if (!list.some(c => c.value === p.offMinutes))
                    list.push({ text: root.duration(Math.round(p.offMinutes * 60)), value: p.offMinutes })
                return list
            }

            Component.onCompleted: backend.refreshPower()

            actions: [
                Kirigami.Action {
                    readonly property bool dark: ppage.p.state === "off" || ppage.p.state === "standby"
                    text: dark ? "Turn displays on" : "Turn displays off now"
                    icon.name: dark ? "video-display" : "system-suspend"
                    tooltip: dark ? "Wakes the headset, like a key or button on it would"
                             : ppage.p.standby ? "Test screen sleep now — press any key, mouse or headset button to wake"
                             : "Test screen sleep now — move the headset or press any key to wake"
                    enabled: ppage.p.service && ppage.p.state !== "away"
                    onTriggered: dark ? backend.displaysOnNow() : backend.displaysOffNow()
                }
            ]

            header: Kirigami.InlineMessage {
                position: Kirigami.InlineMessage.Position.Header
                visible: !ppage.p.service
                type: Kirigami.MessageType.Warning
                text: "Automatic display sleep isn't running yet. To enable it, re-run ./install.sh (or power/run.sh install) in a terminal."
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Headset display sleep" }

                    Controls.ComboBox {
                        Kirigami.FormData.label: "Turn displays off after:"
                        model: ppage.offChoices
                        textRole: "text"
                        valueRole: "value"
                        Component.onCompleted: currentIndex = Math.max(0, indexOfValue(ppage.p.offMinutes))
                        onActivated: backend.setDisplayOffMinutes(currentValue)
                    }
                    Controls.Label {
                        text: "Turns off the headset screens when there's no movement or input — helpful if you set the headset "
                              + "on a stand that triggers its face sensor. "
                              + (ppage.p.standby ? "" : "Move the headset or press any button to wake it immediately. ")
                              + "Taking the headset off normally still turns the displays off within seconds."
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    }
                    Controls.Switch {
                        Kirigami.FormData.label: "With the displays off:"
                        text: "Pause tracking too"
                        checked: ppage.p.standby
                        onToggled: {
                            backend.setDisplayOffStandby(checked)
                            checked = Qt.binding(() => ppage.p.standby)
                        }
                    }
                    Controls.Label {
                        text: ppage.p.standby
                              ? "A few seconds after the displays, the headset goes into SteamVR's standby, as if "
                                + "you had taken it off: tracking, the cameras and drawing pause, and it uses about a "
                                + "third of the power. The desktop, services and remote access keep running. Any key, "
                                + "mouse or headset button wakes it, and so does putting it on once its proximity "
                                + "sensor notices. Moving it doesn't, because tracking is paused. Steam can put a "
                                + "headset in standby to sleep: turn on Stay awake below to keep it reachable."
                              : "Only the displays turn off. The headset keeps tracking and drawing, so it wakes the "
                                + "moment it moves, but it still uses most of its power."
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    }
                    Controls.Label {
                        Kirigami.FormData.label: "Now:"
                        visible: ppage.p.service
                        text: ppage.p.state === "standby" ? "Off, with tracking paused. Use any key, mouse or headset "
                                                            + "button, or put the headset on, to wake it."
                              : ppage.p.state === "off" ? (ppage.p.standby
                                    ? "Off. Tracking still runs: move the headset or use any input to turn them on."
                                    : "Off. Move the headset or use any input to turn them on.")
                              : ppage.p.state === "away" ? "Off. SteamVR turned them off because the headset isn't being worn."
                              : ppage.p.offMinutes > 0
                                ? "On, unused for " + (ppage.p.unused < 60 ? Math.floor(ppage.p.unused) + " s"
                                    : Math.floor(ppage.p.unused / 60) + " min " + Math.floor(ppage.p.unused % 60) + " s")
                                : "On"
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Startup" }

                    Controls.Switch {
                        Kirigami.FormData.label: "Frametop desktop:"
                        text: "Start with SteamVR"
                        checked: backend.autostart
                        enabled: backend.autostartInstalled || backend.autostart
                        onToggled: {
                            backend.setAutostart(checked)
                            checked = Qt.binding(() => backend.autostart)
                        }
                    }
                    Controls.Label {
                        text: backend.autostartInstalled
                              ? "Automatically launches your Frametop desktop whenever the headset boots or SteamVR restarts. "
                                + "If you manually close the desktop, it stays closed until SteamVR starts again."
                              : "Auto-start isn't installed yet. Re-run ./install.sh (or desktops.sh install) in a terminal to enable it."
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Headset sleep" }

                    Controls.Switch {
                        id: awake
                        Kirigami.FormData.label: "While plugged in:"
                        text: "Stay awake"
                        checked: ppage.p.acSleep === 0
                        enabled: ppage.p.steam && !ppage.p.steamBusy
                        onToggled: {
                            backend.setStayAwake(checked)
                            checked = Qt.binding(() => ppage.p.acSleep === 0)  // follow what Steam has
                        }
                    }
                    Controls.Label {
                        text: !ppage.p.steam
                              ? (ppage.p.steamBusy ? "Checking Steam power setting…" : "Couldn't reach Steam: " + ppage.p.steamError)
                              : "Keeps the headset awake while charging so remote access, downloads, and background apps stay connected "
                                + "(mirrors Steam Settings → Power → When Plugged In and Idle). The power button still sleeps the headset anytime. "
                                + (ppage.p.acSleep > 0 ? "Currently set to sleep after " + root.duration(ppage.p.acSleep) + " while charging. " : "")
                                + "On battery, Steam's battery sleep timer still applies ("
                                + (ppage.p.batterySleep > 0 ? "sleeps after " + root.duration(ppage.p.batterySleep) : "never sleeps")
                                + ")."
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: ppage.p.standby
                          ? "If SteamVR doesn't go into standby (Now stays at Off without tracking paused), only the "
                            + "displays are off and the headset keeps tracking. Leave it on a charger that keeps up "
                            + "with it in use."
                          : "Tip: When only the displays are off, the headset stays active so it wakes instantly. It still draws "
                            + "significant power, so keep it plugged into a strong charger if leaving it idle."
                }
            }
        }
    }
}
