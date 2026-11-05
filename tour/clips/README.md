# Tour videos

The looping videos on the tour's pages (`ft_tour.py` names them with `clip()`). They're animated WebP: Qt's
AnimatedImage plays them like GIFs (the image has `qt6-qtimageformats`), at about a fifth of a GIF's size. All
twelve are 5.8 MB; as GIFs they were 37 MB.

They're cut from a narrated demo recorded in the headset on 2026-10-20 (VR Recorder, 1920x1080, 30 fps, 8.5
minutes). The recording itself isn't in the repository: it shows the room through passthrough, and the
narration includes retakes.

## Making them again

`make/` has the scripts. They ran on a Mac (Homebrew `ffmpeg`, `img2webp` from `webp`, and a venv with
`pillow`, `numpy` and `opencv-python-headless`), not on the headset. In a work folder:

```
src/VR 2026-10-20 21-31-57.mp4      the recording
fonts/NotoSans-Bold.ttf              the captions' font
scripts/                             make/'s files
```

1. `python scripts/detect.py "src/VR 2026-10-20 21-31-57.mp4" dots.csv` finds Frametop's pointer dot in every
   frame (pure white, about 4 px, on a darker patch). The clips with `"ring": true` draw a magenta ring round it
   so it can be seen at this size, and `"follow": true` zooms in and follows it. Detections whose surroundings
   are bright (`ring` over 50: window icons, a terminal's cursor) are left out.
2. `GNOSTEPS=1 GFMT=webp GQ=50 GW=576 python scripts/render.py NAME...` (or `all`) renders the clips in
   `clips.json`: each is scenes (`a` to `b` seconds of the recording, a `speed`, an optional `zoom` round `cx`,
   `cy`) with captions (`caps`: from a time in the recording, until the next). The frames go to `out/NAME/`, the
   video to `gifs/NAME.webp`. 576 px wide at 10 fps; without `GFMT=webp` it makes a GIF.
3. Copy `gifs/*.webp` here. `tour/test/tour-test.py` checks every page's videos are here and every video here is
   on a page.
