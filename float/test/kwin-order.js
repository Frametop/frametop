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

function desktop(tileIs, without, area = FULL) {
    let now = 1000, command;
    const timers = [], sent = [], signal = new Signal();
    class QTimer {
        constructor() { this.timeout = new Signal(); timers.push(this); }
        start() { this.due = now + this.interval; }
        stop() { this.due = undefined; }
    }
    const screen = {name: "WL-0", geometry: {x: 0, y: 0, width: 1920, height: 1080}};
    const spare = {name: "WL-1", geometry: area};
    const w = {internalId: "w1", managed: true, normalWindow: true, output: spare, keepBelow: true, fullScreen: false,
               asked: null, frame: HOME, committed: null, requested: null};
    ["frameGeometryChanged", "outputChanged", "keepBelowChanged", "interactiveMoveResizeStarted", "interactiveMoveResizeFinished",
     "fullScreenChanged", "minimizedChanged", "maximizedChanged", "tileChanged"].forEach(n => { w[n] = new Signal(); });
    if (without) delete w[without];
    Object.defineProperty(w, "frameGeometry", {get: () => w.frame, set: g => { w.asked = g; }});
    Object.defineProperty(w, "clientGeometry", {get: () => w.frame});
    if (tileIs) Object.defineProperty(w, "tile", {get: () => w[tileIs], set: t => { w.requested = t; w.committed = t; w.asked = HALF; }});
    w.setMaximize = () => { w.asked = FULL; };  // (not the way back: asItWas asks last)
    const workspace = {windowList: () => [w], screens: [screen, spare], screensChanged: new Signal(), windowAdded: signal, windowRemoved: signal,
                       windowActivated: signal, clientArea: () => spare.geometry, sendClientToScreen() {}};
    const context = {workspace, KWin: {MaximizeArea: 2}, QTimer, Date: {now: () => now}, print() {}, registerUserActionsMenu() {},
                     callDBus: (s, p, i, method, arg) => { if (method === "Event") sent.push(JSON.parse(arg)); else command = arg; }};
    vm.createContext(context);
    vm.runInContext(fs.readFileSync(SCRIPT, "utf8"), context);
    command(JSON.stringify([{cmd: "config", screens: 1}]));
    sent.length = 0;
    return {
        w,
        outputsChange() {
            screen.geometry = {x: 0, y: 0, width: 2560, height: 1440};
            workspace.screensChanged.emit();
        },
        size(g) { w.frame = g; w.frameGeometryChanged.emit(); },
        place: () => command(JSON.stringify([{cmd: "place", id: "w1", output: "WL-1", x: 4300, y: 4300, w: 800, h: 600}])),
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
for (const mode of [3, undefined]) {  // (KWin 6.2 has no mode to read)
    const was = mode === undefined ? "KWin 6.2" : "KWin 6.6";
    let d = desktop("requested");
    d.w.maximizeMode = mode;
    d.size(FULL);
    d.w.maximizedChanged.emit();
    asBefore("maximized in " + was, d);
    d = desktop("requested", null, {x: 4000, y: 4000, width: 1400.36, height: 1200.27});
    d.w.maximizeMode = mode;
    d.size(FULL);
    d.w.maximizedChanged.emit();
    asBefore("...on an output of a fractional size", d);
    d = desktop("requested");
    d.size(FULL);
    d.turn();
    d.place();  // an app that opens maximized, floated: un-maximized at its remembered size in one go
    d.size(HOME);
    d.w.maximizeMode = mode && 0;
    d.w.maximizedChanged.emit();
    d.turn();
    check("un-maximized by being floated in " + was + ": its size is reported", d.sizes(), ["1400x1200", "800x600"]);
}
let d = desktop("requested");
d.w.maximizeMode = 3;
d.size(FULL);
d.w.maximizedChanged.emit();
d.size({x: 4300, y: 4300, width: 900, height: 650});  // an app that won't have the size it had
d.w.maximizeMode = 0;
d.w.maximizedChanged.emit();
d.turn(20);
check("refused the size it had: asked for once more", [d.w.asked, d.sizes()], [HOME, []]);
d.turn(1200);
check("...then the app has its way", d.sizes(), ["900x650"]);
d = desktop("requested");
d.size({x: 4300, y: 4300, width: 900, height: 650});
d.outputsChange();
check("an app's resize just before an output change: not undone", d.w.asked && d.w.asked.width, 900);
d.turn();
check("...and reported", d.sizes(), ["900x650"]);
d = desktop("requested");
d.size({x: 4310, y: 4300, width: 800, height: 600});
d.w.requested = {};
d.turn();
check("in a tile with its size still to come: reported as it is", [d.sizes(), d.w.asked], [["800x600"], HOME]);
d.size(HALF);
d.turn();
check("...and the tile's size isn't taken", [d.sizes(), d.w.asked.width], [["800x600"], 800]);
d = desktop("requested");
d.size({x: 4310, y: 4300, width: 800, height: 600});
d.w.requested = {};
d.w.tileChanged.emit();
d.turn();
check("...nor lost when the tile is told first", d.sizes(), ["800x600"]);
d = desktop(null, "tileChanged");
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
