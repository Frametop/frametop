#!/usr/bin/bash
# The root half of npu/install.sh: puts the tree npu/build.sh built in place as ft-npud.service's, or takes it out.
# Run it through npu/install.sh, which builds the tree first and asks for the password (sudo).
# Everything ft-npud runs is root's and writable by root only (the unit's preflight and the daemon's own audit
# refuse to start otherwise, app/npud/trust.py): app/, python/, graphs/, the unit, MANIFEST, SOURCE.
# The tree lives on the home partition, in SteamOS's root-owned offload area (as /var/lib/flatpak does): /var is a
# small partition that also holds /etc's overlay, too small for two copies. The unit binds it read-only at
# /var/lib/ft-npud, the path everything else names. A new tree is built beside the live one, checked, then
# swapped in; the previous one stays at ft-npud.prev. /home survives SteamOS updates, and
# /etc/systemd/system/*.service is on the update keep list, so nothing needs an /etc/atomic-update.conf.d entry.
# Root opens nothing in the source tree itself: its owner (the user it serves) lists and packs every file (tar -h,
# links followed, with the owner's own permissions), and root unpacks that into a stage only root can enter. So a
# link in the tree to /etc/shadow or /dev/sda can't make root copy out what the owner couldn't read.
# Usage: root.sh install SRC UID   SRC: npu/build/tree, owned by UID, the user ft-npud serves (root); exit 3: in place, not started yet
#        root.sh uninstall (root) | status
#        root.sh check UNIT ROOT: run UNIT's ExecStartPre over the tree at ROOT (status does this; tests too)
set -euo pipefail
# a clean environment: nothing the caller exported (TAR_OPTIONS, BASH_ENV, PYTHON*, ...) reaches what runs as root
# (an allowlist of what's exported, both PATH and LC_ALL among them, and no exported functions)
clean=1 seen=0
for v in $(compgen -e); do
    case $v in
        PWD | OLDPWD | SHLVL | _) ;;
        PATH) [ "$PATH" = /usr/bin ] && seen=$((seen + 1)) || clean=0 ;;
        LC_ALL) [ "$LC_ALL" = C ] && seen=$((seen + 1)) || clean=0 ;;
        *) clean=0 ;;
    esac
done
[ $clean$seen = 12 ] && [ -z "$(declare -F)" ] || exec /usr/bin/env -i PATH=/usr/bin LC_ALL=C /usr/bin/bash "$0" "$@"
umask 022
cd /        # nothing below may pick up code from the caller's directory (python -m puts it on sys.path)
TREE=/home/.steamos/offload/var/lib/ft-npud
MNT=/var/lib/ft-npud
UNIT=ft-npud.service
ETCUNIT=/etc/systemd/system/$UNIT
PY=/usr/bin/python3
PYI=("$PY" -I -S -B)    # isolated: no cwd, no PYTHON* variables, no site, nothing written
CAP=$((1 << 30))        # bytes one pack may stream: more (a loop made after the check) fails the install
MAXN=50000             # entries the owner's check may walk (a link fan-out or a /sys link): more fails it
MEMBERS=(app python graphs "$UNIT")
NEEDED=(app/npud/hnet_serve.py app/npud/trust.py python/bin/python3 graphs/landmarks.npz "$UNIT")

usage() { echo "usage: root.sh install SRC UID | uninstall | status | check UNIT ROOT" >&2; exit 2; }
fail() { echo "ft-npud: $*" >&2; exit 1; }
need_root() { [ "$(id -u)" = 0 ] || fail "run as root"; }
# Type=simple: systemctl restart returns once the program has started, not once it serves. This run serves once it
# logs its self-test, "... serving /run/ft-npud/ft-npud.sock" (about a second in); one that ends first hasn't started
serving() {
    local inv
    inv=$(systemctl show -P InvocationID "$UNIT") && [ -n "$inv" ] || return 1
    for _ in $(seq 60); do   # 30 s
        # read whole, then matched: grep -q would stop reading early, and journalctl would die of SIGPIPE
        [[ $(journalctl -q -o cat "_SYSTEMD_INVOCATION_ID=$inv" 2>/dev/null) == *'; serving /'* ]] && return 0
        [ "$(systemctl show -P InvocationID "$UNIT")" = "$inv" ] && systemctl is-active -q "$UNIT" || return 1
        sleep 0.5
    done
    return 1
}

