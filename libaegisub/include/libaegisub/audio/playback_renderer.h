// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace agi {
class AudioProvider;

/// Sequential PCM renderer used by audio output backends.
///
/// The renderer preserves pitch while changing playback speed and keeps all
/// positions in source-audio frames. Input providers are expected to use the
/// canonical Aegisub playback format (signed 16-bit PCM).
class AudioPlaybackRenderer final {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	explicit AudioPlaybackRenderer(AudioProvider *provider);
	~AudioPlaybackRenderer();

	AudioPlaybackRenderer(AudioPlaybackRenderer const&) = delete;
	AudioPlaybackRenderer& operator=(AudioPlaybackRenderer const&) = delete;

	/// Start a new sequential render of [start, start + count).
	void Reset(int64_t start, int64_t count, double speed);

	/// Render up to frames frames of interleaved signed 16-bit PCM.
	/// Returns the number of frames written.
	size_t Render(int16_t *output, size_t frames, double volume);

	/// Change the source end position of the active range.
	void SetEndPosition(int64_t end);

	bool IsFinished() const;
	double GetSpeed() const;
	int64_t GetStartPosition() const;
	int64_t GetEndPosition() const;
	int64_t GetExpectedOutputFrames() const;
	int64_t GetRenderedOutputFrames() const;

	/// Convert a played output-frame count to the corresponding source frame.
	int64_t SourceFrameAtOutputFrame(int64_t output_frame) const;
};
}
