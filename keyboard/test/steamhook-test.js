// steamhook.js in a fake Steam window, under node (test_steamhook.py runs it):
//   node keyboard/test/steamhook-test.js keyboard/host/steamhook.js
// Steam's parts are stand-ins with the names the hook reaches for: SteamUIStore's windows and
// their VirtualKeyboardManager (its show on the prototype, as Steam's is), a document with
// execCommand and a selection, vrGamepadInput's focus steps.
"use strict";
const fs = require("fs");
const assert = require("assert");
const install = eval(fs.readFileSync(process.argv[2], "utf8"));

let clock = 0, failures = 0;
const timers = [];
function check(what, fn) {
    try { fn(); console.log(`ok    ${what}`); }
    catch (e) { failures++; console.log(`FAIL  ${what}: ${e.message}`); }
}

class KeyboardEvent { constructor(type, init) { this.type = type; Object.assign(this, init); } }
function makeDoc() {
    const doc = {
        visibilityState: "visible", defaultView: {closed: false, KeyboardEvent}, commands: [], activeElement: null,
        execCommand(cmd, ui, value) {
            this.commands.push(value === null || value === undefined ? cmd : `${cmd}:${value}`);
            if (cmd === "insertText") this.activeElement.value += value;
            return true;
        },
        getSelection() { return {isCollapsed: true, modify: (...a) => doc.commands.push(`modify:${a.join(",")}`)}; },
        querySelectorAll() { return []; },
    };
    return doc;
}
function makeField(doc, type = "text", tag = "INPUT") {
    return {tagName: tag, type, isConnected: true, ownerDocument: doc, disabled: false, readOnly: false, value: "",
        events: [], dispatchEvent(ev) { this.events.push(ev.type); return true; },
        form: {requestSubmit() { doc.commands.push("submit"); }}};
}
class Manager {
    constructor(key, doc) { this.m_Instance = {GetVROverlayKey: () => key}; this.m_ownerWindow = {document: doc}; this.shown = []; }
    SetVirtualKeyboardShownInternal(open) { this.shown.push(open); }
}
const gamepadCalls = [];
const root = {
    performance: {now: () => clock},
    setInterval: fn => { timers.push(fn); return timers.length; },
    clearInterval: () => {},
    SteamUIStore: {WindowStore: {SteamUIWindows: []}},
    vrGamepadInput: {FocusItemUnderMouse(tree) { gamepadCalls.push(tree); return "moved"; }},
};
const doc = makeDoc(), main = new Manager("valve.steam.gamepadui.main", doc);
const otherDoc = makeDoc(), other = new Manager("valve.steam.gamepadui.keyboard", otherDoc);
root.SteamUIStore.WindowStore.SteamUIWindows.push({VirtualKeyboardManager: main}, {VirtualKeyboardManager: other});
const api = () => root.__frametopKeyboardV1;
const focusStep = root.vrGamepadInput.FocusItemUnderMouse;

check("it goes in", () => {
    assert.strictEqual(install(root, 1500), true);
    assert.deepStrictEqual(api().poll(true), {events: [], session: null});
});

