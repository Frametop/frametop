# scarlet-voice

Local voice input for the Steam Frame. Nothing leaves the headset.

- **Dictation**: hold a key, speak, release. The text is typed into whatever has focus (desktop apps, search bars, terminals, game-mode windows) through a virtual uinput keyboard.
- **Wake word**: say "Hey Scarlet", then a command. The command is transcribed and handed to a hook script and to any event subscribers. This is the entry point for the future Scarlet assistant.

## Pieces

| Part | What it does |
| --- | --- |
| `scarlet-whisper.service` | `whisper-server` (whisper.cpp, Vulkan on the Adreno 750) with `ggml-small.en-q5_1` loaded, on `127.0.0.1:8178` |
| `scarlet-voice.service` | `scarlet_voice.py`: mic capture, hotkey, wake word, typing, control socket |
| `scarlet-vr-input.service` | `scarlet_vr_input.py`: controller push-to-talk, reads left d-pad down from SteamVR's raw input stream |
| `scarlet-config` | Settings app (GTK 4 / libadwaita): pick push-to-talk buttons by pressing them. In the app menu as *Scarlet Voice Settings* |
| `scarletctl` | CLI for the control socket |
| `hooks/on-command` | Runs after "Hey Scarlet ..." with the command text as `$1` |

The mic is captured once with `pw-record`. The wake word spotter (sherpa-onnx open-vocabulary KWS, about 5% of one core) listens continuously. Silero VAD ends commands on silence and trims clips before transcription.

## Triggers

| Where | How |
| --- | --- |
| Keyboard or mouse | Tap **Right Ctrl** (default) to start, tap again to stop. Shortcuts with it (Ctrl+C) don't start or stop dictation. Hold mode is available in the settings app. |
| Steam Frame controllers | Hold left **d-pad down** (default), speak, release. Works in games, the Steam UI and the desktop with no per-app setup. The button is observed, not captured, so whatever else it is bound to still happens. |
| Desktop mode shortcut | KDE System Settings > Shortcuts > add a command shortcut running `scarletctl toggle`. |
| Scripts, the assistant | `scarletctl toggle`, `start`, `stop`, `cancel`, `type "text"` |
| Voice | "Hey Scarlet, ..." runs `hooks/on-command`. "Hey Scarlet, type hello world" types "hello world" (example in the hook). |

Recordings shorter than 2 s (`min_seconds`) are discarded with the cancel tone, so brief or accidental presses never type anything.

