// Reader's words for the fake-worn guard chart (FakeWornGuard.tla). Three groups by what
// protects the host's keyboard and mouse; each draws as a dashed box. ft-stream's own steps (its
// tick, the helper's answers) only show where they lease or where the flag stops a lease.
const alive = (v) => v.flag === "guard" && v.guard === "holding";

const SAFE = "Wearer not faked";
const GUARDED = "Faked, the guard holds the lock";
const EXPOSED = "Faked or leased, no guard";

const IDLE = "Idle";
const CHECKING = "worn on: checking";
const STARTING = "worn on: starting a guard";
const READY = "Guard holds the lock, worn 1 next";
const GONE_READY = "Guard gone, worn 1 next";
const FLAG_LEFT = "Guard gone, flag left";
const WAITING = "Guard gone, a stream waits for the lock";
const NOFLAG_HELD = "No flag, guard still holds the lock";
const NOFLAG_FREE = "No flag, lock free";
const NOFLAG_CMD = "No flag, a command running";
const BLOCKED = "Guard gone, a stream holds the lock, the flag stops its lease";
const TAKEN = "ft-stream leases the host: host taken";

const where = (v) => {
  if (v.lease) return [EXPOSED, TAKEN];
  if (!v.faked) {
    if (v.pc === "on_check") return [SAFE, CHECKING];
    if (v.pc === "on_stop" || (v.pc === "on_wait" && !alive(v))) return [SAFE, STARTING];
    if ((v.pc === "on_wait" || v.pc === "on_fake") && alive(v)) return [SAFE, READY];
    if (v.pc === "on_fake") return [SAFE, GONE_READY];
    return [SAFE, IDLE];
  }
  if (alive(v)) return [GUARDED];
  if (v.flag !== "none")   // a dead guard's or a command's
    return [EXPOSED, v.stream === "holding" ? BLOCKED : v.stream === "live" ? WAITING : FLAG_LEFT];
  if (v.pc !== "idle") return [EXPOSED, NOFLAG_CMD];
  return [EXPOSED, v.lock === "guard" ? NOFLAG_HELD : NOFLAG_FREE];
};
const leaf = (v) => where(v).at(-1);

// The arrows the reader needs; every other step is hidden (listed in the hand-off).
const KEEP = new Set([
  [IDLE, CHECKING], [CHECKING, IDLE], [CHECKING, STARTING], [STARTING, IDLE], [STARTING, READY],
  [READY, GUARDED], [READY, NOFLAG_HELD], [READY, GONE_READY], [GONE_READY, FLAG_LEFT], [GONE_READY, NOFLAG_CMD],
  [GUARDED, IDLE], [GUARDED, FLAG_LEFT],
  [FLAG_LEFT, WAITING], [WAITING, BLOCKED], [BLOCKED, IDLE], [FLAG_LEFT, IDLE], [FLAG_LEFT, NOFLAG_FREE],
  [NOFLAG_HELD, NOFLAG_FREE], [NOFLAG_FREE, NOFLAG_CMD],
  [NOFLAG_CMD, IDLE], [NOFLAG_CMD, GUARDED], [NOFLAG_CMD, TAKEN], [TAKEN, IDLE],
].map((p) => p.join(" -> ")));

const RELEASED = "the wearer is released";

const DROPPED = "a release times out, the flag is dropped anyway";

const label = (action, from, to) => {
  const up = from.proxy === "up";
  if (!from.lease && to.lease) return "no flag, and the helper said worn: ft-stream leases";
  if (from.lease && !to.lease) return "ft-stream drops the lease";
  if (from.faked && !to.faked) return RELEASED;   // acted, even when its reply timed out
  if (from.faked && to.faked && from.flag !== "none" && to.flag === "none" && action !== "OnFake") return DROPPED;
  switch (action) {
    case "YouRunWornOn": return "you run worn on";
    case "YouRunWornOff": return "you run worn off";
    case "YouRunOther": return up ? RELEASED : "the next command's release times out";
    case "OnCheck":
      return to.pc === "idle" ? (from.stream !== "off" ? "a live stream refuses it" : "an unconfirmed release refuses it")
        : to.pc === "on_fake" ? "a guard is already up" : "no live guard";
    case "OnStartGuard": return "it starts a guard unit";
    case "OnWait": return to.pc === "on_fake" ? "the guard holds the lock" : "the lock was busy";
    case "GuardStarts": return to.guard === "holding" ? "the guard takes the lock" : "the lock was busy";
    case "OnFake": return up ? "worn 1 answers" : "worn 1 and its release time out";
    case "OnVerify": return up ? RELEASED : "the post-check's release times out";
    case "OffSend": return up ? RELEASED : "the release times out";
    case "GuardLetsGo": return "the flag went: the guard lets go";
    case "GuardKilled": return "the guard is killed";
    case "GuardStopped": return "the guard is stopped (SIGTERM)";
    case "GuardCleanup": return up && from.flag !== "none" ? RELEASED : "ExecStopPost runs";
    case "StreamStarts": return "a stream starts";
    case "StreamTakesLock": return "a stream takes the lock";
    case "StreamTick": return to.saidTest ? "the flag stops ft-stream's lease" : "ft-stream's tick";
    case "HelperAnswers": return "the helper answers";
    case "ProxyHangs": return "the proxy hangs";   // never changes a chart state, so never drawn
  }
  return null;   // ProxyRecovers, StopOldGuard, StreamEnds
};

export const title = "frame-testbench fake-worn guard vs a stream to an input host";

export const state = where;

const GROUPS = { [SAFE]: "Not faked", [GUARDED]: "Faked, guarded", [EXPOSED]: "Faked, unguarded" };
export const group = (v) => GROUPS[where(v)[0]];
export const colors = { "Not faked": "teal", "Faked, guarded": "blue", "Faked, unguarded": "orange" };

export const step = (action, from, to) =>
  leaf(from) === leaf(to) || KEEP.has(`${leaf(from)} -> ${leaf(to)}`) ? label(action, from, to) : null;
