// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include "vector2d.h"

#include <array>
#include <functional>
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
	int coordinate_width = 0;
	int coordinate_height = 0;
	std::vector<Sample> samples;
	std::string adapter;

	bool IsOk() const { return !samples.empty(); }
	Homography MapAt(size_t sample, size_t reference) const;
};

/// What a Mocha project file says about a shot, without anything having been exported from it.
struct ProjectTrack {
	/// The motion, as one Corner Pin sample per frame - which carries the whole of a projective
	/// track rather than a similarity approximation of it.
	Track track;
	/// Which of the tracker's own channels actually moved. These say what the shot contains, as
	/// against what the tracker was allowed to look for.
	bool has_scale = false;
	bool has_rotation = false;
	bool has_perspective = false;
	bool has_shear = false;
	/// The layer the track was taken from, for saying so out loud.
	std::string layer;
};

/// Read a Mocha project file.
///
/// Returns no track and no error for a project with nothing tracked in it: a shot that was opened
/// and never worked on is not a failure, and the caller has to leave everything as it was.
std::optional<ProjectTrack> ParseMochaProject(std::string const& text, int script_width,
	int script_height, std::string& error);

/// Parse the requested Mocha After Effects Transform Data or Corner Pin track.
/// File contents and clipboard contents use the same adapter; future trackers plug
/// in beside this one.
std::optional<Track> ParseMocha(std::string const& text, int script_width,
	int script_height, TrackKind expected_kind, std::string& error);

struct ApplyOptions {
	struct Components {
		bool track_x = true;
		bool track_y = true;
		bool scale = true;
		bool rotate = true;
		bool perspective = true;

		bool Any() const { return track_x || track_y || scale || rotate || perspective; }
	};

	size_t reference_sample = 0;
	std::optional<size_t> clip_reference_sample;
	Components main;
	Components clip;
	bool interpolate_animations = true;
	bool linear = false;
	bool clip_only = false;
	bool map_clips = true;
	bool scale_border = true;
	bool scale_shadow = true;
	bool scale_blur = true;
};

enum class ApplyProgressStage {
	Preparing,
	Applying,
	Writing
};

using ApplyProgressCallback = std::function<void(ApplyProgressStage stage,
	size_t complete, size_t total)>;

/// Apply frame-by-frame and store lossless source rows as extradata for Revert.
bool Apply(agi::Context *context, Track const& main_track,
	std::optional<Track> const& main_perspective_track,
	std::optional<Track> const& clip_track,
	std::optional<Track> const& clip_perspective_track,
	ApplyOptions const& options, std::string& error,
	ApplyProgressCallback progress = {});

/// Restore every selected motion group, including ImageMask groups, in one undo step.
bool Revert(agi::Context *context, std::string& error);

} // namespace typesetting::motion
