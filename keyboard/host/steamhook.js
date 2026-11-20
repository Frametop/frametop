// Frametop's Steam hook (keyboard/host/steamhook.py installs it): Steam's own text fields open
// Full Keyboard instead of Steam's keyboard, and what Full Keyboard types goes into them.
// Evaluated in Steam's SharedJSContext. It never reads a field's text.
//
// Each Steam window's VirtualKeyboardManager shows Steam's keyboard through
// SetVirtualKeyboardShownInternal(open). While ft-kbhost renews the lease and says Steam's fields
// may take the keyboard (poll(take)), a show for an editable field in one of Steam's own windows
// doesn't happen: the field is remembered as a session, and poll() reports it. Text goes in with
// execCommand, as typing would. Without renewal for leaseMs the hook takes itself out, and Steam's
// own keyboard works as before. The way in follows Full Keyboard's own native bridge
// (bridge/steam_bridge.js, TaiKeid), much reduced.
(function install(root, leaseMs) {
    "use strict";
    const NAME = "__frametopKeyboardV1";
    if (root[NAME]) root[NAME].dispose();
    const now = () => root.performance.now();
    const own = key => typeof key === "string" && (key === "valve.steam.gamepadui.main" ||
        key === "valve.steam.gamepadui.barpopup" || key.startsWith("valve.steam.gamepadui.appoverlay."));
    const managers = () => {
        const windows = root.SteamUIStore?.WindowStore?.SteamUIWindows;
        return Array.isArray(windows) ? windows.map(w => w?.VirtualKeyboardManager).filter(Boolean) : [];
    };
    // The fields execCommand edits; others (number, date, ...) keep Steam's keyboard.
    const editable = (e, d) => e?.isConnected && e.ownerDocument === d && !e.disabled && !e.readOnly &&
        (e.tagName === "TEXTAREA" || e.tagName === "INPUT" &&
            ["text", "search", "password", "tel", "url"].includes(e.type) || e.isContentEditable === true);
    let lease = now() + leaseMs, take = false, session = null, nextId = 1, disposed = false;
    const events = [], undo = [], hooked = new WeakSet();
    const fresh = () => !disposed && now() < lease;

    function hook(object, key, wrap) {
        const descriptor = Object.getOwnPropertyDescriptor(object, key), original = object[key];
        if (typeof original !== "function" || descriptor && !descriptor.configurable && !descriptor.writable)
            throw Error("steam-hook-readonly");
        const replacement = wrap(original);
        Object.defineProperty(object, key, {configurable: true, writable: true, value: replacement});
        undo.push(() => {
            if (object[key] !== replacement) return;
            if (descriptor) Object.defineProperty(object, key, descriptor);
            else delete object[key];
        });
    }
    function purpose(e) {
        if (e.tagName !== "INPUT") return "text";
        if (e.type === "password") return "password";
        if (e.type === "url") return "url";
        if (e.type === "search") return "search";
        if (e.type === "tel" || e.inputMode === "numeric" || e.inputMode === "decimal") return "number";
        if (e.inputMode === "email" || e.autocomplete === "email") return "email";
        return "text";
    }
    function end(reason) {
        if (!session) return;
        events.push({close: session.id, reason});
        session = null;
    }
    // Still typing into the same field: focused, attached, its window shown.
    function holding() {
        const s = session;
        if (!s) return false;
        const d = s.document;
        if (!d || d.defaultView?.closed || d.visibilityState === "hidden" || d.activeElement !== s.element ||
            !editable(s.element, d)) {
            end("blur");
            return false;
        }
        return true;
    }
    // A window whose overlay key isn't known yet is looked at again on the next poll.
    function adopt(manager) {
        if (hooked.has(manager)) return;
        const key = manager.m_Instance?.GetVROverlayKey?.();
        if (!own(key) || typeof manager.SetVirtualKeyboardShownInternal !== "function") return;
        hooked.add(manager);
        hook(manager, "SetVirtualKeyboardShownInternal", original => function(open, ...args) {
            try {
                if (open && take && fresh()) {
                    const d = manager.m_ownerWindow?.document, e = d?.activeElement;
                    if (editable(e, d) && typeof d.execCommand === "function") {
                        if (session?.element !== e) {
                            end("moved");
                            session = {id: nextId++, manager, document: d, element: e};
                            events.push({open: session.id, purpose: purpose(e), key,
                                multiline: e.tagName === "TEXTAREA" || e.isContentEditable === true});
                        }
                        return undefined;  // Full Keyboard, not Steam's
                    }
                }
                if (!open && session?.manager === manager) end("steam");
            } catch (_) { /* Steam's own path below */ }
            return original.call(this, open, ...args);
        });
    }
    function rescan() {
        for (const manager of managers()) {
            try { adopt(manager); } catch (_) { /* that window keeps Steam's keyboard */ }
        }
    }
    // With the dashboard closed, a Steam overlay keeps SteamVR's input focus, and its gamepad
    // navigation would take DOM focus from the field (Full Keyboard's bridge found this): skip
    // those steps for a navigation tree that holds our field.
    const holds = tree => !!session && tree?.Root?.Element?.contains?.(session.element) === true;
    const gamepad = root.vrGamepadInput;
    for (const name of ["FocusItemUnderMouse", "TakeFocusIfNothingIsFocused"]) {
        if (typeof gamepad?.[name] !== "function" || !Object.isExtensible(gamepad)) continue;
        try {
            hook(gamepad, name, original => function(tree, ...args) {
                if (fresh() && holds(tree)) return undefined;
                return original.call(this, tree, ...args);
            });
        } catch (_) { /* focus may then move, as it does for Steam's keyboard */ }
    }
    const navTree = gamepad?.m_NullNavTree && Object.getPrototypeOf(gamepad.m_NullNavTree);
    if (typeof navTree?.TakeFocus === "function") {
        try {
            hook(navTree, "TakeFocus", original => function(...args) {
                if (fresh() && holds(this) && session.document.activeElement === session.element) return false;
                return original.apply(this, args);
            });
        } catch (_) { /* as above */ }
    }

    function press(name, modifiers) {
        const s = session, d = s.document, e = s.element;
        const shift = modifiers.includes("Shift"), ctrl = modifiers.includes("Ctrl");
        const codes = {Backspace: 8, Tab: 9, Enter: 13, Escape: 27, End: 35, Home: 36, ArrowLeft: 37,
            ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Delete: 46, KeyA: 65, KeyC: 67, KeyV: 86, KeyX: 88};
        if (!(name in codes)) return true;  // nothing a field does with it (F keys, Emoji, ...)
        const keyName = name.startsWith("Key") ? name.slice(3).toLowerCase() : name;
        const event = type => new d.defaultView.KeyboardEvent(type, {key: keyName, code: name,
            keyCode: codes[name], which: codes[name], shiftKey: shift, ctrlKey: ctrl, bubbles: true, cancelable: true});
        if (!e.dispatchEvent(event("keydown"))) { e.dispatchEvent(event("keyup")); return true; }
        try {
            const sel = d.getSelection();
            const how = shift ? "extend" : "move";
            if (ctrl && name === "KeyA") d.execCommand("selectAll", false, null);
            else if (ctrl && name === "KeyC") d.execCommand("copy", false, null);
            else if (ctrl && name === "KeyX") d.execCommand("cut", false, null);
            else if (ctrl && name === "KeyV") d.execCommand("paste", false, null);
            else if (name === "Backspace") {
                if (ctrl && sel?.isCollapsed) sel.modify("extend", "backward", "word");
                d.execCommand("delete", false, null);
            } else if (name === "Delete") {
                if (ctrl && sel?.isCollapsed) sel.modify("extend", "forward", "word");
                d.execCommand("forwardDelete", false, null);
            } else if (name === "ArrowLeft" || name === "ArrowRight")
                sel?.modify(how, name === "ArrowLeft" ? "backward" : "forward", ctrl ? "word" : "character");
            else if (name === "ArrowUp" || name === "ArrowDown")
                sel?.modify(how, name === "ArrowUp" ? "backward" : "forward", "line");
            else if (name === "Home" || name === "End")
                sel?.modify(how, name === "Home" ? "backward" : "forward", "lineboundary");
            else if (name === "Enter") {
                if (e.tagName === "TEXTAREA") d.execCommand("insertText", false, "\n");
                else if (e.isContentEditable === true) d.execCommand(shift ? "insertLineBreak" : "insertParagraph", false, null);
                else e.form?.requestSubmit?.();
            } else if (name === "Tab") {
                const items = [...d.querySelectorAll("input,textarea,button,a[href],[tabindex]")]
                    .filter(i => !i.disabled && i.tabIndex >= 0 && i.getClientRects().length);
                const at = items.indexOf(e);
                items[(at + (shift ? items.length - 1 : 1)) % items.length]?.focus();
            } else if (name === "Escape") end("escape");
            return true;
        } finally { e.dispatchEvent(event("keyup")); }
    }
    function dispose() {
        if (disposed) return;
        disposed = true;
        session = null;
        root.clearInterval(timer);
        for (const step of undo.reverse()) { try { step(); } catch (_) { /* gone with its window */ } }
        if (root[NAME]?.dispose === dispose) delete root[NAME];
    }
    const timer = root.setInterval(() => { if (!fresh()) dispose(); }, 250);
    rescan();
    root[NAME] = {
        dispose,
        // Renews the lease, says whether fields may take the keyboard, and returns what
        // happened since the last poll: {events: [{open, purpose, key, multiline} | {close, reason}],
        // session}.
        poll(mayTake) {
            if (disposed) return null;
            lease = now() + leaseMs;
            take = mayTake === true;
            rescan();
            holding();
            return {events: events.splice(0), session: session?.id ?? null};
        },
        type(id, text) {
            if (!fresh() || session?.id !== id || !holding() || typeof text !== "string") return false;
            return session.document.execCommand("insertText", false, text) === true;
        },
        key(id, name, modifiers) {
            if (!fresh() || session?.id !== id || !holding() || typeof name !== "string") return false;
            return press(name, Array.isArray(modifiers) ? modifiers : []);
        },
        // Full Keyboard closed: the field keeps its focus, and a new show for it opens it again.
        close(id) { if (session?.id === id) session = null; return true; },
    };
    return true;
})
