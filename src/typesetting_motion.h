// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include "vector2d.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace agi { struct Context; }

namespace typesetting::motion {

struct Homography {
	std::array<double, 9> value{1, 0, 0, 0, 1, 0, 0, 0, 1};
	Vector2D Map(Vector2D point) const;
	std::optional<Homography> Inverse() const;
};

enum class TrackKind { Transform, CornerPin };

struct Sample {
	int source_frame = 0;
	Vector2D position;
	Vector2D scale{100.f, 100.f};
	double rotation = 0;
	std::array<Vector2D, 4> corners{};
};

struct Track {
	TrackKind kind = TrackKind::Transform;
	int source_width = 0;
	int source_height = 0;
	std::vector<Sample> samples;
	std::string adapter;

	bool IsOk() const { return !samples.empty(); }
	Homography MapAt(size_t sample, size_t reference) const;
};

/// Parse Mocha's After Effects Transform Data or Corner Pin text. File contents and
/// clipboard contents use the same adapter; future trackers plug in beside this one.
std::optional<Track> ParseMocha(std::string const& text, int script_width,
	int script_height, std::string& error);

struct ApplyOptions {
	size_t reference_sample = 0;
	std::optional<size_t> clip_reference_sample;
	bool relative_to_selection = true;
	bool map_clips = true;
	bool scale_border = true;
	bool scale_shadow = true;
	bool scale_blur = true;
};

/// Apply frame-by-frame and store lossless source rows as extradata for Revert.
bool Apply(agi::Context *context, Track const& main_track,
	std::optional<Track> const& clip_track, ApplyOptions const& options,
	std::string& error);

/// Restore every selected motion group, including ImageMask groups, in one undo step.
bool Revert(agi::Context *context, std::string& error);

} // namespace typesetting::motion
