# Release helpers

Three double-clickable helpers for the Muteki Aegisub release flow. Each `.cmd`
just runs its Python script, so they can also be started from a terminal with
extra arguments.

| Script | What it does |
| --- | --- |
| `release-local.cmd` | Copies the built `aegisub.exe` and the Hungarian catalogue into `C:\aegisub-portable`, for trying a build out immediately. |
| `release.cmd` | Translates whatever is new in `po/hu.po` and in `changelog/hu.txt` into the other languages, builds, and packs `aegisub-v<version>.zip` into `C:\aegisub-portable\_release`. |
| `upload.cmd` | Uploads the contents of `C:\aegisub-portable\_release` to the publishing location. |

## Settings

`release.config.json` next to these scripts holds the paths and the upload
destination. It is created with defaults on the first run; edit it and re-run.
Credentials are **not** stored in it - see below.

## Translation

`release.cmd` needs an OpenAI key. It is read from the `OPENAI_API_KEY`
environment variable; if that is unset it asks for one and keeps it for that run
only. Translation is incremental in both directions:

- `po/*.po` only receive msgids that are present in `po/hu.po` and missing from
  that language, written as `#, fuzzy` so they can be reviewed later.
- `changelog/<lang>.txt` only receive releases that the file does not have yet,
  so an already translated release is never paid for twice.

## Upload credentials

`upload.cmd` reads the password or key passphrase from the `MUTEKI_UPLOAD_SECRET`
environment variable, or asks for it. Nothing is written to disk. For an
`sftp://`, `ftp://` or `ftps://` destination curl does the transfer; for a plain
path or a UNC share the files are copied.
