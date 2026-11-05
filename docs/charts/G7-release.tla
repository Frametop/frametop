------------------------------ MODULE G7Release ------------------------------
\* G7 with the W1 to W4 fixes (DEE-286..289): what a Frametop install leaves behind, release after
\* release. At 78e93b1 (check out f85dff9 for that model) the Fix constants switched the fixes on;
\* now the model is the fixed code and they are unused. W5 (an interrupted install) is still open.
\* A release is a folder ~/.local/share/frametop/releases/VERSION copied out of its image, with
\* its own container frametop-<id12> (pack/install-release.sh). Its units point into that
\* folder (@REPO@) and that container (scripts/in-box reads BOX from the tree's .frametop-release).
\* install-release.sh refuses a folder with fast-installed files (W4), notes the release "current"
\* names, copies the folder (atomically: VERSION.new, then mv), runs the folder's install.sh (12
\* steps under set -e, each writing its units, then pointing "current" at its own tree: W1), and
\* then removes every release but the new one and the one current named before.
\* A part the install skips has its units stopped and removed if they point into another
\* release's folder (W2). A local pack refuses an image built from a changed tree (W3).
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
    LocalBuild,   \* releases built locally: ft dev build from the working tree (W3); the pack now
                  \* refuses a changed tree, so it adds no states
    Patches,      \* fast-install puts commits on top of the live release (W4)
    FixCurrent,   \* unused: the model is the fixed code (W1 to W4); the cfgs still set them
    FixSkip,      \* unused
    FixReinstall, \* unused
    FixLocal      \* unused

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
    drops,    \* the parts it skips: their units go if they point into another release (W2)
    wasThere, \* install-release found the release's folder already there (no copy)
    down,     \* the one interruption has happened
    lost,     \* a fast-install patched a tree no running unit uses, and reported success
    last      \* the last thing finished: "none", "new", "reinstall", "tree", "patch", "undo"
vars == <<made, folder, content, dirty, unit, current, phase, kind, target, want, todo, drops,
          wasThere, down, lost, last>>

TypeOK ==
    /\ made \in 0..N
    /\ folder \in [R -> BOOLEAN] /\ dirty \in [R -> BOOLEAN]
    /\ content \in [R -> {"image", "patched"}]
    /\ unit \in [U -> R \cup {None}]
    /\ current \in R \cup {None}
    /\ phase \in {"idle", "run", "half"} /\ kind \in {"release", "tree"}
    /\ target \in R \cup {None} /\ want \subseteq U /\ todo \subseteq want /\ drops \subseteq U
    /\ wasThere \in BOOLEAN /\ down \in BOOLEAN /\ lost \in BOOLEAN
    /\ last \in {"none", "new", "reinstall", "tree", "patch", "undo"}

Init ==
    /\ made = 0 /\ folder = [r \in R |-> FALSE] /\ content = [r \in R |-> "image"]
    /\ dirty = [r \in R |-> FALSE] /\ unit = [u \in U |-> None] /\ current = None
    /\ phase = "idle" /\ kind = "release" /\ target = None /\ want = {} /\ todo = {} /\ drops = {}
    /\ wasThere = FALSE /\ down = FALSE /\ lost = FALSE /\ last = "none"

\* An install run's steps: core's always; hands' writes its units when picked, and when skipped
\* (drop_other_folder in install.sh) stops and removes them if they point into another release.
Skipped(skip) == IF skip THEN {"hands"} ELSE {}

\* install-release.sh VERSION: a new release (copied out, with its own container), or one whose
\* folder is there already (the same IMAGE_ID), not copied again. A folder with fast-installed
\* files is refused (W4): fast-install undo first. A local pack (framedrop/build.sh) refuses an
\* image ft dev build labelled as built from a changed tree or another commit, so a release's
\* image is always its commit's (W3; --allow-changes, a local test release, isn't modelled).
InstallRelease(r) ==
    /\ phase # "run"
    /\ \/ r = made + 1
       \/ folder[r] /\ content[r] = "image"
    /\ \E skip \in (IF Skips THEN BOOLEAN ELSE {FALSE}) :
          /\ want' = U /\ todo' = U /\ drops' = Skipped(skip)
    /\ content' = IF folder[r] THEN content ELSE [content EXCEPT ![r] = "image"]
    /\ dirty' = IF folder[r] THEN dirty ELSE [dirty EXCEPT ![r] = FALSE]
    /\ made' = IF r = made + 1 THEN r ELSE made
    /\ wasThere' = folder[r]
    /\ folder' = [folder EXCEPT ![r] = TRUE]
    /\ phase' = "run" /\ kind' = "release" /\ target' = r
    /\ UNCHANGED <<unit, current, down, lost, last>>

