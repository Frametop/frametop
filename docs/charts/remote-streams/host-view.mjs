// Reader's words for the host-input chart (HostInputLease.tla): who has the computer's keyboard
// and mouse, and what ft-stream knows about the headset.
const HOST = "The computer has its input";
const FRAME = "Frametop has the computer's input";
const ESC = "Escaped: the computer keeps its input";

const head = (v) => (v.worn ? "headset on" : "headset off");
const waiting = (v) => {
  const n = Object.keys(v.inbox).length;
  if (!n) return "";
  const a = v.inbox[n];
  return `, ${a.old ? "an old" : "an"} answer (${a.w ? "worn" : "away"}) waits unread`;
};

const sw = (v) => (v.switchFile ? "" : ", switch off");

const where = (v) => {
  const g = v.esc ? ESC : v.engaged ? FRAME : HOST;
  if (v.proc === "none") return [g, `No stream, ${head(v)}${sw(v)}${v.esc && v.mark ? ", escape kept for the next stream" : ""}`];
  if (v.proc === "pre") return [g, `Stream connecting, ${head(v)}${sw(v)}`];
  if (!v.switchOn)
    return [g, v.switchFile ? `Switch on, not seen yet, ${head(v)}${waiting(v)}` : `Switch off, ${head(v)}${waiting(v)}`];
  if (!v.switchFile) return [g, `Switch off, not seen yet, ${head(v)}${waiting(v)}`];
  if (v.engaged) {
    if (v.basisOld && !v.worn) return [g, "Leasing on an old 'worn' answer, headset off"];
    return [g, v.worn ? "Leasing, headset on" : "Leasing, headset just came off"];
  }
  if (v.esc && v.leasing) return [g, `Leasing, the computer refuses, ${head(v)}`];
  if (!v.locked || !v.fresh) return [g, `Live, asking about the headset, ${head(v)}${waiting(v)}`];
  return [g, `Live, not leasing, ${head(v)}${waiting(v)}`];
};

export const state = (v) => where(v);
export const group = (v) => where(v)[0];
export const colors = { [HOST]: "teal", [FRAME]: "blue", [ESC]: "orange" };
export const title = "Frametop host input: ft-stream's lease on a computer's keyboard and mouse (stream/ft-stream.cpp)";

export const step = (action, from, to) => {
  switch (action) {
    case "HeadsetOff": return "you take the headset off";
    case "HeadsetOn": return "you put the headset on";
    case "SwitchOff": return "you turn the switch off";
    case "SwitchOn": return "you turn the switch on";
    case "HostEscape": return "someone at the computer presses the escape hotkey";
    case "Age": return "3 s pass";
    case "HelperAnswers": return `the pointer helper answers "${from.worn ? "worn" : "away"}"`;
    case "StreamStarts": return "a stream starts";
    case "StreamLive": return "the stream goes live";
    case "StreamEnds": return "the stream ends";
    case "ReadSwitch": return to.switchOn ? "ft-stream sees the switch on" : "ft-stream sees the switch off";
    case "TickIdle": return "ft-stream waits for live";
    case "TickLive":
      if (from.esc && !to.esc) return to.leasing ? "ft-stream releases (the escape lifts), then leases" : "ft-stream releases (the escape lifts)";
      if (to.engaged && !from.engaged) return "ft-stream reads the answer and leases";
      if (to.leasing && !from.leasing) return "ft-stream reads the answer and leases";
      if (from.leasing && !to.leasing) return "ft-stream reads the answer and stops leasing";
      return from.locked ? "ft-stream asks the helper" : "ft-stream takes the lock and asks the helper";
    default: return action;
  }
};
