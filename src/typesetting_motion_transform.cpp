// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_motion_transform.h"

#include <cmath>

namespace typesetting::motion::detail {

Homography TransformMap(Sample const& current, Sample const& reference) {
	double sx = std::abs(reference.scale.X()) > 1e-9 ?
		current.scale.X() / reference.scale.X() : 1.0;
	double sy = std::abs(reference.scale.Y()) > 1e-9 ?
		current.scale.Y() / reference.scale.Y() : sx;
	// AE rotation is clockwise in its y-down screen coordinates. ASS \frz is the
	// opposite sign, but the homography here works directly in screen coordinates.
	double radians = (current.rotation - reference.rotation) * 3.14159265358979 / 180.0;
	double cosine = std::cos(radians), sine = std::sin(radians);
	double a = cosine * sx, b = -sine * sy;
	double c = sine * sx, d = cosine * sy;
	Homography out;
	out.value = {a, b,
		current.position.X() - a * reference.position.X() - b * reference.position.Y(),
		c, d,
		current.position.Y() - c * reference.position.X() - d * reference.position.Y(),
		0, 0, 1};
	return out;
}

} // namespace typesetting::motion::detail
