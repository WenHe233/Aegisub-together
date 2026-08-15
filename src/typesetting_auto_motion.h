// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include "typesetting_motion.h"

#include <functional>
#include <optional>
#include <string>

namespace agi { struct Context; }

namespace typesetting::motion {

struct AutoTrackSettings {
	int patch_radius = 3;
	int search_radius = 18;
	int grid_columns = 7;
	int grid_rows = 6;
	int maximum_features = 24;
	double minimum_correlation = .70;
	double minimum_correlation_separation = .025;
	double maximum_forward_backward_error = 1.75;
	int reference_relock_radius = 7;
	bool track_x = true;
	bool track_y = true;
	bool scale = false;
	bool rotate = false;
	bool perspective = false;
};

/// Follow a screen-space region with an internal planar model, then retain only
/// the output components requested in AutoTrackSettings. The result uses the
/// common Corner Pin representation so Apply, Revert and ImageMask handling do
/// not depend on the tracking backend.
std::optional<Track> TrackRegion(agi::Context *context, Vector2D top_left,
	Vector2D bottom_right, int first_frame, int last_frame, int reference_frame,
	AutoTrackSettings const& settings, std::function<bool(int, int)> progress,
	std::string& error);

} // namespace typesetting::motion
