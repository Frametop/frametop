# Testing the hand recorder's bounties

Version: 2026-10-14c

The bounties and testing sessions are new, and we're trying them out with a few people before they open to everyone. This page says what you agree to by taking part in that test. It's shorter than the hand dataset's consent because **nothing you send now is published**.

## Who runs this

Frametop is an independent open-source project, maintained by DeeJanuz (<https://github.com/DeeJanuz>). It isn't affiliated with or endorsed by Valve or Hugging Face. This text was written without a lawyer, in good faith. You can reach the maintainer through the Frametop repository's issues (<https://github.com/Frametop/frametop/issues>), which are public.

## Who can take part

- You must be **18 or older**.
- For now, you can't take part if you live in **Illinois, Texas or Washington** (USA): their laws about biometric data (such as hand geometry) need more than this project can provide yet.

## What is recorded

- **Camera images** from the headset's 4 tracking cameras, 30 sets a second: your hands and arms, your clothing and whatever is in front of you. The size and shape of your hands can be measured from them.
- **Motion** of the headset and, if you use them, the controllers; **what you were asked to do** and when.
- **Calibration** of the cameras, with serial numbers and other fields that identify your headset removed.
- In a **testing session**, also what Frametop's hand tracking made of your hands, moment by moment, what Frametop did with them (each poke, pinch, grab or stroke it acted on or held back, and which of its windows, by number, not by what was in them), the moments you marked, and your hand tracking settings (file paths left out).
- **Your voice**, only in a testing session where you tick "Record my voice": the headset's microphone while you record, so you can say what went wrong. Anyone else who can be heard is recorded too. The maintainer turns it into text on their own computers (not an online service) and uses it to find those moments.
- **Your answers** in the checklist, the **times** of the recordings, and a **contributor id**: a random number made on your headset, not from your name, Steam account or headset.

No other sound, no name, no email address, no Steam account. The cameras look outward, so your face isn't recorded unless a mirror shows it.

## Where it goes and who sees it

- Nothing leaves your headset until you upload it. Before that you can watch every recording in Review and delete any part of it.
- Uploads go to a gated test dataset on Hugging Face (<https://huggingface.co/datasets/DeeJanuz/frametop-hands-test>). Its servers may be outside your country, for example in the USA.
- **The recordings are encrypted on your headset before they're sent.** Only the maintainer holds the keys, so other people who can see the test dataset can't open them. What stays readable is the description of the upload: its length, lighting, your contributor id and checklist answers.
- Your pull request shows your Hugging Face username next to your contributor id, to anyone who can see the test dataset.

## What it's used for

- Checking that the bounties and the recorder work, and **testing** Frametop's hand tracking: new versions are run on your recordings to see whether they do better, especially at the moments you marked.
- **Not for training** models, and **not published**. If we'd like to move your test recordings into the public hand dataset later, we'll ask you first, and they move only if you agree to that dataset's own terms.

## Withdrawing

- Your uploads are deleted when the test ends, or earlier if you ask: close your pull request, or open an issue with your contributor id (shown in the Hand Recorder) and "delete my test uploads".
- You can delete recordings on your headset at any time in Review.

## Keep other people and private things out of view

Face away from other people, mirrors and screens or papers you wouldn't want seen; don't record anyone else, and don't record children. Clear an arm's reach around you, and stop whenever anything is uncomfortable. You take part at your own risk.

## Agreeing

By ticking the three boxes and "Agree and continue", you confirm that you're 18 or older, that you don't live in Illinois, Texas or Washington, and that you agree to the above. You can still decide not to upload anything.
