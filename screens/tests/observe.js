// A KWin script for experiments (screens/tests/headless.sh script ...): prints what happens
// to windows, for checking the script API floating windows rely on.
function desc(w) {
    const g = w.frameGeometry;
    return String(w.internalId).slice(1, 9) + " " + w.resourceClass + " pid=" + w.pid + " popup=" + w.popupWindow +
        " transient=" + w.transient + " normal=" + w.normalWindow + " dialog=" + w.dialog +
        " out=" + (w.output ? w.output.name : "?") + " geo=" + g.x + "," + g.y + " " + g.width + "x" + g.height +
        " '" + w.caption + "'";
}
function watch(w) {
    print("added " + desc(w));
    w.interactiveMoveResizeStarted.connect(() => print("move-start move=" + w.move + " resize=" + w.resize + " " + desc(w)));
    w.interactiveMoveResizeStepped.connect(g => print("move-step " + g.x + "," + g.y + " " + g.width + "x" + g.height));
    w.interactiveMoveResizeFinished.connect(() => print("move-end " + desc(w)));
    w.outputChanged.connect(() => print("output " + desc(w)));
    w.fullScreenChanged.connect(() => print("fullscreen " + w.fullScreen + " " + desc(w)));
    w.minimizedChanged.connect(() => print("minimized " + w.minimized));
}
workspace.windowList().forEach(watch);
workspace.windowAdded.connect(watch);
workspace.windowRemoved.connect(w => print("removed " + desc(w)));
workspace.windowActivated.connect(w => print("activated " + (w ? w.resourceClass : "none")));
