# Avatars and hand models

How you look in Frametop: the hands ft-handview draws for you today, and the avatar other people will see in a shared room ([docs/together.md](../docs/together.md), "Avatars and hands"). This folder has the model file, the importer that writes it, the solver that poses a body from a headset's wearer, the program that draws bodies, and Frametop Settings' Appearance page.

| File | What |
|---|---|
| `modelfile.h`, `modelfile.cpp` | Frametop's own model file (`.ftmodel`): the format, a strict reader and a writer for its hands. No GPU, SteamVR or network code |
| `bodyfile.h`, `bodyfile.cpp` | The same for an avatar's body in that file: its skeleton, mesh, materials, pictures and face shapes |
| `modelbytes.h` | Reading and writing the file's numbers, for both |
| `vec.h` | A point in three dimensions, for the importer and the solver |
| `pose/` | The pose solver (`bodypose.h`): a body's 55 bones from the wearer's head and hands. Sums only, no tracking, GPU or SteamVR code |
| `view/` | `ft-avatars`: bodies drawn with GLES 2. It is built by `screens/build.sh` into `screens/build/`, because it wears the hands ft-handview draws |
| `import/` | `ft-model-import`: a VRM, glTF, GLB or FBX model into a `.ftmodel` (`build.sh` builds it into `avatar/build/`) |
| `ftmodel.py` | The same file read in Python, for the tests and the Settings page |
| `hand_models.py`, `ft_avatar_settings.py`, `AvatarPages.qml` | Frametop Settings' Appearance page: the hand models with their pictures, the look, import and remove |
| `tests/` | `modelfile-test.cpp` and `bodyfile-test.cpp` (the readers against damaged files, in `just test-c`), `test_import.py` (the importer on made-up hands and the bundled ones), `test_import_body.py` (on a made-up whole person), `bodypose-test.cpp` (the solver on a made-up skeleton, `person.h`) and `bodyview-test.cpp` (the drawing's sums: the mesh, the hands fitted to a body's, the demo wearer, the views), both in `just test-c`, `test_appearance.py` (the page's backend) |

## The model file

One format on disk, for three reasons: the programs that draw (ft-handview now, the avatar renderer later) carry no glTF or FBX code; reducing and bone matching happen once, at import; and when avatars are sent between people, the file a stranger sends is read by one small reader that trusts nothing, never by the importers.

The layout is in `modelfile.h`. In short: `FTMD`, a version, then chunks, each a four-letter tag, a size and a payload. `INFO` holds the name, licence, author and source format as text, and whether the model may be sent to other people. `HAND` holds one hand: a skinned mesh on WebXR's 25 hand joints with their bind positions, positions and normals quantized, four joints and weights a vertex. A reader skips chunks it doesn't know: ft-handview reads the hands of an avatar's file and never looks at its body.

An avatar's file has three more kinds of chunk, laid out in `bodyfile.h`. `BODY` holds the skeleton and the mesh: where each of VRM's 55 humanoid bones is in the rest pose (a body may lack the ones a person can do without, such as the toes, the jaw or a second chest bone), then the vertices with a place in a picture and four bones and weights each, the triangles, the parts they are drawn in, and each part's material (a colour, a picture, how it is see-through, whether it has two sides, and whether it is shaded as a cartoon, lit, or not at all). `TEXR` holds one picture as plain RGBA. `SHAP` holds one face shape, as how far it moves each vertex it moves: five mouth shapes (aa, ih, ou, ee, oh) and three blinks. A body stands on y = 0 with its hips over the origin, faces -z, and has its right hand toward +x, in metres. The file also says where the wearer's eyes are, which is what the avatar is scaled by to fit you.

The readers check every count, size, index and number before using it, and refuse the whole file on the first one that is wrong. Limits: 20,000 vertices and 40,000 triangles a hand; 65,535 vertices, 60,000 triangles, 32 parts and materials, 24 pictures of at most 2048 pixels a side and 32 MB in all, and 16 shapes a body; 64 MB a file, 64 chunks, 96 bytes of text a field with no control characters. `modelfile-test.cpp` and `bodyfile-test.cpp` break each rule on its own, cut the file at every byte and change every byte, under the address and undefined-behaviour sanitizers.

A model's name is its file's name: `NAME.ftmodel`, with NAME lower-case letters, digits, `-` and `_`, at most 31. Your models are in `~/.local/share/frametop/avatars` (`$XDG_DATA_HOME`; `FT_AVATARS_DIR` names another folder).

## The importer

```
avatar/build/ft-model-import FILE [--name NAME] [--to DIR] [--side left|right] [--may-share] [--check]
```

It reads a VRM (0.x or 1.0), glTF or GLB with cgltf, or an FBX with ufbx, in the rest pose. It takes the model's hands, which become the hands ft-handview draws for you, and, when the model is a whole person, its body. A model whose hands can be told but whose body can't is a hand model, and the import says why it has no body (`nobody`). An avatar whose hands have no finger bones keeps the hands Frametop comes with. `import/main.cpp` has the options and what it prints: a fact a line, which the Settings page reads, or `error` and the reason.

**Finding the hand bones** (`import/handbones.cpp`). A VRM names its humanoid bones, so those are used. Otherwise the bones' names are split into words and matched: a hand is a bone the mesh moves with whose name says hand or wrist and a side, and each finger a chain under it named for that finger. That reads rigs named as Mixamo, Unity, Blender and Unreal name them, and the flat skeletons of the WebXR and Babylon hands. A thumb has three or four bones; a finger three, four or five (with or without a metacarpal and a tip). Nothing is guessed: a skeleton whose bones can't be told is refused, with the bone that is missing or the count that doesn't fit, and nothing is written. A file with one hand gets the other mirrored.

**Cutting the hand out** (`import/handpart.cpp`). The mesh the hand's bones move is kept, and the forearm's as far as 4 cm behind the wrist. Each vertex's weights move onto WebXR's 25 joints (a bone under the hand that isn't one of them, such as a twist or a palm bone, gives its weight to the joint above it; the forearm's share goes to the wrist). A missing metacarpal goes a third of the way from the wrist to the knuckle, and a missing tip where the mesh that follows the last bone ends. A hand whose palm isn't a person's size (the wrist's distances to the four knuckles add up to 30 to 45 cm) is scaled to one, so a model in centimetres or a giant's hand comes out the right size; ft-handview then fits it to your own hand as it does the bundled ones. A hand with more than 6,000 triangles is reduced to that with meshoptimizer (the bundled hands have 1,800 to 3,000).

**Finding the body's bones** (`import/bodybones.cpp`). A VRM's humanoid names them. Otherwise they are found by name and by where they hang: the hips, the head under them, the spine as the bones between the two, an arm as the bones above each hand, a leg as the bones above each foot. The fingers are the ones the hand was found with. A body needs hips, a spine, a head, and both arms and legs down to the hands and feet. One that lacks any of them is not imported as a body, and the bones that are missing are named.

**Cutting the body** (`import/bodypart.cpp`). Every mesh the skeleton moves is kept. A bone that isn't a humanoid's (hair, a skirt, a twist bone) gives its weight to the humanoid bone it hangs from, so hair and cloth move with the head and the hips and stay rigid. VRM 1.0's constraints are read for the one case that matters to how a body looks: a helper bone that hangs beside a limb and aims along it (VRoid's sleeves and shorts hang from these) belongs to the limb's bone, when it sits where that bone does. A bone that copies a turn from somewhere else (a robot arm on a backpack that mirrors the wearer's) can't be a bone of the body, whose bones each turn about their own place, so it stays rigid on what it hangs from. The body is turned so that its right is +x, moved so that its hips are over the origin, and stood on the floor: the lowest point of what the feet move, so a stand or a long dress doesn't lift it. A body with more than 20,000 triangles is reduced to that with meshoptimizer. The reduction keeps the seams between materials and between places in a picture, and leaves alone every vertex a face shape moves.

