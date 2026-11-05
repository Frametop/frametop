// Evaluated only in Steam's SharedJSContext. Never read field or clipboard text.
// Native presentation is closed; input stays bound to the owning Steam window.
(function installNativeBridge(root, testStore, externalSupported) {
    "use strict";
    const VERSION = 1, LEASE_MS = 900, REQUEST_MS = 400, IDLE_INSPECT_MS = 100;
    const now = () => root.performance.now();
    let store = testStore;
    if (!store) {
        let require;
        const chunks = root.webpackChunksteamui;
        if (!chunks || typeof chunks.push !== "function") throw Error("steam-ui-unavailable");
        const id = Symbol("framekeyboard");
        chunks.push([[id], {}, r => { require = r; }]);
        const index = chunks.findIndex(c => c[0]?.[0] === id);
        if (index >= 0) chunks.splice(index, 1);
        for (const key of Object.keys(require?.m || {})) {
            if (!String(require.m[key]).includes("get VRKeyboardStatus()")) continue;
            for (const value of Object.values(require(key))) {
                if (value && typeof value === "object" && "VRKeyboardStatus" in value &&
                    value.m_VRKeyboardState) { store = value; break; }
            }
            if (store) break;
        }
    }
    const popup = root.g_PopupManager?.GetPopupForVROverlayKey?.("valve.steam.gamepadui.keyboard");
    const win = popup?.m_popup || popup?.window;
    const windowApi = win?.SteamClient?.Window;
    const windows = root.SteamUIStore?.WindowStore?.SteamUIWindows;
    const managers = Array.isArray(windows) ? windows.map(w => w.VirtualKeyboardManager).filter(Boolean) : [];
    if (!store?.m_VRKeyboardState || !win?.document || !windowApi || !managers.length ||
        typeof windowApi.ShowWindow !== "function" || typeof windowApi.BringToFront !== "function")
        throw Error("steam-hooks-unavailable");
    function nativeStatus() {
        // VRKeyboardStatus also contains initial text. Read only these raw metadata fields.
        if (testStore) {
            const s = store.VRKeyboardStatus;
            return {open: s.bIsOpen, key: s.sOverlayKey, dispatch: s.bDispatchEventsToSteamVR,
                flags: s.bMinimal ? 13 : 0};
        }
        const s = store.m_VRKeyboardState;
        if (typeof s?.m_bIsOpen !== "boolean") throw Error("steam-status-changed");
        return {open: s.m_bIsOpen, key: s.m_sOpenForOverlayKey,
            dispatch: s.m_bDispatchEventsToSteamVR, flags: s.m_eVRKeyboardFlags};
    }
    const hooks = [], owners = [];
    const keyboardApi = root.SteamClient?.OpenVR?.Keyboard;
    const externalHide = keyboardApi?.Hide;
    const localTarget = s => externalSupported && s.dispatch === false && s.flags === 13 &&
        /^gamescope\.gamescope-[0-9]+(?:\.window\.[0-9]+)?$/.test(s.key) && typeof externalHide === "function";
    let preparedExternal = null, externalRestore = null, handoff = null;
    const localFrames = new Map();
    function selectedFrame() {
        const f = root.vrFrameStore?.activeFrame;
        if (!f || !Number.isSafeInteger(f.frameID)) return null;
        return {id: f.frameID, visible: f.isVisible === true,
            keys: Array.from(f.steamUIVROverlayKeysInPages || []).filter(key => ownSteam(key))};
    }
    function hook(object, key, wrap) {
        const descriptor = Object.getOwnPropertyDescriptor(object, key), original = object[key];
        if (typeof original !== "function" || descriptor && !descriptor.configurable && !descriptor.writable)
            throw Error("steam-hook-readonly");
        const replacement = wrap(original);
        Object.defineProperty(object, key, {configurable: true, writable: true, value: replacement});
        hooks.push(() => {
            if (object[key] !== replacement) return;
            if (descriptor) Object.defineProperty(object, key, descriptor);
            else delete object[key];
        });
        return original;
    }
    const ownSteam = key => typeof key === "string" && (key === "valve.steam.gamepadui.main" ||
        key === "valve.steam.gamepadui.barpopup" || key.startsWith("valve.steam.gamepadui.appoverlay."));
    let native = nativeStatus(), previousOpen = native.open, previousKey = native.key;
    let ignoreOpen = native.open, rawDeadline = 0, restoringUntil = 0;
    let restoringOwner = null, restoreTimer;
    let logical = null, closingNative = false, deadline = 0, request = 1, acknowledged = 0;
    let expectedClose = null;
    let eventSequence = 0;
    const events = [];
    function record(code) {
        events.push({id: ++eventSequence, request, code});
        if (events.length > 32) events.shift();
    }
    let lease = 0, progress = -1, disposed = false, inspecting = false, interceptedShow = false;
    let lastClock = now(), lastStep = now(), lastInspection = now(), timer, subscription;
    const oldWindowShow = windowApi.ShowWindow;
    const style = win.document.createElement("style");
    style.setAttribute("data-framekeyboard-native", "1");
    style.textContent = ":root, body { opacity: 0 !important; pointer-events: none !important; }";
    let styled = false;
    function setHidden(hidden) {
        if (hidden && !styled) { win.document.documentElement.appendChild(style); styled = true; }
        if (!hidden && styled) { style.remove(); styled = false; }
    }
    const fresh = () => !disposed && now() < lease;
    function editingDocument(manager) {
        const d = manager.m_ownerWindow?.document;
        return d && typeof d.execCommand === "function" &&
            typeof d.getSelection?.()?.modify === "function" &&
            typeof d.defaultView?.KeyboardEvent === "function" &&
            d.queryCommandSupported?.("insertText") && d.queryCommandSupported?.("delete") ? d : null;
    }
    const editableIn = (e, d) => e?.isConnected && e.ownerDocument === d && !e.disabled && !e.readOnly &&
        (e.tagName === "TEXTAREA" || e.tagName === "INPUT" &&
            ["text", "search", "password", "tel", "url"].includes(e.type));
    function routeFor(owner) {
        const m = owner.manager;
        if (owner.document?.visibilityState === "hidden") return null;
        if (typeof m.m_ActiveElementProps?.onTextEntered === "function") return {kind: "callback"};
        const d = owner.document;
        if (!d || m.m_ownerWindow?.document !== d) return null;
        const e = d.activeElement;
        const ref = m.m_currentVirtualKeyboardRef || m.m_lastActiveVirtualKeyboardRef;
        if (editableIn(e, d) && typeof ref?.BIsElementValidForInput === "function" &&
            (owner.focusRefs.get(ref) === e || ref.BIsElementValidForInput() === true) && (m.m_currentVirtualKeyboardRef ||
                typeof ref.SetAsCurrentVirtualKeyboardTarget === "function"))
            return {kind: "dom", element: e, ref};
        // A button can summon the panel without a field. Such a session can
        // navigate this window but never sends text to global compositor focus.
        return !m.m_currentVirtualKeyboardRef && !m.m_ActiveElementProps ? {kind: "dashboard"} : null;
    }
    const routeReady = owner => !!routeFor(owner);
    function sendDom(session, text, key) {
        const d = session.owner.document, element = session.route.element;
        if (!key) {
            // With no field, ordinary typing has no recipient. Never queue it
            // for a later field or forward it to a background application.
            return !element || d.execCommand("insertText", false, text) === true;
        }
        if (text === "VKClose") { releaseLogical(false); return true; }
        const codes = {Backspace: 8, Enter: 13, Tab: 9, ArrowLeft: 37, ArrowUp: 38,
            ArrowRight: 39, ArrowDown: 40};
        if (!(text in codes)) return false;
        if (element && text === "Enter" && session.props?.onEnterKeyPress != null) {
            const enter = session.props.onEnterKeyPress;
            const action = typeof enter === "function" ? enter.call(session.props) : enter;
            if (!action) return true;
            if (action === "VKClose") { releaseLogical(false); return true; }
            if (action !== "Enter") return false;
        }
        const target = element || d.activeElement || d.body;
        const event = type => new d.defaultView.KeyboardEvent(type,
            {key: text, code: text, keyCode: codes[text], which: codes[text],
                bubbles: true, cancelable: true});
        const accepted = target.dispatchEvent(event("keydown"));
        try {
            if (!accepted || element && d.activeElement !== element) return true;
            if (text === "Tab") {
                const items = [...d.querySelectorAll("input,textarea,button,a[href],[tabindex]")]
                    .filter(e => !e.disabled && e.tabIndex >= 0 && e.getClientRects().length)
                    .sort((a, b) => (a.tabIndex || Infinity) - (b.tabIndex || Infinity));
                const next = items[(items.indexOf(d.activeElement) + 1) % items.length];
                next?.focus();
            } else if (element) {
                if (text === "Backspace") d.execCommand("delete", false, null);
                else if (text.startsWith("Arrow")) {
                    const backward = text === "ArrowLeft" || text === "ArrowUp";
                    d.getSelection().modify("move", backward ? "backward" : "forward",
                        text === "ArrowLeft" || text === "ArrowRight" ? "character" : "line");
                } else if (text === "Enter") {
                    if (element.tagName === "TEXTAREA") d.execCommand("insertText", false, "\n");
                    else element.form?.requestSubmit?.();
                }
            }
            return true;
        } finally { target.dispatchEvent(event("keyup")); }
    }
    function potential(s) {
        return localTarget(s) || ownSteam(s.key) && (s.dispatch === false || (s.flags & 1) !== 0) &&
            owners.some(o => o.key === s.key && routeReady(o));
    }
    function candidate(s) {
        if (localTarget(s)) return preparedExternal?.key === s.key ? preparedExternal : null;
        const choices = owners.filter(o => {
            const route = routeFor(o);
            return o.key === s.key && route && (route.kind !== "callback" ||
                typeof o.manager.m_fnVROnTextEnteredOverride === "function");
        });
        return potential(s) && choices.length === 1 ? choices[0] : null;
    }
    const targetOf = manager => manager.m_currentVirtualKeyboardRef || manager.m_ActiveElementProps || null;
    function fieldRequest(s) {
        const owner = candidate(s), intent = owner?.fieldIntent;
        return owner && !owner.external && intent && now() < intent.until &&
            intent.target === targetOf(owner.manager) ? owner : null;
    }
    function buttonRequest(s) {
        if (localTarget(s)) return true;
        const owner = candidate(s);
        // Dashboard buttons bypass the field manager. A retained field reference
        // can be stale after returning from an app, so it is not proof of focus.
        return owner?.fieldHook && !fieldRequest(s);
    }
    function blocking() {
        return fresh() && !ignoreOpen && !restoringUntil &&
            (logical || !native.open || potential(native) && now() < rawDeadline);
    }
    function restoreWindow() {
        setHidden(false);
        if (interceptedShow && native.open) oldWindowShow.call(windowApi);
        interceptedShow = false;
    }
    function unsubscribe() {
        if (typeof subscription === "function") subscription();
        else subscription?.unregister?.();
        subscription = null;
    }
    function finishRestore(success) {
        const owner = restoringOwner;
        restoringOwner = null;
        restoringUntil = 0;
        root.clearTimeout(restoreTimer);
        if (disposed) unsubscribe();
        if (owner) record(success ? "restore-complete" : "restore-failed");
        if (owner && !success) owner.setOpen.call(owner.manager, false);
    }
    function checkRestore(final = false) {
        if (!restoringOwner) return;
        const status = nativeStatus();
        if (status.open && status.key === restoringOwner.key) finishRestore(true);
        else if (status.open || final || now() >= restoringUntil) finishRestore(false);
    }
    function releaseLogical(restore, closeNative = true,
        reason = restore ? "release-fallback" : "release-dismissed") {
        const session = logical;
        // Expiry runs before ordinary competing-owner inspection, and disposal
        // may precede a status callback. Always check the current provider here.
        try { native = nativeStatus(); } catch (_) {
            // Unknown presentation cannot authorize Show/Hide. Still release
            // our retained manager even if Steam's status interface changed.
            native = {open: false};
            restore = closeNative = false;
        }
        if (restore && session && !session.owner.external &&
            session.owner.document?.visibilityState === "hidden") {
            restore = closeNative = false;
            reason = "owner-hidden";
        } else if (restore && session?.route?.kind === "dom") {
            const current = routeFor(session.owner);
            if (current?.kind !== "dom" || current.element !== session.route.element ||
                targetOf(session.owner.manager) !== session.target) {
                // Blur can precede Steam clearing its ref, even on expiry or
                // helper shutdown. Do not revive that field or hide a newer one.
                restore = closeNative = false;
                reason = "target-cleared";
            }
        }
        logical = null;
        handoff = null;
        expectedClose = null;
        closingNative = false;
        acknowledged = 0;
        ++request;
        ignoreOpen = true;
        if (!restore) interceptedShow = false;
        restoreWindow();
        if (!session) return;
        if (restore && native.open && native.key !== session.owner.key) {
            record("competing-owner");
            if (!session.owner.external) session.owner.setOpen.call(session.owner.manager, false);
            preparedExternal = null;
            return;
        }
        record(reason);
        if (session.owner.external) {
            if (restore) {
                externalRestore = {key: session.owner.key, request};
                restoringUntil = now() + REQUEST_MS;
            } else if (closeNative) session.owner.hide.call(keyboardApi);
            preparedExternal = null;
        } else if (restore &&
            (!session.target || targetOf(session.owner.manager))) {
            // Preserve the field while the exact owning window's original provider opens again.
            restoringUntil = now() + REQUEST_MS;
            restoringOwner = session.owner;
            // Disposal removes the hooks and regular timer. This bounded check
            // still clears a failed restore without touching a newer provider.
            restoreTimer = root.setTimeout(() => {
                try { checkRestore(true); } catch (_) { finishRestore(false); }
            }, REQUEST_MS);
            session.owner.show.call(session.owner.api);
            oldWindowShow.call(windowApi);
        } else {
            // A missing field cannot justify closing another provider's newer
            // native request while fallback withdraws the old session.
            if (closeNative && (!restore || !native.open || native.key === session.owner.key))
                session.owner.hide.call(session.owner.api);
            // Hide can fail or complete asynchronously. An already-open native
            // keyboard for this owner must retain its ordinary Steam state.
            let status;
            try { status = nativeStatus(); } catch (_) { /* Ownership must still be released. */ }
            if (!status?.open || status.key !== session.owner.key)
                session.owner.setOpen.call(session.owner.manager, false);
        }
    }
    function begin(owner) {
        const route = owner.external ? null : routeFor(owner);
        // Steam clears its current ref on X. Restore only its own still-valid
        // last field, in response to this new explicit native summon.
        if (route?.kind === "dom" && !owner.manager.m_currentVirtualKeyboardRef)
            route.ref.SetAsCurrentVirtualKeyboardTarget();
        const frame = selectedFrame();
        logical = {owner, target: owner.external ? owner.key : targetOf(owner.manager),
            props: owner.manager?.m_ActiveElementProps, route, frame: frame?.id};
        handoff = null;
        if (owner.external && frame?.visible && frame.keys.length === 0) {
            localFrames.delete(frame.id);
            localFrames.set(frame.id, owner.key);
            if (localFrames.size > 16) localFrames.delete(localFrames.keys().next().value);
        }
        ++request;
        record("begin");
        acknowledged = 0;
        deadline = now() + REQUEST_MS;
        closingNative = !!native.open;
        expectedClose = !native.open || owner.external ? null : {owner, request};
        owner.fieldIntent = null;
        if (!owner.external) owner.setOpen.call(owner.manager, true);
        // Window.HideWindow leaves dashboard scene/hit geometry alive and can
        // trigger focus-driven reopening. Close presentation through its provider.
        if (native.open) owner.hide.call(owner.external ? keyboardApi : owner.api);
    }
    function inspect() {
        if (inspecting || disposed) return;
        inspecting = true;
        try {
            const time = now();
            lastInspection = time;
            if (time - lastClock > LEASE_MS || time < lastClock) lease = 0;
            lastClock = time;
            if (win.closed || root.g_PopupManager.GetPopupForVROverlayKey("valve.steam.gamepadui.keyboard") !== popup)
                throw Error("steam-popup-changed");
            native = nativeStatus();
            checkRestore();
            if (!restoringOwner && restoringUntil && (native.open || time >= restoringUntil)) restoringUntil = 0;
            if (!native.open && !restoringUntil) ignoreOpen = false;
            const newOpen = native.open && (!previousOpen || native.key !== previousKey);
            if (newOpen) rawDeadline = time + REQUEST_MS;
            previousOpen = native.open;
            previousKey = native.key;
            const frame = selectedFrame();
            if (logical && frame && logical.frame != null && logical.frame !== frame.id && !native.open) {
                // Frame selection can leave a hidden Steam input active in its
                // document. It is not the typing owner after an app switch.
                const key = frame.keys.length === 1 ? frame.keys[0] : localFrames.get(frame.id);
                releaseLogical(false, false, "owner-changed");
                if (fresh() && frame.visible && key) {
                    handoff = {key, frame: frame.id, until: time + REQUEST_MS};
                    ignoreOpen = false;
                }
            }
            if (handoff) {
                if (!fresh() || time >= handoff.until || native.open)
                    handoff = null;
                else if (handoff.previous != null) {
                    // Visibility and selected-tab metadata arrive separately.
                    // Stop old input immediately, then wait within the same
                    // bounded handoff for the new visible selected frame.
                    if (frame?.visible && frame.id !== handoff.previous) {
                        const key = frame.keys.length === 1 ? frame.keys[0] : localFrames.get(frame.id);
                        handoff = key ? {...handoff, key, frame: frame.id, previous: null} : null;
                    }
                } else if (!frame?.visible || frame.id !== handoff.frame) handoff = null;
            }
            if (handoff && ownSteam(handoff.key)) {
                const choices = owners.filter(o => o.key === handoff.key && routeReady(o));
                if (choices.length === 1) begin(choices[0]);
            }
            if (logical && !logical.owner.external && logical.owner.document?.visibilityState === "hidden") {
                const previous = logical.frame;
                releaseLogical(false, false, "owner-hidden");
                if (fresh() && previous != null)
                    handoff = {previous, until: time + REQUEST_MS};
            }
            if (logical) {
                if (!fresh() || time >= deadline && acknowledged !== request) {
                    releaseLogical(true, true, fresh() ? "request-timeout" : "lease-expired");
                } else if (newOpen && !closingNative && acknowledged === request && buttonRequest(native)) {
                    // The resident panel is already open, even if its previous app
                    // was closed. Consume this button toggle across supported owners.
                    releaseLogical(false);
                } else if (newOpen && !closingNative && fieldRequest(native)) {
                    const owner = fieldRequest(native), previous = logical.owner;
                    logical = null;
                    expectedClose = null;
                    preparedExternal = null;
                    if (!previous.external && previous !== owner)
                        previous.setOpen.call(previous.manager, false);
                    begin(owner);
                    record("target-changed");
                } else if (native.open && (native.key !== logical.owner.key || !potential(native))) {
                    // A different provider can open before our close acknowledgement.
                    // Its native session must survive; never reopen the old owner over it.
                    const previous = logical.owner;
                    logical = null;
                    expectedClose = null;
                    if (!previous.external && native.key !== previous.key)
                        previous.setOpen.call(previous.manager, false);
                    closingNative = false;
                    acknowledged = 0;
                    ++request;
                    record("competing-owner");
                    ignoreOpen = true;
                    preparedExternal = null;
                } else if (closingNative) {
                    if (!native.open && !expectedClose) closingNative = false;
                } else if (newOpen) {
                    const owner = candidate(native);
                    const changedTarget = owner !== logical.owner ||
                        owner && !owner.external && targetOf(owner.manager) !== logical.target;
                    if (changedTarget) {
                        const previous = logical.owner;
                        logical = null;
                        expectedClose = null;
                        if (!previous.external && previous !== owner)
                            previous.setOpen.call(previous.manager, false);
                        if (owner) begin(owner);
                        else { acknowledged = 0; ++request; ignoreOpen = true; }
                    } else {
                        // Native presentation is closed, so the next same-target
                        // native button Show toggles our resident panel off.
                        releaseLogical(false);
                    }
                } else if (logical.owner.external) {
                    // Gamescope input stays on the existing local libei/Unicode backend.
                } else {
                    const m = logical.owner.manager;
                    // React can replace props after every character while the field
                    // reference stays stable. Refresh callbacks without retargeting.
                    logical.props = m.m_ActiveElementProps;
                    const target = targetOf(m), route = routeFor(logical.owner);
                    if (!target && target !== logical.target || logical.route?.kind === "dom" &&
                        (!route || route.kind === "dashboard")) {
                        // Field loss proves only that Steam no longer owns
                        // this target. It cannot authorize libei input into
                        // whichever local app currently has compositor focus.
                        releaseLogical(false, true, "target-cleared");
                    } else if (!route || route.kind === "callback" &&
                        typeof m.m_fnVROnTextEnteredOverride !== "function") {
                        releaseLogical(true);
                    } else if (target !== logical.target || route.kind !== logical.route?.kind ||
                        route.element !== logical.route?.element) {
                        logical.target = target;
                        logical.route = route;
                        ++request;
                        acknowledged = 0;
                        deadline = time + REQUEST_MS;
                        record("target-changed");
                    }
                }
            } else if (native.open && !ignoreOpen && !restoringUntil) {
                const owner = candidate(native);
                if (fresh() && owner && time < rawDeadline) begin(owner);
                else if (!fresh() || !potential(native) || time >= rawDeadline) ignoreOpen = true;
            }
            // A show can arrive before its native status. Replay it when that
            // request falls back, even if we never acquired a logical session.
            if (blocking()) setHidden(true);
            else restoreWindow();
        } finally { inspecting = false; }
    }
    function idle() {
        return !logical && !native.open && !handoff && !preparedExternal && !externalRestore &&
            !restoringOwner && !restoringUntil && !expectedClose && !closingNative && !interceptedShow;
    }
    function snapshot() {
        const eligible = !!logical && !closingNative && !ignoreOpen;
        return {version: VERSION, request, open: !!logical || !!native.open || !!handoff, eligible, idle: idle(),
            active: eligible && acknowledged === request && fresh(),
            local: !!logical?.owner.external,
            external: logical?.owner.external ? {key: logical.owner.key, request} : null,
            externalCandidate: handoff && !native.open && externalSupported &&
                /^gamescope\.gamescope-[0-9]+(?:\.window\.[0-9]+)?$/.test(handoff.key) ?
                {key: handoff.key, flags: 13, request, requireActive: true} :
                !logical && !ignoreOpen && native.open && localTarget(native) &&
                fresh() && now() < rawDeadline ? {key: native.key, flags: native.flags, request} : null,
            externalRestore, events: events.slice()};
    }
    function dispose(restore = true) {
        if (disposed) return;
        lease = 0;
        try {
            let closeNative = true;
            if (restore === false) {
                // Update cleanup can race a new owner's native request. Release
                // our logical flag without hiding that unrelated presentation.
                try {
                    const current = nativeStatus();
                    closeNative = !current.open || current.key === logical?.owner.key;
                } catch (_) { closeNative = false; }
            }
            releaseLogical(restore !== false, closeNative);
            restoreWindow();
        } finally {
            disposed = true;
            root.clearInterval(timer);
            // Observe a pending restore until its bounded check completes. A
            // later summon for this owner must cancel cleanup before another
            // bridge can take it over and close native again.
            if (!restoringOwner) unsubscribe();
            else if (!subscription) checkRestore(true);
            for (const undo of hooks.reverse()) undo();
        }
    }
    try {
        const hookedApis = new Map();
        for (const manager of managers) {
            const api = manager.m_ownerWindow?.SteamClient?.OpenVR?.Keyboard;
            const key = manager.m_Instance?.GetVROverlayKey?.();
            if (!ownSteam(key) || typeof api?.Show !== "function" || typeof api?.Hide !== "function" ||
                typeof manager.SetVRKeyboardOpenForThisInstance !== "function") continue;
            const originals = hookedApis.get(api) || {show: api.Show, hide: api.Hide};
            const owner = {manager, api, key, document: editingDocument(manager), focusRefs: new WeakMap(), ...originals};
            const d = owner.document;
            if (d && typeof d.addEventListener === "function" && typeof d.removeEventListener === "function" &&
                typeof manager.SetVirtualKeyboardActiveRef === "function") {
                // Steam's shared-context predicate can consult the wrong document.
                // Associate a ref only when Steam selects it synchronously inside
                // this owner's actual DOM focus/click event. Never infer it from polling.
                const focused = event => { owner.focusEvent = event; };
                d.addEventListener("focus", focused, true);
                d.addEventListener("click", focused, true);
                hooks.push(() => {
                    d.removeEventListener("focus", focused, true);
                    d.removeEventListener("click", focused, true);
                    owner.focusEvent = null;
                });
                hook(manager, "SetVirtualKeyboardActiveRef", original => function(ref, ...args) {
                    const event = owner.focusEvent, e = event?.target;
                    if (event?.eventPhase && e === d.activeElement && editableIn(e, d) &&
                        ref && typeof ref === "object") owner.focusRefs.set(ref, e);
                    return original.call(this, ref, ...args);
                });
            }
            if (typeof manager.SetVirtualKeyboardShownInternal === "function") {
                owner.fieldHook = true;
                hook(manager, "SetVirtualKeyboardShownInternal", original => function(open, ...args) {
                    if (open && targetOf(this))
                        owner.fieldIntent = {target: targetOf(this), until: now() + REQUEST_MS};
                    else if (!open) owner.fieldIntent = null;
                    return original.call(this, open, ...args);
                });
            }
            owner.setOpen = hook(manager, "SetVRKeyboardOpenForThisInstance", original => function(open) {
                if (!open && logical?.owner.manager === this) {
                    // Consume exactly the close reaction caused by this request's
                    // Hide. Keep acquisition pending if status arrives first.
                    if (closingNative && expectedClose?.owner === owner &&
                        expectedClose.request === request && !nativeStatus().open) {
                        expectedClose = null;
                        inspect();
                        return;
                    }
                    // Steam has independently closed its field. Never restore it.
                    releaseLogical(false, false, "steam-closed");
                }
                return original.call(this, open);
            });
            owner.keyDown = hook(manager, "HandleVirtualKeyDown", original => function(...args) {
                if (logical?.owner.manager === this && fresh()) return;
                return original.apply(this, args);
            });
            owners.push(owner);
            if (!hookedApis.has(api)) {
                hookedApis.set(api, originals);
                hook(api, "Hide", original => function(...args) {
                    if (logical?.owner.api === this) releaseLogical(false);
                    return original.apply(this, args);
                });
            }
        }
        if (!owners.length) throw Error("steam-target-restoration-unavailable");
        for (const name of ["ShowWindow", "BringToFront"]) hook(windowApi, name, original => function(...args) {
            try { inspect(); if (blocking()) { interceptedShow = true; return; } }
            catch (_) { dispose(); }
            return original.apply(this, args);
        });
        // Ignore every callback argument, including initial field text.
        const keyboard = root.SteamClient?.OpenVR?.Keyboard;
        if (typeof keyboard?.RegisterForStatus === "function") subscription = keyboard.RegisterForStatus(() => {
            try { if (disposed) checkRestore(); else inspect(); } catch (_) { dispose(); }
        });
        inspect();
        timer = root.setInterval(() => {
            try {
                const time = now();
                // Keep the independent 50 ms watchdog. Only an idle full scan
                // can be coalesced with a recent helper/event inspection. Lease
                // expiry must still remove suppression at the ordinary deadline.
                if (!idle() || time < lastClock || (styled && !fresh()) ||
                    time - lastInspection >= IDLE_INSPECT_MS) inspect();
                if (!fresh() && now() - lastStep > 1500) dispose();
            } catch (_) { dispose(); }
        }, 50);
    } catch (error) { dispose(); throw error; }
    return {
        version: VERSION,
        expired() { return !fresh(); },
        diagnostics() { return {...snapshot(), route: logical?.route?.kind || null,
            nativeOpen: !!native.open, closingNative,
            expectedClose: !!expectedClose, session: !!logical, restoring: !!restoringOwner}; },
        step(frame) {
            if (disposed) return {version: VERSION, unavailable: true};
            lastStep = now();
            try {
                if (Number.isSafeInteger(frame.progress) && frame.progress > progress) {
                    progress = frame.progress;
                    lease = frame.ready ? now() + LEASE_MS : 0;
                }
                inspect();
                if (externalRestore && frame.externalRestored === externalRestore.request) externalRestore = null;
                if (frame.fallback && frame.fallbackRequest === request) releaseLogical(true);
                if (snapshot().eligible && fresh() && frame.ack === request &&
                    (acknowledged === request || now() < deadline)) {
                    if (acknowledged !== request) record("acknowledged");
                    acknowledged = request;
                }
                inspect();
                return snapshot();
            } catch (_) { dispose(); return {version: VERSION, unavailable: true}; }
        },
        send(generation, text, key = false) {
            inspect();
            if (!snapshot().active || generation !== request || typeof text !== "string" ||
                text.length > 8192 || typeof key !== "boolean" || key &&
                !["Backspace", "Enter", "Tab", "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "VKClose"].includes(text))
                return false;
            const session = logical, manager = session.owner.manager;
            if (session.owner.external) return false;
            const previous = manager.m_fnVROnTextEnteredOverride;
            if (session.route?.kind !== "callback") {
                const ok = sendDom(session, text, key);
                inspect();
                return ok;
            }
            const targetText = session.props?.onTextEntered;
            if (typeof targetText !== "function") return false;
            // The renderer's override can call VRKeyboard.SendText, which requires
            // an open native session. Keep Enter/Done handling in the manager, but
            // dispatch this one call only to its retained field callback.
            const override = value => targetText(value);
            manager.m_fnVROnTextEnteredOverride = override;
            try {
                if (key) session.owner.keyDown.call(manager, text, false);
                else targetText(text);
            }
            finally {
                if (manager.m_fnVROnTextEnteredOverride === override)
                    manager.m_fnVROnTextEnteredOverride = previous;
            }
            inspect();
            return true;
        },
        dismiss(generation) {
            inspect();
            if (!snapshot().active || generation !== request) return false;
            releaseLogical(false);
            inspect();
            return true;
        },
        prepareExternal(generation, key) {
            inspect();
            const pending = snapshot().externalCandidate;
            if (!pending || pending.request !== generation || pending.key !== key) return snapshot();
            preparedExternal = {external: true, key, api: keyboardApi, hide: externalHide};
            if (handoff && !native.open) begin(preparedExternal);
            inspect();
            return snapshot();
        },
        dispose,
    };
})
