// kwin-order-test.py's KWin: one floating window (800x600 on a spare with a 300 px margin), with the order
// KWin's signals come in (Plasma 6.2 and 6.6, read from their sources; 6.6's maximize seen live, 2026-11-25).
const fs = require("fs"), vm = require("vm"), path = require("path");
const SCRIPT = path.join(__dirname, "..", "frametop-float.js");
let failed = 0;
function check(what, got, want) {
    const ok = JSON.stringify(got) === JSON.stringify(want);
    failed += !ok;
    console.log((ok ? "ok    " : "FAIL  ") + what + (ok ? "" : ": got " + JSON.stringify(got) + ", want " + JSON.stringify(want)));
}
class Signal {
    constructor() { this.slots = []; }
    connect(f) { this.slots.push(f); }
    emit(...a) { this.slots.slice().forEach(f => f(...a)); }
}
const HOME = {x: 4300, y: 4300, width: 800, height: 600};
const HALF = {x: 4000, y: 4000, width: 700, height: 1200}, FULL = {x: 4000, y: 4000, width: 1400, height: 1200};

function desktop(tileIs, without) {
    let now = 1000, command;
    const timers = [], sent = [], signal = new Signal();
    class QTimer {
        constructor() { this.timeout = new Signal(); timers.push(this); }
        start() { this.due = now + this.interval; }
        stop() { this.due = undefined; }
    }
    const screen = {name: "WL-0", geometry: {x: 0, y: 0, width: 1920, height: 1080}};
    const spare = {name: "WL-1", geometry: FULL};
    const w = {internalId: "w1", managed: true, normalWindow: true, output: spare, keepBelow: true, fullScreen: false,
               asked: null, frame: HOME, committed: null, requested: null};
    ["frameGeometryChanged", "outputChanged", "keepBelowChanged", "interactiveMoveResizeStarted", "interactiveMoveResizeFinished",
     "fullScreenChanged", "minimizedChanged", "maximizedChanged", "tileChanged"].forEach(n => { w[n] = new Signal(); });
    if (without) delete w[without];
    Object.defineProperty(w, "frameGeometry", {get: () => w.frame, set: g => { w.asked = g; }});
    Object.defineProperty(w, "clientGeometry", {get: () => w.frame});
    if (tileIs) Object.defineProperty(w, "tile", {get: () => w[tileIs], set: t => { w.requested = t; w.committed = t; w.asked = HALF; }});
    w.setMaximize = () => { w.asked = FULL; };  // (not the way back: asItWas asks last)
    const workspace = {windowList: () => [w], screens: [screen, spare], screensChanged: signal, windowAdded: signal, windowRemoved: signal,
                       windowActivated: signal, clientArea: () => FULL, sendClientToScreen() {}};
    const context = {workspace, KWin: {MaximizeArea: 2}, QTimer, Date: {now: () => now}, print() {}, registerUserActionsMenu() {},
                     callDBus: (s, p, i, method, arg) => { if (method === "Event") sent.push(JSON.parse(arg)); else command = arg; }};
    vm.createContext(context);
    vm.runInContext(fs.readFileSync(SCRIPT, "utf8"), context);
    command(JSON.stringify([{cmd: "config", screens: 1}]));
    sent.length = 0;
    return {
        w,
        size(g) { w.frame = g; w.frameGeometryChanged.emit(); },
        turn(ms = 0) {
            now += ms;
            timers.forEach(t => { if (t.due <= now) { t.due = undefined; t.timeout.emit(); } });
        },
        sizes: () => sent.filter(e => e.ev === "geometry" || e.ev === "fullscreen").map(e => e.frame.w + "x" + e.frame.h),
    };
}
function asBefore(what, d) {
    d.turn();
    check(what + ": not reported", d.sizes(), []);
    check("...and asked for as it was", d.w.asked, HOME);
    d.size(HOME);
    d.turn(20);
    check("...which it is", d.sizes().filter(s => s !== "800x600"), []);
    d.turn(2000);
    check("...and stays", d.sizes().filter(s => s !== "800x600"), []);
}

for (const ms of [10, 1500]) {  // (the app's answer)
    let d = desktop("committed");
    d.turn(ms);
    d.size(HALF);
    d.w.committed = {};
    d.w.tileChanged.emit();
    asBefore("KWin 6.2's snap (the size, then the tile) after " + ms + " ms", d);
    d = desktop("requested");
    d.w.requested = {};
    d.turn(ms);
    d.size(HALF);
    d.w.tileChanged.emit();
    asBefore("KWin 6.6's snap (the tile, then the size) after " + ms + " ms", d);
    d = desktop("requested");
    d.turn(ms);
    d.size(FULL);
    d.w.maximizedChanged.emit();
    asBefore("maximized (the size, then the mode) after " + ms + " ms", d);
}
let d = desktop(null, "tileChanged");
d.size({x: 4300, y: 4300, width: 900, height: 650});
check("a resize isn't taken in the middle of KWin's own work", d.sizes(), []);
d.turn();
check("...and is a turn later (no tiles in this KWin)", d.sizes(), ["900x650"]);
d = desktop("requested");
d.w.requested = {};
d.w.tileChanged.emit();  // (snapped: asked for as it was, for a second)
d.size(FULL);
d.w.fullScreen = true;
d.w.fullScreenChanged.emit();
d.w.asked = null;
d.turn();
check("full screen just after a snap key: its size is taken", [d.w.asked, d.sizes()], [null, ["1400x1200", "1400x1200"]]);

process.exit(failed ? 1 : 0);
