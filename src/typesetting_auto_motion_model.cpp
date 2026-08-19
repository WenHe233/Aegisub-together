// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_auto_motion_model.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace typesetting::motion::detail {
namespace {

size_t MinimumInliers(size_t matches, size_t absolute_minimum) {
	return std::max(absolute_minimum,
		static_cast<size_t>(std::ceil(static_cast<double>(matches) * .4)));
}

bool Allowed(PlanarTransform const& transform, bool incremental) {
	double minimum_scale = incremental ? .82 : .4;
	double maximum_scale = incremental ? 1.22 : 2.5;
	double maximum_angle = incremental ? .22 : 3.14159265358979;
	return std::isfinite(transform.scale_x) && std::isfinite(transform.scale_y) &&
		std::isfinite(transform.angle) &&
		transform.scale_x >= minimum_scale && transform.scale_x <= maximum_scale &&
		transform.scale_y >= minimum_scale && transform.scale_y <= maximum_scale &&
		std::abs(transform.angle) <= maximum_angle;
}

} // namespace

Vector2D PlanarTransform::Map(Vector2D point) const {
	double cosine = std::cos(angle), sine = std::sin(angle);
	return Vector2D(
		static_cast<float>(cosine * scale_x * point.X() -
			sine * scale_y * point.Y() + tx),
		static_cast<float>(sine * scale_x * point.X() +
			cosine * scale_y * point.Y() + ty));
}

std::optional<PlanarTransform> FitPlanar(std::vector<PointMatch> const& matches,
	std::vector<size_t> const& indices) {
	if (indices.size() < 3) return std::nullopt;
	double from_x = 0, from_y = 0, to_x = 0, to_y = 0;
	for (size_t index : indices) {
		if (index >= matches.size()) return std::nullopt;
		from_x += matches[index].from.X(); from_y += matches[index].from.Y();
		to_x += matches[index].to.X(); to_y += matches[index].to.Y();
	}
	double count = static_cast<double>(indices.size());
	from_x /= count; from_y /= count; to_x /= count; to_y /= count;
	double xx = 0, xy = 0, yy = 0;
	double ux = 0, uy = 0, vx = 0, vy = 0;
	for (size_t index : indices) {
		double ox = matches[index].from.X() - from_x;
		double oy = matches[index].from.Y() - from_y;
		double nx = matches[index].to.X() - to_x;
		double ny = matches[index].to.Y() - to_y;
		xx += ox * ox; xy += ox * oy; yy += oy * oy;
		ux += ox * nx; uy += oy * nx;
		vx += ox * ny; vy += oy * ny;
	}
	double determinant = xx * yy - xy * xy;
	if (std::abs(determinant) < 1e-8) return std::nullopt;
	double a = (ux * yy - uy * xy) / determinant;
	double b = (uy * xx - ux * xy) / determinant;
	double c = (vx * yy - vy * xy) / determinant;
	double d = (vy * xx - vx * xy) / determinant;

	// Project the unconstrained affine fit onto R(angle) * diag(scale_x, scale_y).
	// This retains non-uniform scale without allowing shear to leak into output.
	double angle = std::atan2(c - b, a + d);
	double cosine = std::cos(angle), sine = std::sin(angle);
	PlanarTransform out;
	out.angle = angle;
	out.scale_x = a * cosine + c * sine;
	out.scale_y = -b * sine + d * cosine;
	out.tx = to_x - cosine * out.scale_x * from_x +
		sine * out.scale_y * from_y;
	out.ty = to_y - sine * out.scale_x * from_x -
		cosine * out.scale_y * from_y;
	if (!std::isfinite(out.tx) || !std::isfinite(out.ty)) return std::nullopt;
	return out;
}

std::optional<PlanarTransform> EstimatePlanar(
	std::vector<PointMatch> const& matches, bool incremental) {
	if (matches.size() < 4) return std::nullopt;
	std::vector<size_t> best_inliers;
	double best_error = std::numeric_limits<double>::max();
	for (size_t first = 0; first < matches.size(); ++first) {
		for (size_t second = first + 1; second < matches.size(); ++second) {
			for (size_t third = second + 1; third < matches.size(); ++third) {
				double area = std::abs((matches[second].from - matches[first].from).Cross(
					matches[third].from - matches[first].from));
				if (area < 12.0) continue;
				auto candidate = FitPlanar(matches, {first, second, third});
				if (!candidate || !Allowed(*candidate, incremental)) continue;
				std::vector<size_t> inliers;
				double error = 0;
				for (size_t index = 0; index < matches.size(); ++index) {
					double residual = (candidate->Map(matches[index].from) -
						matches[index].to).Len();
					if (residual <= 1.8) { inliers.push_back(index); error += residual; }
				}
				if (inliers.size() > best_inliers.size() ||
					(inliers.size() == best_inliers.size() && error < best_error)) {
					best_inliers = std::move(inliers);
					best_error = error;
				}
			}
		}
	}
	if (best_inliers.size() < MinimumInliers(matches.size(), 4)) return std::nullopt;
	auto result = FitPlanar(matches, best_inliers);
	if (!result || !Allowed(*result, incremental)) return std::nullopt;
	return result;
}

bool IsUnchangedRegion(size_t compared_pixels, uint64_t absolute_difference,
	size_t materially_changed_pixels) {
	if (!compared_pixels) return false;
	if (!absolute_difference) return true;
	// Static blocks in inter-frame codecs can differ by an occasional luma unit
	// after decoding. Motion changes many edge pixels by substantially more.
	return absolute_difference * 2 <= compared_pixels &&
		materially_changed_pixels * 200 <= compared_pixels;
}

} // namespace typesetting::motion::detail
