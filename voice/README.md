# Frametop Voice

Local voice input for the Steam Frame, part of Frametop. Nothing leaves the headset.

- **Dictation**: tap Right Ctrl (or hold a controller button), speak, and stop. What you said is typed where you type: the desktop window you clicked last, or Steam and games. While you talk a pill in the headset shows that it's listening, and nothing is typed until you stop: the recognizer hears the whole recording at once, which is what makes it accurate. Parakeet transcribes 25 languages on the headset's CPU; whisper (on its GPU) takes the others.
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

The install downloads about 870 MB of speech models into `~/.local/share/frametop/voice/models` (`voice/models.sh`, each from a fixed URL and checked by sha256). They stay there across updates and reinstalls, and `uninstall.sh` offers to delete them. A standalone frame-voice found running is turned off (its folder stays), and its settings are carried over once: its push-to-talk keys become key combinations, its controller buttons and the rest go to `~/.config/frametop-voice.json`.

The input relay types the text, so it has to be one from the same Frametop. `voice/run.sh install` says so when the running relay is older; it starts again with SteamVR after a reboot, or right away with `systemctl --user restart frametop-input-relay`.

## Using it

| Where | How |
| --- | --- |
| Keyboard | Tap **Right Ctrl** to start, tap again to stop. It counts only on its own: a shortcut with it (Ctrl+C), another modifier held when you press it (Shift, then Right Ctrl), or a click in between isn't dictation. Left Ctrl isn't it (crouch in many games). It's a key combination with the action *Dictation: start or stop*, so change it or add others in Frametop Settings, Keyboard. *Keys do* on the Voice page switches them all between tap to start and stop and **hold to talk** (*Dictation: while held*): held, Right Ctrl records until you let go, and another key with it (Right Ctrl+C) makes it the shortcut after all, so that recording is dropped without a tone. |
| Mouse | Map a button to either dictation action on the Buttons page (pointer mode). |
| Steam Frame controllers | Hold left **d-pad down**, speak, release. Set the buttons and hold or press-to-toggle on the Voice page. They're read from SteamVR's web socket (`input/vrws.py`), so they work in games and the Steam menu, and whatever else the button does still happens. |
| Commands | `voice/ft-voice toggle`, `start`, `stop`, `cancel`, `type "text"`, and from a key combination, `command:ft-voice toggle` (the relay's commands have `voice/` on their path). |
| Voice | "<wake word>, ..." runs a matching voice command, otherwise `hooks/on-command`. "Hey Computer, type hello world" types "hello world" (the hook's example). |

A single word works: recordings shorter than 0.3 s (`min_seconds`) are dropped as slips, and a longer one is typed when the speech detector hears speech in it; one with only room noise types nothing. Dictation works while Frametop is paused for a VR game too.

Frametop Settings has a **Voice** page: voice dictation on or off (on also means it starts at login), the dictation keys and whether they're tapped or held, controller buttons (capture one by pressing it) and their mode, the language (and the whisper model, for a language whisper takes), sounds, and the wake word and its phrases. Everything but the keys lives in `~/.config/frametop-voice.json`, along with voice commands and advanced settings, which you edit by hand; the app keeps them when it saves. ft-voiced applies changes within 2 s. Delete the file to go back to the defaults.

### From speech to text

The method is the one dictation apps such as Superwhisper use: record, cut the silence, decode once, type. (Frametop Voice typed English as you talked until 2026-11, by decoding the speech so far every 0.8 s with Moonshine and typing the words two decodes agreed on. Hearing half sentences cost accuracy, and the pill in the headset now says what live text was there to say: that it hears you.)

1. **Record.** The mic's audio is kept while you hold or until you tap again, up to 120 s (`max_seconds`). The pill shows *listening* with the mic's level.
2. **Cut the silence.** Silero VAD finds the speech. The silence before, after and inside it is cut down to 0.6 s around each stretch of speech, so a long think costs no decoding and the recognizer isn't left guessing at room noise. A recording without speech stops here.
3. **Decode once.** Parakeet TDT 0.6B v3 (NVIDIA's model, through sherpa-onnx, on the CPU) writes the text with its punctuation and capitals. It loads when a recording starts (about 2 s, while you talk) and leaves memory again after 2 minutes unused (`unload_after_seconds`; `0` keeps it).
4. **Type.** The pill shows *typing* from the release until the text is typed, with a space after it (`trailing_space`).

A recording longer than 20 s (`max_piece_seconds`) is decoded in pieces while it's still being made: a decode's time and memory grow with its audio, and this way the release waits for the last piece only. A piece ends in the middle of its longest pause of 0.3 s or more (`min_pause_seconds`) in its second half, or failing one after its quietest moment. The pieces are typed as one text. The recognizer reads the end of a piece as the end of a sentence, so a period it put after a word no sentence ends on ("the", "to", "we are") is dropped along with the capital after it: "increase the. Width" comes out as "increase the width".

Wake word commands are decoded the same way. `"engine": "whisper"` in the advanced dictation settings sends every language to whisper-server instead.


### Languages

The Voice page's **You speak** sets the language (`"language"`: `en`, `auto` to detect it, or a code such as `de`, `fr`, `ja`; the page lists 31, and any code whisper knows works in the file). Parakeet takes the 25 it knows: Bulgarian, Croatian, Czech, Danish, Dutch, English, Estonian, Finnish, French, German, Greek, Hungarian, Italian, Latvian, Lithuanian, Maltese, Polish, Portuguese, Romanian, Russian, Slovak, Slovenian, Spanish, Swedish and Ukrainian. It tells them apart itself, so the setting only decides who recognizes. Every other language, and `auto`, goes to whisper-server (the English-only prompt is left out), and so does everything while Parakeet's model isn't there (the log says so; `voice/models.sh` fetches it).

**Whisper model** (`"model"`) picks what whisper-server loads for those (`voice/whisper-run` asks `voice_settings.py model-file`); the page shows it only then:

| Model | Size | Languages |
| --- | --- | --- |
| `small.en` | 190 MB | English only; fetched with the install, and what whisper-server falls back to |
| `small` | 190 MB | multilingual; the default for any other language |
| `large-v3-turbo` | 574 MB | multilingual, better and slower; next to VR it made the headset stutter in a 2026-09-26 test (Measurements) |

An English-only model with another language means `small`. A model that isn't there yet is downloaded when you choose it (`voice/models.sh model NAME`, pinned to a whisper.cpp commit and checked by sha256); until it is, small.en is loaded and the log says so. ft-voiced restarts the whisper service when the model changes.

### Typing

ft-voiced sends its text to the input relay (`type TEXT` on `@frametop_relay`, which the relay takes only from its own user). The relay types it like a keyboard, one key every 4 ms, where typing goes: to ft-screens while it's the desktop, otherwise on its own virtual keyboard, which SteamVR passes on to Steam and games. Never to both, so a dictated space can't also pause a video in the Steam app. On the desktop each character is typed with the desktop's keyboard layout (Plasma's, in `~/.config/frametop/kxkbrc`, through libxkbcommon), Shift and AltGr included: on German QWERTZ, "way" comes out as "way" and "für" as "für". An accent the layout types with a dead key is that key, then the letter: "ê" on French AZERTY is ^, then e. Native Desktop, while it has gamescope's focus, gets its own layout (`~/.config/kxkbrc`) the same way; Steam and games get a US layout. Curly quotes and dashes become plain ones; a character the layout has no key for (another script) is dropped, and the relay's log says which.

A character waits while Ctrl, Alt, Shift or Meta is held on any keyboard: the letters would reach apps as shortcuts. The text comes after you stop, so with hold to talk on Right Ctrl the key is up again by then.

Sounds are off by default (Voice page): rising tone = recording, falling tone = processing, single high tone = wake word heard, low tone = cancelled or nothing heard. The start tone plays once the mic sends audio (or after 0.6 s), tones play one after another, and the mic stays open until the last one is done: on the Frame the speaker and the mic share an echo-cancelling filter, and closing the mic mid-tone could cut it. Each tone has 0.12 s of silence before it and 0.2 s after (`sounds.lead_seconds`, `tail_seconds` in the advanced settings), since a stream's start and end can be clipped.

## Control and events

```
voice/ft-voice status
voice/ft-voice wake off        # stop listening for the wake word until a restart; the mic closes when idle
voice/ft-voice wake on
voice/ft-voice wake            # act as if the wake word was heard
voice/ft-voice type "text"     # type where typing goes
voice/ft-voice events          # stream JSON events: wake, command, dictation_start, dictation, dictation_cancel, command_timeout, error
```

The socket is `/run/user/<uid>/frametop-voice/control.sock` (the clients ignore `$XDG_RUNTIME_DIR`, which Frametop's desktop points elsewhere), mode 0600. It can type into any focused window, so it's owner-only. Push-to-talk from the relay comes on the abstract socket `@frametop_voice` (`toggle|start|stop|cancel <source> [quiet]`; `quiet` cancels without the tone), taken only from ft-voiced's own user.

An assistant can either replace `hooks/on-command` or subscribe with `ft-voice events` and act on `{"event": "command", "text": ...}`.

## Voice commands

Your Frametop commands ([docs/commands.md](../docs/commands.md), Frametop Settings, Commands) need no setup here: say the wake word, then "run" and the command's name.

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
    "dictation": {"min_seconds": 0.5, "unload_after_seconds": 600},
    "wake": {"threshold": 0.3, "command_hook": "hooks/on-command"},
    "audio": {"source": "alsa_input.some_mic"}
  }
```

They apply when the service restarts (`voice/run.sh restart`). Wake phrase tuning: raise `threshold` for fewer false triggers, raise `boost` if it misses you. Phrases can be anything spelled in English words; they're tokenized at startup, no training needed. A relative hook path is relative to `voice/`.

## Pieces

| Part | What it does |
| --- | --- |
| `ft-voiced` | The service (`frametop-voice.service`): mic capture (`pw-record`), push-to-talk, the wake word, a recording's way to its text, the control socket. It runs in Frametop's container (`scripts/in-box`), named `ft-voiced` there. |
| `voice_engine.py` | The recognizer: Parakeet's model (loaded when wanted, dropped when idle), cutting a recording down to its speech, cutting a long one into pieces and joining their texts. |
| `frametop-voice-whisper.service` | `whisper-server` (whisper.cpp, Vulkan on the Adreno 750) with the chosen model (`whisper-run`; `ggml-small.en-q5_1` by default) on `127.0.0.1:8178`, for the languages Parakeet doesn't know. ft-voiced starts it only while one of those is the language (it holds about 300 MB of the GPU's memory, more for large-v3-turbo) and stops it after. |
| `ft-voice` | CLI for the control socket. |
| `voice_settings.py` | The settings file, and carrying over frame-voice's. Standard library only: the installer runs it with the host's Python, and Frametop Settings reads it. |
| `voice_commands.py` | Voice commands: matching, and checking the entries. |
| `hooks/on-command` | Runs after a wake word with the command as `$1` when no voice command matches. The example logs it to `~/.local/state/frametop/voice/commands.log`. |
| `build.sh` | Builds `build/bin/whisper-server` (whisper.cpp at a fixed commit, Vulkan, static) and `build/venv` (sherpa-onnx, sentencepiece, numpy; in a release, the image's locked venv). |
| `models.sh` | Fetches and checks the models (Parakeet, whisper small.en, the wake word spotter, the VAD); `models.sh model NAME` one of the extra whisper models. |
| `whisper-run` | Starts whisper-server with the model the settings choose. |
| `run.sh` | Installs and runs the services. |

The mic is captured once. The wake word spotter (sherpa-onnx open-vocabulary KWS, about 5% of one core) listens only while the wake word is on; with it off, the mic is open only while you dictate. Silero VAD ends commands on silence and finds the speech in a recording before transcription.

At boot, ft-voiced waits until WirePlumber has run for 10 s before it opens the mic: a capture stream that connects while SteamOS builds its echo-cancel chain can leave the mic reading silence until PipeWire restarts.

## Logs and privacy

The journal has what was said: transcripts and wake word commands, as frame-voice logged them for tuning. `scripts/report.sh` leaves those lines out of bug reports. Clip saving (`"save_clips"` in the advanced dictation settings, the last N recordings in `~/.local/state/frametop/voice/clips`) is off by default.

## Tests

`voice/test/test_commands.py` (standard library: matching, settings, which engine a language gets, carrying over frame-voice's), `voice/test/test_engine.py` (numpy: cutting silence, cutting and joining pieces, when the model loads and leaves memory, with a stand-in for it), `voice/test/test_daemon.py` (needs the image's packages: typing through a stand-in relay, push-to-talk from the relay and from a stand-in for SteamVR's web socket, a recording typed once or in pieces, tones, when whisper runs) and `voice/test/test_settings_page.py` (the Voice page's keys, language, engine and model, with PySide6) run in `just test`. The relay's side is in `input/test/keys-test.py`.

## Measurements

### Parakeet (2026-11-27, on the Frame, SteamVR running)

Text after the release, model loaded, recordings replayed through ft-voiced's own path:

| Recording | 2 threads | 1 thread |
| --- | --- | --- |
| One word (1.3 s) | 0.1 s | 0.2 s |
| A sentence (8.5 s) | 0.6 s | 0.8 s |
| Two sentences with a 6 s think between (22 s) | 1.1 s | 1.5 s |
| 66 s with pauses, in 4 pieces | 1.3 s | 1.6 s |

Decoding takes about 7% of the speech's length on two threads (17 s in 1.2 s) and 11% on one (1.9 s). ft-voiced holds about 100 MB without the model, 1.2 to 1.4 GB with it, and is back under 150 MB after the model leaves. One decode of 50 s took 7.5 s and 2.1 GB, which is what the pieces are for. The model loads in 1.6 to 2.2 s.

Against Moonshine base, the model before it, on the same clips: "lazy dog" where Moonshine heard "lazy book", "Thank you." for "Staying, you.", "Hello?" for "Homer?". German, Spanish and French test sentences came out right with the language set to English. Padding matters: with 0.25 s kept around the speech, "The quick brown fox" after a pause lost its "The" in half the tries (the VAD finds a soft first word 0.4 to 0.7 s late); with 0.4 s and 0.6 s, in none.

### whisper-server (2026-09-25, frame-voice, jfk.wav)


Server with the model loaded, `small.en-q5_1` on Vulkan:

| Clip | Latency |
| --- | --- |
| 3.2 s phrase | 0.4 s |
| 11 s sentence | 1.2 s |

Whisper always encodes a 30 s window. ft-voiced shrinks `audio_ctx` to the clip length (floor 512), which is most of that speedup. The CPU backend took over 20 s.

Bigger models (2026-09-26, 9 real dictations replayed, 512 window, VR running): `medium.en-q5_0` and `large-v3-turbo-q5_0` were no more accurate than `small.en` on this voice, took 2-3x longer (about 3 s and 3.5 s per piece versus 1 s), and loading them next to VR made the headset stutter. `small.en` stays the model.

## Limits

- Steam and games get a US keyboard layout. On the desktop, characters its layout has no key for are dropped.
- Nothing is typed while you talk; the text comes 0.1 to 1.6 s after you stop.
- Parakeet writes one voice. With a second voice in the recording (someone talking nearby, a video) it can drop one speaker's sentences, in testing sometimes all of them.
- Parakeet holds 1.2 to 1.4 GB of memory while loaded: during a dictation and for 2 minutes after the last one.
- A recording with no pause for 20 s is cut at its quietest moment, which can fall inside a word.
- Languages Parakeet doesn't know go to whisper on the GPU, which is slower and less accurate with the small model.
- Not yet verified: typing into SteamVR dashboard and overlay text fields.
