# Release helpers

Double-clickable helpers for the Muteki Aegisub release flow. Each `.cmd`
just runs its Python script, so they can also be started from a terminal with
extra arguments.

| Script | What it does |
| --- | --- |
| `release-local.cmd` | Copies the built `aegisub.exe` and the Hungarian catalogue into `C:\aegisub-portable`, for trying a build out immediately. |
| `release.cmd` | Translates whatever is new in `po/hu.po` and in `changelog/hu.txt` into the other languages, builds, packs `aegisub-v<version>.zip` into `_release/`, and offers to publish it as a GitHub release. |
| `upload-releases.cmd` | One-off catch-up: publishes every package in `_release/` that is not on GitHub yet. |
| `backfill-translations.cmd` | One-off catch-up: runs only the two translation passes, no build and no packing. Prints how much is missing and asks before spending anything. |

## Settings

`release.config.json` next to these scripts holds the paths. It is created with
defaults on the first run; edit it and re-run. It is committed, so **no keys
belong in it**.

`release.config.apikey.json` holds the keys and is kept out of git. It is created
empty on the first run:

```json
{
  "openai_api_key": "sk-...",
  "github_token": "github_pat_..."
}
```

## Translation

`release.cmd` reads the OpenAI key from `release.config.apikey.json`, falling
back to `release.config.json` and then to the `OPENAI_API_KEY` environment
variable. Translation is incremental in both directions:

- `po/*.po` only receive msgids that are present in `po/hu.po` and missing from
  that language. They go in as live entries carrying the comment
  `# machine translation from Hungarian, not reviewed yet`, which is what to
  grep for when reviewing. They must not be marked `#, fuzzy`: msgfmt leaves
  fuzzy entries out of the compiled catalogue, so the program would never show
  them.
- `changelog/<lang>.txt` only receive releases that the file does not have yet,
  so an already translated release is never paid for twice.

## Building

`release.cmd` builds only when it would change what gets packed, because every
build relinks an 85 MB executable — the version header is regenerated on every
run. It builds when a catalogue was translated, when there is no executable yet,
or when something under `src/`, `libaegisub/` or `po/` is newer than the one
that is there; otherwise it says so and goes straight to packing. Translating
the changelog never triggers a build: the changelog is neither compiled nor
packed.

## Where the files live

The program reads its changelogs straight out of this repository, from
`https://raw.githubusercontent.com/croni1012/Aegisub/HEAD/changelog/<lang>.txt`,
and downloads each version from that release's GitHub asset. The address of the
asset is written in the changelog itself, on the line after each release header.

That means two things:

- `changelog/` is **tracked**, and a new release is only visible to anyone once
  it has been committed and pushed.
- `_release/` is **gitignored**. It only holds the built zips, which go to GitHub
  as release assets rather than into the repository.

`HEAD` in the address is not a branch: raw.githubusercontent.com resolves it to
whatever the default branch is, so renaming the branch does not break it.

## Publishing

The upload needs a GitHub token in `release.config.apikey.json` as
`github_token`, or one in `GH_TOKEN` / `GITHUB_TOKEN`. A fine-grained token with
**Contents: read and write** on this repository is enough — create it at
<https://github.com/settings/personal-access-tokens>.

Both scripts skip what is already published, so either can be re-run after a
failure part way through.
