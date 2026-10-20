# Configuration and VR switching

File-based layouts, language support and editable themes are core requirements. Users must be able to add profiles, then select them inside VR without recompiling or restarting the keyboard.

The app implements runtime profiles, VR selectors, reload and persistent selection. The bundled resources are also embedded for fallback. Bundled XKB layouts include English, German, French, Spanish, Italian, Brazilian Portuguese, Russian and Ukrainian. The normal libei/Gamescope path resolves characters locally and sends Unicode, independently of system layouts. Physical uinput and external JIS still require a matching `--target-language` declaration; application coverage varies. Integrated Japanese, Chinese and Korean composition is available; see [languages and system settings](languages.md).

## Independent profiles

| Profile | Owns | Example |
| --- | --- | --- |
| Layout | Key positions, sizes, physical key IDs and shortcut actions | `layouts/en-us.json`, `layouts/international.json` |
| Language | Input keymap, key legends, locale and font preferences | `languages/en-us.json`, `languages/de-de.json` |
| Theme | Colors, gradients, rounding, sides, press movement and animation timing | `themes/graphite.json` |
| Settings | Default profile IDs, favorite layout/language pairs and pin state | `config/default.json` |

Keep these separate. The same geometry can display English or German legends, and a theme can apply to either language. A favorite saves a layout/language pair for cycling from the main keyboard. Cycling preserves the active theme. Favorite records contain `id`, `name`, `layout` and `language`. The legacy `theme` field is optional, retained when saving, and does not affect cycling. Full-size is the default, but the runtime must not assume a fixed key count or hardcode their arrangement.

The current `en-us-full` layout ID identifies the approved ANSI geometry and its fallback legends. Its name does not restrict it to English. Languages declare `required_keys` as physical action names. The German profile requires `IntlBackslash`, provided by `international-full`. Activation rejects a layout missing that key. This international variant retains the rectangular Enter from the approved design.

## File locations and discovery

Packaged defaults live in the versioned installation's `share/framekeyboard/layouts/`, `share/framekeyboard/languages/`, and `share/framekeyboard/themes/` directories. User files live under `$XDG_CONFIG_HOME/framekeyboard`, defaulting to `~/.config/framekeyboard`:

```text
config.json
layouts/*.json
languages/*.json
themes/*.json
```

The installer must preserve user files during updates and must not rewrite them with bundled defaults. The OS image is not a configuration store. Each profile has a stable `id`, a display `name`, and a `schema_version`. Settings reference IDs, not absolute paths. A user profile with the same ID can override a bundled profile; the VR picker shows its display name. Reject duplicate IDs within a source directory instead of depending on filesystem order.

Keep a compiled safe default available. If a user override is invalid, retain the last validated profile and show its filename and error in settings. On a cold start, fall back to the bundled profile or compiled default. Unknown schema versions must not be guessed or silently rewritten.

## Layouts and styles

Layout JSON defines key rectangles and actions. An optional `icon` field accepts `copy`, `paste`, `steam-frame`, or `steam-os`; omitting it renders the text label. Icons use the theme legend color and move with the key face. Modifiers latch on tap by default. Set a key's `sticky` field to `false` for a native press/release hold; bundled right Super uses this for the desktop menu. The existing validator checks bounds and overlap. The runtime validator limits profile size, key count, label length and action types. Profiles are data and cannot execute commands or scripts.

Theme JSON defines renderer properties directly. Support surface and legend colors, key-face gradients, side colors/depth, corner radii, spacing/padding and font sizing, plus hover, pressed, latched and disabled state styles. The current renderer consumes the graphite fields plus `font_size`, `small_font_size`, `padding`, `hover` and `latched`. It uses immediate hover/latched colors and animated press travel. The `disabled` color blends with the legend color to dim unavailable settings controls; keyboard keys have no separate disabled state. Per-key fonts, arbitrary spacing rules and additional state styles remain future work. Native rendering does not require CSS or JavaScript theme execution.

Validate dimensions and animation limits. In particular, press travel cannot exceed side depth, and painting must stay aligned with hit regions. A theme may change appearance but cannot change the meaning of a key or the input keymap. Geometry changes belong to layout profiles.

## Languages and actual input

A language profile selects a keymap and derives character legends from it, including Shift and AltGr levels. Optional legend overrides are keyed by physical key ID and never change output by themselves. Keep physical positions such as `KeyY` separate from the symbol that a German keymap produces there. Special actions such as Enter and Copy remain explicit actions.

