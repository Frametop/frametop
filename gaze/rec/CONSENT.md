# Recording your eyes for the Frametop eye dataset

Version: 2026-10-16

Frametop's eye tracking needs a better model than it has: one that keeps working when you look far to the side, blink, squint, wear glasses or makeup, or when the headset moves on your face. To train it, we're collecting recordings of many people's eyes into a dataset that only the maintainers can read. This page explains what the eye recorder records, where it goes, who can see it, and what you agree to if you take part. Please read all of it.

## Who runs this

Frametop is an independent open-source project, maintained by DeeJanuz (<https://github.com/DeeJanuz>). It isn't affiliated with or endorsed by Valve or Hugging Face. Steam and Steam Frame are trademarks of Valve Corporation. This text was written without a lawyer, in good faith; if something in it seems wrong or unclear, please say so before you take part.

You can reach the maintainer through the Frametop repository's issues (<https://github.com/Frametop/frametop/issues>) or the dataset's discussion page on Hugging Face. Both are public.

## Who can take part

- You must be **18 or older**.
- For now, you can't take part if you live in **Illinois, Texas or Washington** (USA). Those states have laws about biometric data, which includes images of the iris, that need more than this project can provide yet.
- Take part only if you're comfortable with the maintainers seeing close-up images of your eyes.

## Your eyes are biometric data

The headset's eye cameras film each eye from a few centimetres away. The iris in those images can identify a person, the way a fingerprint can. That's why this dataset works differently from Frametop's hand dataset:

- **The eye images are never public.** Your headset encrypts them before they're uploaded, so that only the maintainers' keys can read them. Hugging Face, other contributors and people who download the dataset get only the encrypted files.
- **What's trained from them can be public.** Models trained on the dataset can be published and shared. They don't contain your images, though no one can rule out that a model remembers something about the eyes it was trained on.

## What is recorded

While a round runs, the eye recorder saves:

- **Eye images.** Infrared images from the headset's 2 eye cameras, about 90 a second each. They show your eyes, eyelids, eyelashes and the skin around them, and your glasses or contact lenses if you wear them. Encrypted in the export.
- **Where you looked.** Where each dot was when you looked at it, what Frametop's eye tracker and SteamVR's found in each image (pupil and glint positions, gaze directions), and how the fit check rated the headset's position. These stay readable in the export, so a pull request can be checked before anyone decrypts it.
- **Motion and settings.** The headset's position and rotation, many times a second, and its IPD setting (the distance between the lenses).
- **Prompts.** What you were asked to do and when.
- **Your answers.** Glasses, contacts or inserts, eye makeup, the face pad and strap, and your measured IPD if you gave it. Readable in the export.
- **Health answers, only if you tick their box.** Eye surgery and conditions such as a turned eye, a droopy eyelid or nystagmus. They're health data, so they're kept in a separate file and encrypted like the images. You can leave them out, and still take part.
- **Times.** When each round was recorded, with your time zone.
- **A contributor id.** A random number made on your headset the first time you agree to this page. It isn't made from your name, your Steam account or your headset. It keeps your rounds together and lets you withdraw them.

The recorder doesn't record sound, the headset's outward cameras, your name, your email address or your Steam account.

**Your Hugging Face account.** You upload with your own Hugging Face account, and your pull request shows its username next to your contributor id, publicly. The images in it are encrypted, but anyone can see that this account contributed eye recordings, and read the readable parts listed above. Use an account you're happy to have linked to them.

## What it's used for

- Training and testing eye-tracking models: finding the pupil, the glints and the eyelids, and working out where you're looking, including when the headset sits badly. Mainly for Frametop, and possibly elsewhere (below, "The license").
- Checking and improving the recorder's fit coach, which helps people put the headset on well.
- It isn't used to recognise or identify people, and the maintainers won't try.

## Nothing leaves your headset unless you send it

- Recordings stay on your headset, in `~/.local/share/frametop/eyes/contrib`. Nothing is uploaded automatically.
- Before you share anything, you can look through every recording in the Review page. You can delete any stretch of a take, a whole take or a whole round. Export leaves out what you deleted.
- You upload the export yourself: from the Upload page, or with Export and upload, which sends it once it's exported. That opens a pull request on the dataset. The maintainer checks it before it becomes part of the dataset, and may decline it. Until it's merged, you can close the pull request yourself.

## Where it goes, and who can read it

- The dataset is hosted on Hugging Face (<https://huggingface.co/datasets/DeeJanuz/frametop-eyes>), whose servers may be outside your country, for example in the USA. It's gated: people must accept its terms to download it, and what they get has the eye images and health answers encrypted.
- Only the maintainers hold the keys that decrypt them. Today that's DeeJanuz, with a key on each of two of their own computers (one of them trains the models) and a backup copy stored offline. The keys are never uploaded. If a key ever leaked, uploads made to it could be read: the maintainers would say so on the dataset's page and stop using it.
- Decrypted images are kept only on the maintainers' own computers, for training and checking models, and aren't shared with anyone else or uploaded to other services.
- Recordings stay in the dataset until they're withdrawn or the dataset is taken down.

## Safety

The rounds ask you to look at dots, including far to the sides, to blink and squint, and sometimes to turn your head or move the headset on your face. Sit where you normally use the headset and stop whenever anything is uncomfortable, or if your eyes get tired or sore. If you have a condition that bright or moving lights affect, such as photosensitive epilepsy, don't take part. You take part at your own risk.

## The license

- **You give the maintainer of Frametop a non-exclusive, worldwide, royalty-free license to use your contribution for any purpose, including commercially.** That includes copying it, changing it, and training, publishing and selling models made from it, in Frametop and elsewhere. It doesn't include publishing your eye images or health answers, or giving them to anyone outside the maintainers. The maintainer today is DeeJanuz. If someone else, or an organization, takes over maintaining Frametop, this license passes to them, with the same limits. It's non-exclusive: you keep any rights you have in your recordings and can do what you like with your own copies. It ends for a recording when you withdraw it, except for models already trained with it.
- You confirm that you have the right to give this license: the recordings are of your own eyes.
- There is no payment. The dataset and the eye recorder come with no warranty, and as far as the law allows, the maintainer isn't liable for any loss or damage from taking part.

## Withdrawing

You can withdraw a contribution at any time, without giving a reason:

- **Before it's merged:** close your pull request on Hugging Face. The maintainer deletes its files.
- **After it's merged:** post on the dataset's discussion page, or in the Frametop repository's issues, with your contributor id (shown in the eye recorder) and which rounds, or "all". Post from the Hugging Face account that uploaded them if you can, so the maintainer can tell it's you; otherwise, say how to check. These requests are public, so don't add anything else that identifies you.

Then, usually within 30 days:

- your recordings are deleted from the dataset and purged from its repository's history, and the maintainers delete their decrypted copies;
- they're left out of anything trained after that.

What can't be undone: models already trained with them.

## Your rights

Depending on where you live (for example in the EU or the UK, under the GDPR), the law may give you more rights over this data: to get a copy of it, to have it corrected or deleted, to object to its use, and to complain to your data protection authority. Eye images that can identify you and health answers are special kinds of data there, and they're used only because you explicitly agreed to it, here and with the health answers' own tick. You can withdraw that agreement at any time, as above. Withdrawing doesn't make earlier use unlawful. To use any of these rights, contact the maintainer as above.

## Changes to this text

If this text changes, the eye recorder shows the new version and asks you again before your next round. Each upload records the version you agreed to, and recordings you already uploaded stay under that version, unless you agree to a newer one or withdraw them.

## Agreeing

By ticking the three boxes and "Agree and continue", you confirm that you're 18 or older, that you don't live in Illinois, Texas or Washington, and that you agree to the above, including the use of images of your eyes as described. You can still decide not to upload anything.
