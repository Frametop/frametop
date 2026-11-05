// Reader's words for the remote display chart (RemoteStreams.tla): one remote display N, the
// ft-stream that ft-screens runs for it, and any older one that hasn't gone yet.
const slots = (v) => Object.keys(v.used).map((k) => ({ used: v.used[k], proc: v.proc[k], restart: v.restart[k] }));

const OFF = "Not streaming";
const ON = "Streaming";
const BETWEEN = "Between streams";
const TWO = "Two ft-streams for one display";

const where = (v) => {
  const s = slots(v);
  const mine = s.find((x) => x.used);
  const old = s.find((x) => !x.used && x.proc !== "none");
  const olds = s.filter((x) => !x.used && x.proc !== "none").length;
  if (!mine && olds > 1) return [TWO, "Disconnected, two old streams still going"];
  if (!mine) return old ? [OFF, "Disconnected, its stream still going"] : [OFF, "Disconnected"];
  if (old && mine.proc === "starting" && old.proc === "releasing")
    return [TWO, "New stream connecting while the old one releases its Remote Monitor"];
  if (old && mine.proc !== "none") return [TWO, "New stream up, the old one not gone yet"];
  if (old) return [BETWEEN, "Start due, the old stream still going"];
  if (mine.proc === "starting") return [ON, "Connecting"];
  if (mine.proc === "live") return [ON, "Live"];
  if (mine.proc === "none") return [BETWEEN, "Start due"];
  return [BETWEEN, mine.restart ? "New settings: the old stream going" : "Ended: its stream going"];
};

export const state = (v) => where(v);
export const group = (v) => where(v)[0];
export const colors = { [OFF]: "teal", [ON]: "blue", [BETWEEN]: "yellow", [TWO]: "orange" };
export const title = "Frametop remote display: ft-screens' ft-stream for one display (screens/remote.c)";

const T1 = "New stream connecting while the old one releases its Remote Monitor";
const T2 = "New stream up, the old one not gone yet";

export const step = (action, from, to) => {
  const a = where(from).at(-1), b = where(to).at(-1);
  // Hidden (the hand-off lists them): from T1 only the old stream's release is drawn, and from
  // T2 only the reap that leaves the new stream live.
  if (a === T1 && b === T2 && action !== "ReleaseDone") return null;
  if (a === T2 && action === "Reaped" && b !== "Live") return null;
  switch (action) {
    case "YouStart": {
      const had = slots(from).some((x) => x.used);
      return had ? "its settings change (one start)" : "you connect it";
    }
    case "YouStop": return "you disconnect it";
    case "TickStarts": return "its stream starts";
    case "StreamLive": return "the picture comes";
    case "CantConnect": return "it can't connect";
    case "StreamEnds": return "the connection ends";
    case "ReleaseDone": return "the old stream releases its Remote Monitor";
    case "Reaped": return "the old stream's exit is reaped";
    default: return action;
  }
};
