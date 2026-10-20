# Languages and system keyboard settings

The typing language is an app setting, separate from SteamOS's interface locale.
A fresh installation starts in English US. After Apply and save, the keyboard
reopens with the last saved language, layout and theme. A Russian system locale
does not automatically select Russian. Updating the app preserves your selection.

## Included profiles

| Language | Profile ID | Layout | Character mapping |
| --- | --- | --- | --- |
| English US | `en-us` | US full-size | US XKB |
| German | `de-de` | International full-size | German XKB |
| French | `fr-fr` | International full-size | French AZERTY, France |
| Spanish | `es-es` | International full-size | Spanish, Spain |
| Italian | `it-it` | International full-size | Italian XKB |
| Portuguese, Brazil | `pt-br` | Brazilian ABNT2 full-size | Brazilian XKB, including extra slash and keypad separator |
| Russian | `ru-ru` | US full-size | Russian ЙЦУКЕН |
| Ukrainian | `uk-ua` | US full-size | Ukrainian ЙЦУКЕН |
| Simplified Chinese | `zh-cn-pinyin` | US full-size | Local Pinyin with candidates |
| Traditional Chinese | `zh-tw-pinyin` | US full-size | Local Pinyin with Traditional output |
| Korean | `ko-kr` | US full-size | Local two-set Hangul |
| Japanese | `ja-romaji`, `ja-kana`, `ja-jis` | US or Japanese JIS | See [Japanese input](japanese.md) |

Open Settings, select Language, then Apply and save. When the current geometry
lacks required keys, the language selector chooses the smallest compatible
layout. Layout remains independently editable. Theme selection is unchanged.
French and Spanish regional variants beyond France and Spain, Chinese Zhuyin,
Cangjie and Wubi, and Korean Hanja conversion are not included.

## Independent layout selection

The normal Frame launcher sends characters as Unicode from the profile selected
in Full Keyboard. Switch languages in Settings and Apply and save. You do not
need to install matching system layouts, change SteamOS's active layout, pass a
launch flag, or restart the app. Shift, Caps Lock, AltGr, dead-key accents and
numpad digits are resolved locally. Legend overrides remain display-only; the
profile's XKB mapping determines the characters.

A Russian system locale still does not choose the initial app language. A fresh
install starts in English; subsequent launches restore your saved selection.
With several system layouts installed, Full Keyboard continues using its own
selected language. System layout-switch hotkeys do not change our selection.

Enter, arrows, Backspace, Tab and shortcuts remain native key events. For Latin
shortcuts, the app reads the compositor's advertised keymap and group to find the
right physical key. For example, the A shown on an AZERTY layout sends Ctrl+A,
not Ctrl+Q. Copy/Paste use the same lookup. A non-Latin target group uses its
Latin physical counterpart when the compositor exposes one. Applications still
define shortcut behavior; a compositor that supplies no usable keymap falls back
to conventional physical Latin positions.

Unicode output needs the Gamescope text connection. If it is unavailable, typing
is disabled with an error instead of falling back to a potentially wrong physical
layout. A failed single-character commit remains queued; release other held keys
and press Enter to retry without submitting a form. Hiding or cancellation clears
queued text. Character repeat starts after 500 ms, repeats at 25 Hz, and stops on
release/cancellation without replaying a backlog.

The developer `--input uinput` backend and Japanese external JIS/system-IME mode
remain physical-key modes. They still require the receiving keymap to match
`--target-language`. Integrated Japanese, Chinese, Korean and the ordinary XKB
profiles use independent Unicode output through the normal Frame launcher.

## Chinese and Korean composition

These profiles compose inside Full Keyboard, then submit Unicode text through
the same Gamescope text connection as the other layouts. English and the local
Japanese, Chinese and Korean modes can be selected in VR without changing the
system keymap or relaunching. The receiving app does not need its own IME enabled.
Shortcut and navigation actions remain native. This still requires a local app
that accepts Gamescope's text delivery.

For Chinese, type Pinyin without tone numbers. Use `v` for ü and an apostrophe
for syllable separation where needed. The candidate row shows five entries at a
time. Arrow Up/Down or the candidate-row arrows change the highlighted entry.
Space, number keys 1–5, or clicking a candidate selects it. If it covers the
whole reading, selection commits the text. Otherwise the remaining reading stays
available for the next selection. Enter or Commit submits the current conversion;
Escape or Cancel discards it. A second Enter sends an ordinary Enter. Both script
profiles use Pinyin; Traditional here does not mean a Zhuyin layout.

For Korean, type the displayed two-set jamo. Shift selects doubled consonants and
the shifted vowels. Caps Lock does not double jamo. Backspace undoes one typed
jamo, including changes across syllable boundaries. Enter or Commit submits the
composition. Space and other ordinary keys commit the word before sending the
key. Use the `한 / A` control to switch between Hangul and Latin typing. Chinese
has a corresponding `中文 / A` control. Punctuation uses ordinary US keys.

Both modes keep a bounded preedit. Commit before the 30-input-character limit.
Shortcuts and navigation that leave composition commit first; if delivery fails,
the preedit stays visible and the forwarding action is blocked. Hiding, dragging,
recentring, changing profiles, and closing discard unfinished preedit, as with
Japanese. Automatic character repeat is not implemented for local composition.

Chinese uses the Frame's PyZy library and dictionary; Korean uses libhangul.
Both are loaded only when their language is selected. Missing libraries disable
that composition profile, not the whole keyboard.
No IBus daemon configuration, system language setting, clipboard access, network
service, or persistent typing history is used. PyZy's conversion/selection
learning APIs are deliberately avoided, and its user database has no writable
path. A build without an engine rejects its profile on Apply while retaining the
previous profile. A saved unsupported composition profile falls back to English
at launch with an error message.

Automated tests cover profile legends, Chinese script conversion/candidates,
Korean syllables/editing, and commit ordering. A dedicated receiver checks Unicode
transport separately from composition. Fluent-speaker review and per-application
acceptance are still needed; passing a local conversion test does not establish
that every browser or desktop session accepts it.
