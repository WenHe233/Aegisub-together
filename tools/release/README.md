# Release helpers

Double-clickable helpers for the Aegisub - nyaa's edition release flow. Each `.cmd`
just runs its Python script, so they can also be started from a terminal with
extra arguments.

| Script | What it does |
| --- | --- |
| `release-local.cmd` | Copies the built `aegisub.exe` and the Hungarian catalogue into `C:\aegisub-portable`, for trying a build out immediately. |
| `translate.cmd` | Translates whatever is new in `po/hu.po` and in `changelog/hu.txt` into the other languages. It does not build or pack anything. |
| `release.cmd` | Builds and verifies the Windows updater ZIP, then offers to create and push the `v<version>` tag. The tag starts the complete Windows, macOS, and Linux GitHub Actions release. It does not run translations. |

## Settings

`release.config.json` next to these scripts holds the paths. It is created with
defaults on the first run; edit it and re-run. It is committed, so **no keys
belong in it**.

`source_language` remains the source used by the translation helper, while
`release_language` independently selects the changelog used for package versions
and GitHub release notes. The package and release title formats are controlled by
`release_asset_template` and `release_name_template`.

`release.config.apikey.json` holds the keys and is kept out of git. It is created
empty on the first run:

```json
{
  "openai_api_key": "sk-...",
  "github_token": "github_pat_..."
}
```

## Translation

`translate.cmd` reads the OpenAI key from `release.config.apikey.json`, falling
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
run. It builds when there is no executable yet or when something under `src/`,
`libaegisub/` or `po/` is newer than the one that is there; otherwise it says so
and goes straight to packing. This also detects catalogue changes made by
`translate.cmd`. Translating the changelog never triggers a build: the changelog
is neither compiled nor packed.

## Where the files live

The program reads its changelogs straight out of this repository, from
`https://raw.githubusercontent.com/croni1012/Aegisub/HEAD/changelog/<lang>.txt`,
and downloads each version from that release's GitHub asset. The address of the
asset is written in the changelog itself, on the line after each release header.

That means two things:

- `changelog/` is **tracked**, and a new release is only visible to anyone once
  it has been committed and pushed.
- `.releases/` is **gitignored**. It only holds the built zips, which go to GitHub
  as release assets rather than into the repository.

`HEAD` in the address is not a branch: raw.githubusercontent.com resolves it to
whatever the default branch is, so renaming the branch does not break it.

## Publishing

For a fork, first open the repository's **Actions** page and enable workflows.
This only has to be done once. `release.cmd` uses the Git authentication already
configured for the repository; it does not need a separate GitHub API token.

Before pushing a tag, the script verifies that the worktree is clean, the
current branch is the default branch, its HEAD is already pushed, and the tag
does not exist locally or on GitHub. It then creates an annotated `v<version>`
tag and pushes it to `origin` after confirmation.

The tag creates the complete cross-platform release in GitHub Actions. The
release contains the Windows installer, a full Windows portable ZIP, the
smaller `-update.zip` used only by the Windows in-program updater, Intel and
Apple Silicon macOS DMGs, a Linux x86_64 AppImage, and the source tarball. The
workflow creates a missing GitHub release or updates the assets of an existing
release without replacing hand-written release notes.
