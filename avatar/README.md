# Avatars and hand models

How you look in Frametop: the hands ft-handview draws for you today, and the avatar other people will see in a shared room ([docs/together.md](../docs/together.md), "Avatars and hands"). This folder has the model file, the importer that writes it, and Frametop Settings' Appearance page.

| File | What |
|---|---|
| `modelfile.h`, `modelfile.cpp` | Frametop's own model file (`.ftmodel`): the format, a strict reader, a writer. No GPU, SteamVR or network code |
| `import/` | `ft-model-import`: a VRM, glTF, GLB or FBX model into a `.ftmodel` (`build.sh` builds it into `avatar/build/`) |
| `ftmodel.py` | The same file read in Python, for the tests and the Settings page |
| `hand_models.py`, `ft_avatar_settings.py`, `AvatarPages.qml` | Frametop Settings' Appearance page: the hand models with their pictures, the look, import and remove |
| `tests/` | `modelfile-test.cpp` (the reader against damaged files, in `just test-c`), `test_import.py` (the importer on made-up rigs and the bundled hands), `test_appearance.py` (the page's backend) |

## The model file

One format on disk, for three reasons: the programs that draw (ft-handview now, the avatar renderer later) carry no glTF or FBX code; reducing and bone matching happen once, at import; and when avatars are sent between people, the file a stranger sends is read by one small reader that trusts nothing, never by the importers.

The layout is in `modelfile.h`. In short: `FTMD`, a version, then chunks, each a four-letter tag, a size and a payload. `INFO` holds the name, licence, author and source format as text, and whether the model may be sent to other people. `HAND` holds one hand: a skinned mesh on WebXR's 25 hand joints with their bind positions, positions and normals quantized, four joints and weights a vertex. A reader skips chunks it doesn't know, which is how bodies will be added.

The reader checks every count, size, index and number before using it, and refuses the whole file on the first one that is wrong. Limits: 20,000 vertices and 40,000 triangles a hand, 64 MB a file, 64 chunks, 96 bytes of text a field with no control characters. `modelfile-test.cpp` breaks each rule on its own, cuts the file at every byte and changes every byte, under the address and undefined-behaviour sanitizers.

A model's name is its file's name: `NAME.ftmodel`, with NAME lower-case letters, digits, `-` and `_`, at most 31. Your models are in `~/.local/share/frametop/avatars` (`$XDG_DATA_HOME`; `FT_AVATARS_DIR` names another folder).

## The importer

```
avatar/build/ft-model-import FILE [--name NAME] [--to DIR] [--side left|right] [--may-share] [--check]
```

It reads a VRM (0.x or 1.0), glTF or GLB with cgltf, or an FBX with ufbx, in the rest pose, and for now takes the hands. `import/main.cpp` has the options and what it prints: a fact a line, which the Settings page reads, or `error` and the reason.

**Finding the hand bones** (`import/handbones.cpp`). A VRM names its humanoid bones, so those are used. Otherwise the bones' names are split into words and matched: a hand is a bone the mesh moves with whose name says hand or wrist and a side, and each finger a chain under it named for that finger. That reads rigs named as Mixamo, Unity, Blender and Unreal name them, and the flat skeletons of the WebXR and Babylon hands. A thumb has three or four bones; a finger three, four or five (with or without a metacarpal and a tip). Nothing is guessed: a skeleton whose bones can't be told is refused, with the bone that is missing or the count that doesn't fit, and nothing is written. A file with one hand gets the other mirrored.

**Cutting the hand out** (`import/handpart.cpp`). The mesh the hand's bones move is kept, and the forearm's as far as 4 cm behind the wrist. Each vertex's weights move onto WebXR's 25 joints (a bone under the hand that isn't one of them, such as a twist or a palm bone, gives its weight to the joint above it; the forearm's share goes to the wrist). A missing metacarpal goes a third of the way from the wrist to the knuckle, and a missing tip where the mesh that follows the last bone ends. A hand whose palm isn't a person's size (the wrist's distances to the four knuckles add up to 30 to 45 cm) is scaled to one, so a model in centimetres or a giant's hand comes out the right size; ft-handview then fits it to your own hand as it does the bundled ones. A hand with more than 6,000 triangles is reduced to that with meshoptimizer (the bundled hands have 1,800 to 3,000).

**Licences.** A VRM says whether it may be redistributed (0.x `licenseName`, 1.0 `allowRedistribution`), and the importer writes that into the file; `--may-share` can't override it. A glTF or FBX can't say, so it is marked as not to be sent unless `--may-share` is given. Nothing is sent to anyone yet. When shared rooms send avatars, a file marked that way stays on your Frame and the others see the stock avatar; Settings imports without the mark and will ask the first time a model would be sent.

**Untrusted files.** The file you import is a download. The readers cap the file's size, the nodes, vertices and triangles, check every index and number, and never open a file the model points at (textures, external buffers). `test_import.py` feeds it truncated and randomly damaged files and glTFs whose counts lie; the same was run under the sanitizers on 9,000 damaged files and 6,000 glTFs with changed structure (2026-11-10) with nothing found.

## Other people's code

Fetched at build time by `scripts/modeldeps.sh`, each file at a pinned commit and checked by its sha256, and built into `ft-model-import` only:

| Library | For | Version | Licence |
|---|---|---|---|
| [cgltf](https://github.com/jkuhlmann/cgltf) | glTF, GLB, VRM | 1.15 | MIT |
| [ufbx](https://github.com/ufbx/ufbx) | FBX | 0.23.1 | MIT or public domain |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | Reducing a mesh (its simplifier only) | 1.3 | MIT |

jsoncpp, from the container's packages, reads the VRM extension's JSON.
