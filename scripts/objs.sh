# Compile only what changed, several files at once. Sourced by a build script, in its build folder:
#   obj COMPILER FLAGS... -c -o OBJ SOURCE    one compile, queued (the line a script had, "obj" before it)
#   objs_done                                 waits for them all; fails, naming them, if any failed
#
# The build scripts compiled every file, one after another, whatever had changed: ft-screens' 70
# files took over ten minutes on the one core a build gets while the headset is worn, for a one-line
# change (user, 2026-11-24: "Why are you having to do a build? Can't you just fast install properly?").
#
# An object is kept when it is newer than its source and every header it read (gcc's -MMD list, in
# OBJ's .d), and the same command made it (OBJ's .cmd: other flags, another compiler or a new
# library path build it again). A header that's gone builds it again too. A clean folder builds all.
# As many at once as FT_BUILD_JOBS says, else the CPUs this shell may run on (nproc: a taskset pin
# counts, so fast-install's pin for a worn headset still holds the build to one core).

_objs_jobs=${FT_BUILD_JOBS:-$(nproc)}
_objs_queued=()

# OBJ COMMAND-TEXT: is OBJ still what COMMAND-TEXT would make?
_objs_fresh() {
    local obj=$1 dep
    [ -f "$obj" ] && [ -f "${obj%.o}.d" ] && [ -f "${obj%.o}.cmd" ] && [ "$(cat "${obj%.o}.cmd")" = "$2" ] || return 1
    # (the .d is make's: "OBJ: source header \" and more lines of headers)
    while read -r dep; do
        [ -e "$dep" ] && [ ! "$dep" -nt "$obj" ] || return 1
    done < <(sed -e "1s/^[^:]*: *//" -e "s/ *\\\\\$//" "${obj%.o}.d" | tr -s " " "\n" | sed "/^\$/d")
}

obj() {
    local a prev= out= cmd="$*"
    for a in "$@"; do
        [ "$prev" = -o ] && out=$a
        prev=$a
    done
    [ -n "$out" ] || { echo "obj: no -o OBJ in: $cmd" >&2; return 2; }
    if _objs_fresh "$out" "$cmd"; then return 0; fi
    while [ "$(jobs -rp | wc -l)" -ge "$_objs_jobs" ]; do wait -n || true; done
    _objs_queued+=("$out")
    # (the .cmd goes first and comes back only with the object made: a failed or stopped compile is built again)
    rm -f "${out%.o}.cmd"
    { "$@" -MMD -MF "${out%.o}.d" && printf "%s\n" "$cmd" > "${out%.o}.cmd"; } &
}

objs_done() {
    local out failed=
    wait
    for out in "${_objs_queued[@]}"; do
        [ -f "${out%.o}.cmd" ] || failed="$failed $out"
    done
    echo "objs: ${#_objs_queued[@]} compiled, $_objs_jobs at once"
    _objs_queued=()
    [ -z "$failed" ] || { echo "objs: failed:$failed" >&2; return 1; }
}
