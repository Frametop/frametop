"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const factory = vm.runInNewContext(fs.readFileSync(process.argv[2], "utf8"));
const MAIN = "valve.steam.gamepadui.main";
const EVENT_CODES = new Set(["begin", "acknowledged", "release-dismissed", "release-fallback",
    "competing-owner", "target-changed", "target-cleared", "lease-expired", "request-timeout",
    "steam-closed", "restore-complete", "restore-failed", "owner-changed", "owner-hidden"]);
const nativeSource = fs.readFileSync(require("node:path").join(__dirname, "../src/native_replacement.cpp"), "utf8");
const sessionCodes = nativeSource.match(/const char\* session_code\([\s\S]*?\n\}/)?.[0];
assert(sessionCodes, "native session-code parser anchor must match the production function");
assert.deepEqual([...sessionCodes.matchAll(/"([a-z-]+)"/g)].map(m => m[1]).sort(), [...EVENT_CODES].sort(),
    "JavaScript session diagnostics and the native log allow-list must stay in sync");

function editingFixture(manager) {
    const fields = new Map(), events = [], listeners = {focus: new Set(), click: new Set()};
    const body = {tagName: "DIV", dispatchEvent() { return true; }, focus() { doc.activeElement = body; }};
    const doc = {activeElement: body, body, visibilityState: "visible", defaultView: {KeyboardEvent: class {
        constructor(type, options) { Object.assign(this, {type, defaultPrevented: false}, options); }
        preventDefault() { this.defaultPrevented = true; }
    }}, queryCommandSupported: key => ["insertText", "delete"].includes(key),
        querySelectorAll: () => [...fields.keys()],
        addEventListener(type, fn) { listeners[type].add(fn); },
        removeEventListener(type, fn) { listeners[type].delete(fn); },
        execCommand(command, _ui, text) {
            const e = this.activeElement, f = fields.get(e);
            if (!f || e.disabled || e.readOnly) return false;
            let start = e.selectionStart, end = e.selectionEnd;
            if (command === "delete" && start === end)
                start -= [...f.text.slice(0, start)].at(-1)?.length || 0;
            if (command === "delete") text = "";
            f.text = f.text.slice(0, start) + text + f.text.slice(end);
            e.selectionStart = e.selectionEnd = start + text.length;
            events.push("input"); f.onInput?.(); return true;
        }, getSelection() { return {modify(_action, direction, unit) {
            const e = doc.activeElement, f = fields.get(e);
            if (!f) return;
            let pos = e.selectionStart;
            if (unit === "line") pos = direction === "backward" ? 0 : f.text.length;
            else if (direction === "backward") pos -= [...f.text.slice(0, pos)].at(-1)?.length || 0;
            else pos += [...f.text.slice(pos)][0]?.length || 0;
            e.selectionStart = e.selectionEnd = pos;
        }}; }};
    return {doc, events, text: element => fields.get(element).text,
        onInput(element, fn) { fields.get(element).onInput = fn; },
        field(validPredicate = true, focusEvent = true) {
            const e = {ownerDocument: doc, isConnected: true, tagName: "INPUT", type: "search",
                disabled: false, readOnly: false, selectionStart: 0, selectionEnd: 0, tabIndex: 0,
                getClientRects: () => [{}],
                dispatchEvent(event) { events.push(event.type + ":" + event.key); return !event.defaultPrevented; },
                focus() {
                    doc.activeElement = e;
                    const event = {target: e, eventPhase: 1};
                    for (const listener of listeners.focus) listener(event);
                    event.eventPhase = 2; ref.SetAsCurrentVirtualKeyboardTarget(); event.eventPhase = 0;
                }, click() {
                    const event = {target: e, eventPhase: 1};
                    for (const listener of listeners.click) listener(event);
                    event.eventPhase = 2; ref.SetAsCurrentVirtualKeyboardTarget(); event.eventPhase = 0;
                }};
            Object.defineProperty(e, "value", {get() { throw Error("must not read existing field text"); }});
            const props = {BIsElementValidForInput: () => validPredicate && doc.activeElement === e};
            const ref = {BIsElementValidForInput: props.BIsElementValidForInput,
                SetAsCurrentVirtualKeyboardTarget() {
                    manager.m_ActiveElementProps = props; manager.SetVirtualKeyboardActiveRef(ref);
                }};
            fields.set(e, {text: ""});
            if (focusEvent) e.focus();
            else { doc.activeElement = e; ref.SetAsCurrentVirtualKeyboardTarget(); }
            return e;
        }};
}
function fixture(open = false, production = false, external = false, sharedApi = false, withDom = false,
    discoveryProbe = false) {
    let time = 0, tick, observer, progress = 0, deferHide = false, deferClose = false, inspectionCalls = 0;
    const pendingCloses = [];
    const timeouts = new Map();
    let nextTimer = 1, showKey = MAIN;
    const calls = [], deliveries = [], wrongTarget = [], nodes = new Set();
    const status = {bIsOpen: open, bMinimal: true, sOverlayKey: MAIN, bDispatchEventsToSteamVR: false};
    Object.defineProperty(status, "sInitialText", {get() { throw Error("must not read field text"); }});
    const controllerText = value => { // Backspace uses Steam's existing ControllerKeyboardSendText mapping.
        root.SteamClient.Input.ControllerKeyboardSendText(value);
    };
    const store = {VRKeyboardStatus: status, controllerText};
    function raw() {
        const value = {m_bIsOpen: status.bIsOpen, m_sOpenForOverlayKey: status.sOverlayKey,
            m_bDispatchEventsToSteamVR: status.bDispatchEventsToSteamVR,
            m_eVRKeyboardFlags: status.bMinimal ? 13 : 0};
        Object.defineProperty(value, "m_sInitialKeyboardText", {get() { throw Error("must not read raw text"); }});
        store.m_VRKeyboardState = value;
    }
    raw();
    const manager = {logicalOpen: open, m_fnVROnTextEnteredOverride() { throw Error("native text route must stay unused"); },
        m_ActiveElementProps: {onTextEntered(value) { deliveries.push(value); }},
        m_Instance: {GetVROverlayKey() { return MAIN; }},
        SetVRKeyboardOpenForThisInstance(value) {
            if (this.logicalOpen === value) return;
            this.logicalOpen = value;
            if (!value) {
                this.m_lastActiveVirtualKeyboardRef = this.m_currentVirtualKeyboardRef;
                this.m_currentVirtualKeyboardRef = null; this.m_ActiveElementProps = null;
            }
        },
        SetVirtualKeyboardHidden() {
            this.m_currentVirtualKeyboardRef = null; this.m_ActiveElementProps = null;
            keyboard.Hide();
        },
        SetVirtualKeyboardShownInternal(open) {
            if (open) keyboard.Show();
            else keyboard.Hide();
        },
        HandleVirtualKeyDown(text) {
            if (text === "Enter" && this.m_ActiveElementProps?.onEnterKeyPress?.() === "VKClose" || text === "VKClose")
                this.SetVirtualKeyboardHidden();
            else { calls.push(text); this.m_fnVROnTextEnteredOverride(text); }
        }};
    const reactions = [];
    function update(values, notify = true) {
        Object.assign(status, values); raw();
        // Steam's MobX reaction runs only when this owner's computed value changes.
        for (const reaction of reactions) {
            const open = status.bIsOpen && status.sOverlayKey === reaction.key;
            if (open === reaction.previous) continue;
            reaction.previous = open;
            if (!open && deferClose)
                pendingCloses.push(() => reaction.manager.SetVRKeyboardOpenForThisInstance(false));
            else reaction.manager.SetVRKeyboardOpenForThisInstance(open);
        }
        if (notify) { observer?.(); tick?.(); }
    }
    const keyboard = {
        Show() { calls.push("native-show"); if (showKey) update({bIsOpen: true, sOverlayKey: showKey}); },
        Hide() { calls.push("native-hide"); if (!deferHide) update({bIsOpen: false}); },
        RegisterForStatus(fn) { observer = fn; return {unregister() { observer = null; }}; }
    };
    manager.m_ownerWindow = {SteamClient: {OpenVR: {Keyboard: keyboard}}};
    const dom = withDom ? editingFixture(manager) : null;
    if (dom) {
        manager.m_ownerWindow.document = dom.doc;
        manager.m_currentVirtualKeyboardRef = null; manager.m_ActiveElementProps = null;
        manager.SetVirtualKeyboardActiveRef = function(ref) { this.m_currentVirtualKeyboardRef = ref; };
    }
    const api = {ShowWindow() { calls.push("window-show"); }, HideWindow() {
        calls.push("window-hide"); update({bIsOpen: false}); update({bIsOpen: true});
    }, BringToFront() { calls.push("focus"); }};
    const oldKey = manager.HandleVirtualKeyDown, oldOpen = manager.SetVRKeyboardOpenForThisInstance;
    const oldShow = api.ShowWindow, oldHide = keyboard.Hide;
    const oldFront = api.BringToFront, oldFieldShow = manager.SetVirtualKeyboardShownInternal;
    const popup = {window: {SteamClient: {Window: api}, document: {
        documentElement: {appendChild(node) { nodes.add(node); }},
        createElement() { return {setAttribute() {}, remove() { nodes.delete(this); }}; }
    }}};
    const root = {SteamClient: {OpenVR: {Keyboard: keyboard}, Input: {
        ControllerKeyboardSendText(value) { wrongTarget.push(value); deliveries.push(value); }
    }}, performance: {now: () => time},
        setInterval(fn) { tick = fn; return 1; }, clearInterval() { tick = null; },
        setTimeout(fn, ms) { const id = nextTimer++; timeouts.set(id, {fn, until: time + ms}); return id; },
        clearTimeout(id) { timeouts.delete(id); },
        SteamUIStore: {WindowStore: {SteamUIWindows: [{VirtualKeyboardManager: manager}]}},
        g_PopupManager: {GetPopupForVROverlayKey() { ++inspectionCalls; return popup; }}};
    if (production) {
        const require = key => key === "store" ? {store} : key === "decoy" ?
            {store: {VRKeyboardStatus: {}}} : {controllerText};
        require.m = {...(discoveryProbe ? {decoy: function() { return "get VRKeyboardStatus()"; }} : {}),
            store: function() { return "get VRKeyboardStatus()"; },
            keyboard: function() { return "SetVirtualKeyboardShownInternal ControllerKeyboardSendText"; }};
        if (discoveryProbe) require.m.unused = {toString() {
            throw Error("discovery must stop once a valid keyboard store is found");
        }};
        root.webpackChunksteamui = [];
        root.webpackChunksteamui.push = function(chunk) { Array.prototype.push.call(this, chunk); chunk[2](require); };
    }
    const otherKey = "valve.steam.gamepadui.barpopup";
    const other = sharedApi ? {...manager, m_Instance: {GetVROverlayKey() { return otherKey; }},
        m_ActiveElementProps: {onTextEntered(value) { deliveries.push(value); }}} : null;
    if (other) root.SteamUIStore.WindowStore.SteamUIWindows.push({VirtualKeyboardManager: other});
    for (const owner of [manager, other].filter(Boolean)) {
        const key = owner.m_Instance.GetVROverlayKey();
        reactions.push({manager: owner, key, previous: open && status.sOverlayKey === key});
    }
    const bridge = factory(root, production ? undefined : store, external);
    function checkOwnership() {
        const s = bridge.diagnostics();
        assert.equal(wrongTarget.length, 0, "replacement input never reaches the global background-app sink");
        assert(s.events.length <= 32, "session diagnostics have bounded storage");
        let id = 0;
        for (const event of s.events) {
            assert(EVENT_CODES.has(event.code) && Number.isSafeInteger(event.id) && event.id > id &&
                Number.isSafeInteger(event.request) && event.request > 0,
                "session diagnostics contain only ordered fixed codes and numeric metadata");
            assert.deepEqual(Object.keys(event).sort(), ["code", "id", "request"]);
            id = event.id;
        }
        if (s.session || s.restoring) return;
        for (const {manager, key} of reactions)
            assert.equal(manager.logicalOpen, status.bIsOpen && status.sOverlayKey === key,
                "without a session or restore, each Steam flag matches its native owner");
    }
    return {bridge, calls, deliveries, wrongTarget, nodes, manager, other, dom, api, keyboard, oldKey, oldOpen, oldHide, oldShow,
        oldFront, oldFieldShow,
        nativeOpen() { return status.bIsOpen; },
        step(ack = 0, ready = true, fallback = false, advance = true) {
            const s = bridge.step({progress: advance ? ++progress : progress, ready, ack, fallback, fallbackRequest: ack});
            checkOwnership(); return s;
        },
        status(values, notify = true) { update(values, notify); checkOwnership(); },
        now() { return time; },
        advance(ms, inspect = true) {
            time += ms;
            for (const [id, timeout] of timeouts) if (time >= timeout.until) {
                timeouts.delete(id); timeout.fn();
            }
            if (inspect) tick?.();
            checkOwnership();
        },
        restoreAs(key) { showKey = key; },
        selectFrame(id, keys = []) {
            root.vrFrameStore = {activeFrame: {frameID: id, isVisible: true, steamUIVROverlayKeysInPages: keys}};
        },
        breakStatus() { store.m_VRKeyboardState.m_bIsOpen = undefined; },
        checkOwnership,
        newTarget() {
            manager.m_currentVirtualKeyboardRef = {};
            manager.m_ActiveElementProps = {onTextEntered(value) { deliveries.push(value); }};
            tick?.();
        },
        deferHide(value) { deferHide = value; },
        deferClose(value) { deferClose = value; },
        flushCloses() { while (pendingCloses.length) pendingCloses.shift()(); tick?.(); },
        alive() { return tick !== null; },
        observing() { return !!observer; }, inspections() { return inspectionCalls; }};
}
{
    const f = fixture(false, true, false, false, false, true);
    f.bridge.dispose();
}
function owned(f) {
    if (typeof f.manager.m_ActiveElementProps?.onTextEntered !== "function") f.newTarget();
    f.step(); f.status({bIsOpen: true}); const s = f.step();
    assert(s.open && s.eligible && !s.active);
    assert(!f.nativeOpen(), "takeover closes native scene, collision and move handle");
    assert(f.step(s.request).active);
    return s.request;
}
function externalOwned(f) {
    f.step(); f.status({bIsOpen: true, sOverlayKey: "gamescope.gamescope-0.window.62"});
    let s = f.step();
    assert(f.nativeOpen() && !s.eligible && s.externalCandidate, "leave native until exact-target guard is armed");
    s = f.bridge.prepareExternal(s.request, s.externalCandidate.key);
    s = f.step();
    assert(s.local && s.eligible && !f.nativeOpen(), "close the complete external native presentation");
    assert(f.step(s.request).active);
    return s.request;
}