**Its look** (`import/bodylook.cpp`, `gltflook.cpp`, `fbxlook.cpp`). Of a material, the importer takes the base colour, its picture, how it is see-through, whether it has two sides and whether it is unlit; a VRM's MToon materials are marked as cartoon shaded. Materials that come out the same become one, and the parts are ordered so that what is see-through is drawn last. PNG and JPEG pictures are decoded with stb_image, made no larger than 2048 pixels a side, and then the largest are halved until all of them fit in 16 MB. A picture that can't be read leaves its material its colour, and the import says how many couldn't. An FBX says less: its colour or its picture, and a picture with see-through pixels makes its material cut out along them. A body with more than 32 materials or 24 pictures is not imported as a body.

**Face shapes** (`import/shapenames.cpp`, `gltfvrm.cpp`). A VRM's expressions name the mouth shapes and the blinks (0.x `blendShapeGroups`, 1.0 `expressions`). In other files the shapes' own names are matched: VRChat's (`vrc.v_aa`), VRoid's (`Fcl_MTH_A`, `Fcl_EYE_Close_L`), plain ones (`aa`, `blink_left`) and MMD's Japanese ones. An avatar without them still imports, and its face stays still.

**Licences.** A VRM says whether it may be redistributed (0.x `licenseName`, 1.0 `allowRedistribution`), and the importer writes that into the file; `--may-share` can't override it. A glTF or FBX can't say, so it is marked as not to be sent unless `--may-share` is given. Nothing is sent to anyone yet. When shared rooms send avatars, a file marked that way stays on your Frame and the others see the stock avatar; Settings imports without the mark and will ask the first time a model would be sent.