\* RELEASES/OLD/install.sh run by hand, to go back to OLD (pack/README.md step 5)
InstallTree(r) ==
    /\ Rollback /\ phase # "run" /\ folder[r] /\ r # current
    /\ \E skip \in (IF Skips THEN BOOLEAN ELSE {FALSE}) :
          want' = U /\ todo' = U /\ drops' = Skipped(skip)
    /\ phase' = "run" /\ kind' = "tree" /\ target' = r /\ wasThere' = TRUE
    /\ UNCHANGED <<made, folder, content, dirty, unit, current, down, lost, last>>

\* one install.sh step: core's steps (3 to 7) come before hands' (8, 9). A skipped part's units
\* stay if they point into this tree, and go if they point into another.
Step ==
    /\ phase = "run" /\ todo # {}
    /\ LET u == IF "core" \in todo THEN "core" ELSE "hands" IN
          /\ unit' = [unit EXCEPT ![u] = IF u \notin drops \/ unit[u] = target THEN target ELSE None]
          /\ todo' = todo \ {u}
    /\ UNCHANGED <<made, folder, content, dirty, current, phase, kind, target, want, drops, wasThere,
                   down, lost, last>>

\* a step fails under set -e, or the power goes: install.sh stops before it points "current" at
\* its tree, and install-release.sh before its clean-up (W5, still open: install.sh's exit says
\* to run the same command again)
Interrupt ==
    /\ Interrupts /\ ~down /\ phase = "run" /\ todo # {}
    /\ phase' = "half" /\ down' = TRUE
    /\ UNCHANGED <<made, folder, content, dirty, unit, current, kind, target, want, todo, drops,
                   wasThere, lost, last>>

\* the same command again (its folder is there now: no copy). install-release.sh refuses it if
\* fast-install patched that folder meanwhile, and the person runs fast-install undo as it says.
Rerun ==
    /\ phase = "half"
    /\ phase' = "run" /\ todo' = want
    /\ content' = IF kind = "release" THEN [content EXCEPT ![target] = "image"] ELSE content
    /\ UNCHANGED <<made, folder, dirty, unit, current, kind, target, want, drops, wasThere,
                   down, lost, last>>

\* install.sh is done: it points current at its own tree (W1). install-release.sh read prev
\* (readlink current) before it ran install.sh; every release but this one and prev goes, with
\* its container. (Nothing moves current between that read and here, so prev is current.)
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
          ELSE /\ current' = target
               /\ UNCHANGED <<folder, content, dirty>>
               /\ last' = "tree"
    /\ phase' = "idle"
    /\ UNCHANGED <<made, unit, kind, target, want, todo, drops, wasThere, down, lost>>

\* scripts/fast-install: patches the tree current names (135-142), restarts the units whose
\* programs come from it (unit_dirs 349-372), says done. It doesn't know about a stopped install.
FastInstall ==
    /\ Patches /\ phase # "run" /\ current # None
    /\ content' = [content EXCEPT ![current] = "patched"]
    /\ lost' = (lost \/ unit["core"] # current)
    /\ last' = "patch"
    /\ UNCHANGED <<made, folder, dirty, unit, current, phase, kind, target, want, todo, drops,
                   wasThere, down>>

FastUndo ==
    /\ Patches /\ phase # "run" /\ current # None /\ content[current] = "patched"
    /\ content' = [content EXCEPT ![current] = "image"]
    /\ last' = "undo"
    /\ UNCHANGED <<made, folder, dirty, unit, current, phase, kind, target, want, todo, drops,
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
