# Recording your hands for the Frametop hand dataset

Version: 2026-10-14

Frametop's hand tracking needs a small model that finds hands in the headset's camera images. To train it, we're collecting recordings of many people's hands into a public dataset. This page explains what the hand recorder records, where it goes, and what you agree to if you take part. Please read all of it.

It covers the **bounty rounds** (short rounds of what the hand tracker needs most, such as taps, pinches and grabs on windows that appear in front of you) and Frametop's **full sessions** and **quick rounds**. **Testing sessions**, where you use Frametop and mark what went wrong, have their own, separate text: they're never used for training and aren't part of this dataset.

## Who runs this

Frametop is an independent open-source project, maintained by DeeJanuz (<https://github.com/DeeJanuz>). It isn't affiliated with or endorsed by Valve or Hugging Face. Steam and Steam Frame are trademarks of Valve Corporation. This text was written without a lawyer, in good faith; if something in it seems wrong or unclear, please say so before you take part.

You can reach the maintainer through the Frametop repository's issues (<https://github.com/Frametop/frametop/issues>) or the dataset's discussion page on Hugging Face. Both are public.

## Who can take part

- You must be **18 or older**.
- For now, you can't take part if you live in **Illinois, Texas or Washington** (USA). Those states have laws about biometric data (such as hand geometry) that need more than this project can provide yet.
- Take part only if you're comfortable with images of your hands and the room in front of you being public.

## What is recorded

While a session runs, the hand recorder saves:

- **Camera images.** Infrared images from the headset's 4 tracking cameras: about 10 sets a second in a full session, 30 in a bounty round. They show your hands, your arms, your clothing and whatever is in front of you: your room, your desk and the things on it. They're grey, low-detail images, but people and things can be recognised in them. The size and shape of your hands can be measured from them: that's part of what they're for.
- **Motion.** The position and rotation of the headset, many times a second, and of the controllers when you use them in the recording.
- **Prompts.** What you were asked to do and when (in a bounty round, which window and which part of it), and what the live hand tracker saw at the time.
- **Calibration.** Where the cameras sit on the headset and how their lenses bend the image. Serial numbers and other fields that identify your headset are removed first, and the export lists what was removed.
- **Your answers.** Which objects you had, the lighting, whether you wore sleeves, rings or a watch, your handedness if you gave it, and any notes you typed in the checklist or in Review. Notes are uploaded with the recordings and read by the maintainer: don't put anything in them that identifies you.
- **Times.** When each session was recorded, with your time zone.
- **A contributor id.** A random number made on your headset the first time you agree to this page. It isn't made from your name, your Steam account or your headset. It keeps your sessions together and lets you withdraw them.

The recorder doesn't record sound, your name, your email address or your Steam account. The tracking cameras look outward, so your eyes and face aren't recorded, unless a mirror or something shiny shows them: avoid those.

**Your Hugging Face account.** You upload with your own Hugging Face account, and your pull request shows its username next to your contributor id, publicly. So anyone can see which Hugging Face account contributed which recordings. Use an account you're happy to have linked to them.

## What it's used for

- Training and testing hand-tracking models: finding hands, their keypoints, their shape and how far away they are. Mainly for Frametop's hand cutouts, and by anyone else for non-commercial work under the dataset's license.
- It isn't used to recognise or identify people, and the dataset's terms forbid anyone from trying.

## Nothing leaves your headset unless you send it

- Recordings stay on your headset, in `~/.local/share/frametop/hands/contrib`. Nothing is uploaded automatically.
- Before you share anything, you can watch every recording in the Review page. You can delete any stretch of a recording, a whole take or a whole session. Export leaves out what you deleted.
- You upload the export yourself: from the Upload page, or with Export and upload, which sends it once it's exported. That opens a pull request on the dataset. The maintainer checks it before it becomes part of the dataset, and may decline it. Until it's merged, you can close the pull request yourself.

## Where it goes

- **The dataset is public.** It's hosted on Hugging Face (<https://huggingface.co/datasets/DeeJanuz/frametop-hands>), whose servers may be outside your country, for example in the USA. Anyone who accepts the dataset's terms can download it.
- People who download it agree not to try to identify anyone or anything in it, and to delete recordings that are later withdrawn. We can't enforce that against everyone: assume copies may exist.
- Recordings stay in the dataset until they're withdrawn or the dataset is taken down.

## Keep other people and private things out of view

While recording, please:

- face away from other people, mirrors, screens showing private things, papers, letters, photos and anything else you wouldn't want public;
- make sure nobody else's face or hands are in view, and record only where the people you share the space with are fine with it;
- don't record children.

If something private got into a recording, delete that stretch in Review before you export. If you notice it after uploading, withdraw the session (below).

## Safety

The sessions ask you to move your hands and arms around you, out to full reach. Sit where you normally use the headset, clear an arm's reach around you, and stop whenever anything is uncomfortable. You take part at your own risk.

## The license

- **The dataset is published under Creative Commons Attribution-NonCommercial 4.0 (CC BY-NC 4.0).** Anyone may use it for non-commercial purposes, with attribution. Your contribution is credited by its contributor id, not your name.
- **You also give the maintainer of Frametop a non-exclusive, worldwide, royalty-free license to use your contribution for any purpose, including commercially.** That includes copying it, changing it, and training, publishing and selling models made from it, in Frametop and elsewhere. The maintainer today is DeeJanuz. If someone else, or an organization, takes over maintaining Frametop, this license passes to them. It's non-exclusive: you keep any rights you have in your recordings and can do what you like with your own copies. It ends for a recording when you withdraw it, except for models already trained with it.
- You confirm that you have the right to give these licenses: the recordings are yours, and nothing in them belongs to someone who hasn't agreed.
- There is no payment. The dataset and the hand recorder come with no warranty, and as far as the law allows, the maintainer isn't liable for any loss or damage from taking part.

## Withdrawing

You can withdraw a contribution at any time, without giving a reason:

- **Before it's merged:** close your pull request on Hugging Face. The maintainer deletes its files.
- **After it's merged:** post on the dataset's discussion page, or in the Frametop repository's issues, with your contributor id (shown in the hand recorder) and which sessions, or "all". Post from the Hugging Face account that uploaded them if you can, so the maintainer can tell it's you; otherwise, say how to check. These requests are public, so don't add anything else that identifies you.

Then, usually within 30 days:

- your recordings are deleted from the dataset and purged from its repository's history, so they can't be downloaded from there again;
- they're left out of anything trained after that.

What can't be undone: copies others downloaded before the withdrawal, and models already trained with them.

## Your rights

Depending on where you live (for example in the EU or the UK, under the GDPR), the law may give you more rights over this data: to get a copy of it, to have it corrected or deleted, to object to its use, and to complain to your data protection authority. The data is used because you agreed to it, and you can withdraw that agreement at any time, as above. Withdrawing doesn't make earlier use unlawful. To use any of these rights, contact the maintainer as above.

## Changes to this text

If this text changes, the hand recorder shows the new version and asks you again before your next session. Each upload records the version you agreed to, and recordings you already uploaded stay under that version, unless you agree to a newer one or withdraw them.

## Agreeing

By ticking the three boxes and "Agree and continue", you confirm that you're 18 or older, that you don't live in Illinois, Texas or Washington, and that you agree to the above. You can still decide not to upload anything.