**Untrusted files.** The file you import is a download. The readers cap the file's size, the nodes, vertices, triangles, materials and pictures, and check every index and number. The only other files they open are ones in the model's own folder: a glTF's buffer, and a picture the model names. A name with a folder in it has the folder dropped (an FBX holds the path on the machine that wrote it), and a glTF's picture named with a folder, `..`, a scheme or an escape is not opened. The pictures read are capped too: 64 of them, each halved to 2048 pixels a side as it is read, and to 512 once the ones read take 96 MB. `test_import.py` and `test_import_body.py` feed it truncated and randomly damaged files and glTFs whose numbers lie. Before bodies were read, the same ran under the sanitizers on 9,000 damaged files and 6,000 glTFs with changed structure; with bodies, on 2,400 damaged files and 2,400 glTFs with changed numbers (2026-11-10). Nothing was found.

## Posing a body

A headset knows where its wearer's head is and, while they are tracked, the hands. `pose/bodypose.cpp` makes up the rest (the design decision is upper-body IK, [docs/together.md](../docs/together.md)):

- **The head** is where the wearer's eyes put it. The body is scaled so that its eyes stand as high as the wearer's do (`Stature`: the highest the head has stayed, and never less than a short adult's, so someone who has only sat gets a body that sits).
- **The trunk** hangs from the head. It turns after the head once the head has turned 15 degrees from it, never lags more than 75, and takes about a third of a second to follow. When the head comes down while looking down, the spine leans; when it comes down level, the knees bend.
- **An arm** reaches a tracked wrist with two bones, the elbow down and out (or where the tracker says it is). The forearm takes half of the hand's twist. An arm whose hand isn't tracked hangs, and it moves between the two over about a tenth of a second.
- **The legs** stand under the hips. The lower the hips, the further forward the feet, so a seated wearer's body sits.
- **The fingers** take the tracked hand's turns (`view/handfit.cpp` fits the tracker's hand to the body's own finger bones).

The same solver poses your own body and the people you share a room with. It refuses input that isn't numbers and never produces any: `bodypose-test.cpp` runs it on thousands of random inputs under the sanitizers.

## Drawing a body

```
screens/build/ft-avatars --snapshot OUT.png MODEL [--pose NAME] [--at SECONDS] [--size PX] [--shape NAME=WEIGHT]...
screens/build/ft-avatars --thumb OUT.png MODEL [--size PX]
screens/build/ft-avatars --poses
```

`ft-avatars` draws a model file's body on the GPU (`view/bodygpu.cpp`): the mesh is skinned in the vertex shader by the pose's 55 matrices, the face shapes move the vertices before that (`view/bodymesh.cpp`), and each part is drawn with its material's look: lit, in two tones for a cartoon material, or in its own colours; solid, cut out or see-through.

For now it makes pictures with no headset on (`view/picture.cpp`). `--snapshot` poses the body as a made-up wearer (`view/demo.cpp`: stand, wave, reach, type, sit, lean, look; `rest` is the file's own pose) and writes a sheet of four views, which is how an import is checked by eye. `--thumb` writes the one picture Settings shows. The mirror panel and the people in a shared room come next.

## Other people's code

Fetched at build time by `scripts/modeldeps.sh` and `scripts/stb.sh`, each file at a pinned commit and checked by its sha256, and built into `ft-model-import` only:

| Library | For | Version | Licence |
|---|---|---|---|
| [cgltf](https://github.com/jkuhlmann/cgltf) | glTF, GLB, VRM | 1.15 | MIT |
| [ufbx](https://github.com/ufbx/ufbx) | FBX | 0.23.1 | MIT or public domain |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | Reducing a mesh (its simplifier only) | 1.3 | MIT |
| [stb_image](https://github.com/nothings/stb) | Decoding a model's PNG and JPEG pictures | 2.30 | MIT or public domain |

jsoncpp, from the container's packages, reads the VRM extension's JSON.