// Updating ends the old typing session. It must not summon either keyboard,
// retain Steam's logical-open flag, or deliver work to that retired request.
for (const local of [false, true]) {
    const f = fixture(false, false, local);
    const request = local ? externalOwned(f) : owned(f);
    const shown = f.calls.filter(c => c === "native-show").length;
    f.bridge.dispose(false);
    assert(!f.nativeOpen(), "update shutdown leaves native presentation closed");
    assert(!f.manager.logicalOpen, "update shutdown clears Steam's keyboard-open flag");
    assert.equal(f.calls.filter(c => c === "native-show").length, shown);
    assert(!f.bridge.send(request, "retired input"), "the retired request cannot receive input");
    assert(!f.alive() && !f.observing(), "the outgoing bridge relinquishes its hooks");
    f.bridge.dispose();
    assert(!f.nativeOpen(), "later ordinary cleanup cannot undo update dismissal");
}
{
    const f = fixture(); owned(f);
    f.status({bIsOpen: true, sOverlayKey: "valve.steam.gamepadui.barpopup"}, false);
    f.bridge.dispose(false);
    assert(f.nativeOpen(), "update shutdown cannot close a competing native keyboard");
    assert(!f.manager.logicalOpen, "the outgoing owner still releases its own Steam flag");
}