Change the buttons in *Scarlet Voice Settings* (`./scarlet-config`): **Add** waits for you to press a controller button, key or mouse button (not left/right click, not the controller's system button). The app also sets hold or tap mode per trigger and switches live typing, sounds and the wake word. Everything lives in `~/.config/scarlet-voice/bindings.json`; both services apply changes without a restart. Delete the file to go back to the defaults.

Live text works by agreement, with the approach and model borrowed from the Faceclaw app for Even Realities G2 glasses (Moonshine base through sherpa-onnx, on the CPU): the speech since your last pause is re-decoded every 0.8 s, and words two decodes in a row agree on are typed, except the last one, whose punctuation can still change. At a 0.8 s pause, or at the quietest point once the window reaches 8 s, the window is decoded one last time and its remaining words are typed. On test audio the first words appeared at about 2.3 s, then every 1-1.5 s, and the text was complete 0.1-0.2 s after release. Nothing is cut while it is still being decoded, so words aren't split or doubled at the edges the way fixed pieces did. Moonshine adds fewer commas than whisper, and a long pause can still end a sentence with a period. `engine = "whisper"` switches live text to the whisper-server (it dropped words when re-decoded this way in testing). The "Hey Scarlet" command path still uses whisper.

Live text never types while Ctrl, Alt, Shift or Super is held on any keyboard: the letters would reach apps as shortcuts. It waits for the release. That is why the keyboard default is tap mode: holding Right Ctrl to talk would hold back the text until you let go. (Pressing and releasing Ctrl on the virtual keyboard to "unhold" it did not work: letters still arrived as Ctrl shortcuts in the Frametop desktop.)

Audio cues are off by default (`[sounds] enabled = true` turns them on): rising tone = recording, falling tone = processing, single high tone = wake word heard, low tone = cancelled or nothing heard.

## Control and events

```
./scarletctl status
./scarletctl wake off        # stop wake word listening; the mic fully closes when idle
./scarletctl wake on
./scarletctl wake            # act as if "Hey Scarlet" was heard
./scarletctl type "text"     # type into the focused app
./scarletctl events          # stream JSON events: wake, command, dictation, command_timeout, error
```

The socket is `$XDG_RUNTIME_DIR/scarlet-voice/control.sock`, mode 0600. It can type into any focused window, so keep it owner-only.

An assistant can either replace `hooks/on-command` or subscribe with `scarletctl events` and act on `{"event": "command", "text": ...}`.

## Config

Copy `config.example.toml` to `~/.config/scarlet-voice/config.toml`. Every key is optional. Then `systemctl --user restart scarlet-voice`.

Wake phrase tuning: raise `threshold` for fewer false triggers, raise `boost` if it misses you. Phrases can be anything spelled in English words; they are tokenized at startup, no training needed.

## Services

```
systemctl --user status scarlet-whisper scarlet-voice scarlet-vr-input
journalctl --user -u scarlet-voice -f
systemctl --user stop scarlet-voice scarlet-whisper scarlet-vr-input   # the units are linked, so `disable` removes the links
```

## Build notes

whisper.cpp is in `vendor/whisper.cpp`, built with:

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGGML_VULKAN=ON -DGGML_NATIVE=OFF \
  -DGGML_CPU_ARM_ARCH=armv9.2-a+dotprod+i8mm+sve2+fp16 \
  -DCMAKE_PREFIX_PATH=$PWD/../prefix \
  -DCMAKE_C_FLAGS="-mtune=cortex-a720 -I$PWD/../prefix/include" \
  -DCMAKE_CXX_FLAGS="-mtune=cortex-a720 -I$PWD/../prefix/include"
cmake --build build -j5 --target whisper-server whisper-cli whisper-bench
```

`vendor/prefix` holds SPIRV-Headers, which SteamOS does not ship. The Python side lives in `.venv` (`evdev sherpa-onnx numpy sentencepiece websockets`). `models/sherpa-onnx-moonshine-base-en-quantized-2026-02-27` holds the Moonshine files from huggingface.co/csukuangfj2 (`encoder_model.ort`, `decoder_model_merged.ort`, `tokens.txt`). The settings app uses the system Python for GTK (`gi`) and the venv for the rest; the `scarlet-config` launcher sets that up.

The whisper service sets `DISABLE_GAMESCOPE_WSI=1`. Without it the Gamescope Vulkan layer aborts whisper-server at startup.

Typing goes through a uinput keyboard that SteamVR must have open: SteamVR opens `/dev/input` devices only when it starts, and gamescope gets keyboard input from SteamVR. `scarlet-voice.service` is ordered `Before=steamvr.service` and parks the device's fd in systemd's fd store, so daemon restarts keep the same device. After the very first install, restart SteamVR once. The device is a full keyboard (fd store name `typer2`; the first version, `typer`, had only the typing keys and is replaced automatically, which also needs one SteamVR restart). Multidesk's input relay picks the device up immediately for its desktop screens. To drop the device: `systemctl --user clean --what=fdstore scarlet-voice`.

Controller input: SteamVR sends OpenVR actions only to the app with input focus, and while the 2D Steam or desktop screens are up the compositor's laser mouse holds focus, so a background or overlay OpenVR app never sees the button there. `scarlet_vr_input.py` instead subscribes to vrserver's local web socket (`ws://127.0.0.1:27062`, mailbox `input_server`, `request_input_state_updates`), the stream the controller binding page uses for its live view. It is undocumented and could change with a SteamVR update. `scarlet-vr-input.service` has no `After=steamvr.service`: with `WantedBy=default.target` that is an ordering cycle at boot.

## Measurements (2026-09-25, jfk.wav)

Server with the model loaded, `small.en-q5_1` on Vulkan:

| Clip | Latency |
| --- | --- |
| 3.2 s phrase | 0.4 s |
| 11 s sentence | 1.2 s |

Whisper always encodes a 30 s window. The daemon shrinks `audio_ctx` to the clip length (floor 512), which is most of that speedup. The CPU backend took over 20 s. This process only gets CPUs 0-4; the faster cores are reserved.

Bigger models (2026-09-26, 9 real dictations replayed, 512 window, VR running): `medium.en-q5_0` and `large-v3-turbo-q5_0` were no more accurate than `small.en` on this voice (the one difference: "T3 Code Write" vs "right"), took 2-3x longer (about 3 s and 3.5 s per piece versus 1 s), and loading them next to VR made the headset stutter. Turbo also added stray "..." on streamed pieces. Below a 512 window both loop and take 10-20 s. `small.en` stays the default; both bigger models are still in `models/` (about 1.1 GB).

## Limits

- Typing assumes a US keyboard layout and ASCII. Accents are stripped, other characters dropped.
- Transcription uses the GPU. A heavy VR game may stutter briefly while a clip transcribes. `ggml-base.en.bin` is faster and lighter if that matters.
- Not yet verified: typing into SteamVR dashboard/overlay text fields and into apps inside VR games.