# the unit's own ExecStartPre (one copy of the check: the unit's), with /var/lib/ft-npud read as ROOT: trust.py's
# path first, every directory from / down to it root's, not writable by others and no symlink, then trust.py.
# Read as systemd reads it: the one ExecStartPre= in [Service], none anywhere else, no reset line, no continuation.
preflight() {   # UNIT ROOT
    [[ $1 = /* ]] || fail "the unit must be an absolute path: $1"
    [[ $2 =~ ^/[A-Za-z0-9._/-]+$ ]] || fail "bad tree path: $2"
    "${PYI[@]}" -c 'import shlex, sys
unit, root = sys.argv[1], sys.argv[2]
def logical(lines):     # as systemd reads a unit: comment lines skipped, an odd number of trailing \\ joins on
    buf = None
    for raw in lines:
        s = raw.rstrip("\n")
        if s.strip().startswith(("#", ";")):
            continue
        tail = len(s) - len(s.rstrip("\\"))
        if tail % 2:
            buf = (buf or "") + s[:-1] + " "
            continue
        yield (buf or "") + s
        buf = None
    if buf is not None:
        yield buf
section, pre, stray = None, [], 0
for s in map(str.strip, logical(open(unit))):
    if s.startswith("["):
        section = s
    elif "=" in s:
        key, value = s.split("=", 1)
        if key.strip() == "ExecStartPre":
            if section == "[Service]":
                pre.append(value.strip())
            else:
                stray += 1
argv = shlex.split(pre[0]) if len(pre) == 1 and not stray else []
if len(argv) != 6 or argv[:5] != ["/usr/bin/python3", "-I", "-S", "-B", "-c"]:
    sys.exit("ft-npud preflight: %s has no single python3 -I -S -B -c ExecStartPre in [Service]" % unit)
body = argv[5].replace("%%", "%").replace("/var/lib/ft-npud", root)   # systemd unescapes %%
exec(compile(body, "ExecStartPre", "exec"), {"__name__": "__main__"})' "$1" "$2"
}

# what status compares: every entry under the tree (type, mode, owner, link target), every file's hash, the unit's
manifest() {    # TREE UNIT
    (cd "$1" && find . -mindepth 1 ! -path ./MANIFEST -printf '%y %m %U:%G %p %l\n' | sort &&
        find . -mindepth 1 ! -path ./MANIFEST -type f -print0 | sort -z | xargs -0 sha256sum)
    echo "unit $(sha256sum < "$2" | cut -d' ' -f1)"
}

cmd=${1:-}; [ $# -gt 0 ] && shift
case $cmd in
check)
    [ $# = 2 ] || usage
    preflight "$1" "$2"
    ;;
install)
    need_root
    [ $# = 2 ] || usage
    SRC=$1 USERID=$2
    [[ $SRC =~ ^/[A-Za-z0-9._/-]+$ && ! $SRC =~ (^|/)\.\.(/|$) ]] || fail "not a plain absolute path: $SRC"
    [[ $USERID =~ ^[1-9][0-9]{0,8}$ ]] || fail "not a user's uid: $USERID"
    [ -d "$SRC" ] && [ ! -L "$SRC" ] || fail "no tree at $SRC (npu/build.sh builds it)"
    OWNER=$(stat -c %u "$SRC") GROUP=$(stat -c %g "$SRC")
    # packed with the permissions of the user it serves, never root's
    [ "$OWNER" = "$USERID" ] || fail "$SRC is uid $OWNER's, not uid $USERID's"
    as_owner() { setpriv --reuid="$OWNER" --regid="$GROUP" --init-groups --no-new-privs -- "$@"; }
    # pack DIR MEMBER...: as the owner, cd into DIR, refuse anything find -L sees as not a file or a directory
    # (FIFOs, devices, dangling links) or a link loop, then stream it; unpack DEST: root extracts that stream
    # Members are plain relative paths (never an option to find or tar). The walk is bounded: at most MAXN entries
    # and a few problems, then find is cut off.
    pack() {
        as_owner /usr/bin/env MAXN="$MAXN" /usr/bin/bash -c 'cd -- "$1" && shift || exit 1
            for m; do
                [[ $m =~ ^[A-Za-z0-9_][A-Za-z0-9._/-]*$ && ! $m =~ (^|/)\.\.(/|$) ]] ||
                    { echo "ft-npud: not a plain relative path: $m" >&2; exit 1; }
            done
            odd=$(find -L "$@" -printf "%Y %p\n" 2>&1 | head -n "$((MAXN + 1))" |
                  awk -v max="$MAXN" '"'"'NR > max { print "more than " max " entries"; exit }
                                         !/^[fd] / { print; if (++n == 5) exit }'"'"') || true
            [ -z "$odd" ] || { echo "ft-npud: not files and directories only: ${odd:0:300}" >&2; exit 1; }
            exec tar -chf - --exclude=__pycache__ --exclude="*.egg-info" -- "$@"' _ "$@" | capped
    }
    capped() {      # CAP bytes through, and fail if there is even one more
        head -c "$CAP"
        [ "$(head -c 1 | wc -c)" = 0 ] || { echo "ft-npud: a pack is over $CAP bytes" >&2; exit 1; }
    }
    unpack() { tar -C "$1" -xf - --no-same-owner --no-same-permissions --no-overwrite-dir; }
    # a second install before a restart would delete the tree the running unit still has bound (ft-npud.prev)
    pid=$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null || echo 0)
    if [ "${pid:-0}" != 0 ] && [ -d "$TREE.prev" ] &&
       [ "$(stat -L -c %d:%i "/proc/$pid/root$MNT" 2>/dev/null)" = "$(stat -c %d:%i "$TREE.prev")" ]; then
        fail "the running $UNIT still uses $TREE.prev: systemctl restart $UNIT first"
    fi
    install -d -m 0755 -o root -g root "$(dirname "$TREE")"
    avail=$(df -Pm "$(dirname "$TREE")" | awk 'NR == 2 { print $4 }')
    [ "$avail" -ge 500 ] || fail "only $avail MB free for $TREE"
    stage=$TREE.new
    trap 'rm -rf "$stage"' EXIT         # a failed build leaves nothing behind
    rm -rf "$stage"
    install -d -m 0700 "$stage"         # nobody else enters it until it has passed every check
    pack "$SRC" "${MEMBERS[@]}" | unpack "$stage"
    for f in "${NEEDED[@]}"; do [ -e "$stage/$f" ] || fail "$f didn't copy"; done
    [ -f "$stage/$UNIT" ] && [ ! -L "$stage/$UNIT" ] || fail "$UNIT is not a file"
    # the one user it serves: the template's @UID@, and nothing else left to fill in
    sed -i "s/@UID@/$USERID/g" "$stage/$UNIT"
    ! grep -q '@[A-Z]*@' "$stage/$UNIT" || fail "$UNIT has a placeholder left"
    find "$stage" -depth \( -name __pycache__ -o -name '*.egg-info' \) -exec rm -rf {} +
    # Files and directories only, and only the members at the top. tar -h left no links, so one here came another
    # way (a header added to the owner's stream while it packed), as would a FIFO or a device node: root writes
    # and chmods below, and must never do that through a link.
    odd=$(find -P "$stage" -mindepth 1 ! -type f ! -type d | head -5)
    [ -z "$odd" ] || fail "not files and directories only in the new tree: $odd"
    for e in "$stage"/* "$stage"/.[!.]* "$stage"/..?*; do
        [ -e "$e" ] || continue
        ok=0
        for m in "${MEMBERS[@]}"; do [ "${e##*/}" = "$m" ] && ok=1; done
        [ "$ok" = 1 ] || fail "not part of the tree: ${e##*/}"
    done
    rm -f "$stage/SOURCE" "$stage/MANIFEST"
    printf 'from %s\ngraph sha256 %s\nfor uid %s\ninstalled %s\n' "$SRC" \
        "$(sha256sum < "$stage/graphs/landmarks.npz" | cut -d' ' -f1)" "$USERID" "$(date -u +%FT%TZ)" > "$stage/SOURCE"
    chown -R -h root:root "$stage"
    find -P "$stage" -mindepth 1 \( -type d -o -type f \) -exec chmod u=rwX,go=rX {} +    # the stage itself stays 0700
    manifest "$stage" "$stage/$UNIT" > "$stage/MANIFEST"
    preflight "$stage/$UNIT" "$stage"   # also walks every directory above the tree
    chmod 0755 "$stage"
    rm -rf "$TREE.prev"
    if [ -d "$TREE" ]; then mv "$TREE" "$TREE.prev"; fi
    mv "$stage" "$TREE"
    trap - EXIT
    # the bind's mount point: an empty root dir. Only an earlier install's tree (in /var itself) is emptied out:
    # the host never sees the bind, and removing the dir would detach a running unit's mount
    if [ -d "$MNT" ] && ! mountpoint -q "$MNT" && [ -n "$(ls -A "$MNT")" ]; then rm -rf "$MNT"; fi
    rm -rf "$MNT.prev" "$MNT.new"
    install -d -m 0755 -o root -g root "$MNT"
    install -m 0644 -o root -g root "$TREE/$UNIT" "$ETCUNIT"
    systemctl daemon-reload
    systemctl enable -q "$UNIT"
    # the new tree now: a client falls back to the CPU for the second this takes, then connects again
    systemctl reset-failed "$UNIT" 2>/dev/null || true
    if ! systemctl restart "$UNIT" || ! serving; then
        # in place and enabled, and it tries again by itself (the DSP not ready yet, dsp_service restarting)
        echo "ft-npud: installed $TREE, but it hasn't started yet (it keeps trying): journalctl -u $UNIT" >&2
        exit 3
    fi
    echo "ft-npud: installed $TREE and started it"
    ;;
status)
    echo "enabled: $(systemctl is-enabled "$UNIT" 2>/dev/null || true)"
    echo "active: $(systemctl is-active "$UNIT" 2>/dev/null || true)"
    [ -d "$TREE" ] || fail "not installed ($TREE missing)"
    # the unit first, without running anything from it: root's file, the one systemd loaded, nothing on top
    for p in / /etc /etc/systemd /etc/systemd/system "$ETCUNIT"; do
        [ ! -L "$p" ] && [ "$(stat -c %u "$p")" = 0 ] && [ $((0$(stat -c %a "$p") & 022)) = 0 ] ||
            fail "$p is a link, not root's, or writable by others"
    done
    [ "$(systemctl show -p FragmentPath --value "$UNIT")" = "$ETCUNIT" ] || fail "systemd loads $UNIT from elsewhere"
    [ "$(systemctl show -p NeedDaemonReload --value "$UNIT")" = no ] || fail "$UNIT changed since systemd loaded it"
    [ -z "$(systemctl show -p DropInPaths --value "$UNIT")" ] || fail "drop-ins change $UNIT"
    pre=$(systemctl show -p ExecStartPre --value "$UNIT")     # what systemd parsed: one python3 -I -S -B -c, no more
    [ "$(grep -o '{ path=' <<< "$pre" | wc -l)" = 1 ] &&
        [[ $pre == "{ path=/usr/bin/python3 ; argv[]=/usr/bin/python3 -I -S -B -c "* ]] ||
        fail "systemd doesn't run exactly the one preflight for $UNIT"
    manifest "$TREE" "$ETCUNIT" | cmp -s - "$TREE/MANIFEST" ||
        fail "the tree or the unit differs from the install's MANIFEST"
    preflight "$ETCUNIT" "$TREE"
    echo "preflight: ok"
    cat "$TREE/SOURCE"
    echo "files: as installed"
    ;;
uninstall)
    need_root
    systemctl disable --now -q "$UNIT" 2>/dev/null || true
    rm -f "$ETCUNIT"
    systemctl daemon-reload
    systemctl reset-failed "$UNIT" 2>/dev/null || true
    rm -rf "$TREE" "$TREE.prev" "$TREE.new" "$MNT" "$MNT.prev" "$MNT.new"
    echo "ft-npud: removed"
    ;;
*) usage ;;
esac
