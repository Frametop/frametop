# Frametop verification map

This folder is the maintained source for verifying what a Frametop user sees and does in the
headset. Read this index, then use the feature's file as the recipe. `V` and `T` are as in
`../SKILL.md`.

## Baseline preconditions

- The Frame runs SteamVR and the Frametop desktop, and `V doctor` exits 0.
- Nobody wears the headset (`V doctor` refuses otherwise).
- The change under test is deployed into the live release (`../SKILL.md`, Launch).
- `V launch NAME` has made the run's evidence folder.
- For anything the user sees: `T worn on`, `T compositor awake`, and a head pose facing the
  panels (`T pose set --position 0 1.1 0 --rotation 0 0 0`, then `T pointer measure KEY` to aim).

## Driving conventions

- Drive through the user's inputs only (controllers, headset buttons, eyes, hands, keyboard,
  mouse, the headset on or off). Never write Frametop's state files or settings to fake a result.
- Press and release as two commands with a short pause (0.2 s): a held button is a long press.
- Clicks land on the user's real desktop. Aim at something harmless (empty wallpaper, a panel's
  move bar) and never click inside remote displays, close buttons or dialogs you didn't open.
- Leave what you changed as you found it: close what you opened, end held gestures, and let
  `V cleanup` restore the rest.

## Proof and skip reporting

- `V snapshot N-LABEL` before and after each action; add `--capture` when the result is visual.
- Name the feature ID and the entry point you used in the report, with the snapshot folders.
- A visual proof is a capture you have looked at, plus the matching readback (an overlay's
  visibility, a log line, a service's status).
- If a precondition can't be met (no scene app, the headset was put on), report the step as not
  verified with the reason. Don't swap in a different entry point and call it the same proof.
- What only a person can judge (comfort, tracking quality, how a look or pinch feels) goes in
  `~/Desktop/Projects/HEADSET-TESTS.md`.

## Feature entry contract

Each feature file starts with an H1 title and a paragraph on the user-visible behaviour, then
exactly these H2s in order: `Sub-features`, `How to get to it (user POV)`,
`Driving it with the testbench`, `Gotchas`.

## Features

- [SteamVR dashboard](./dashboard.md): open and close it with a controller's system button.
- [3D mouse](./mouse-pointer.md): a mouse or keyboard plugged into the relay moves the cursor
  over the panels, clicks, scrolls and types.
- [Gaze pointer](./gaze-pointer.md): the eyes aim the cursor in gaze mode.
- [Hand tracking](./hands.md): posed hands, pinch clicks and grips.
- [Display power](./display-power.md): the headset displays off and back on.