Language profiles use XKB identifiers. `input_method` defaults to `xkb`; `japanese-romaji` and `japanese-kana` select local composition. `chinese-pinyin-simplified`, `chinese-pinyin-traditional`, and `korean-2set` select local Chinese/Korean composition. Other input methods and nonempty XKB options are rejected. Korean `composition_keys` and `composition_shift` objects supply configurable jamo legends by physical action name; two-set conversion belongs to libhangul. They do not redefine the engine mapping. Direct Kana profiles supply `kana` and optional `kana_shift` objects mapping physical action names to UTF-8 text. `legends.overrides` maps a key ID to a display string. In normal Frame mode, XKB resolves characters inside the app. A persistent XKB compose state handles dead-key sequences and never consults legend overrides for output. Native shortcuts use the compositor's advertised keymap and active group where available. Only uinput and external JIS retain the frozen launch declaration check, including rules, model, layout, variant and options. Character input never changes global keyboard settings or falls back silently to raw keys when Unicode delivery fails.

Prove US English and German as the first two profiles. German exercises swapped Y/Z, umlauts and AltGr. Additional languages must be addable through files where the backend already supports their input method. Local dead-key composition and Unicode delivery are tested, along with the bundled CJK engines. Arbitrary Compose-key sequences, right-to-left scripts and other IMEs still need explicit capabilities and tests; do not claim support from locale names alone. If a profile needs an unavailable capability, show the reason and leave the working profile active.

Use font fallback and text shaping for non-Latin legends. Treat interface translation separately from typing language; changing the typing language must not unexpectedly change the settings UI language. Do not use the clipboard as an implicit text-insertion fallback.

## Switching inside VR

The main toolbar has Settings, Recenter, smaller/larger zoom buttons, then Pin and, when favorites exist, a language abbreviation. The pin changes its icon when enabled without an active background. Pin prevents both movement methods and hides the line. Its state is saved immediately in the optional boolean `pinned` field of `config.json`. It does not stop dashboard following, resizing or recentering. Missing `pinned` defaults to false. A new install has no favorites.

Settings has Back, Apply and save, and Reload config icons at the top left. The Languages and Layouts card contains Layout and Language dropdowns and a Favorite checkbox. The Experimental card contains left-aligned Replace native keyboard and Start with SteamOS labels followed by their checkboxes. Start with SteamOS is disabled while replacement is unchecked. The General card contains a Theme dropdown and vertically aligned Hide numpad, Use Drag to Move and Auto leveling checkboxes, with labels on the right. Dropdowns have no search and show up to five choices. Scroll to reach the rest, click a choice to select it, or click outside the list to dismiss it. Dismissal consumes that click. Escape in the desktop preview dismisses a list first, then leaves Settings.

The card row scrolls horizontally when more cards are added. Each card has its own vertical scroll offset when its contents overflow. Cards and fields outside their viewports are hidden from painting and hit testing. Dropdowns are drawn and hit-tested above those clips and can extend beyond the keyboard case. Use scroll input or drag a scrollbar with the trigger. In the desktop preview, Shift plus the mouse wheel scrolls horizontally. A vertical wheel over a card with no vertical overflow scrolls the card row. The production UI has three cards, Languages and Layouts, General and Experimental; automated tests supply extra cards and fields to exercise both axes.

Num Lock is saved immediately as the optional global boolean `num_lock`, defaulting to false. It survives closing, reopening, layout changes and favorite cycling. Restoring it sets local legends and Unicode mapping without sending startup input. In physical uinput and external JIS modes, keypad input first reconciles the target lock through reported LED or XKB modifier state. If synchronization is needed, the panel asks you to press the keypad key again after feedback arrives. Targets without lock feedback cannot receive lock-dependent keypad input safely; ordinary keys remain available.

The General card's Hide numpad checkbox removes Num Lock and all Numpad keys, narrows the case, and keeps the remaining keys at the same size. It is saved as the optional global boolean `hide_numpad`, defaulting to false, and applies to every layout and favorite. Showing the numpad again restores its remembered Num Lock state. The transparent space beside the narrower case is excluded from VR interaction. Settings retains its full card area.

Checkbox edits stay pending across dropdown selections. Apply and save activates the chosen profiles and saves all pending favorites, numpad visibility and movement behavior; Back discards pending edits. The checkbox reflects the current layout/language pair, irrespective of theme. Unchecking removes duplicate legacy records of that pair. At most 32 favorite records can be saved.

The main language button cycles through the default saved by Apply and save, then unique valid favorites in saved order. A default that is already a favorite appears once. If the only favorite matches the default, the button has no other state to cycle to. Invalid or unavailable favorites are skipped. Cycling changes the current keyboard without replacing the saved default. Saving pin state while on a favorite also preserves that default. Reload rereads profiles, favorites, pin state, Num Lock and numpad visibility, closes dropdowns, cancels UI captures, and resets pending edits while retaining the current active keyboard. Malformed config edits report an error and retain the last loaded global settings and favorites. Newly added profile files appear after reload; an automatic file watcher is optional.

Japanese Romaji, Kana and external JIS modes are choices in the Language dropdown. They use the same compatibility validation and Apply path as other languages.

