# Changelog

## 4.0 - 2026-08-02

### Added

- OpenAI connection settings with a shared, configurable model and a link to the available model documentation.
- AI subtitle review and translation tools in the AI menu.
- Hungarian subtitle post-check for spelling, punctuation, wording, repetition, consistency, and source-line mismatches.
- Modal suggestion review with three alternatives, previous/skip/apply controls, progress display, and subtitle/video context playback.

### Changed

- AI requests use compact payloads and concise responses to reduce token usage and cost.
- The post-check automatically reviews the selected lines when at least 100 lines are selected; otherwise it reviews all Default-style lines.
- The subtitle grid follows the line currently being reviewed or played.
- Hungarian UI text and character encoding were corrected.

### Fixed

- Removed duplicate warning text and the unnecessary final review summary.
- Fixed post-check cancellation, window closing, and navigation behavior.
- Restored the repository wxWidgets setup and fixed subtitle-grid flicker during drag selection.