{
    const f = fixture();
    assert.equal(f.bridge.diagnostics().idle, true, "a closed bridge explicitly identifies quiescence");
    const request = owned(f);
    assert.equal(f.bridge.diagnostics().idle, false, "a pending or active session must keep fast polling");
    f.step(request, true, true);
    assert.equal(f.bridge.diagnostics().idle, false, "native restoration keeps fast polling");
    f.status({bIsOpen: false}); f.step();
    assert.equal(f.bridge.diagnostics().idle, true, "fully closed restoration returns to idle");
    f.bridge.dispose();
}

{
    const f = fixture(); f.step();
    for (let i = 0; i < 8; ++i) {
        const before = f.inspections();
        f.advance(75);
        assert.equal(f.inspections(), before, "idle watchdog coalesces a recent helper inspection");
        f.step();
        assert(f.inspections() > before, "helper polls still perform full inspection");
    }
    f.bridge.dispose();
}
{
    const f = fixture(); f.step();
    assert(f.nodes.size > 0, "idle readiness can mask native presentation before a summon");
    f.advance(850); // A recent full scan must not postpone the 900 ms lease expiry.
    f.advance(51);
    assert.equal(f.nodes.size, 0, "lease expiry removes idle suppression without a helper poll");
    f.advance(600);
    assert(!f.alive(), "the independent orphan timer still disposes an unstepped idle bridge");
    f.bridge.dispose();
}
{
    const f = fixture(); f.step();
    f.status({bIsOpen: true}, false);
    f.advance(100);
    assert(f.bridge.diagnostics().session, "periodic inspection discovers status when callbacks are absent");
    f.advance(401);
    assert(f.nativeOpen(), "an unacknowledged request still reaches independent native fallback");
    f.bridge.dispose();
}
{
    const f = fixture(); owned(f);
    const before = f.inspections(); f.advance(50);
    assert(f.inspections() > before, "busy sessions keep full watchdog inspections");
    f.bridge.dispose();
}
{
    const f = fixture(); f.step(); f.advance(500); f.step();
    f.advance(-100);
    assert(f.bridge.expired() && f.nodes.size === 0, "an idle clock reversal withdraws suppression immediately");
    f.bridge.dispose();
}
function domOwned(f, automatic = true) {
    f.step();
    if (automatic) f.manager.SetVirtualKeyboardShownInternal(true);
    else f.status({bIsOpen: true});
    const s = f.step();
    assert(s.eligible && !s.local && !f.nativeOpen());
    assert(f.step(s.request).active); return s.request;
}
for (const production of [false, true]) for (const field of [false, true]) {
    const f = fixture(false, production, false, false, true), e = field ? f.dom.field() : null;
    const generation = domOwned(f, field);
    assert(f.bridge.send(generation, "VKClose") && f.step(generation).active,
        "literal VKClose remains text rather than dismissing the DOM session");
    if (e) assert.equal(f.dom.text(e), "VKClose");
    const shows = f.calls.filter(c => c === "native-show").length;
    assert(f.bridge.send(generation, "VKClose", true), "DOM Escape acknowledges dismissal");
    const state = f.step(generation);
    assert(!state.open && !state.active && !state.eligible && !f.nativeOpen(),
        "Escape closes field and bare-dashboard sessions without native fallback");
    assert(!f.manager.logicalOpen && !f.manager.m_currentVirtualKeyboardRef && !f.manager.m_ActiveElementProps,
        "Escape clears Steam's logical keyboard state and retained target");
    assert.equal(state.events.at(-1).code, "release-dismissed");
    assert(!f.bridge.send(generation, "late"), "dismissed requests reject queued text");
    assert.equal(f.calls.filter(c => c === "native-show").length, shows, "Escape never reopens native");
    if (e) assert.equal(f.dom.text(e), "VKClose");
    f.bridge.dispose();
}
for (const production of [false, true]) {
    const f = fixture(false, production), generation = owned(f);
    assert(f.bridge.send(generation, "VKClose") && f.step(generation).active,
        "literal VKClose remains text on the callback route");
    assert.deepEqual(f.deliveries, ["VKClose"]);
    assert(f.bridge.send(generation, "VKClose", true) && !f.step().open && !f.nativeOpen(),
        "callback Escape retains its existing dismissal behavior");
    f.bridge.dispose();
}
for (const production of [false, true]) {
    const f = fixture(false, production, true, false, true);
    f.selectFrame(1);
    let generation = externalOwned(f);
    f.selectFrame(2, [MAIN]);
    const e = f.dom.field(false);
    let state = f.step(generation);
    assert(state.eligible && !state.local && state.request !== generation);
    generation = state.request;
    assert(f.step(generation).active && f.bridge.send(generation, "search"));
    for (let cycle = 0; cycle < 4; ++cycle) {
        // The hidden Steam document keeps exactly the same active input/ref.
        f.dom.doc.visibilityState = "hidden";
        f.selectFrame(1);
        assert(!f.bridge.send(generation, "wrong owner"));
        state = f.step(generation);
        assert(!state.active && !state.local && state.externalCandidate?.requireActive === true,
            "a remembered local frame needs a fresh independent active-overlay guard");
        const pending = state.externalCandidate;
        assert(!f.bridge.send(pending.request, "before guard"));
        state = f.bridge.prepareExternal(pending.request, pending.key);
        generation = state.request;
        assert(state.local && f.step(generation).active,
            "verified local ownership follows the selected frame without a native summon");
        f.dom.doc.visibilityState = "visible";
        f.selectFrame(2, [MAIN]);
        state = f.step(generation);
        assert(state.eligible && !state.local && !state.externalRestore);
        generation = state.request;
        assert(f.step(generation).active && f.bridge.send(generation, "!"));
    }
    assert.equal(f.dom.text(e), "search!!!!", "only Steam-owned generations can edit search");
    f.bridge.dismiss(generation);
    f.selectFrame(1);
    assert(!f.step().open && !f.step().externalCandidate, "X prevents selection-driven reopening");
    f.bridge.dispose();
}
for (const expire of [false, true]) {
    const f = fixture(false, false, true, false, true);
    f.selectFrame(1); externalOwned(f);
    f.selectFrame(2, [MAIN]); f.dom.field();
    let state = f.step(); f.step(state.request);
    f.dom.doc.visibilityState = "hidden"; f.selectFrame(1);
    state = f.step(); const pending = state.externalCandidate;
    assert(pending?.requireActive);
    if (expire) f.advance(401);
    else f.selectFrame(3); // Unknown/streaming frames must not inherit local input.
    assert(!f.bridge.prepareExternal(pending.request, pending.key).active && !f.step().external,
        "a late guard reply cannot revive an expired or superseded selection");
    f.bridge.dispose();
}
{
    const f = fixture(false, false, true, false, true);
    f.selectFrame(1); externalOwned(f);
    f.selectFrame(2, [MAIN]); const e = f.dom.field();
    let state = f.step(); const request = state.request; f.step(request);
    f.dom.doc.visibilityState = "hidden";
    assert(!f.bridge.send(request, "stale"), "owner visibility loss stops input before tab metadata catches up");
    assert(f.step().open && !f.step().active && !f.step().externalCandidate);
    f.advance(50); f.selectFrame(1);
    state = f.step(); const candidate = state.externalCandidate;
    assert(candidate?.requireActive, "the selected-frame update completes the bounded handoff");
    state = f.bridge.prepareExternal(candidate.request, candidate.key);
    assert(f.step(state.request).active && f.step(state.request).local && f.dom.text(e) === "");
    f.bridge.dispose();
}
for (const dispose of [false, true]) {
    const f = fixture(false, false, false, false, true), e = f.dom.field();
    const generation = domOwned(f);
    f.dom.doc.visibilityState = "hidden";
    if (dispose) f.bridge.dispose();
    else assert(!f.bridge.send(generation, "hidden"));
    assert(!f.nativeOpen() && f.dom.text(e) === "", "a hidden owner is never typed into or restored");
    f.bridge.dispose();
}
for (const production of [false, true]) for (const automatic of [false, true]) {
    const f = fixture(false, production, false, false, true), e = f.dom.field();
    const generation = domOwned(f, automatic);
    assert(f.bridge.send(generation, "a🧪b"));
    assert(f.bridge.send(generation, "ArrowLeft", true));
    assert(f.bridge.send(generation, "Backspace", true));
    assert(f.bridge.send(generation, "é"));
    assert.equal(f.dom.text(e), "aéb", "DOM route edits the retained field including surrogate pairs");
    f.dom.onInput(e, () => {
        f.manager.m_ActiveElementProps = {...f.manager.m_ActiveElementProps};
    });
    assert(f.bridge.send(generation, "ArrowLeft"));
    assert.equal(f.dom.text(e), "aéArrowLeftb", "text spelling a control name remains ordinary text");
    assert.equal(f.step(generation).request, generation, "React prop refresh keeps the same DOM generation");
    assert.equal(f.wrongTarget.length, 0, "Steam input cannot escape to a background app");
    assert.equal(f.deliveries.length, 0, "DOM delivery never uses the global/native text routes");
    const next = f.dom.field(), changed = f.step(generation);
    assert(changed.request !== generation && !changed.active);
    assert(!f.bridge.send(generation, "stale"), "a changed DOM target rejects old queued text");
    assert(f.step(changed.request).active && f.bridge.send(changed.request, "new"));
    assert.equal(f.dom.text(next), "new");
    const shows = f.calls.filter(x => x === "native-show").length;
    f.dom.doc.activeElement = f.dom.doc.body;
    const blurred = f.step(changed.request);
    assert(!blurred.open && !blurred.active && !f.nativeOpen(), "DOM blur dismisses instead of restoring a stale field");
    assert(!f.bridge.send(changed.request, "late") &&
        f.calls.filter(x => x === "native-show").length === shows);
    f.bridge.dispose();
}
{
    const f = fixture(false, false, false, false, true), e = f.dom.field();
    let generation = domOwned(f);
    assert(f.bridge.dismiss(generation));
    assert(!f.manager.m_currentVirtualKeyboardRef && f.manager.m_lastActiveVirtualKeyboardRef);
    generation = domOwned(f, false);
    assert(f.bridge.send(generation, "reopened") && f.dom.text(e) === "reopened",
        "button summon restores only Steam's still-valid last field after X");
    f.manager.m_ActiveElementProps.onEnterKeyPress = () => "VKClose";
    assert(f.bridge.send(generation, "Enter") && f.step(generation).active,
        "literal Enter text does not invoke a Steam action");
    assert(f.bridge.send(generation, "Enter", true) && !f.step(generation).open,
        "real Enter preserves the owning field's action and dismissal");
    f.bridge.dispose();
}
for (const production of [false, true]) {
    const f = fixture(false, production, false, false, true);
    let generation = domOwned(f, false);
    assert(f.bridge.send(generation, "no recipient") && !f.dom.events.includes("input"),
        "dashboard button opens ours without a field and cannot send text to any app");
    assert(f.bridge.send(generation, "ArrowDown", true));
    const e = f.dom.field(); f.manager.SetVirtualKeyboardShownInternal(true);
    const next = f.step(generation);
    assert(next.request !== generation && next.eligible && !next.active);
    generation = next.request;
    assert(f.step(generation).active && f.bridge.send(generation, "field"));
    assert.equal(f.dom.text(e), "field", "search focus transfers the bare dashboard session to its own field");
    f.bridge.dispose();
}
{
    // A field whose focus predates bridge installation still emits a click.
    const g = fixture(false, false, false, false, true), input = g.dom.field(false, false);
    const ref = g.manager.m_currentVirtualKeyboardRef;
    g.manager.m_currentVirtualKeyboardRef = {BIsElementValidForInput: () => false};
    input.click();
    assert.equal(g.manager.m_currentVirtualKeyboardRef, ref);
    const request = domOwned(g);
    assert(g.bridge.send(request, "click") && g.dom.text(input) === "click");
    g.bridge.dispose();
}
for (const dispose of [false, true]) {
    const f = fixture(false, false, false, false, true); f.dom.field();
    const request = domOwned(f), shows = f.calls.filter(x => x === "native-show").length;
    f.dom.doc.activeElement = f.dom.doc.body;
    if (dispose) f.bridge.dispose(); else { f.advance(901, false); f.step(request); }
    assert(!f.nativeOpen() && f.calls.filter(x => x === "native-show").length === shows,
        "expiry and shutdown cannot restore a blurred DOM field before Steam clears its ref");
    f.checkOwnership(); f.bridge.dispose();
}
for (const production of [false, true]) {
    const f = fixture(false, production, false, false, true), e = f.dom.field(false);
    assert(!f.manager.m_currentVirtualKeyboardRef.BIsElementValidForInput(),
        "Steam's shared-context document can make its predicate false for an owner-window field");
    const request = domOwned(f);
    assert(f.bridge.send(request, "bound") && f.dom.text(e) === "bound",
        "synchronous owner-document focus binds the exact Steam ref despite the wrong-document predicate");
    const ref = f.manager.m_currentVirtualKeyboardRef;
    f.bridge.dismiss(request);
    const next = domOwned(f, false);
    assert(f.bridge.send(next, " again") && f.dom.text(e) === "bound again");
    // Merely polling another active input or calling SetActiveRef outside its
    // focus event cannot attach a stale field ref to that input.
    const other = f.dom.field(false);
    f.manager.SetVirtualKeyboardActiveRef(ref);
    assert(!f.bridge.send(next, "stale") && f.dom.text(other) === "");
    f.bridge.dispose();
}
{
    const f = fixture(false, false, false, false, true), e = f.dom.field();
    f.manager.m_ActiveElementProps.BIsElementValidForInput = () => false;
    // The actual field ref is authoritative; a stale or foreign ref cannot bind text.
    f.manager.m_currentVirtualKeyboardRef = {BIsElementValidForInput: () => false};
    f.step(); f.manager.SetVirtualKeyboardShownInternal(true);
    assert(f.nativeOpen() && !f.step().eligible && !f.bridge.send(1, "no"));
    assert.equal(f.dom.text(e), ""); f.bridge.dispose();
}
for (const production of [false, true]) {
    const f = fixture(false, production); f.newTarget();
    // Ordinary Steam search has Enter handling but no field-bound text callback.
    // Its global controller route can still target the app behind the dashboard.
    f.manager.m_ActiveElementProps = {onEnterKeyPress() {}};
    f.step(); f.status({bIsOpen: true}); const s = f.step(); f.step(s.request);
    f.bridge.send(s.request, "private fixture");
    assert.equal(f.wrongTarget.length, 0, "Steam fields cannot deliver through the global app-targeted controller route");
    assert(f.nativeOpen() && !f.step().eligible,
        "leave native available when no field-bound text delivery exists");
    f.bridge.dispose();
}
for (const production of [false, true]) {
    const f = fixture(false, production); f.newTarget(); const request = owned(f);
    const target = f.manager.m_currentVirtualKeyboardRef;
    f.manager.m_ActiveElementProps = {onEnterKeyPress() {}};
    assert(!f.bridge.send(request, "late private fixture"),
        "losing the bound callback cancels stale delivery before any global dispatch");
    assert(f.nativeOpen() && !f.step().eligible && f.manager.m_currentVirtualKeyboardRef === target,
        "callback loss restores the same native field without losing its target");
    assert.equal(f.wrongTarget.length, 0);
    assert.equal(f.deliveries.length, 0);
    f.bridge.dispose();
}
for (const deferred of [false, true]) {
    const f = fixture(); f.newTarget();
    f.deferHide(deferred);
    f.step(); f.status({bIsOpen: true});
    const s = f.step();
    if (!deferred) assert(f.step(s.request).active);
    f.status({bIsOpen: true, sOverlayKey: "unknown.overlay"});
    assert(!f.manager.logicalOpen && !f.manager.m_currentVirtualKeyboardRef,
        "a competing provider clears the former owner's retained Steam state");
    assert(f.nativeOpen() && !f.step().eligible);
    f.bridge.dispose();
    assert(!f.manager.logicalOpen && f.nativeOpen(), "dispose preserves only the newer native owner");
}
for (const external of [false, true]) for (const shutdown of [false, true]) {
    const f = fixture(false, false, external); f.newTarget();
    if (external) externalOwned(f); else owned(f);
    if (!shutdown) f.advance(901, false);
    // Native metadata can change before its status notification is delivered.
    f.status({bIsOpen: true, sOverlayKey: "unknown.overlay"}, !shutdown);
    if (shutdown) f.bridge.dispose();
    const s = f.bridge.diagnostics();
    assert(!s.eligible && !s.externalRestore && !f.manager.logicalOpen,
        "expiry or shutdown abandons the old owner when a newer native request already exists");
    assert(f.nativeOpen() && !f.calls.includes("native-show"),
        "fallback cannot restore the old Steam provider over a newer native request");
    f.checkOwnership(); f.bridge.dispose();
}
for (const external of [false, true]) {
    const f = fixture(false, true, external); f.newTarget();
    if (external) externalOwned(f); else owned(f);
    f.breakStatus();
    assert(f.step().unavailable && !f.manager.logicalOpen,
        "incompatible native status cannot prevent retained Steam ownership cleanup");
    if (!external) assert(!f.manager.m_currentVirtualKeyboardRef);
    assert(!f.calls.includes("native-show") && !f.alive() && !f.observing());
    assert.equal(f.calls.filter(c => c === "native-hide").length, 1,
        "unknown presentation cannot authorize another native Hide during cleanup");
}
{
    const f = fixture(); f.newTarget(); const target = f.manager.m_currentVirtualKeyboardRef;
    owned(f);
    f.status({bIsOpen: true, bMinimal: false, bDispatchEventsToSteamVR: true});
    assert(!f.step().eligible && f.nativeOpen() && f.manager.logicalOpen &&
        f.manager.m_currentVirtualKeyboardRef === target,
        "an unsupported native mode for the same owner retains its native field and flag");
    f.bridge.dispose(); f.checkOwnership();
}
for (const dispose of [false, true]) for (const key of [null, "unknown.overlay", MAIN]) {
    const f = fixture(); f.newTarget(); const target = f.manager.m_currentVirtualKeyboardRef;
    const request = owned(f);
    f.restoreAs(key);
    if (dispose) f.bridge.dispose();
    else f.step(request, true, true);
    f.advance(401);
    assert.equal(f.manager.logicalOpen, key === MAIN, "restore retains Steam state only for the exact native owner");
    assert.equal(!!f.manager.m_currentVirtualKeyboardRef, key === MAIN);
    if (key === MAIN) assert.equal(f.manager.m_currentVirtualKeyboardRef, target);
    assert.equal(f.nativeOpen(), key !== null, "failed restore cannot close another native provider");
    f.checkOwnership();
    f.bridge.dispose();
}
{
    const f = fixture(); f.newTarget(); owned(f); f.restoreAs(null);
    f.bridge.dispose();
    assert(f.observing(), "shutdown observes a pending restore for its bounded verification window");
    f.status({bIsOpen: true, sOverlayKey: MAIN});
    const nextTarget = {};
    f.manager.m_currentVirtualKeyboardRef = nextTarget;
    f.advance(401);
    assert(!f.observing() && f.manager.logicalOpen && f.manager.m_currentVirtualKeyboardRef === nextTarget,
        "a later same-owner native summon cancels old cleanup and releases the temporary observer");
}
{
    const f = fixture(); f.newTarget(); const request = owned(f);
    f.deferHide(true); f.status({bIsOpen: true});
    assert(f.nativeOpen() && f.manager.logicalOpen && !f.step(request).eligible,
        "a failed dismissal retains state for the native keyboard that is still open");
    f.checkOwnership(); f.bridge.dispose();
}
for (const deferred of [false, true]) {
    const f = fixture(); f.newTarget(); f.deferClose(deferred);
    f.step(); f.status({bIsOpen: true});
    if (deferred) {
        assert(!f.step().eligible, "wait for our delayed physical close reaction");
        f.flushCloses();
    }
    const s = f.step();
    assert(f.step(s.request).active && f.manager.logicalOpen && f.manager.m_currentVirtualKeyboardRef,
        "our single close acknowledgement preserves the Steam field in either callback order");
    f.manager.SetVRKeyboardOpenForThisInstance(false);
    assert(!f.manager.logicalOpen && !f.manager.m_currentVirtualKeyboardRef && !f.step().eligible,
        "a subsequent Steam close passes through and ends replacement without native restore");
    assert(!f.nativeOpen() && !f.calls.includes("native-show"));
    assert(!f.bridge.diagnostics().expectedClose, "the close marker cannot outlive its request");
    assert.deepEqual(Array.from(f.bridge.diagnostics().events, e => e.code),
        ["begin", "acknowledged", "steam-closed"], "events retain transitions that happen between polls");
    f.bridge.dispose();
}
{
    const f = fixture(false, false, true), request = externalOwned(f);
    // App, helper and native guard are all gone: no further step/restore ack.
    f.advance(901);
    assert(f.bridge.expired() && f.nodes.size === 0 && !f.bridge.diagnostics().active);
    assert(f.bridge.diagnostics().externalRestore && !f.nativeOpen(),
        "no external provider can restore a prior request after its guard is gone");
    f.status({bIsOpen: true, sOverlayKey: "gamescope.gamescope-0.window.62"});
    f.api.ShowWindow();
    assert(f.nativeOpen() && f.calls.includes("window-show") && !f.bridge.send(request, "stale"),
        "a fresh native summon stays visible after all replacement processes exit");
    f.advance(601);
    assert(!f.alive() && !f.observing() && f.keyboard.Hide === f.oldHide,
        "the expired hook removes itself without a surviving helper");
}
for (const production of [false, true]) {
    const f = fixture(false, production, false, true);
    f.step(); f.status({bIsOpen: true, sOverlayKey: "valve.steam.gamepadui.barpopup"});
    const s = f.step();
    assert(s.eligible && f.step(s.request).active && !f.nativeOpen(),
        "a second owner sharing the provider can acquire its native session");
    assert.equal(f.calls.filter(c => c === "native-hide").length, 1,
        "takeover calls the original provider rather than another owner's hook");
    assert(f.bridge.send(s.request, "shared provider fixture"));
    assert(f.bridge.dismiss(s.request) && !f.step().open && !f.nativeOpen());
    assert.equal(f.calls.filter(c => c === "native-hide").length, 2, "dismiss invokes the provider once");
    f.status({sOverlayKey: MAIN});
    assert(owned(f), "the first owner can still summon after the shared owner's dismissal");
    f.bridge.dispose();
    assert.equal(f.keyboard.Hide, f.oldHide, "restore the single shared provider hook");
}
{
    const f = fixture(false, false, true), generation = externalOwned(f);
    assert.equal(f.bridge.send(generation, "private fixture"), false, "external text stays in the local input backend");
    for(let i=0;i<8;++i) { f.status({bIsOpen:false}); assert.equal(f.step(generation).request,generation); }
    assert(f.bridge.dismiss(generation));
    assert(!f.step().open && !f.step().externalRestore, "X cancels native recovery without reopening");
    const next = externalOwned(f); assert.notEqual(next, generation);
    f.step(next, true, true);
    const s=f.step();
    assert(s.externalRestore && !s.active && !s.external, "escape asks the guard to restore its exact target");
    assert(!f.calls.includes("native-show"), "never restore an external target through Steam's own window");
    f.bridge.dispose();
}
{
    const f=fixture(false, false, true), generation=externalOwned(f);
    f.advance(901); const s=f.bridge.diagnostics();
    assert(!s.active && s.externalRestore && f.nodes.size===0, "UI lease withdraws external suppression before guard recovery");
    f.bridge.dispose();
}
for (const staleField of [false, true]) {
    const f = fixture(false, false, true);
    if (staleField) f.newTarget();
    const generation = externalOwned(f);
    // The app closes, leaving ours visible. The dashboard's keyboard button
    // now opens for Steam instead of the former Gamescope window.
    f.status({bIsOpen: true, sOverlayKey: MAIN, bDispatchEventsToSteamVR: true});
    const s = f.step(generation);
    assert(!s.open && !s.eligible && !f.nativeOpen(),
        "dashboard button after closing an app must dismiss both presentations");
    assert(!s.external && !s.externalRestore, "cancel the former app guard rather than restore native");
    assert(!f.calls.includes("native-show"));
    f.status({bIsOpen: true, sOverlayKey: MAIN});
    const next = f.step();
    assert(f.nativeOpen() && !next.eligible && !f.step(next.request).active,
        "a later dashboard summon without a field callback stays native");
    f.bridge.dispose();
}
{
    const f = fixture(false, false, true), previous = externalOwned(f);
    f.newTarget();
    f.manager.SetVirtualKeyboardShownInternal(true);
    const s = f.step();
    assert(s.eligible && !s.local && s.request !== previous && !f.nativeOpen(),
        "automatic Steam field focus transfers an app session instead of toggling off");
    assert(f.step(s.request).active && f.bridge.send(s.request, "field fixture"));
    f.manager.SetVirtualKeyboardShownInternal(true);
    const again = f.step();
    assert(again.eligible && f.step(again.request).active,
        "explicitly refocusing the same field must keep replacement open");
    f.bridge.dispose();
}
{
    const f = fixture(false, false, true), generation = owned(f);
    f.status({bIsOpen:true,sOverlayKey:"gamescope.gamescope-0.window.72"});
    const s=f.step(generation);
    assert(!s.open && !f.nativeOpen() && !s.externalRestore,
        "the local-app button can dismiss a panel opened in Steam too");
    f.bridge.dispose();
}
{
    const f=fixture(false,false,true), generation=externalOwned(f);
    f.status({bIsOpen:true,sOverlayKey:"unknown.overlay"});
    assert(f.nativeOpen() && !f.step(generation).eligible,
        "unsupported providers still stay native rather than becoming global toggles");
    f.bridge.dispose();
}
{
    const f=fixture(false, false, true); f.step();
    f.status({bIsOpen:true,sOverlayKey:"gamescope.gamescope-0.window.62"});
    const stale=f.step(); f.status({sOverlayKey:"unknown.overlay"});
    assert(!f.bridge.prepareExternal(stale.request,stale.externalCandidate.key).eligible);
    assert(f.nativeOpen() && !f.calls.includes("native-hide"), "stale guard preparation must not close another target");
    f.bridge.dispose();
}
for(const values of [{bDispatchEventsToSteamVR:true},{bMinimal:false},{sOverlayKey:"gamescope.other"}]) {
    const f=fixture(false,false,true); f.step();
    f.status({bIsOpen:true,sOverlayKey:"gamescope.gamescope-0.window.62",...values});
    const s=f.step(); assert(!s.eligible && !s.externalCandidate && f.nativeOpen());
    f.bridge.dispose();
}
for (const production of [false, true]) {
    for (const reason of ["unsupported", "missing route", "unprepared external", "not ready"]) {
        const f = fixture(false, production, true);
        if (reason === "missing route") f.manager.m_fnVROnTextEnteredOverride = null;
        f.step(); f.api.ShowWindow();
        assert(!f.calls.includes("window-show"), "pre-status show is initially gated");
        f.status({bIsOpen: true, sOverlayKey: reason === "unsupported" ? "unknown.overlay" :
            reason === "unprepared external" ? "gamescope.gamescope-0.window.62" : MAIN});
        if (reason === "not ready") f.step(0, false);
        for (let i = 0; i < 8; ++i) { f.advance(100); f.step(0, reason !== "not ready"); }
        assert(f.nativeOpen() && !f.step(0, reason !== "not ready").eligible);
        assert.equal(f.nodes.size, 0, "fallback removes suppression");
        assert.equal(f.calls.filter(c => c === "window-show").length, 1,
            `${reason} replays the gated show exactly once`);
        f.bridge.dispose();
    }
}
{
    const f = fixture(); f.newTarget(); const previous = owned(f);
    f.manager.m_currentVirtualKeyboardRef = null;
    f.manager.m_ActiveElementProps = null;
    f.advance(901, false);
    f.status({bIsOpen: true, sOverlayKey: "unknown.overlay"});
    assert(f.nativeOpen() && !f.step(previous).eligible,
        "expired missing-field fallback preserves a newer unrelated native request");
    assert(!f.calls.includes("native-show"));
    f.bridge.dispose();
}
{
    const f = fixture(); const generation = owned(f);
    f.api.ShowWindow();
    assert(f.bridge.dismiss(generation));
    assert(!f.calls.includes("window-show"), "explicit dismissal discards a gated show");
    f.bridge.dispose();
}
for (const production of [false, true]) {
    const f = fixture(false, production); f.step();
    assert.equal(f.nodes.size, 1, "prearm transparent texture without reading initial text");
    f.api.ShowWindow(); assert.equal(f.calls.length, 0, "gate a pre-show call");
    f.status({bIsOpen: true}); const s = f.step();
    assert(!f.nativeOpen() && s.open && s.eligible && !s.active);
    assert.equal(f.bridge.send(s.request, "early"), false);
    f.manager.HandleVirtualKeyDown("stock-controller");
    assert.deepEqual(f.calls, ["native-hide"], "stock events stay blocked during readiness");
    assert(f.step(s.request).active);
    assert(f.bridge.send(s.request, "café 世界"));
    for (let i = 0; i < 10; ++i) {
        f.status({bIsOpen: false}); // Duplicate native callbacks must not create requests.
        assert.equal(f.step(s.request).request, s.request);
        assert(f.bridge.send(s.request, "a"));
    }
    assert.equal(f.calls.filter(c => c === "native-hide").length, 1);
    assert(!f.calls.includes("window-hide"), "never use the focus-reopening browser hide path");
    assert(!f.calls.includes("native-show"), "clicking/typing cannot reopen the native session");
    f.bridge.dispose();
    assert(f.nativeOpen(), "dispose independently restores native presentation");
    assert.equal(f.manager.HandleVirtualKeyDown, f.oldKey);
    assert.equal(f.manager.SetVRKeyboardOpenForThisInstance, f.oldOpen);
    assert.equal(f.keyboard.Hide, f.oldHide);
    assert.equal(f.api.ShowWindow, f.oldShow);
    assert(!f.alive() && !f.observing(), "remove watchdog and status callback");
}
{
    const f = fixture(false, true); f.status({bDispatchEventsToSteamVR: true});
    const generation = owned(f);
    assert(f.bridge.send(generation, "a") && f.bridge.send(generation, "Backspace", true));
    assert.deepEqual(f.deliveries, ["a", "Backspace"],
        "native-button requests with a field callback retain target-bound text delivery");
    f.bridge.dispose();
}
{
    const f = fixture(); f.newTarget(); const field = [];
    f.manager.m_ActiveElementProps.onTextEntered = value => field.push(value);
    const original = f.manager.m_fnVROnTextEnteredOverride;
    f.status({bDispatchEventsToSteamVR: true}); const generation = owned(f);
    assert(f.bridge.send(generation, "café 世界") && f.bridge.send(generation, "ArrowLeft", true));
    assert.deepEqual(field, ["café 世界", "ArrowLeft"], "retained Steam field receives direct logical keys");
    assert.equal(f.deliveries.length, 0, "field dispatch cannot leak to another focused input route");
    assert.equal(f.manager.m_fnVROnTextEnteredOverride, original, "restore renderer override after each dispatch");
    const refreshed = [];
    f.manager.m_ActiveElementProps = {onTextEntered: value => refreshed.push(value)};
    assert.equal(f.step(generation).request, generation, "field rerenders do not invalidate an acknowledged request");
    assert(f.bridge.send(generation, "b"));
    assert.deepEqual(refreshed, ["b"], "a stable field uses its refreshed callback");
    f.manager.m_ActiveElementProps.onTextEntered = () => { throw Error("fixture field rejected text"); };
    assert.throws(() => f.bridge.send(generation, "unsent"));
    assert.equal(f.manager.m_fnVROnTextEnteredOverride, original, "dispatch errors also restore the renderer override");
    f.bridge.dispose();
}
for (const production of [false, true]) {
    const f = fixture(false, production); f.newTarget(); const previous = owned(f);
    f.manager.m_currentVirtualKeyboardRef = null;
    f.manager.m_ActiveElementProps = null;
    const dashboard = f.step(previous);
    assert(dashboard.request !== previous && !dashboard.open && !dashboard.eligible && !dashboard.local,
        "a cleared Steam field ends its session instead of guessing compositor focus");
    assert(!dashboard.active && !f.bridge.send(previous, "stale"),
        "field loss invalidates old delivery and requires a fresh acknowledgement");
    assert(!f.step(dashboard.request).active && !f.step(dashboard.request).local,
        "a fresh acknowledgement cannot revive input without a new native request");
    assert(!f.manager.logicalOpen, "field loss clears Steam's retained keyboard-open state");
    assert(!f.bridge.send(dashboard.request, "dashboard"),
        "dashboard input cannot use the former Steam field or controller callback");
    assert.equal(f.deliveries.length, 0);
    const stock = [];
    f.manager.m_fnVROnTextEnteredOverride = value => stock.push(value);
    f.manager.HandleVirtualKeyDown("ArrowDown");
    assert.deepEqual(stock, ["ArrowDown"], "dashboard navigation is no longer suppressed as field input");
    f.manager.m_currentVirtualKeyboardRef = undefined;
    f.manager.m_ActiveElementProps = undefined;
    assert.equal(f.step(dashboard.request).request, dashboard.request,
        "missing field metadata cannot repeatedly change the dashboard generation");
    f.newTarget(); f.manager.SetVirtualKeyboardShownInternal(true);
    const next = f.step();
    assert(next.request !== dashboard.request && next.eligible && !next.local && !next.active,
        "a new field focus restores Steam routing with fresh acknowledgement");
    assert(f.step(next.request).active && f.bridge.send(next.request, "new field"));
    f.bridge.dispose();
}
for (const production of [false, true]) {
    for (const beforePoll of [false, true]) {
        const f = fixture(false, production); f.newTarget(); const previous = owned(f);
        f.manager.m_currentVirtualKeyboardRef = null;
        f.manager.m_ActiveElementProps = null;
        if (!beforePoll) {
            const dashboard = f.step(previous);
            assert(!f.step(dashboard.request).active && !dashboard.local && !dashboard.open);
        }
        // A UI hitch can expire the lease before inspect observes the lost
        // field. The fallback must not restore an empty, modal native session.
        f.advance(901);
        assert(!f.nativeOpen() && !f.manager.logicalOpen && !f.bridge.diagnostics().eligible,
            "expired field ownership closes instead of restoring a missing target");
        assert(!f.calls.includes("native-show") && !f.calls.includes("window-show"));
        f.bridge.dispose();
    }
}
{
    const f = fixture(); f.newTarget(); const target = f.manager.m_currentVirtualKeyboardRef;
    const generation = owned(f);
    assert.equal(f.manager.m_currentVirtualKeyboardRef, target, "physical close preserves Steam field target");
    assert(f.manager.logicalOpen);
    assert(f.bridge.dismiss(generation));
    assert(!f.step(generation).open && !f.nativeOpen() && !f.manager.logicalOpen);
    assert.equal(f.manager.m_currentVirtualKeyboardRef, null, "explicit X runs target cleanup");
    for (let i = 0; i < 15; ++i) { f.status({bIsOpen: false}); assert(!f.step(generation).open); }
    assert(!f.calls.includes("native-show"), "X cannot start a show/hide loop");
    f.newTarget(); // A newly focused supported field can acquire another request.
    f.status({bIsOpen: true}); let s = f.step();
    assert(s.request !== generation && f.step(s.request).active);
    assert(!f.bridge.dismiss(generation), "stale dismissal cannot close a later request");
    f.bridge.dispose();
}
{
    const f = fixture(); const generation = owned(f);
    f.status({bIsOpen: true}); // Same-target native keyboard button toggles ours off.
    assert(!f.step(generation).open && !f.nativeOpen());
    f.status({bIsOpen: true}); const s = f.step();
    assert(s.request !== generation && !f.step(s.request).active && f.nativeOpen(),
        "a button press without a new field callback leaves the keyboard native");
    f.bridge.dispose();
}
{
    const f = fixture(); let generation = owned(f);
    f.newTarget(); const s = f.step(generation);
    assert(s.request !== generation && !s.active, "same-overlay field change needs fresh readiness");
    assert(!f.bridge.send(generation, "stale"));
    assert(f.step(generation, true, true).eligible, "old escape cannot affect new target");
    generation = s.request; assert(f.step(generation).active);
    f.manager.m_ActiveElementProps.onEnterKeyPress = () => "VKClose";
    assert(f.bridge.send(generation, "Enter", true));
    assert(!f.step(generation).open && !f.nativeOpen(), "Steam field Enter can close the retained session");
    f.bridge.dispose();
}
{
    const f = fixture(); f.newTarget(); const target = f.manager.m_currentVirtualKeyboardRef;
    const generation = owned(f);
    f.step(generation, true, true);
    assert(f.nativeOpen() && !f.step(generation).eligible);
    assert.equal(f.manager.m_currentVirtualKeyboardRef, target, "escape preserves exact field target");
    assert.equal(f.nodes.size, 0);
    assert(!f.step(generation).active, "escape cannot retake its restored native session");
    f.bridge.dispose();
}
{
    const f = fixture(); f.step(); f.status({bIsOpen: true}); const s = f.step();
    f.advance(401);
    assert(f.nativeOpen() && !f.step(s.request).active, "late readiness restores native once");
    assert.equal(f.calls.filter(c => c === "native-show").length, 1);
    f.bridge.dispose();
}
{
    const f = fixture(); const generation = owned(f);
    f.advance(901);
    assert(f.nativeOpen() && !f.step(generation).active, "independent lease restores usable native");
    assert.equal(f.nodes.size, 0);
    assert(!f.bridge.send(generation, "late"));
    f.bridge.dispose();
}
{
    const f = fixture(); const generation = owned(f);
    for (let i = 0; i < 5; ++i) { f.advance(200); f.step(generation, true, false, false); }
    assert(f.nativeOpen() && !f.step(generation).active, "helper traffic cannot renew render-loop health");
    f.bridge.dispose();
}
for (const values of [
    {sOverlayKey: "gamescope.gamescope-0.window.72"},
    {sOverlayKey: "unknown.overlay"},
    {bDispatchEventsToSteamVR: true, bMinimal: false},
    {sOverlayKey: "valve.steam.gamepadui.barpopup"},
]) {
    const f = fixture(); f.step(); f.status({bIsOpen: true, ...values});
    assert(f.nativeOpen() && !f.step().eligible, "unrestorable/external targets remain native");
    assert(!f.calls.includes("native-hide"));
    assert.equal(f.nodes.size, 0);
    f.bridge.dispose();
}
{
    const f = fixture(true);
    assert(!f.step().eligible && f.nativeOpen(), "leave an already-open native session alone");
    f.status({bIsOpen: false}); f.newTarget(); f.status({bIsOpen: true}); const s = f.step();
    assert(f.step(s.request).active);
    f.bridge.dispose();
}
{
    const f = fixture(); f.manager.m_fnVROnTextEnteredOverride = null;
    f.step(); f.status({bIsOpen: true}); assert(!f.step().eligible);
    f.advance(60); f.manager.m_fnVROnTextEnteredOverride = () => {}; f.advance(50);
    const s = f.step(); assert(s.eligible && f.step(s.request).active, "wait briefly for renderer callback");
    f.bridge.dispose();
}
{
    const f = fixture(); f.deferHide(true); f.step(); f.status({bIsOpen: true}); const s = f.step();
    assert(f.nativeOpen() && !s.eligible && !f.step(s.request).active,
        "never show ours while native close has not completed");
    f.advance(401);
    assert(f.nativeOpen() && !f.step(s.request).eligible, "failed close leaves stock usable");
    assert.equal(f.calls.filter(c => c === "native-hide").length, 1, "failed close does not retry in a loop");
    f.bridge.dispose();
}
{
    const f = fixture(); f.deferHide(true); f.step(); f.status({bIsOpen: true}); const s = f.step();
    f.status({sOverlayKey: "gamescope.other.window"});
    assert(f.nativeOpen() && !f.step(s.request).eligible,
        "a different provider opening before close acknowledgement must remain native");
    assert(!f.calls.includes("native-show"), "never restore the old provider over a new native request");
    f.bridge.dispose();
}
{
    const f = fixture(); const other = () => 42;
    f.api.ShowWindow = other; f.bridge.dispose();
    assert.equal(f.api.ShowWindow, other, "cleanup preserves another tool's later hook");
}
// Reproducible event walks combine real production hooks with independent safety
// properties. Include the seed/trace on failure so every discovered sequence can
// become a focused regression. No real endpoint or user text is involved.
let deadlineChecks = 0, laterDeadlineChecks = 0;
for (let seed = 1; seed <= 128; ++seed) {
    let random = seed, previousRequest = 0;
    const next = n => { random = (Math.imul(random, 1664525) + 1013904223) >>> 0; return random % n; };
    const f = fixture(false, seed % 2 === 0, true, seed % 3 === 0), trace = [];
    const opened = key => f.status({bIsOpen: true, sOverlayKey: key,
        bMinimal: true, bDispatchEventsToSteamVR: false});
    // Keep the deadline in the test, independently of the bridge's timer. A new
    // target gets its own deadline; readiness heartbeats cannot extend it.
    let pending;
    const observeRequest = () => {
        const s = f.bridge.diagnostics();
        if (s.active || !(s.eligible || s.closingNative || s.externalCandidate)) pending = null;
        else if (pending?.request !== s.request) pending = {request: s.request, until: f.now() + 400};
    };
    const advanceAndCheck = (ms, later) => {
        const due = pending && f.now() + ms >= pending.until;
        f.advance(ms);
        if (due) {
            const s = f.bridge.diagnostics();
            assert(!s.expectedClose || s.closingNative,
                "the one-shot close marker belongs only to an acquiring request");
            f.checkOwnership();
            assert(!s.eligible && !s.closingNative && (f.nativeOpen() || s.externalRestore),
                "every unacknowledged request reaches native or exact-overlay recovery by its deadline");
            ++deadlineChecks;
            if (later) ++laterDeadlineChecks;
        }
    };
    try {
        f.step(); f.api.ShowWindow(); f.deferHide(next(2) === 0);
        opened([MAIN, "gamescope.gamescope-0.window.62", "unknown.overlay",
            "valve.steam.gamepadui.barpopup"][next(4)]);
        const candidate = f.bridge.diagnostics().externalCandidate;
        if (candidate && next(2)) f.bridge.prepareExternal(candidate.request, candidate.key);
        observeRequest();
        advanceAndCheck(401, false);
        const expiredRequest = f.bridge.diagnostics();
        assert(!expiredRequest.eligible && (f.nativeOpen() || expiredRequest.externalRestore),
            "an unacknowledged request reaches native or exact-overlay recovery by its deadline");
        observeRequest();
        for (let step = 0; step < 160 && f.alive(); ++step) {
            const op = next(17), before = f.bridge.diagnostics();
            trace.push(op);
            switch (op) {
            case 0: f.step(before.request); break;
            case 1: f.step(before.request - 1, true, false, false); break;
            case 2: f.step(0, false); break;
            case 3: opened(MAIN); break;
            case 4: opened("gamescope.gamescope-0.window.62"); break;
            case 5: opened("unknown.overlay"); break;
            case 6: opened("valve.steam.gamepadui.barpopup"); break;
            case 7: f.status({bIsOpen: false}); break;
            case 8: f.api.ShowWindow(); break;
            case 9: f.api.BringToFront(); break;
            case 10: f.newTarget(); break;
            case 11: f.deferHide(next(2) === 0); break;
            case 12: f.bridge.dismiss(before.request); break;
            case 13: f.step(before.request, true, true); break;
            case 14:
                if (before.externalCandidate)
                    f.bridge.prepareExternal(before.request - next(2), before.externalCandidate.key);
                break;
            case 15:
                f.newTarget(); f.manager.SetVirtualKeyboardShownInternal(true); break;
            case 16: advanceAndCheck([16, 50, 100, 401, 901][next(5)], true); break;
            }
            const s = f.bridge.diagnostics();
            assert(s.request >= previousRequest, "request identities never go backwards");
            f.checkOwnership();
            assert(!s.expectedClose || s.closingNative,
                "the one-shot close marker belongs only to an acquiring request");
            previousRequest = s.request;
            assert(!s.active || s.eligible && !f.nativeOpen(), "takeover never overlaps native presentation");
            if (f.bridge.expired()) assert.equal(f.nodes.size, 0, "expired leases leave nothing hidden");
            const delivered = f.deliveries.length;
            if (!s.active) assert(!f.bridge.send(s.request, "inactive fixture"), "inactive requests reject input");
            assert(!f.bridge.send(s.request + 1, "stale fixture"), "wrong request identities reject input");
            assert.equal(f.deliveries.length, delivered, "rejected dispatch cannot reach any target");
            if (step % 20 === 19) {
                f.step(0, false);
                assert(f.bridge.expired() && f.nodes.size === 0 && !f.bridge.diagnostics().active,
                    "readiness withdrawal always removes takeover and presentation suppression");
            }
            observeRequest();
        }
        f.bridge.dispose();
        assert(!f.bridge.diagnostics().expectedClose, "dispose cancels any expected close");
        f.checkOwnership();
        assert.equal(f.nodes.size, 0);
        assert(!f.alive() && !f.observing());
        assert.equal(f.manager.HandleVirtualKeyDown, f.oldKey);
        assert.equal(f.manager.SetVRKeyboardOpenForThisInstance, f.oldOpen);
        assert.equal(f.manager.SetVirtualKeyboardShownInternal, f.oldFieldShow);
        assert.equal(f.keyboard.Hide, f.oldHide);
        assert.equal(f.api.ShowWindow, f.oldShow);
        assert.equal(f.api.BringToFront, f.oldFront);
    } catch (error) {
        throw new Error(`seed ${seed}, events ${trace.join(",")}: ${error.message}`, {cause: error});
    } finally { f.bridge.dispose(); }
}
assert(laterDeadlineChecks > 0, "random walks must exercise deadlines after their initial request");
console.log(`Native bridge: lifecycle regressions, 128 seeded event sequences and ${deadlineChecks} request deadlines (${laterDeadlineChecks} after initial setup) passed.`);
