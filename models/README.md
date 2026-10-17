# Model versions

Users pick which hand and eye models Frametop runs on Frametop Input Settings' Hands and Gaze pages.
Each page lists the versions in a catalog, with notes on what to expect and benchmark numbers set
against the one in use. `catalog.py` reads the catalogs, downloads versions and says what a setting
runs; `ft-models` is the same from a terminal.

| Kind | Catalog | Setting (frametop.conf) | Downloads go to | Files from |
|---|---|---|---|---|
| hands | `hands/models.json` | `HANDS_MODEL`: `latest`, `stock`, a version | `hands/models/versions/NAME` | `DeeJanuz/frametop-hands-models` |
| eyes | `gaze/tracker/models.json` | `EYES_MODEL`: `classic`, `latest`, a version; `GAZE_TRACKER=steam` for SteamVR's | `gaze/tracker/models/NAME` | `DeeJanuz/frametop-eyes-models` |

- **Latest** follows the catalog's `latest`, so it moves when a new version ships. A version picked
  by name stays put. For hands, latest is `hands/models/student`, which `hands/models.sh` keeps on
  `hands/models.pin` (older installs only read that pin). The catalog's latest must be the same
  files: `models/test/test_catalog.py` checks it.
- **Where the list comes from.** Installs read their branch's catalog from GitHub (`main` for a
  stable release, `experimental` for an experimental one or a clone on that branch), so a push
  reaches them without a release: Input Settings' "Check for new versions", and the services when
  they start (`ft-models auto`, at most every 15 minutes; `HANDS_MODELS_UPDATE=0` /
  `EYES_MODELS_UPDATE=0` turn that off). Offline, or when GitHub's copy isn't valid, the copy in
  the install is used.
- **Trust.** A catalog only names files. They come from the kind's Hugging Face repo, which is
  fixed in `catalog.py` and never taken from a catalog. Each download is pinned to a commit and must
  match its sha256, or it isn't kept. Catalog checks are strict: one bad entry and the whole
  catalog is ignored. The texts are shown as plain text.
- **Who applies it.** ft-hands reads `HANDS_MODEL` at start, and Input Settings restarts hand
  tracking after a change. ft-eyes reads `EYES_MODEL` at start, and the gaze service restarts it
  when the setting changes. Each eye net keeps its own calibration (`net-NAME/`), so a switch asks
  for one calibration.

## Shipping a version

1. Upload the version's folder to the kind's Hugging Face repo, next to the earlier ones (hands:
   frame-hands' `model-repo/`, eyes: frame-eyes' `model-repo/`), with a NOTICE: how it was trained,
   what it was tested on, the numbers.
2. Print its files' hashes from that staging folder:
   `models/ft-models hashes eyes ~/Desktop/Projects/frame-eyes/model-repo eyes-v1`
3. Add an entry to the catalog: name, title, kind (`custom`), status (`testing` until it's the one
   to recommend), date, summary, notes (what to expect against the current latest and the
   baseline), cost, `bench` (the catalog's metrics, measured the same way as the other entries),
   `format`, `rev` (the Hugging Face commit) and `files`. Set the old latest's status to `older`
   when a new one is recommended.
4. To make it latest, set `latest`. For hands, also re-pin `hands/models.pin` with frame-hands'
   `model-repo/pin.py VERSION REV CHECKOUT`.
5. Run `python3 models/test/test_catalog.py` and `models/ft-models fetch KIND NAME` (a real
   download, checked), then push to experimental. Stable installs see it once it's on main.

Raise `format` (and `SPECS[kind]["format"]` in `catalog.py`) when a version needs a newer runtime.
Older installs then list the version as needing a newer Frametop instead of running it.
