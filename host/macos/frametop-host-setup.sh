#!/bin/bash
# Frametop host setup for macOS: makes this Mac a host for Frametop's remote displays, its
# monitors shown as screens on a Steam Frame (docs/remote-displays.md). Run it in Terminal, or
# open "Setup Frametop host.command".
#
# Sunshine streams one display per running copy, so this runs one copy (an instance) per display,
# each with its own settings, pairings and ports. Only real displays: Sunshine can't make virtual
# ones on macOS.
#
#  1. Sunshine (github.com/LizardByte/Sunshine), the pinned build below, into /Applications
#     (checked against its SHA-256 first). It's a pre-release: the first build where the mouse
#     reaches displays other than the main one (Sunshine #5733).
#  2. One instance per display: settings in ~/Library/Application Support/Frametop/sunshine,
#     instance N on ports 47989 + 100*N (its Web UI on the next port), started at login by the
#     LaunchAgent frametop.sunshine.N and started again if it stops. An instance is tied to its
#     monitor, not to a port: plug the monitor in elsewhere and it keeps its instance, and while
#     it's unplugged its instance waits for it. Run this again after connecting a new monitor.
#  3. The Web UI login you sign in with once from the Steam Frame, the same for every instance:
#     keep the one there is, take the one an earlier Sunshine install had, or set one. The
#     password is typed here and never shown or saved.
#  4. macOS's permissions, Screen Recording and Accessibility, which only you can grant. One
#     grant covers every instance.
#  5. Checks: every instance answers, the permissions, the firewall, and whether the Mac sleeps
#     while idle.
#
# Options:
#   --set-login     set a new Web UI login even if there is one
#   --skip-login    leave the Web UI login alone
#   --check         only say what would change
#   --undo          remove the instances' LaunchAgents (settings, pairings and Sunshine stay)
# A log goes to ~/Library/Logs/Frametop/host-setup.log.
set -euo pipefail

SUNSHINE_VERSION=2026.1010.141104
DMG_URL="https://github.com/LizardByte/Sunshine/releases/download/v$SUNSHINE_VERSION/Sunshine-macOS-arm64.dmg"
DMG_SHA=c40d7d860a77d8e37ce0105b211b453f7c59ff1f1bbbf4970cdd3b66d4f26a58
APP=/Applications/Sunshine.app
SUNSHINE_ID=dev.lizardbyte.app.Sunshine
SUNSHINE="$APP/Contents/MacOS/Sunshine"
DATA="$HOME/Library/Application Support/Frametop/sunshine"
LOGS="$HOME/Library/Logs/Frametop"
AGENTS="$HOME/Library/LaunchAgents"
LABEL=frametop.sunshine  # instance N's agent is frametop.sunshine.N
BASE_PORT=47989
GUI="gui/$(id -u)"
OLD_STATE="$HOME/.config/sunshine/sunshine_state.json"  # a stock Sunshine install's login
BREW_LABEL=homebrew.mxcl.sunshine
HERE=$(cd "$(dirname "$0")" && pwd)

check=0 undo=0 login=auto
for arg in "$@"; do
    case $arg in
        --check) check=1 ;;
        --undo) undo=1 ;;
        --set-login) login=set ;;
        --skip-login) login=skip ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $arg (see --help)" >&2; exit 2 ;;
    esac
done

mkdir -p "$LOGS"
LOG="$LOGS/host-setup.log"
say() { echo "$*"; printf '%s  %s\n' "$(date '+%F %T')" "$*" >> "$LOG"; }
fail() { say "Failed: $*"; exit 1; }

agent_loaded() { launchctl print "$GUI/$1" >/dev/null 2>&1; }
our_agents() { ls "$AGENTS" 2>/dev/null | sed -n "s/^\($LABEL\.[0-9][0-9]*\)\.plist$/\1/p"; }
# bootout returns before the job is gone, and bootstrap fails until it is.
start_agent() {
    local i
    for i in 1 2 3 4 5; do
        launchctl bootstrap "$GUI" "$AGENTS/$1.plist" 2>/dev/null && return 0
        agent_loaded "$1" && return 0
        sleep 1
    done
    say "Couldn't start the LaunchAgent $1."
    return 1
}
stop_agents() {
    local label
    for label in $(our_agents); do
        launchctl bootout "$GUI/$label" 2>/dev/null || true
    done
}

# Answers when the instance on this port is up (its HTTP serverinfo, port N).
answers() { curl -s -m 2 -o /dev/null "http://127.0.0.1:$1/serverinfo"; }
wait_answers() {
    local i
    for i in $(seq 1 "${2:-20}"); do
        answers "$1" && return 0
        sleep 1
    done
    return 1
}