Validate and prepare a candidate configuration off to the side. On Apply, cancel active pointer presses, stop repeat and release held keys through the old backend before changing mappings. Clear latched modifiers, replace geometry and hit regions together, and redraw. Preserve the target application's focus. A failed apply keeps the previous working configuration and reports the error in VR.

Persist successful selection to `config.json` using an atomic write. If saving fails, keep the usable runtime configuration and report that it will not survive restart. Reloading files must not overwrite unrelated favorites or silently activate an edited profile while a key is held. The active selection survives app restart; the installer preserves user files across application updates. Reload releases held keys and reloads favorites. After Apply or Reload, typing requires `--start-enabled`, a live backend, and an available Unicode connection. Only physical uinput/external JIS require the selected language to match the declared target keymap. In those physical modes, a saved mismatch also permits startup with Settings accessible. A mismatch leaves the panel visible but input-disabled until a matching profile is selected or the app is reopened with matching target options. It keeps the active selection rather than activating a config.json edit behind the user's back.

## Implementation order

Profile parsing, catalog discovery, native rendering and VR selector code are implemented. Next verify controller interaction and real language output in both target sessions. See the plan and acceptance checks for the remaining integration work.

The General card has **Use Drag to Move**, saved as the optional boolean `use_drag_to_move`. It defaults to false, including for older configurations. When off, hold the trigger on the white line below the keyboard to move it. When on, point at the keyboard and hold grab/grip to move it. The line is hidden in grab mode. Pinning disables both movement methods and hides the line. Apply and save activates this preference; Back discards pending changes.

## Saved VR placement

`placement.json` is separate from profile selection and honors `--config-dir`. Schema 1 stores `space: "standing"`, a decimal-string `universe` ID, a three-by-four rigid `transform`, and `width_m` between 0.45 and 2.0. An optional `dashboard_anchor` stores the last rigid bottom-center dashboard pose. On reopen the saved relative offset is applied to the current dashboard. The `dashboard_full_rotation` flag distinguishes full-orientation bar poses from older yaw-only saves, which migrate without an initial tilt jump. An optional `dashboard_bar` stores the matching bar pose so the bottom-center anchor can follow app tabs while the main tab is hidden. Legacy files without this field remain valid and use their world-space pose until reset. The app writes it atomically on orderly close, including SIGTERM. It stores position, orientation and size, with no key input data. A missing or invalid file recenters on first launch. A relaunch of an already running instance always requests recentering, without reloading this file. The next close saves the new position. Forced termination cannot save the latest movement.

The General card also has **Auto leveling**, saved as the optional boolean `auto_leveling`. It defaults to true, including for older configurations. When enabled, a sideways roll within 5° eases level over 500 ms after releasing movement, preserving pitch. When disabled, the chosen roll remains unchanged. Applying or reloading a change preserves the displayed pose; enabling starts a fresh easing interval. Back discards pending changes.

## Optional native keyboard replacement

Experimental → Replace native keyboard is saved as the optional boolean `replace_native_keyboard`, defaulting to false. Apply and save activates the preference; Back discards pending edits. The installed VR launch must have typing enabled, a working libei/Unicode backend and access to Steam's existing local debugger at `127.0.0.1:8080`. This dependency applies only to replacement. Enabling the option leaves Developer Mode, ports, system services and autostart unchanged. Missing requirements leave replacement inactive and Steam's keyboard usable. Settings exposes **Use Steam keyboard** during an active native request while this option is on. That action restores native for the current request; future requests may use Full Keyboard again. See [connection requirements, coverage and failure behavior](native-replacement.md).

With replacement enabled in VR, the main × dismisses the typing session and hides the panel while the app stays resident. Native summon requests can then reopen Full Keyboard without another manual launch. **Quit Full Keyboard** is shown only in Settings while replacement is enabled; that action exits the app and restores native fallback. With replacement disabled, × retains its normal quit behavior.

## Manual updates

The Settings header has a centered Check for updates button. Its latest-version message resets after five seconds; an available update remains offered until the process exits. This state is not a saved preference. Download and install update verifies, stages and restarts into a newer stable GitHub release. It preserves configuration and placement. See [updates](updates.md).

Experimental → Start with SteamOS is saved as the optional boolean `start_with_steamos`, defaulting to false. Its choice is remembered while replacement is off, but startup is active only when both saved booleans are true. Applying either option updates the user-local startup unit on the preference worker; the VR loop keeps running. See [background startup](native-replacement.md#start-with-steamos).

Saving VR preferences couples the replacement and startup booleans to the startup-unit transaction. If unit configuration fails, other preferences still save, those two saved booleans keep their prior values, and a separate startup warning explains which changes were not saved. If the JSON write itself fails, the previous unit and enablement link are restored and the user manager is refreshed. A later successful save clears the warning or error.

Reload also reconciles the startup unit with valid saved settings on the same worker. It does not rewrite JSON or activate an edited profile. A failed reconciliation reports a startup configuration error; Reload retries it. Preview and render modes do not configure startup.
