# Frametop Voice

Local voice input for the Steam Frame, part of Frametop. Nothing leaves the headset.

- **Dictation**: tap Right Ctrl (or hold a controller button) and speak. Words are typed as you talk wherever you type: the desktop window you clicked last, or Steam and games. Moonshine transcribes English on the headset's CPU; whisper (on its GPU) takes other languages.
- **Wake words** (off by default): say one ("Hey Computer", or your own), then a command. A command can run a program you set up for it (Voice commands); otherwise it goes to a hook script and to event subscribers. This is the entry point for a voice assistant.

It started as the standalone [frame-voice](https://github.com/DeeJanuz/frame-voice) and moved into Frametop on 2026-10-10. The input relay now does its keyboard side: it watches the push-to-talk keys and types the text.

## Install

`install.sh` asks (yes by default; `--no-voice` skips it), or later, in Frametop's folder:

```
voice/run.sh install      # fetches the models, installs and starts frametop-voice.service
voice/run.sh status       # the services, and ft-voiced's state
voice/run.sh log          # its last lines
voice/run.sh uninstall    # stops and removes the services; the models stay
```

The install downloads about 320 MB of speech models into `~/.local/share/frametop/voice/models` (`voice/models.sh`, each from a fixed URL and checked by sha256). They stay there across updates and reinstalls, and `uninstall.sh` offers to delete them. A standalone frame-voice found running is turned off (its folder stays), and its settings are carried over once: its push-to-talk keys become key combinations, its controller buttons and the rest go to `~/.config/frametop-voice.json`.

The input relay types the text, so it has to be one from the same Frametop. `voice/run.sh install` says so when the running relay is older; it starts again with SteamVR after a reboot, or right away with `systemctl --user restart frametop-input-relay`.

## Using it

| Where | How |
| --- | --- |
| Keyboard | Tap **Right Ctrl** to start, tap again to stop. It counts only on its own: a shortcut with it (Ctrl+C), another modifier held when you press it (Shift, then Right Ctrl), or a click in between isn't dictation. Left Ctrl isn't it (crouch in many games). It's a key combination with the action *Dictation: start or stop*, so change it or add others in Frametop Input Settings, Keyboard. *Keys do* on the Voice page switches them all between tap to start and stop and **hold to talk** (*Dictation: while held*): held, Right Ctrl records until you let go, and another key with it (Right Ctrl+C) makes it the shortcut after all, so that recording is dropped without a tone. |
| Mouse | Map a button to either dictation action on the Buttons page (pointer mode). |
| Steam Frame controllers | Hold left **d-pad down**, speak, release. Set the buttons and hold or press-to-toggle on the Voice page. They're read from SteamVR's web socket (`input/vrws.py`), so they work in games and the Steam menu, and whatever else the button does still happens. |
| Commands | `voice/ft-voice toggle`, `start`, `stop`, `cancel`, `type "text"`, and from a key combination, `command:ft-voice toggle` (the relay's commands have `voice/` on their path). |
| Voice | "<wake word>, ..." runs a matching voice command, otherwise `hooks/on-command`. "Hey Computer, type hello world" types "hello world" (the hook's example). |

A single word works: recordings shorter than 0.3 s (`min_seconds`) are dropped, and anything longer is decoded even when the speech detector missed it. Moonshine can hear nothing in a lone short word with silence around it, so such a window is decoded again cut to its speech, then after 0.2 s of silence, and a lone word it repeats ("Way, way, way") is typed once. Dictation works while Frametop is paused for a VR game too.

Frametop Input Settings has a **Voice** page: voice dictation on or off (on also means it starts at login), the dictation keys and whether they're tapped or held, controller buttons (capture one by pressing it) and their mode, the language and whisper model, typing as you talk, sounds, and the wake word and its phrases. Everything but the keys lives in `~/.config/frametop-voice.json`, along with voice commands and advanced settings, which you edit by hand; the app keeps them when it saves. ft-voiced applies changes within 2 s. Delete the file to go back to the defaults.

### Live text

Live text works by agreement, with the approach and model borrowed from the Faceclaw app for Even Realities G2 glasses (Moonshine base through sherpa-onnx, on the CPU): the speech since your last pause is re-decoded every 0.8 s, and words two decodes in a row agree on are typed, except the last one, whose punctuation can still change. One word the two decodes disagree on (a filler heard as "a" once and "uh" the next time) doesn't hold back the words after it once three of them agree again. At a 0.8 s pause, or at the quietest point once a decode would pass 8 s of audio (Moonshine returns nothing past about 9 s), the window is decoded one last time and its remaining words are typed.

Windows join up like one recording. Each one is decoded after the last 1.5 s of the one before it (`context_seconds`), so Moonshine hears a sentence that goes on after a pause as the continuation it is. Every decode is lined up with the words already typed, by content rather than by word count, and only what comes after them is new; a phrase you really say twice stays twice. The punctuation after a pause waits until the next window is heard: a period Moonshine put there only because the audio stopped is dropped when the next decode runs on and the word can't end a sentence ("the", "to", "we are"), so "increase the. Width" comes out as "increase the width". A window cut without a pause ends mid-speech, where Moonshine can garble or repeat the cut-off words, so only words the last live decode agrees with are typed then; the rest (up to 5) go to the next window, whose context is made long enough to hold them. After a silence, 0.5 s before the speech is kept, and anything loud in the last second the speech detector didn't flag (a short "I" before a gap), since its speech flag comes about 0.27 s late.

On test clips (two TTS voices, 1 s pauses placed mid-sentence and between sentences, replayed in real time) every mid-sentence seam came out without a stray period or capital. The last word of each phrase is typed 0.3-1.8 s after you say it. Moonshine adds fewer commas than whisper. `"engine": "whisper"` in the advanced settings switches live text to the whisper-server (it dropped words when re-decoded this way in testing). Wake word commands use whisper.

### Languages

The Voice page's **You speak** sets the language (`"language"`: `en`, `auto` to detect it, or a code such as `de`, `fr`, `ja`; the page lists 20, and any code whisper knows works in the file). English is typed as you talk by Moonshine, which knows only English. Every other language goes to whisper-server, decoded the same live way but slower, and the English-only prompt is left out.

**Whisper model** (`"model"`) picks what whisper-server loads (`voice/whisper-run` asks `voice_settings.py model-file`):

| Model | Size | Languages |
| --- | --- | --- |
| `small.en` | 190 MB | English only; fetched with the install |
| `small` | 190 MB | multilingual; the default for any other language |
| `large-v3-turbo` | 574 MB | multilingual, better and slower; next to VR it made the headset stutter in a 2026-09-26 test (Measurements) |

An English-only model with another language means `small`. A model that isn't there yet is downloaded when you choose it (`voice/models.sh model NAME`, pinned to a whisper.cpp commit and checked by sha256); until it is, small.en is loaded and the log says so. ft-voiced restarts the whisper service when the model changes.

### Typing

ft-voiced sends its text to the input relay (`type TEXT` on `@frametop_relay`, which the relay takes only from its own user). The relay types it like a keyboard, one key every 4 ms, where typing goes: to ft-screens while it's the desktop, otherwise on its own virtual keyboard, which SteamVR passes on to Steam and games. Never to both, so a dictated space can't also pause a video in the Steam app. On the desktop each character is typed with the desktop's keyboard layout (Plasma's, in `~/.config/frametop/kxkbrc`, through libxkbcommon), Shift and AltGr included: on German QWERTZ, "way" comes out as "way" and "für" as "für". Steam and games get a US layout. Curly quotes and dashes become plain ones; a character the layout has no key for (a dead key's accent, another script) is dropped, and the relay's log says which.

A character waits while Ctrl, Alt, Shift or Meta is held on any keyboard: the letters would reach apps as shortcuts. Holding Right Ctrl to talk is the exception on the desktop: the relay keeps its press from the desktop (and sends it after all if another key comes), so live text is typed while you hold it. Steam and games read the keyboard themselves, so there the text waits until you let go; that is why the default is a tap.

Sounds are off by default (Voice page): rising tone = recording, falling tone = processing, single high tone = wake word heard, low tone = cancelled or nothing heard. The start tone plays once the mic sends audio (or after 0.6 s), tones play one after another, and the mic stays open until the last one is done: on the Frame the speaker and the mic share an echo-cancelling filter, and closing the mic mid-tone could cut it. Each tone has 0.12 s of silence before it and 0.2 s after (`sounds.lead_seconds`, `tail_seconds` in the advanced settings), since a stream's start and end can be clipped.

## Control and events

```
voice/ft-voice status
voice/ft-voice wake off        # stop listening for the wake word until a restart; the mic closes when idle
voice/ft-voice wake on
voice/ft-voice wake            # act as if the wake word was heard
voice/ft-voice type "text"     # type where typing goes
voice/ft-voice events          # stream JSON events: wake, command, dictation_start, dictation_piece, dictation, dictation_cancel, command_timeout, error
```

The socket is `/run/user/<uid>/frametop-voice/control.sock` (the clients ignore `$XDG_RUNTIME_DIR`, which Frametop's desktop points elsewhere), mode 0600. It can type into any focused window, so it's owner-only. Push-to-talk from the relay comes on the abstract socket `@frametop_voice` (`toggle|start|stop|cancel <source> [quiet]`; `quiet` cancels without the tone), taken only from ft-voiced's own user.

An assistant can either replace `hooks/on-command` or subscribe with `ft-voice events` and act on `{"event": "command", "text": ...}`.

## Voice commands

Your Frametop commands ([docs/commands.md](../docs/commands.md), Input Settings, Commands) need no setup here: say the wake word, then "run" and the command's name.

A command that starts with a set phrase can run a program. Add a `"commands"` list to `~/.config/frametop-voice.json` by hand (the Voice page lists them but doesn't edit them). It is empty by default. Example: "Hey Computer, flag that" or "Hey Computer, flag that, fingers crossed" runs a hand tracking tool's flag command.

```json
  "commands": [
    {
      "say": ["flag that", "flag this", "flag it", "flag"],
      "run": ["~/.local/bin/ft-handsctl", "flag"]
    }
  ]
```

| Key | Meaning |
| --- | --- |
| `say` | Phrases. Case and punctuation don't matter. |
| `run` | The program and its arguments. `~` is expanded. It runs directly, never through a shell, so spoken words can't become shell syntax. |
| `rest` | Optional. `"arg"` (default): the words after the phrase are added as one more argument when there are any. `"none"`: they never are. |

Matching works on whole words. A command matches when its words equal a phrase or start with it, so "Flag that." and "Flag that, fingers crossed." match "flag that", but "Flagging that" and "Please flag that" don't. If phrases of several entries match, the longest wins, then the first entry. The rest keeps your words as transcribed, without the punctuation that follows the phrase or a final period: the second example runs `ft-handsctl flag "fingers crossed"`.

A matched command runs instead of `hooks/on-command`, on the host (through `distrobox-host-exec`, since ft-voiced runs in Frametop's container), as your user. The `command` event is still sent, with `"matched"` (the phrase) and `"ran"` (the argv) added. The program gets these environment variables (named as frame-voice named them, so programs written for it keep working):

| Variable | Value |
| --- | --- |
| `FRAME_VOICE_EVENT` | `voice_command` |
| `FRAME_VOICE_TEXT` | The whole transcribed command |
| `FRAME_VOICE_PHRASE` | The phrase that matched, as written in `say` |
| `FRAME_VOICE_REST` | The words after it, or empty |
| `FRAME_VOICE_WAKE_UNIX` | When the wake word was heard, in Unix seconds (e.g. `1791582811.212`) |

ft-voiced doesn't wait for the program. A malformed entry is skipped with a warning in the log (`journalctl --user -u frametop-voice`). Changes to the list apply within a couple of seconds.

## Advanced settings

`"advanced"` in `~/.config/frametop-voice.json` overrides ft-voiced's defaults (`DEFAULTS` in `ft-voiced`), by section, for example:

```json
  "advanced": {
    "dictation": {"min_seconds": 0.5, "pause_seconds": 1.0},
    "wake": {"threshold": 0.3, "command_hook": "hooks/on-command"},
    "audio": {"source": "alsa_input.some_mic"}
  }
```

They apply when the service restarts (`voice/run.sh restart`). Wake phrase tuning: raise `threshold` for fewer false triggers, raise `boost` if it misses you. Phrases can be anything spelled in English words; they're tokenized at startup, no training needed. A relative hook path is relative to `voice/`.

## Pieces

| Part | What it does |
| --- | --- |
| `ft-voiced` | The service (`frametop-voice.service`): mic capture (`pw-record`), push-to-talk, the wake word, live text, the control socket. It runs in Frametop's container (`scripts/in-box`), named `ft-voiced` there. |
| `frametop-voice-whisper.service` | `whisper-server` (whisper.cpp, Vulkan on the Adreno 750) with the chosen model (`whisper-run`; `ggml-small.en-q5_1` by default) on `127.0.0.1:8178`, for wake word commands and languages other than English. ft-voiced starts it only while something uses it (it holds about 300 MB of the GPU's memory, more for large-v3-turbo) and stops it after. |
| `ft-voice` | CLI for the control socket. |
| `voice_settings.py` | The settings file, and carrying over frame-voice's. Standard library only: the installer runs it with the host's Python, and Input Settings reads it. |
| `voice_commands.py` | Voice commands: matching, and checking the entries. |
| `hooks/on-command` | Runs after a wake word with the command as `$1` when no voice command matches. The example logs it to `~/.local/state/frametop/voice/commands.log`. |
| `build.sh` | Builds `build/bin/whisper-server` (whisper.cpp at a fixed commit, Vulkan, static) and `build/venv` (sherpa-onnx, sentencepiece, numpy; in a release, the image's locked venv). |
| `models.sh` | Fetches and checks the models; `models.sh model NAME` one of the extra whisper models. |
| `whisper-run` | Starts whisper-server with the model the settings choose. |
| `run.sh` | Installs and runs the services. |

The mic is captured once. The wake word spotter (sherpa-onnx open-vocabulary KWS, about 5% of one core) listens only while the wake word is on; with it off, the mic is open only while you dictate. Silero VAD ends commands on silence and trims clips before transcription.

At boot, ft-voiced waits until WirePlumber has run for 10 s before it opens the mic: a capture stream that connects while SteamOS builds its echo-cancel chain can leave the mic reading silence until PipeWire restarts.

## Logs and privacy

The journal has what was said: each live window's words, transcripts, and wake word commands, as frame-voice logged them for tuning. `scripts/report.sh` leaves those lines out of bug reports. Clip saving (`"save_clips"` in the advanced dictation settings, the last N recordings in `~/.local/state/frametop/voice/clips`) is off by default.

## Tests

`voice/test/test_commands.py` (standard library: matching, settings, carrying over frame-voice's) and `voice/test/test_daemon.py` (needs the image's packages: typing through a stand-in relay, push-to-talk from the relay and from a stand-in for SteamVR's web socket, short-word retries, tones, when whisper runs) and `voice/test/test_settings_page.py` (the Voice page's keys, language and model, with PySide6) run in `just test`. The relay's side is in `input/test/keys-test.py`.

## Measurements (2026-09-25, frame-voice, jfk.wav)

Server with the model loaded, `small.en-q5_1` on Vulkan:

| Clip | Latency |
| --- | --- |
| 3.2 s phrase | 0.4 s |
| 11 s sentence | 1.2 s |

Whisper always encodes a 30 s window. ft-voiced shrinks `audio_ctx` to the clip length (floor 512), which is most of that speedup. The CPU backend took over 20 s.

Bigger models (2026-09-26, 9 real dictations replayed, 512 window, VR running): `medium.en-q5_0` and `large-v3-turbo-q5_0` were no more accurate than `small.en` on this voice, took 2-3x longer (about 3 s and 3.5 s per piece versus 1 s), and loading them next to VR made the headset stutter. `small.en` stays the model.

## Limits

- Steam and games get a US keyboard layout. On the desktop, characters its layout has no key for are dropped.
- Languages other than English aren't typed as fast as you talk: whisper decodes them on the GPU, Moonshine only knows English.
- Wake word commands use the GPU. A heavy VR game may stutter briefly while one transcribes.
- Not yet verified: typing into SteamVR dashboard and overlay text fields.