# Restart the main display's instance: macOS applies its permissions at the next start, and it
# notes them (and asks for missing ones) a moment after it answers.
restart_first() {
    launchctl kickstart -k "$GUI/$LABEL.$first"
    wait_answers $((BASE_PORT + 100 * first)) || true
    sleep 2
}

# The instance number for a display's key: the one it had, else the lowest free one.
instance_for() {
    local key=$1 d n=0
    for d in "$DATA"/instance-*/display; do
        [ -e "$d" ] || continue
        if [ "$(cut -f1 "$d")" = "$key" ]; then
            basename "$(dirname "$d")" | sed 's/^instance-//'
            return
        fi
    done
    while [ -e "$DATA/instance-$n/display" ] || printf '%s\n' "${taken:-}" | grep -qx "$n"; do
        n=$((n + 1))
    done
    echo "$n"
}

# Sets Frametop's keys in a sunshine.conf and keeps everything else in it (the Web UI's changes).
# The name ends in the display's size: Frametop Remote Displays reads it from the serverinfo.
write_conf() {
    local conf=$1 n=$2 name=$3 w=$4 h=$5 keys tmp
    keys='sunshine_name|port|file_state|credentials_file|file_apps|log_path|pkey|cert|minimum_fps_target|system_tray'
    tmp=$(mktemp)
    if [ -e "$conf" ]; then
        grep -v -E "^[[:space:]]*($keys)[[:space:]]*=|^# (Frametop|Display):" "$conf" > "$tmp" || true
    fi
    {
        echo "# Frametop: instance $n, written by frametop-host-setup.sh. output_name comes from ft-sunshine-instance."
        echo "# Display: $name"
        echo "sunshine_name = $(scutil --get ComputerName 2>/dev/null || hostname -s): $name (${w}x$h)"
        echo "port = $((BASE_PORT + 100 * n))"
        echo "file_state = $DATA/instance-$n/sunshine_state.json"
        echo "credentials_file = $DATA/credentials.json"
        echo "file_apps = $DATA/instance-$n/apps.json"
        echo "log_path = $LOGS/sunshine-$n.log"
        echo "pkey = $DATA/credentials/cakey.pem"
        echo "cert = $DATA/credentials/cacert.pem"
        echo "minimum_fps_target = 1"
        # One menu bar icon is enough; every instance's Web UI is listed at the end.
        echo "system_tray = $([ "$n" = "$first" ] && echo enabled || echo disabled)"
        cat "$tmp"
    } > "$conf"
    rm -f "$tmp"
}

write_agent() {
    local n=$1 plist="$AGENTS/$LABEL.$1.plist"
    cat > "$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL.$n</string>
    <key>ProgramArguments</key>
    <array>
        <string>/bin/bash</string>
        <string>$DATA/bin/ft-sunshine-instance</string>
        <string>$n</string>
    </array>
    <!-- Login Items shows it as Sunshine rather than as the bash that starts it. -->
    <key>AssociatedBundleIdentifiers</key><string>$SUNSHINE_ID</string>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
    <key>ThrottleInterval</key><integer>10</integer>
    <key>ProcessType</key><string>Interactive</string>
    <key>LimitLoadToSessionType</key><string>Aqua</string>
    <key>StandardOutPath</key><string>/dev/null</string>
    <key>StandardErrorPath</key><string>$LOGS/sunshine-$n.launch.log</string>
</dict>
</plist>
EOF
}

# A JSON string's value from a file, or nothing (plutil reads JSON).
json_get() { plutil -extract "$2" raw -o - "$1" 2>/dev/null || true; }

install_sunshine() {
    local tmp mnt
    tmp=$(mktemp -d)
    say "Downloading Sunshine $SUNSHINE_VERSION..."
    curl -fsSL -o "$tmp/Sunshine.dmg" "$DMG_URL" || fail "couldn't download $DMG_URL"
    [ "$(shasum -a 256 "$tmp/Sunshine.dmg" | cut -d' ' -f1)" = "$DMG_SHA" ] ||
        fail "the download's SHA-256 doesn't match; nothing was installed"
    mnt="$tmp/mnt"
    hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$tmp/Sunshine.dmg" >/dev/null
    stop_agents
    pkill -x Sunshine 2>/dev/null || true
    rm -rf "$APP"
    ditto "$mnt/Sunshine.app" "$APP"
    hdiutil detach -quiet "$mnt" || true
    rm -rf "$tmp"
    say "Installed Sunshine $SUNSHINE_VERSION in /Applications."
}

