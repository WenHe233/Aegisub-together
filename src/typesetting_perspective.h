// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file typesetting_perspective.h
/// @brief Where a line turned out of the plane lands, and what tags put it there
///
/// A line with \frx or \fry is not a rectangle on screen any more: the renderer turns its
/// box in space and projects it, so what is on screen is a quadrilateral. Nothing that
/// works in two numbers - a scale, a lean, a turn - can follow that, because a map applied
/// after the projection is not the same as one applied before it.
///
/// A quadrilateral it can follow, though. These two go between the two descriptions: the
/// tags say where the four corners land, and four corners say what the tags have to be.
/// Both are the renderer's own arithmetic, worked out the same way the perspective tool
/// works it out - written out separately here so that changing one cannot disturb it.

#pragma once

#include "vector2d.h"

namespace typesetting {

/// Everything a line says about where its box sits and how it is turned.
struct PerspectiveTags {
	Vector2D pos;
	Vector2D org;
	Vector2D scale{100.f, 100.f};
	double shear_x = 0;   ///< \fax
	double shear_y = 0;   ///< \fay
	double angle_z = 0;   ///< \frz
	double angle_x = 0;   ///< \frx
	double angle_y = 0;   ///< \fry

	/// Whether the four corners could be turned back into numbers at all. A quadrilateral
	/// that is folded over itself, or flattened to a line, has no plane behind it.
	bool ok = false;
};

/// Where the four corners of a line's text box land on screen, in script coordinates,
/// running top left, top right, bottom right, bottom left.
///
/// `box` is the line's own extents before its scale, as the visual tools measure them: for
/// text it starts at zero, for a drawing it can start anywhere.
void PerspectiveQuad(PerspectiveTags const& tags, int alignment,
                     Vector2D box_first, Vector2D box_second, Vector2D screen_scale,
                     Vector2D corners[4]);

/// The other way about: what a line has to say for its box to land on those four corners.
///
/// `previous_org` is where the line turned about before, which is kept when the arithmetic
/// allows it - any origin that reproduces the corners renders the same, so this only decides
/// which of the equivalent ways of saying it comes out.
PerspectiveTags SolvePerspective(Vector2D const corners[4], int alignment,
                                 Vector2D box_first, Vector2D box_second,
                                 Vector2D screen_scale, Vector2D previous_org);

} // namespace typesetting
