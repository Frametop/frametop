# shellcheck shell=bash
# Sourced by the installers, after scripts/_env.sh (it uses fill_template and on_frame): the one
# copy of putting a user unit or a menu entry on the Frame from its template.
#
#   install_unit TEMPLATE [NAME]         the unit, @REPO@ filled, as NAME (default: the
#                                        template's name) in ~/.config/systemd/user
#   enable_units UNIT...                 prints the host command that reloads systemd and
#                                        enables them, for an installer's "$frame" --host step
#   install_menu_entry TEMPLATE [NAME]   the .desktop entry, the same way, in
#                                        ~/.local/share/applications
#
# FT_UNIT_DIR and FT_APPS_DIR move them (tests point them at a temp folder). Each is a word for
# the Frame's shell, as the defaults are (~ is the Frame user's home there).

# Run instead of sourced, it says so and stops.
(return 0 2>/dev/null) || { echo "scripts/install-lib.sh is sourced by the installers, not run" >&2; exit 2; }

# shellcheck disable=SC2088  # the ~ is expanded by the Frame's shell, not here
FT_UNIT_DIR=${FT_UNIT_DIR:-'~/.config/systemd/user'}
# shellcheck disable=SC2088
FT_APPS_DIR=${FT_APPS_DIR:-'~/.local/share/applications'}

install_unit() {  # install_unit TEMPLATE [NAME]
  local name=${2:-${1##*/}}
  fill_template "$1" | on_frame "mkdir -p $FT_UNIT_DIR && cat > $FT_UNIT_DIR/$name"
}

enable_units() {  # enable_units UNIT...: the text of a host command, not run here
  printf '%s' "systemctl --user daemon-reload; systemctl --user enable $*"
}

install_menu_entry() {  # install_menu_entry TEMPLATE [NAME]
  local name=${2:-${1##*/}}
  fill_template "$1" | on_frame "mkdir -p $FT_APPS_DIR && cat > $FT_APPS_DIR/$name"
}
