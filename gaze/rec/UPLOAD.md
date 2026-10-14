DRAFT: this text waits for its review, with CONSENT.md.
### About uploading

- **What's sent:** the export in `@EXPORT_PATH@` (@EXPORT_SIZE@), into `contributions/@CONTRIBUTOR@/@ROUND@` in [@DATASET@](https://huggingface.co/datasets/@DATASET@). Upload first checks that every file is complete and matches its checksum, and that the eye images and health answers are encrypted.
- **Who can read it:** the eye images and health answers are encrypted on this headset to the maintainers' keys. Hugging Face and anyone who downloads the dataset get only the encrypted files. The rest (where the dots were, what the trackers found, the fit readings and your checklist answers) stays readable.
- **Your account:** the pull request comes from your Hugging Face account, and anyone can see its username next to your contributor id, `@CONTRIBUTOR@`. The login stays saved on this headset, so you only log in once.
- **The dataset's terms:** the first time, open the dataset's page and accept its terms. Until you have, Upload stops with "accept the dataset's terms first".
- **Once your pull request's link shows, plug in the headset and leave it plugged in until this page says Uploaded.** A round is several gigabytes, so this can take a while. You can take the headset off: the Eye Recorder keeps it awake until the upload is done. Keep the Eye Recorder open, since closing it stops the upload.
- **If it stops partway** (Cancel, or the network drops), press Upload again. It carries on in the same pull request, and files already sent aren't sent twice.
- **The maintainer's review:** they check that the files are complete and decrypt correctly before merging. You can follow it and answer questions on the pull request's page. Once it's merged, you can delete the round and its export here to free the space.
- **Withdrawing:** see "Withdrawing" in the consent text. You'll need your contributor id, `@CONTRIBUTOR@`.
