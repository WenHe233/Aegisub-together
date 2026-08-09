# Release helpers

Double-clickable helpers for the Muteki Aegisub release flow. Each `.cmd`
just runs its Python script, so they can also be started from a terminal with
extra arguments.

| Script | What it does |
| --- | --- |
| `release-local.cmd` | Copies the built `aegisub.exe` and the Hungarian catalogue into `C:\aegisub-portable`, for trying a build out immediately. |
| `release.cmd` | Translates whatever is new in `po/hu.po` and in `changelog/hu.txt` into the other languages, builds, and packs `aegisub-v<version>.zip` into `C:\aegisub-portable\_release`. |
| `backfill-translations.cmd` | One-off catch-up: runs only the two translation passes, no build and no packing. Prints how much is missing and asks before spending anything. |

## Settings

`release.config.json` next to these scripts holds the paths. It is created with
defaults on the first run; edit it and re-run. It is committed, so **no keys
belong in it**.

`release.config.apikey.json` holds the keys and is kept out of git. It is created
empty on the first run:

```json
{
  "openai_api_key": "sk-..."
}
```

## Translation

`release.cmd` reads the OpenAI key from `release.config.apikey.json`, falling
back to `release.config.json` and then to the `OPENAI_API_KEY` environment
variable. Translation is incremental in both directions:

- `po/*.po` only receive msgids that are present in `po/hu.po` and missing from
  that language, written as `#, fuzzy` so they can be reviewed later.
- `changelog/<lang>.txt` only receive releases that the file does not have yet,
  so an already translated release is never paid for twice.
