------------------------------ MODULE G7Release ------------------------------
\* G7 at 78e93b1: what a Frametop install leaves behind, release after release.
\* A release is a folder ~/.local/share/frametop/releases/VERSION copied out of its image, with
\* its own container frametop-<id12> (pack/install-release.sh). Its units point into that
\* folder (@REPO@) and that container (scripts/in-box reads BOX from the tree's .frametop-release).
\* install-release.sh copies the folder (atomically: VERSION.new, then mv), runs the folder's
\* install.sh (12 steps under set -e, each writing its units), and only then moves the
\* "current" link and removes every release but the new one and the one current named before.
\* scripts/fast-install patches the tree "current" names and restarts the units using it.
\* Two units stand for the rest: core (relay, pointer, power, screens: always installed) and
\* hands (or voice: optional, skipped by --no-hands, an unticked box, or --yes with no sudo).
\* Left out: the image load and its sha256 check (exit 3, nothing written), the SteamOS table,
\* the source checkout's install.sh (it acts as a release tree here), uninstall.sh, the models
\* (pinned and checked per file, shared or per tree, see the chart), two installs at once.
EXTENDS Naturals, FiniteSets

CONSTANTS
    N,            \* releases made, one after another (3: enough for "two releases on")
    Skips,        \* a later install may skip the optional part (W2)
    Rollback,     \* someone runs an older release's install.sh to go back to it (W1),
                  \* as pack/README.md step 5 says
    Interrupts,   \* one install.sh stops mid-way (a failing step under set -e, a power-off)
    LocalBuild,   \* releases built locally: ft dev build from the working tree (W3)
    Patches,      \* fast-install puts commits on top of the live release (W4)
    FixCurrent,   \* W1 fix: install.sh points "current" at its own tree when it finishes
    FixSkip,      \* W2 fix: install.sh renews (or stops) a skipped part's units that point into
                  \* another release, as it already does for the recorders' menu entries
    FixReinstall, \* W4 fix: installing a release whose folder exists copies the folder again
    FixLocal      \* W3 fix: a local pack refuses a tree that differs from the commit it names

R == 1..N
U == {"core", "hands"}
None == 0

VARIABLES
    made,     \* releases made so far (the next new one is made + 1)
    folder,   \* folder[r]: release r's folder and container exist
    content,  \* content[r]: "image" (as copied out) or "patched" (fast-install on top)
    dirty,    \* dirty[r]: r's image wasn't built from the commit its release JSON names
    unit,     \* unit[u]: the release whose folder and container u's unit file points into
    current,  \* the "current" link
    phase,    \* "idle", "run" (an install.sh running), "half" (one stopped mid-way)
    kind,     \* "release" (install-release.sh) or "tree" (a tree's own install.sh)
    target,   \* the tree that install.sh belongs to
    want,     \* the units that install run writes
    todo,     \* the ones it hasn't written yet
    wasThere, \* install-release found the release's folder already there (no copy)
    down,     \* the one interruption has happened
    lost,     \* a fast-install patched a tree no running unit uses, and reported success
    last      \* the last thing finished: "none", "new", "reinstall", "tree", "patch", "undo"
vars == <<made, folder, content, dirty, unit, current, phase, kind, target, want, todo,
          wasThere, down, lost, last>>

TypeOK ==
    /\ made \in 0..N
    /\ folder \in [R -> BOOLEAN] /\ dirty \in [R -> BOOLEAN]
    /\ content \in [R -> {"image", "patched"}]
    /\ unit \in [U -> R \cup {None}]
    /\ current \in R \cup {None}
    /\ phase \in {"idle", "run", "half"} /\ kind \in {"release", "tree"}
    /\ target \in R \cup {None} /\ want \subseteq U /\ todo \subseteq want
    /\ wasThere \in BOOLEAN /\ down \in BOOLEAN /\ lost \in BOOLEAN
    /\ last \in {"none", "new", "reinstall", "tree", "patch", "undo"}

Init ==
    /\ made = 0 /\ folder = [r \in R |-> FALSE] /\ content = [r \in R |-> "image"]
    /\ dirty = [r \in R |-> FALSE] /\ unit = [u \in U |-> None] /\ current = None
    /\ phase = "idle" /\ kind = "release" /\ target = None /\ want = {} /\ todo = {}
    /\ wasThere = FALSE /\ down = FALSE /\ lost = FALSE /\ last = "none"

\* The units an install run writes into tree r: core always; hands when picked, or (FixSkip)
\* when skipped but its unit points into another release.
Writes(r, skip) ==
    {"core"} \cup (IF ~skip \/ (FixSkip /\ unit["hands"] \notin {None, r}) THEN {"hands"} ELSE {})

\* install-release.sh VERSION: a new release (copied out, with its own container), or one whose
\* folder is there already (the same IMAGE_ID: install-release.sh 169-172), not copied again
\* (193) unless FixReinstall.
InstallRelease(r) ==
    /\ phase # "run"
    /\ \/ r = made + 1
       \/ folder[r]
    /\ \E skip \in (IF Skips THEN BOOLEAN ELSE {FALSE}),
          d \in (IF LocalBuild /\ ~FixLocal THEN BOOLEAN ELSE {FALSE}) :
          /\ want' = Writes(r, skip) /\ todo' = Writes(r, skip)
          /\ IF folder[r]
                THEN /\ dirty' = dirty
                     /\ content' = IF FixReinstall THEN [content EXCEPT ![r] = "image"] ELSE content
                ELSE /\ dirty' = [dirty EXCEPT ![r] = d]
                     /\ content' = [content EXCEPT ![r] = "image"]
    /\ made' = IF r = made + 1 THEN r ELSE made
    /\ wasThere' = folder[r]
    /\ folder' = [folder EXCEPT ![r] = TRUE]
    /\ phase' = "run" /\ kind' = "release" /\ target' = r
    /\ UNCHANGED <<unit, current, down, lost, last>>

\* RELEASES/OLD/install.sh run by hand, to go back to OLD (pack/README.md 202)
InstallTree(r) ==
    /\ Rollback /\ phase # "run" /\ folder[r] /\ r # current
    /\ \E skip \in (IF Skips THEN BOOLEAN ELSE {FALSE}) :
          want' = Writes(r, skip) /\ todo' = Writes(r, skip)
    /\ phase' = "run" /\ kind' = "tree" /\ target' = r /\ wasThere' = TRUE
    /\ UNCHANGED <<made, folder, content, dirty, unit, current, down, lost, last>>

\* one install.sh step: core's steps (3 to 7) come before hands' (8, 9)
Step ==
    /\ phase = "run" /\ todo # {}
    /\ LET u == IF "core" \in todo THEN "core" ELSE "hands" IN
          /\ unit' = [unit EXCEPT ![u] = target]
          /\ todo' = todo \ {u}
    /\ UNCHANGED <<made, folder, content, dirty, current, phase, kind, target, want, wasThere,
                   down, lost, last>>

\* a step fails under set -e, or the power goes: install-release.sh stops before its "current"
\* and clean-up lines (222-236)
Interrupt ==
    /\ Interrupts /\ ~down /\ phase = "run" /\ todo # {}
    /\ phase' = "half" /\ down' = TRUE
    /\ UNCHANGED <<made, folder, content, dirty, unit, current, kind, target, want, todo,
                   wasThere, lost, last>>

\* the same command again (its folder is there now: no copy, unless FixReinstall)
Rerun ==
    /\ phase = "half"
    /\ phase' = "run" /\ todo' = want
    /\ content' = IF FixReinstall /\ kind = "release" THEN [content EXCEPT ![target] = "image"]
                                                      ELSE content
    /\ UNCHANGED <<made, folder, dirty, unit, current, kind, target, want, wasThere,
                   down, lost, last>>

\* install.sh is done. install-release.sh: prev = readlink current, current -> this release,
\* every release but this one and prev goes, with its container (222-236). A tree's own
\* install.sh never touches current (W1) unless FixCurrent.
Finish ==
    /\ phase = "run" /\ todo = {}
    /\ IF kind = "release"
          THEN LET prev == current
                   keep(r) == r = target \/ r = prev
               IN /\ current' = target
                  /\ folder' = [r \in R |-> folder[r] /\ keep(r)]
                  /\ content' = [r \in R |-> IF keep(r) THEN content[r] ELSE "image"]
                  /\ dirty' = [r \in R |-> dirty[r] /\ keep(r)]
                  /\ last' = IF wasThere THEN "reinstall" ELSE "new"
          ELSE /\ current' = IF FixCurrent THEN target ELSE current
               /\ UNCHANGED <<folder, content, dirty>>
               /\ last' = "tree"
    /\ phase' = "idle"
    /\ UNCHANGED <<made, unit, kind, target, want, todo, wasThere, down, lost>>

\* scripts/fast-install: patches the tree current names (135-142), restarts the units whose
\* programs come from it (unit_dirs 349-372), says done. It doesn't know about a stopped install.
FastInstall ==
    /\ Patches /\ phase # "run" /\ current # None
    /\ content' = [content EXCEPT ![current] = "patched"]
    /\ lost' = (lost \/ unit["core"] # current)
    /\ last' = "patch"
    /\ UNCHANGED <<made, folder, dirty, unit, current, phase, kind, target, want, todo,
                   wasThere, down>>

FastUndo ==
    /\ Patches /\ phase # "run" /\ current # None /\ content[current] = "patched"
    /\ content' = [content EXCEPT ![current] = "image"]
    /\ last' = "undo"
    /\ UNCHANGED <<made, folder, dirty, unit, current, phase, kind, target, want, todo,
                   wasThere, down, lost>>

Next ==
    \/ \E r \in R : InstallRelease(r) \/ InstallTree(r)
    \/ Step \/ Interrupt \/ Rerun \/ Finish \/ FastInstall \/ FastUndo

\* Fairness: a running install.sh goes on, and someone re-runs a stopped one.
Spec == Init /\ [][Next]_vars /\ WF_vars(Step) /\ WF_vars(Finish) /\ WF_vars(Rerun)

\* ---- the rules ----

Installed == {u \in U : unit[u] # None}

\* Every unit points into a folder (and container) that is still there.
UnitsHaveFolder == \A u \in Installed : folder[unit[u]]

\* Outside a running install, all units point into one release: an interrupted install never
\* leaves a half-installed set of units.
NoMixedUnits == phase # "run" => \A u, v \in Installed : unit[u] = unit[v]

\* Outside a running install, "current" names the release the core units run.
CurrentIsLive == phase # "run" /\ unit["core"] # None => unit["core"] = current

\* fast-install never reports success for a tree nothing runs.
PatchLands == ~lost

\* Every release folder's image was built from the commit its release JSON names.
ReleaseMatchesCommit == \A r \in R : folder[r] => ~dirty[r]

\* Installing a release again gives that release as it was built.
ReinstallGivesRelease == last = "reinstall" => content[current] = "image"

\* A stopped install is finished by the re-run.
InstallSettles == phase = "half" ~> phase = "idle"
=============================================================================
