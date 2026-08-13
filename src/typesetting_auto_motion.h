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
	int maximum_features = 20;
	bool track_x = true;
	bool track_y = true;
	bool scale = false;
	bool rotate = false;
	bool perspective = false;
};

/// Follow a screen-space region with a similarity transform (position, uniform
/// scale and rotation). The result deliberately uses the common Corner Pin
/// representation so a future planar/perspective tracker can replace this
/// adapter without changing Apply, Revert or ImageMask handling.
std::optional<Track> TrackRegion(agi::Context *context, Vector2D top_left,
	Vector2D bottom_right, int first_frame, int last_frame, int reference_frame,
	AutoTrackSettings const& settings, std::function<bool(int, int)> progress,
	std::string& error);

} // namespace typesetting::motion