undo() {
    local label
    for label in $(our_agents); do
        if [ $check = 1 ]; then say "Would remove the LaunchAgent $label."; continue; fi
        launchctl bootout "$GUI/$label" 2>/dev/null || true
        rm -f "$AGENTS/$label.plist"
        say "Removed the LaunchAgent $label."
    done
    if [ -e "$DATA/.disabled-$BREW_LABEL" ]; then
        if [ $check = 1 ]; then
            say "Would turn Homebrew's Sunshine LaunchAgent back on."
        else
            launchctl enable "$GUI/$BREW_LABEL"
            rm -f "$DATA/.disabled-$BREW_LABEL"
            say "Turned Homebrew's Sunshine LaunchAgent back on (it starts at the next login)."
        fi
    fi
    say "Done. Kept: Sunshine in /Applications and the instances' settings and pairings in $DATA."
    say "To remove those too: rm -rf \"$DATA\", then move Sunshine to the Trash."
}

main() {
    say "== Frametop host setup$([ $check = 1 ] && echo ' (check only)')$([ $undo = 1 ] && echo ' (undo)')"
    [ "$(uname -s)" = Darwin ] || fail "this is the macOS host setup"
    if [ $undo = 1 ]; then undo; return; fi
    [ "$(uname -m)" = arm64 ] || fail "only Apple silicon Macs are covered (the pinned build is arm64)"

    # 1. Sunshine
    local have
    have=$(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString 2>/dev/null || true)
    if [ "$have" = "$SUNSHINE_VERSION" ]; then
        say "Sunshine $SUNSHINE_VERSION is installed."
    elif [ $check = 1 ]; then
        say "Would install Sunshine $SUNSHINE_VERSION$([ -n "$have" ] && echo " over $have")."
    else
        [ -w /Applications ] || fail "can't write to /Applications; run this from an admin account"
        install_sunshine
    fi

    # An earlier Homebrew Sunshine starts at login on the first instance's ports.
    if [ -e "$AGENTS/$BREW_LABEL.plist" ] && ! launchctl print-disabled "$GUI" | grep -q "\"$BREW_LABEL\" => disabled"; then
        if [ $check = 1 ]; then
            say "Would turn off Homebrew's Sunshine LaunchAgent ($BREW_LABEL), which takes the same ports."
        else
            launchctl bootout "$GUI/$BREW_LABEL" 2>/dev/null || true
            launchctl disable "$GUI/$BREW_LABEL"
            mkdir -p "$DATA" && touch "$DATA/.disabled-$BREW_LABEL"
            say "Turned off Homebrew's Sunshine LaunchAgent ($BREW_LABEL), which takes the same ports. --undo turns it back on."
        fi
    fi

    # 2. One instance per display, the main display's first.
    local displays line id key name w h n taken="" first="" lines=""
    displays=$(osascript -l JavaScript "$HERE/displays.js") || fail "couldn't list the displays"
    displays=$(printf '%s\n' "$displays" | sort -t $'\t' -k9,9nr -k6,6n -k7,7n)
    while IFS=$'\t' read -r id key name w h _; do
        n=$(instance_for "$key")
        taken="$taken
$n"
        [ -n "$first" ] || first=$n
        lines="$lines$n	$key	$name	$w	$h
"
    done <<< "$displays"
    # Instances whose display isn't connected now keep their place and wait for it.
    local d
    for d in "$DATA"/instance-*/display; do
        [ -e "$d" ] || continue
        n=$(basename "$(dirname "$d")" | sed 's/^instance-//')
        printf '%s' "$lines" | cut -f1 | grep -qx "$n" && continue
        say "Instance $n: $(cut -f2 "$d") isn't connected; its instance waits for it (port $((BASE_PORT + 100 * n)))."
    done
    if [ $check = 1 ]; then
        while IFS=$'\t' read -r n key name w h; do
            [ -n "$n" ] || continue
            say "Would $([ -e "$DATA/instance-$n/display" ] && echo keep || echo add) instance $n: $name ($w x $h), port $((BASE_PORT + 100 * n))."
        done <<< "$lines"
    else
        stop_agents
        mkdir -p "$DATA/bin" "$DATA/credentials" "$AGENTS"
        cp "$HERE/ft-sunshine-instance" "$HERE/displays.js" "$DATA/bin/"
        chmod 755 "$DATA/bin/ft-sunshine-instance"
        # One certificate for every instance, so Frametop pins one key for the Mac.
        if [ ! -s "$DATA/credentials/cacert.pem" ] || [ ! -s "$DATA/credentials/cakey.pem" ]; then
            (umask 077 && openssl req -x509 -newkey rsa:2048 -nodes -days 7300 -subj "/CN=Sunshine Gamestream Host" \
                -keyout "$DATA/credentials/cakey.pem" -out "$DATA/credentials/cacert.pem" 2>/dev/null) ||
                fail "couldn't make the instances' certificate"
        fi
        while IFS=$'\t' read -r n key name w h; do
            [ -n "$n" ] || continue
            mkdir -p "$DATA/instance-$n"
            printf '%s\t%s\t%s\t%s\n' "$key" "$name" "$w" "$h" > "$DATA/instance-$n/display"
            write_conf "$DATA/instance-$n/sunshine.conf" "$n" "$name" "$w" "$h"
            [ -e "$DATA/instance-$n/apps.json" ] ||
                printf '{\n  "env": {},\n  "apps": [{"name": "Desktop", "image-path": "desktop.png"}]\n}\n' > "$DATA/instance-$n/apps.json"
            write_agent "$n"
            say "Instance $n: $name ($w x $h), port $((BASE_PORT + 100 * n))."
        done <<< "$lines"
    fi

    # 3. The Web UI login, one file for every instance.
    local user="" pass pass2 old_user
    user=$(json_get "$DATA/credentials.json" username)
    old_user=$(json_get "$OLD_STATE" username)
    if [ $login = skip ]; then
        say "Web UI login: left as it is$([ -n "$user" ] && echo " ('$user')")."
    elif [ -n "$user" ] && [ $login = auto ]; then
        say "Web UI login: keeping '$user'."
    elif [ -n "$old_user" ] && [ $login = auto ]; then
        if [ $check = 1 ]; then
            say "Would use the earlier Sunshine install's Web UI login ('$old_user')."
        else
            (umask 077 && osascript -l JavaScript -e 'function run(a) {
                const o = JSON.parse($.NSString.stringWithContentsOfFileEncodingError(a[0], $.NSUTF8StringEncoding, null).js);
                const out = JSON.stringify({username: o.username, salt: o.salt, password: o.password}, null, 2);
                if (!$(out).writeToFileAtomicallyEncodingError(a[1], true, $.NSUTF8StringEncoding, null)) throw new Error("write failed");
            }' "$OLD_STATE" "$DATA/credentials.json") || fail "couldn't copy the earlier login"
            user=$old_user
            say "Web UI login: using the earlier Sunshine install's ('$user', from $OLD_STATE)."
        fi
    elif [ $check = 1 ]; then
        say "Would ask for a Web UI login."
    else
        [ -t 0 ] || fail "no Web UI login yet; run this in Terminal to set one"
        read -r -p "Web UI user name for Frametop to sign in with: " user
        [ -n "$user" ] || fail "no user name given"
        while :; do
            read -r -s -p "Password: " pass; echo
            read -r -s -p "Password again: " pass2; echo
            [ -n "$pass" ] && [ "$pass" = "$pass2" ] && break
            echo "They don't match (or it's empty); try again."
        done
        "$SUNSHINE" "$DATA/instance-$first/sunshine.conf" --creds "$user" "$pass" >/dev/null 2>&1 ||
            fail "Sunshine couldn't save the login"
        unset pass pass2
        chmod 600 "$DATA/credentials.json"
        say "Web UI login set: '$user'."
    fi

    if [ $check = 1 ]; then
        say "Would start the instances, the main display's first, and wait for macOS's permissions."
        checks
        return
    fi

    # 4. Start the main display's instance alone: it asks for the permissions, then the rest
    #    start with them (Sunshine remembers its requests per app, not per instance).
    start_agent "$LABEL.$first"
    wait_answers $((BASE_PORT + 100 * first)) || say "Instance $first didn't answer within 20 s; see $LOGS/sunshine-$first.log."
    if [ -t 0 ]; then
        echo
        echo "macOS asks whether Sunshine may record the screen and control the computer (Accessibility)."
        echo "Allow both in System Settings > Privacy & Security, then come back here. If Sunshine is"
        echo "already allowed, nothing appears."
        read -r -p "Press Enter when done: " _
        restart_first
        # A grant macOS lost track of, or one given to another Sunshine (Homebrew's), leaves Sunshine
        # without Accessibility and not asking again: clear its records so it asks once more.
        local yn
        while ! permitted inputPostEventPermissionState; do
            read -r -p "Sunshine still doesn't have Accessibility. Ask again? [Y/n] " yn
            case $yn in [Nn]*) break ;; esac
            tccutil reset Accessibility "$SUNSHINE_ID" >/dev/null 2>&1 || true
            tccutil reset PostEvent "$SUNSHINE_ID" >/dev/null 2>&1 || true
            defaults delete "$SUNSHINE_ID" inputPostEventPermissionState 2>/dev/null || true
            restart_first
            echo "Allow Sunshine in macOS's dialog, or in System Settings > Privacy & Security > Accessibility."
            read -r -p "Press Enter when done: " _
            restart_first
        done
    else
        say "Not in a terminal: if macOS asks, allow Screen Recording and Accessibility for Sunshine, then run this again."
    fi
    while IFS=$'\t' read -r n _; do
        [ -n "$n" ] && [ "$n" != "$first" ] || continue
        start_agent "$LABEL.$n" || true
    done <<< "$lines"
    # Instances whose display isn't connected start too, and wait for it.
    for d in "$DATA"/instance-*/display; do
        [ -e "$d" ] || continue
        n=$(basename "$(dirname "$d")" | sed 's/^instance-//')
        [ -e "$AGENTS/$LABEL.$n.plist" ] || write_agent "$n"
        agent_loaded "$LABEL.$n" || start_agent "$LABEL.$n" || true
    done

    checks
    echo
    say "Done. On the Steam Frame: open Frametop Remote Displays, Add computer, pick $(scutil --get ComputerName 2>/dev/null || hostname -s) ($(addresses)), and sign in as '$user'."
}

