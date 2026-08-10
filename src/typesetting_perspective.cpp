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

/// @file typesetting_perspective.cpp
/// @see typesetting_perspective.h

#include "typesetting_perspective.h"

#include "vector3d.h"

#include <algorithm>
#include <cmath>

namespace typesetting {
namespace {

const double pi = 3.14159265358979;
const double deg2rad = pi / 180.0;
const double rad2deg = 180.0 / pi;

/// The distance the renderer projects from. Not a free parameter: it is the number the
/// renderers use, in the script's own units.
double ScreenZ(Vector2D screen_scale) {
	double scale = screen_scale.Y() > 1e-6 ? screen_scale.Y() : 1.0;
	return 312.5 * scale;
}

/// Two equations, two unknowns, with a pivot so the near-singular case does not blow up.
void Solve2x2(double a11, double a12, double a21, double a22, double b1, double b2,
              double &x1, double &x2) {
	if (std::abs(a11) < std::abs(a21)) {
		std::swap(b1, b2);
		std::swap(a11, a21);
		std::swap(a12, a22);
	}
	a21 = a21 / a11;
	a22 = a22 - a21 * a12;
	double z1 = b1;
	double z2 = b2 - a21 * z1;
	x2 = z2 / a22;
	x1 = (z1 - a12 * x2) / a11;
}

/// Where the two diagonals of a quadrilateral cross.
Vector2D QuadMidpoint(Vector2D const corners[4]) {
	Vector2D diagonal1 = corners[2] - corners[0];
	Vector2D diagonal2 = corners[1] - corners[3];
	Vector2D to_last = corners[3] - corners[0];
	double along1, along2;
	Solve2x2(diagonal1.X(), diagonal2.X(), diagonal1.Y(), diagonal2.Y(),
	         to_last.X(), to_last.Y(), along1, along2);
	return corners[0] + diagonal1 * (float)along1;
}

/// How far the alignment point sits from the top left of the box.
Vector2D AlignShift(int alignment, double width, double height) {
	double shift_x = 0, shift_y = 0;
	switch ((alignment - 1) % 3) {
		case 1: shift_x = -width / 2; break;
		case 2: shift_x = -width; break;
		default: break;
	}
	switch ((alignment - 1) / 3) {
		case 0: shift_y = -height; break;
		case 1: shift_y = -height / 2; break;
		default: break;
	}
	return Vector2D((float)shift_x, (float)shift_y);
}

} // namespace

void PerspectiveQuad(PerspectiveTags const& tags, int alignment,
                     Vector2D box_first, Vector2D box_second, Vector2D screen_scale,
                     Vector2D corners[4]) {
	double screen_z = ScreenZ(screen_scale);
	double width = std::max<double>(box_second.X() - box_first.X(), 1.0);
	double height = std::max<double>(box_second.Y() - box_first.Y(), 1.0);
	Vector2D shift = AlignShift(alignment, width, height);

	Vector2D box[4] = {
		Vector2D(box_first.X(), box_first.Y()),
		Vector2D(box_second.X(), box_first.Y()),
		Vector2D(box_second.X(), box_second.Y()),
		Vector2D(box_first.X(), box_second.Y())
	};

	for (int i = 0; i < 4; ++i) {
		Vector2D point = box[i];
		// The lean, then the alignment, then the scale: the order the renderer works in.
		point = Vector2D((float)(point.X() + point.Y() * tags.shear_x),
		                 (float)(point.X() * tags.shear_y + point.Y()));
		point = point + shift;
		point = Vector2D((float)(point.X() * tags.scale.X() / 100.0),
		                 (float)(point.Y() * tags.scale.Y() / 100.0));
		// Measured from what the turns turn about.
		point = point + tags.pos - tags.org;

		Vector3D turned(point);
		turned = turned.RotateZ((float)(-tags.angle_z * deg2rad));
		turned = turned.RotateX((float)(-tags.angle_x * deg2rad));
		turned = turned.RotateY((float)(tags.angle_y * deg2rad));
		turned = turned * (float)(screen_z / (turned.Z() + screen_z));
		corners[i] = turned.XY() + tags.org;
	}
}

PerspectiveTags SolvePerspective(Vector2D const corners[4], int alignment,
                                 Vector2D box_first, Vector2D box_second,
                                 Vector2D screen_scale, Vector2D previous_org) {
	PerspectiveTags out;
	double screen_z = ScreenZ(screen_scale);

	// A quadrilateral is the projection of a parallelogram, and which one does not depend on
	// where it sits - so the two depths come out of the corners alone.
	double depth1, depth3;
	Vector2D diagonal = corners[2] - corners[0];
	Vector2D side2 = corners[1] - corners[2];
	Vector2D side3 = corners[3] - corners[2];
	Solve2x2(side2.X(), side3.X(), side2.Y(), side3.Y(),
	         -diagonal.X(), -diagonal.Y(), depth1, depth3);

	// Any origin that reproduces the corners renders the same picture, so the one the line
	// already had is kept: the tags then stay as close to what they were as they can.
	Vector2D org = previous_org ? previous_org : QuadMidpoint(corners);

	Vector2D relative[4];
	for (int i = 0; i < 4; ++i) relative[i] = corners[i] - org;

	Vector3D corner[4];
	corner[0] = Vector3D(relative[0], (float)screen_z);
	corner[1] = Vector3D(relative[1], (float)screen_z) * (float)depth1;
	corner[2] = Vector3D(relative[2], (float)screen_z) * (float)(depth1 + depth3 - 1);
	corner[3] = Vector3D(relative[3], (float)screen_z) * (float)depth3;

	// How deep the point that projects to the origin lies.
	double along0, along1;
	Vector3D edge0 = corner[1] - corner[0];
	Vector3D edge1 = corner[3] - corner[0];
	Solve2x2(edge0.X(), edge1.X(), edge0.Y(), edge1.Y(),
	         -corner[0].X(), -corner[0].Y(), along0, along1);
	double origin_z = (corner[0] + edge0 * (float)along0 + edge1 * (float)along1).Z();
	if (!(std::abs(origin_z) > 1e-9)) return out;

	// Put the origin at the distance the projection is done from, and the screen at zero.
	for (int i = 0; i < 4; ++i)
		corner[i] = corner[i] * (float)(screen_z / origin_z) -
			Vector3D(0.f, 0.f, (float)screen_z);

	// The plane's normal says how far it is turned out of the screen.
	Vector3D normal = (corner[1] - corner[0]).Cross(corner[3] - corner[0]);
	double turn_y = std::atan(normal.X() / normal.Z());
	if (normal.Z() < 0) turn_y += pi;
	normal = normal.RotateY((float)turn_y);
	double turn_x = std::atan(normal.Y() / normal.Z());

	for (int i = 0; i < 4; ++i)
		corner[i] = corner[i].RotateY((float)turn_y).RotateX((float)turn_x);

	Vector3D top = corner[1] - corner[0];
	double turn_z = std::atan(top.Y() / top.X());
	if (top.X() < 0) turn_z += pi;

	for (int i = 0; i < 4; ++i) corner[i] = corner[i].RotateZ((float)-turn_z);

	// What is left in the plane is a parallelogram with a horizontal top, so its shape is
	// the scale and the lean.
	top = corner[1] - corner[0];
	Vector3D side = corner[3] - corner[0];
	double raw_shear = side.X() / side.Y();

	double quad_width = top.Len();
	double quad_height = std::abs(side.Y());
	double width = std::max<double>(box_second.X() - box_first.X(), 1.0);
	double height = std::max<double>(box_second.Y() - box_first.Y(), 1.0);
	double scale_x = quad_width / width;
	double scale_y = quad_height / height;

	double shift_h = alignment % 3 == 0 ? 1 : (alignment % 3 == 2 ? 0.5 : 0);
	double shift_v = alignment <= 3 ? 1 : (alignment <= 6 ? 0.5 : 0);
	double shear_x = std::abs(scale_x) > 1e-9 ? raw_shear * scale_y / scale_x : 0.0;

	// Where the box begins, which for a drawing is not the corner of the box: the renderer
	// leans it before it scales it, so the lean has to be taken off the same way round or the
	// line lands beside itself by the height of its own ink.
	double first_x = box_first.X() + box_first.Y() * shear_x;

	out.org = org;
	out.pos = org + corner[0].XY() -
		Vector2D((float)(first_x * scale_x), (float)(box_first.Y() * scale_y)) +
		Vector2D((float)(quad_width * shift_h), (float)(quad_height * shift_v));
	out.scale = Vector2D((float)(100.0 * scale_x), (float)(100.0 * scale_y));
	out.shear_x = shear_x;
	out.shear_y = 0;
	out.angle_x = turn_x * rad2deg;
	out.angle_y = -turn_y * rad2deg;
	out.angle_z = -turn_z * rad2deg;

	double all[] = {out.pos.X(), out.pos.Y(), out.org.X(), out.org.Y(),
	                out.scale.X(), out.scale.Y(), out.shear_x,
	                out.angle_x, out.angle_y, out.angle_z};
	for (double value : all)
		if (!std::isfinite(value)) return out;
	// A box with no size behind it is not a solution, it is a collapse.
	if (!(quad_width > 1e-6) || !(quad_height > 1e-6)) return out;

	out.ok = true;
	return out;
}

} // namespace typesetting