const field = makeField(doc);
doc.activeElement = field;
check("a Steam field's show opens Full Keyboard, not Steam's", () => {
    main.SetVirtualKeyboardShownInternal(true);
    assert.deepStrictEqual(main.shown, []);
    assert.deepStrictEqual(api().poll(true).events,
        [{open: 1, purpose: "text", key: "valve.steam.gamepadui.main", multiline: false}]);
});
check("...a second show for the same field: no second session", () => {
    main.SetVirtualKeyboardShownInternal(true);
    assert.deepStrictEqual(api().poll(true), {events: [], session: 1});
});
check("typing goes in with execCommand, keys with keydown and keyup", () => {
    assert.strictEqual(api().type(1, "hi"), true);
    assert.strictEqual(field.value, "hi");
    assert.strictEqual(api().key(1, "Backspace", []), true);
    assert.strictEqual(api().key(1, "ArrowLeft", ["Shift", "Ctrl"]), true);
    assert.strictEqual(api().key(1, "Enter", []), true);
    assert.deepStrictEqual(doc.commands, ["insertText:hi", "delete", "modify:extend,backward,word", "submit"]);
    assert.deepStrictEqual(field.events, ["keydown", "keyup", "keydown", "keyup", "keydown", "keyup"]);
    assert.strictEqual(api().type(2, "x"), false);   // another session's
});
check("the gamepad's focus step leaves the field alone", () => {
    const tree = {Root: {Element: {contains: e => e === field}}};
    assert.strictEqual(root.vrGamepadInput.FocusItemUnderMouse(tree), undefined);
    assert.deepStrictEqual(gamepadCalls, []);
    assert.strictEqual(root.vrGamepadInput.FocusItemUnderMouse({Root: {Element: {contains: () => false}}}), "moved");
});
check("the field losing focus ends it, and typing stops", () => {
    doc.activeElement = null;
    assert.deepStrictEqual(api().poll(true).events, [{close: 1, reason: "blur"}]);
    assert.strictEqual(api().type(1, "x"), false);
});
check("Steam hiding its keyboard ends the session, and the hide goes through", () => {
    doc.activeElement = field;
    main.SetVirtualKeyboardShownInternal(true);
    main.SetVirtualKeyboardShownInternal(false);
    assert.deepStrictEqual(api().poll(true).events.map(e => e.open || e.reason), [2, "steam"]);
    assert.deepStrictEqual(main.shown, [false]);
    main.shown.length = 0;
});
check("a password field says so; a number field keeps Steam's keyboard", () => {
    const pw = makeField(doc, "password");
    doc.activeElement = pw;
    main.SetVirtualKeyboardShownInternal(true);
    assert.strictEqual(api().poll(true).events[0].purpose, "password");
    doc.activeElement = makeField(doc, "number");
    main.SetVirtualKeyboardShownInternal(true);
    assert.deepStrictEqual(main.shown, [true]);
    main.shown.length = 0;
    api().close(3);
});
// Opens a session for element in main's window; returns its open event.
function open(element) {
    doc.activeElement = element;
    main.SetVirtualKeyboardShownInternal(true);
    const events = api().poll(true).events;
    assert.strictEqual(events.length, 1);
    assert.ok(events[0].open, JSON.stringify(events));
    return events[0];
}
check("each field says what it's for", () => {
    const purposes = [["email", {inputMode: "email"}], ["url", {type: "url"}], ["search", {type: "search"}],
                      ["number", {type: "tel"}], ["number", {inputMode: "numeric"}], ["text", {type: "text"}]];
    for (const [purpose, extra] of purposes) {
        const event = open(Object.assign(makeField(doc), extra));
        assert.strictEqual(event.purpose, purpose, JSON.stringify(extra));
        assert.strictEqual(event.multiline, false);
        api().close(event.open);
    }
    assert.deepStrictEqual(main.shown, []);
});
check("the other editing keys", () => {
    const one = makeField(doc), two = makeField(doc);
    for (const f of [one, two]) { f.tabIndex = 0; f.getClientRects = () => [{}]; f.focus = () => { doc.activeElement = f; }; }
    doc.querySelectorAll = () => [one, two];
    const {open: id} = open(one);
    doc.commands.length = 0;
    for (const [name, mods] of [["Delete", []], ["Delete", ["Ctrl"]], ["Home", []], ["End", ["Shift"]],
                                ["KeyA", ["Ctrl"]], ["KeyC", ["Ctrl"]], ["KeyX", ["Ctrl"]], ["KeyV", ["Ctrl"]],
                                ["F5", []]])
        assert.strictEqual(api().key(id, name, mods), true, name);
    assert.deepStrictEqual(doc.commands, ["forwardDelete", "modify:extend,forward,word", "forwardDelete",
        "modify:move,backward,lineboundary", "modify:extend,forward,lineboundary", "selectAll", "copy", "cut", "paste"]);
    assert.strictEqual(api().key(id, "Tab", []), true);
    assert.strictEqual(doc.activeElement, two);   // focus moved on: that ends this field's session
    assert.deepStrictEqual(api().poll(true).events, [{close: id, reason: "blur"}]);
    const {open: next} = open(two);
    assert.strictEqual(api().key(next, "Escape", []), true);
    assert.deepStrictEqual(api().poll(true).events, [{close: next, reason: "escape"}]);
    assert.strictEqual(api().type(next, "x"), false);
    doc.querySelectorAll = () => [];
});
check("Enter: a new line in a textarea or contentEditable, a submit in a one-line field", () => {
    const area = makeField(doc, "", "TEXTAREA");
    let event = open(area);
    assert.strictEqual(event.multiline, true);
    doc.commands.length = 0;
    api().key(event.open, "Enter", []);
    assert.deepStrictEqual(doc.commands, ["insertText:\n"]);
    api().close(event.open);
    const editor = Object.assign(makeField(doc, "", "DIV"), {isContentEditable: true});
    event = open(editor);
    assert.strictEqual(event.multiline, true);
    doc.commands.length = 0;
    api().key(event.open, "Enter", []);
    api().key(event.open, "Enter", ["Shift"]);
    assert.deepStrictEqual(doc.commands, ["insertParagraph", "insertLineBreak"]);
    api().close(event.open);
    assert.deepStrictEqual(main.shown, []);
});
check("a Steam window whose overlay key comes late is hooked once it does", () => {
    let key;
    const lateDoc = makeDoc(), late = new Manager(undefined, lateDoc);
    late.m_Instance.GetVROverlayKey = () => key;
    root.SteamUIStore.WindowStore.SteamUIWindows.push({VirtualKeyboardManager: late});
    api().poll(true);
    lateDoc.activeElement = makeField(lateDoc);
    late.SetVirtualKeyboardShownInternal(true);
    assert.deepStrictEqual(late.shown, [true]);   // not Steam's own yet: Steam's keyboard
    key = "valve.steam.gamepadui.appoverlay.7";
    api().poll(true);
    late.SetVirtualKeyboardShownInternal(true);
    assert.deepStrictEqual(late.shown, [true]);   // now Full Keyboard's
    const events = api().poll(true).events;
    assert.strictEqual(events[0].key, key);
    api().close(events[0].open);
    root.SteamUIStore.WindowStore.SteamUIWindows.pop();
});
check("while fields may not take it, and in a window that isn't Steam's own: Steam's keyboard", () => {
    doc.activeElement = field;
    api().poll(false);
    main.SetVirtualKeyboardShownInternal(true);
    otherDoc.activeElement = makeField(otherDoc);
    api().poll(true);
    other.SetVirtualKeyboardShownInternal(true);
    assert.deepStrictEqual([main.shown, other.shown], [[true], [true]]);
    assert.deepStrictEqual(api().poll(true).events, []);
    main.shown.length = 0;
});
check("no renewal for its lease: it takes itself out", () => {
    clock += 1600;
    for (const t of timers) t();
    assert.strictEqual(api(), undefined);
    assert.ok(!Object.prototype.hasOwnProperty.call(main, "SetVirtualKeyboardShownInternal"));
    assert.strictEqual(root.vrGamepadInput.FocusItemUnderMouse, focusStep);
    main.SetVirtualKeyboardShownInternal(true);
    assert.deepStrictEqual(main.shown, [true]);
});
check("installing again replaces the one before", () => {
    install(root, 1500);
    const first = api();
    install(root, 1500);
    assert.notStrictEqual(api(), first);
    assert.strictEqual(first.poll(true), null);
    api().dispose();
});

if (failures) { console.log(`${failures} failed`); process.exit(1); }
console.log("all passed");