addresses() {
    local ifc ip out=""
    for ifc in $(ifconfig -l); do
        case $ifc in lo*|utun*|bridge*|awdl*|llw*|anpi*|ap*|gif*|stf*) continue ;; esac
        ip=$(ipconfig getifaddr "$ifc" 2>/dev/null) || continue
        out="$out${out:+, }$ip"
    done
    echo "${out:-no network}"
}

# Whether Sunshine had a macOS permission when it last started: it notes 2 then, 1 when it
# asked and didn't get it. Without Accessibility the pointer still moves (Sunshine warps it) but
# clicks, scrolling and typing are dropped.
permitted() { [ "$(defaults read "$SUNSHINE_ID" "$1" 2>/dev/null)" = 2 ]; }
permission() {
    if permitted "$1"; then
        say "$2: allowed."
    else
        say "$2: not allowed for Sunshine, so $3."
        say "  Allow it in System Settings > Privacy & Security > $2, then run this again."
    fi
}

# 5. Checks
checks() {
    local n key name w h port
    if [ $check = 0 ]; then
        for d in "$DATA"/instance-*/display; do
            [ -e "$d" ] || continue
            n=$(basename "$(dirname "$d")" | sed 's/^instance-//')
            IFS=$'\t' read -r key name w h < "$d"
            port=$((BASE_PORT + 100 * n))
            if wait_answers "$port" 15; then
                say "Instance $n answers: $name, Web UI https://localhost:$((port + 1))"
            elif [ -e "$DATA/instance-$n/.waiting" ]; then
                say "Instance $n waits for $name to be connected."
            else
                say "Instance $n doesn't answer yet: see $LOGS/sunshine-$n.log and $LOGS/sunshine-$n.launch.log."
            fi
        done
    fi

    permission screenRecordingPermissionState "Screen Recording" "the screens it sends stay black"
    permission inputPostEventPermissionState "Accessibility" "the pointer moves but clicks, scrolling and typing don't reach the Mac"

    local fw=/usr/libexec/ApplicationFirewall/socketfilterfw
    if "$fw" --getglobalstate 2>/dev/null | grep -q enabled; then
        if "$fw" --getappblocked "$SUNSHINE" 2>/dev/null | grep -q 'is blocked'; then
            say "Firewall: Sunshine is blocked. Allow it in System Settings > Network > Firewall > Options."
        elif "$fw" --getallowsigned 2>/dev/null | grep -q 'ENABLED'; then
            say "Firewall: on; Sunshine is signed, so macOS lets it accept connections."
        else
            say "Firewall: on. If macOS asks whether Sunshine may accept incoming connections, allow it."
        fi
    else
        say "Firewall: off."
    fi

    # The Steam Frame can't reach a sleeping Mac, and Sunshine keeps it awake only while streaming.
    local sleep_min
    sleep_min=$(pmset -g custom 2>/dev/null | awk '/^AC Power/ { ac = 1 } ac && $1 == "sleep" { print $2; exit }')
    if [ -n "$sleep_min" ] && [ "$sleep_min" != 0 ]; then
        say "Sleep: on power, the Mac sleeps after $sleep_min idle minutes, and Frametop can't connect while it sleeps."
        say "  To keep it reachable: System Settings > Battery > Options > Prevent automatic sleeping on power adapter when the display is off."
    fi
}

main
