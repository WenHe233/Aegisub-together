// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include "vector2d.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace typesetting::motion::detail {

struct PointMatch {
	Vector2D from;
	Vector2D to;
};

/// Rotation followed by independent X/Y scale in the rotated axes, plus
/// translation. This is the most expressive model supported by Auto Motion.
struct PlanarTransform {
	double scale_x = 1.0;
	double scale_y = 1.0;
	double angle = 0.0;
	double tx = 0.0;
	double ty = 0.0;

	Vector2D Map(Vector2D point) const;
};

std::optional<PlanarTransform> FitPlanar(std::vector<PointMatch> const& matches,
	std::vector<size_t> const& indices);
std::optional<PlanarTransform> EstimatePlanar(
	std::vector<PointMatch> const& matches, bool incremental);

/// Decide whether a tracked region is visually unchanged. Exact duplicates are
/// always accepted; a very small amount of decoder rounding noise is tolerated.
bool IsUnchangedRegion(size_t compared_pixels, uint64_t absolute_difference,
	size_t materially_changed_pixels);

} // namespace typesetting::motion::detail
