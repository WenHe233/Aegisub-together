// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_auto_motion.h"

#include "ass_file.h"
#include "include/aegisub/context.h"
#include "video_controller.h"
#include "video_frame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace typesetting::motion {
namespace {

struct GrayFrame {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;

	uint8_t At(int x, int y) const {
		return pixels[static_cast<size_t>(y) * width + x];
	}
};

struct Match {
	Vector2D from;
	Vector2D to;
};

struct Similarity {
	double a = 1.0;
	double b = 0.0;
	double tx = 0.0;
	double ty = 0.0;

	Vector2D Map(Vector2D point) const {
		return Vector2D(static_cast<float>(a * point.X() - b * point.Y() + tx),
			static_cast<float>(b * point.X() + a * point.Y() + ty));
	}
};

GrayFrame MakeGray(VideoFrame const& source) {
	GrayFrame out;
	out.width = source.width;
	out.height = source.height;
	out.pixels.resize(static_cast<size_t>(out.width) * out.height);
	for (int y = 0; y < out.height; ++y) {
		int source_y = source.flipped ? out.height - y - 1 : y;
		auto row = source.data.data() + static_cast<size_t>(source_y) * source.pitch;
		for (int x = 0; x < out.width; ++x) {
			auto pixel = row + static_cast<size_t>(x) * 4;
			out.pixels[static_cast<size_t>(y) * out.width + x] =
				static_cast<uint8_t>((29 * pixel[0] + 150 * pixel[1] + 77 * pixel[2]) >> 8);
		}
	}
	return out;
}

bool InsideQuad(std::array<Vector2D, 4> const& quad, Vector2D point) {
	bool positive = false, negative = false;
	for (size_t index = 0; index < quad.size(); ++index) {
		Vector2D edge = quad[(index + 1) % quad.size()] - quad[index];
		Vector2D relative = point - quad[index];
		double cross = edge.X() * relative.Y() - edge.Y() * relative.X();
		positive = positive || cross > .001;
		negative = negative || cross < -.001;
		if (positive && negative) return false;
	}
	return true;
}

double CornerScore(GrayFrame const& frame, int x, int y) {
	constexpr int radius = 2;
	double xx = 0, xy = 0, yy = 0;
	if (x - radius - 1 < 0 || y - radius - 1 < 0 ||
		x + radius + 1 >= frame.width || y + radius + 1 >= frame.height) return 0;
	for (int py = -radius; py <= radius; ++py) {
		for (int px = -radius; px <= radius; ++px) {
			double gx = static_cast<double>(frame.At(x + px + 1, y + py)) -
				frame.At(x + px - 1, y + py);
			double gy = static_cast<double>(frame.At(x + px, y + py + 1)) -
				frame.At(x + px, y + py - 1);
			xx += gx * gx;
			xy += gx * gy;
			yy += gy * gy;
		}
	}
	double discriminant = std::sqrt(std::max(0.0, (xx - yy) * (xx - yy) + 4 * xy * xy));
	return (xx + yy - discriminant) * .5;
}

std::vector<Vector2D> FeaturePoints(GrayFrame const& frame,
	std::array<Vector2D, 4> const& quad, AutoTrackSettings const& settings) {
	Vector2D low = quad[0], high = quad[0];
	for (auto point : quad) { low = low.Min(point); high = high.Max(point); }
	int margin = settings.patch_radius + 2;
	int left = std::max(margin, static_cast<int>(std::floor(low.X())));
	int top = std::max(margin, static_cast<int>(std::floor(low.Y())));
	int right = std::min(frame.width - margin - 1, static_cast<int>(std::ceil(high.X())));
	int bottom = std::min(frame.height - margin - 1, static_cast<int>(std::ceil(high.Y())));
	std::vector<std::pair<double, Vector2D>> candidates;
	for (int y = top; y <= bottom; y += 2) {
		for (int x = left; x <= right; x += 2) {
			Vector2D point(x, y);
			if (!InsideQuad(quad, point)) continue;
			double score = CornerScore(frame, x, y);
			if (score > 1) candidates.emplace_back(score, point);
		}
	}
	std::stable_sort(candidates.begin(), candidates.end(),
		[](auto const& left, auto const& right) { return left.first > right.first; });
	double horizontal = ((quad[1] - quad[0]).Len() + (quad[2] - quad[3]).Len()) * .5 /
		std::max(1, settings.grid_columns);
	double vertical = ((quad[3] - quad[0]).Len() + (quad[2] - quad[1]).Len()) * .5 /
		std::max(1, settings.grid_rows);
	double spacing = std::clamp(std::min(horizontal, vertical) * .45, 4.0, 12.0);
	double spacing_squared = spacing * spacing;
	std::vector<Vector2D> out;
	out.reserve(settings.maximum_features);
	for (auto const& candidate : candidates) {
		bool separated = std::all_of(out.begin(), out.end(), [&](Vector2D selected) {
			return (selected - candidate.second).SquareLen() >= spacing_squared;
		});
		if (!separated) continue;
		out.push_back(candidate.second);
		if (out.size() >= static_cast<size_t>(settings.maximum_features)) break;
	}
	return out;
}

struct PatchTemplate {
	int radius = 0;
	int count = 0;
	std::vector<int> centred;
	long long energy = 0;
};

std::optional<PatchTemplate> MakePatchTemplate(GrayFrame const& frame, Vector2D centre,
	int radius) {
	int cx = static_cast<int>(std::lround(centre.X()));
	int cy = static_cast<int>(std::lround(centre.Y()));
	if (cx - radius < 0 || cy - radius < 0 || cx + radius >= frame.width ||
		cy + radius >= frame.height) return std::nullopt;
	PatchTemplate out;
	out.radius = radius;
	out.count = (radius * 2 + 1) * (radius * 2 + 1);
	int sum = 0;
	for (int y = -radius; y <= radius; ++y)
		for (int x = -radius; x <= radius; ++x) sum += frame.At(cx + x, cy + y);
	out.centred.reserve(out.count);
	for (int y = -radius; y <= radius; ++y) {
		for (int x = -radius; x <= radius; ++x) {
			int value = frame.At(cx + x, cy + y) * out.count - sum;
			out.centred.push_back(value);
			out.energy += static_cast<long long>(value) * value;
		}
	}
	if (out.energy < static_cast<long long>(out.count) * out.count * 100) return std::nullopt;
	return out;
}

double PatchCorrelation(PatchTemplate const& patch, GrayFrame const& frame, int cx, int cy) {
	int radius = patch.radius;
	if (cx - radius < 0 || cy - radius < 0 || cx + radius >= frame.width ||
		cy + radius >= frame.height) return -2;
	int sum = 0;
	for (int y = -radius; y <= radius; ++y)
		for (int x = -radius; x <= radius; ++x) sum += frame.At(cx + x, cy + y);
	long long numerator = 0, energy = 0;
	size_t index = 0;
	for (int y = -radius; y <= radius; ++y) {
		for (int x = -radius; x <= radius; ++x, ++index) {
			int value = frame.At(cx + x, cy + y) * patch.count - sum;
			numerator += static_cast<long long>(patch.centred[index]) * value;
			energy += static_cast<long long>(value) * value;
		}
	}
	if (!energy) return -2;
	return numerator / std::sqrt(static_cast<double>(patch.energy) * energy);
}

struct PatchResult {
	Vector2D point;
	double correlation = -2;
};

std::optional<PatchResult> FindPatch(GrayFrame const& source, GrayFrame const& target,
	Vector2D source_point, Vector2D target_guess, int search_radius,
	AutoTrackSettings const& settings, bool reject_ambiguous) {
	auto patch = MakePatchTemplate(source, source_point, settings.patch_radius);
	if (!patch) return std::nullopt;
	int guess_x = static_cast<int>(std::lround(target_guess.X()));
	int guess_y = static_cast<int>(std::lround(target_guess.Y()));
	int diameter = search_radius * 2 + 1;
	std::vector<double> correlations(static_cast<size_t>(diameter) * diameter, -2);
	double best = -2;
	int best_dx = 0, best_dy = 0;
	for (int dy = -search_radius; dy <= search_radius; ++dy) {
		for (int dx = -search_radius; dx <= search_radius; ++dx) {
			double score = PatchCorrelation(*patch, target, guess_x + dx, guess_y + dy);
			correlations[static_cast<size_t>(dy + search_radius) * diameter +
				dx + search_radius] = score;
			if (score > best) { best = score; best_dx = dx; best_dy = dy; }
		}
	}
	if (best < settings.minimum_correlation) return std::nullopt;
	if (reject_ambiguous) {
		double second = -2;
		for (int dy = -search_radius; dy <= search_radius; ++dy)
			for (int dx = -search_radius; dx <= search_radius; ++dx)
				if (std::abs(dx - best_dx) > 1 || std::abs(dy - best_dy) > 1)
					second = std::max(second, correlations[static_cast<size_t>(dy + search_radius) *
						diameter + dx + search_radius]);
		if (second > -1.5 && best - second < settings.minimum_correlation_separation)
			return std::nullopt;
	}
	auto score_at = [&](int dx, int dy) {
		if (dx < -search_radius || dx > search_radius ||
			dy < -search_radius || dy > search_radius) return -2.0;
		return correlations[static_cast<size_t>(dy + search_radius) * diameter +
			dx + search_radius];
	};
	auto subpixel = [&](double before, double centre, double after) {
		if (before < -1.5 || after < -1.5) return 0.0;
		double denominator = before - 2 * centre + after;
		if (denominator >= -1e-6) return 0.0;
		return std::clamp(.5 * (before - after) / denominator, -.5, .5);
	};
	double offset_x = subpixel(score_at(best_dx - 1, best_dy), best,
		score_at(best_dx + 1, best_dy));
	double offset_y = subpixel(score_at(best_dx, best_dy - 1), best,
		score_at(best_dx, best_dy + 1));
	return PatchResult{Vector2D(static_cast<float>(guess_x + best_dx + offset_x),
		static_cast<float>(guess_y + best_dy + offset_y)), best};
}

std::optional<Vector2D> MatchPatch(GrayFrame const& previous, GrayFrame const& next,
	Vector2D point, Vector2D prediction, int search_radius,
	AutoTrackSettings const& settings) {
	auto forward = FindPatch(previous, next, point, prediction, search_radius, settings, true);
	if (!forward) return std::nullopt;
	auto backward = FindPatch(next, previous, forward->point, point, 2, settings, false);
	if (!backward || (backward->point - point).Len() > settings.maximum_forward_backward_error)
		return std::nullopt;
	return forward->point;
}

Similarity FromPair(Match const& first, Match const& second) {
	Vector2D old_delta = second.from - first.from;
	Vector2D new_delta = second.to - first.to;
	double denominator = old_delta.SquareLen();
	if (denominator < 1e-6) return {};
	Similarity out;
	out.a = (old_delta.X() * new_delta.X() + old_delta.Y() * new_delta.Y()) / denominator;
	out.b = (old_delta.X() * new_delta.Y() - old_delta.Y() * new_delta.X()) / denominator;
	out.tx = first.to.X() - out.a * first.from.X() + out.b * first.from.Y();
	out.ty = first.to.Y() - out.b * first.from.X() - out.a * first.from.Y();
	return out;
}

Similarity Fit(std::vector<Match> const& matches, std::vector<size_t> const& indices) {
	double from_x = 0, from_y = 0, to_x = 0, to_y = 0;
	for (size_t index : indices) {
		from_x += matches[index].from.X(); from_y += matches[index].from.Y();
		to_x += matches[index].to.X(); to_y += matches[index].to.Y();
	}
	double count = static_cast<double>(indices.size());
	from_x /= count; from_y /= count; to_x /= count; to_y /= count;
	double numerator_a = 0, numerator_b = 0, denominator = 0;
	for (size_t index : indices) {
		double ox = matches[index].from.X() - from_x;
		double oy = matches[index].from.Y() - from_y;
		double nx = matches[index].to.X() - to_x;
		double ny = matches[index].to.Y() - to_y;
		numerator_a += ox * nx + oy * ny;
		numerator_b += ox * ny - oy * nx;
		denominator += ox * ox + oy * oy;
	}
	Similarity out;
	if (denominator > 1e-6) {
		out.a = numerator_a / denominator;
		out.b = numerator_b / denominator;
	}
	out.tx = to_x - out.a * from_x + out.b * from_y;
	out.ty = to_y - out.b * from_x - out.a * from_y;
	return out;
}

size_t MinimumInliers(size_t matches, size_t absolute_minimum) {
	return std::max(absolute_minimum,
		static_cast<size_t>(std::ceil(static_cast<double>(matches) * .4)));
}

std::optional<Similarity> EstimateSimilarity(std::vector<Match> const& matches,
	bool incremental) {
	if (matches.size() < 4) return std::nullopt;
	std::vector<size_t> best_inliers;
	double best_error = std::numeric_limits<double>::max();
	for (size_t first = 0; first < matches.size(); ++first) {
		for (size_t second = first + 1; second < matches.size(); ++second) {
			if ((matches[first].from - matches[second].from).Len() < 5.f) continue;
			Similarity candidate = FromPair(matches[first], matches[second]);
			double scale = std::hypot(candidate.a, candidate.b);
			double angle = std::abs(std::atan2(candidate.b, candidate.a));
			double minimum_scale = incremental ? .82 : .4;
			double maximum_scale = incremental ? 1.22 : 2.5;
			double maximum_angle = incremental ? .22 : 3.14159265358979;
			if (scale < minimum_scale || scale > maximum_scale || angle > maximum_angle) continue;
			std::vector<size_t> inliers;
			double error = 0;
			for (size_t index = 0; index < matches.size(); ++index) {
				double residual = (candidate.Map(matches[index].from) - matches[index].to).Len();
				if (residual <= 1.8) { inliers.push_back(index); error += residual; }
			}
			if (inliers.size() > best_inliers.size() ||
				(inliers.size() == best_inliers.size() && error < best_error)) {
				best_inliers = std::move(inliers);
				best_error = error;
			}
		}
	}
	if (best_inliers.size() < MinimumInliers(matches.size(), 4)) return std::nullopt;
	Similarity result = Fit(matches, best_inliers);
	double scale = std::hypot(result.a, result.b);
	double angle = std::abs(std::atan2(result.b, result.a));
	if (incremental && (scale < .82 || scale > 1.22 || angle > .22))
		return std::nullopt;
	return result;
}

bool SolveLinear(std::array<std::array<double, 9>, 8> matrix,
	std::array<double, 8>& result) {
	for (size_t column = 0; column < 8; ++column) {
		size_t pivot = column;
		for (size_t row = column + 1; row < 8; ++row)
			if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) pivot = row;
		if (std::abs(matrix[pivot][column]) < 1e-9) return false;
		if (pivot != column) std::swap(matrix[pivot], matrix[column]);
		double divisor = matrix[column][column];
		for (size_t at = column; at <= 8; ++at) matrix[column][at] /= divisor;
		for (size_t row = 0; row < 8; ++row) {
			if (row == column) continue;
			double factor = matrix[row][column];
			for (size_t at = column; at <= 8; ++at)
				matrix[row][at] -= factor * matrix[column][at];
		}
	}
	for (size_t row = 0; row < 8; ++row) result[row] = matrix[row][8];
	return true;
}

std::optional<Homography> FitProjective(std::vector<Match> const& matches,
	std::vector<size_t> const& indices) {
	if (indices.size() < 4) return std::nullopt;
	double from_x = 0, from_y = 0, to_x = 0, to_y = 0;
	for (size_t index : indices) {
		from_x += matches[index].from.X(); from_y += matches[index].from.Y();
		to_x += matches[index].to.X(); to_y += matches[index].to.Y();
	}
	double count = static_cast<double>(indices.size());
	from_x /= count; from_y /= count; to_x /= count; to_y /= count;
	double from_distance = 0, to_distance = 0;
	for (size_t index : indices) {
		from_distance += std::hypot(matches[index].from.X() - from_x,
			matches[index].from.Y() - from_y);
		to_distance += std::hypot(matches[index].to.X() - to_x,
			matches[index].to.Y() - to_y);
	}
	from_distance /= count; to_distance /= count;
	if (from_distance < 1e-6 || to_distance < 1e-6) return std::nullopt;
	double from_scale = std::sqrt(2.0) / from_distance;
	double to_scale = std::sqrt(2.0) / to_distance;
	std::array<std::array<double, 9>, 8> normal{};
	auto add_row = [&](std::array<double, 8> const& row, double value) {
		for (size_t y = 0; y < 8; ++y) {
			for (size_t x = 0; x < 8; ++x) normal[y][x] += row[y] * row[x];
			normal[y][8] += row[y] * value;
		}
	};
	for (size_t index : indices) {
		auto const& match = matches[index];
		double x = (match.from.X() - from_x) * from_scale;
		double y = (match.from.Y() - from_y) * from_scale;
		double u = (match.to.X() - to_x) * to_scale;
		double v = (match.to.Y() - to_y) * to_scale;
		add_row({x, y, 1, 0, 0, 0, -u * x, -u * y}, u);
		add_row({0, 0, 0, x, y, 1, -v * x, -v * y}, v);
	}
	std::array<double, 8> solved{};
	if (!SolveLinear(normal, solved)) return std::nullopt;
	if (!std::all_of(solved.begin(), solved.end(), [](double value) {
		return std::isfinite(value);
	})) return std::nullopt;
	using Matrix = std::array<double, 9>;
	auto multiply = [](Matrix const& left, Matrix const& right) {
		Matrix out{};
		for (size_t row = 0; row < 3; ++row)
			for (size_t column = 0; column < 3; ++column)
				for (size_t at = 0; at < 3; ++at)
					out[row * 3 + column] += left[row * 3 + at] * right[at * 3 + column];
		return out;
	};
	Matrix normalized = {solved[0], solved[1], solved[2], solved[3], solved[4],
		solved[5], solved[6], solved[7], 1};
	Matrix normalize_from = {from_scale, 0, -from_scale * from_x,
		0, from_scale, -from_scale * from_y, 0, 0, 1};
	Matrix denormalize_to = {1 / to_scale, 0, to_x,
		0, 1 / to_scale, to_y, 0, 0, 1};
	Matrix values = multiply(denormalize_to, multiply(normalized, normalize_from));
	if (std::abs(values[8]) < 1e-12) return std::nullopt;
	for (auto& value : values) value /= values[8];
	Homography out;
	out.value = values;
	return out;
}

double QuadArea(std::array<Vector2D, 4> const& quad) {
	double area = 0;
	for (size_t i = 0; i < 4; ++i) {
		auto const& first = quad[i];
		auto const& second = quad[(i + 1) % 4];
		area += first.X() * second.Y() - first.Y() * second.X();
	}
	return area * .5;
}

bool Plausible(Homography const& map, std::array<Vector2D, 4> const& quad,
	bool incremental) {
	std::array<Vector2D, 4> mapped;
	double minimum_edge = incremental ? .78 : .3;
	double maximum_edge = incremental ? 1.28 : 3.0;
	for (size_t i = 0; i < 4; ++i) {
		mapped[i] = map.Map(quad[i]);
		if (!std::isfinite(mapped[i].X()) || !std::isfinite(mapped[i].Y())) return false;
		double before = (quad[(i + 1) % 4] - quad[i]).Len();
		double after = (mapped[(i + 1) % 4] - mapped[i]).Len();
		if (before < 1 || after / before < minimum_edge || after / before > maximum_edge)
			return false;
	}
	double old_area = QuadArea(quad), new_area = QuadArea(mapped);
	if (old_area * new_area <= 0) return false;
	double ratio = std::abs(new_area / old_area);
	return incremental ? ratio >= .6 && ratio <= 1.65 : ratio >= .15 && ratio <= 6;
}

std::optional<Homography> EstimateProjective(std::vector<Match> const& matches,
	std::array<Vector2D, 4> const& quad, bool incremental) {
	if (matches.size() < 6) return std::nullopt;
	std::vector<size_t> best_inliers;
	double best_error = std::numeric_limits<double>::max();
	for (size_t a = 0; a + 3 < matches.size(); ++a)
		for (size_t b = a + 1; b + 2 < matches.size(); ++b)
			for (size_t c = b + 1; c + 1 < matches.size(); ++c)
				for (size_t d = c + 1; d < matches.size(); ++d) {
					auto candidate = FitProjective(matches, {a, b, c, d});
					if (!candidate || !Plausible(*candidate, quad, incremental)) continue;
					std::vector<size_t> inliers;
					double error = 0;
					for (size_t index = 0; index < matches.size(); ++index) {
						double residual = (candidate->Map(matches[index].from) -
							matches[index].to).Len();
						if (residual <= 2.0) { inliers.push_back(index); error += residual; }
					}
					if (inliers.size() > best_inliers.size() ||
						(inliers.size() == best_inliers.size() && error < best_error)) {
						best_inliers = std::move(inliers);
						best_error = error;
					}
				}
	if (best_inliers.size() < MinimumInliers(matches.size(), 6)) return std::nullopt;
	auto result = FitProjective(matches, best_inliers);
	if (!result || !Plausible(*result, quad, incremental)) return std::nullopt;
	return result;
}

std::optional<std::array<Vector2D, 4>> EstimateQuad(std::vector<Match> matches,
	std::array<Vector2D, 4> const& source, AutoTrackSettings const& settings,
	bool incremental) {
	std::array<Vector2D, 4> out = source;
	if (settings.perspective) {
		if (matches.size() > 14) matches.resize(14);
		auto map = EstimateProjective(matches, source, incremental);
		if (!map) return std::nullopt;
		for (auto& corner : out) corner = map->Map(corner);
	}
	else {
		// Output component switches must not weaken the internal tracker. Even an
		// X/Y-only result needs a similarity model to keep following a region that
		// rotates or changes size.
		auto map = EstimateSimilarity(matches, incremental);
		if (!map) return std::nullopt;
		for (auto& corner : out) corner = map->Map(corner);
	}
	if (incremental) {
		double maximum = settings.search_radius * 2.5 + 2;
		for (size_t index = 0; index < out.size(); ++index)
			if ((out[index] - source[index]).Len() > maximum) return std::nullopt;
	}
	return out;
}

std::optional<std::array<Vector2D, 4>> RelockToReference(GrayFrame const& reference,
	GrayFrame const& current, std::array<Vector2D, 4> const& reference_quad,
	std::vector<Vector2D> const& reference_points,
	std::array<Vector2D, 4> const& prediction, AutoTrackSettings const& settings,
	int search_radius) {
	std::vector<Match> corner_matches;
	for (size_t index = 0; index < reference_quad.size(); ++index)
		corner_matches.push_back({reference_quad[index], prediction[index]});
	auto predictor = FitProjective(corner_matches, {0, 1, 2, 3});
	if (!predictor) return std::nullopt;
	std::vector<Match> matches;
	for (auto point : reference_points) {
		Vector2D expected = predictor->Map(point);
		if (auto matched = MatchPatch(reference, current, point, expected,
			search_radius, settings)) matches.push_back({point, *matched});
	}
	return EstimateQuad(std::move(matches), reference_quad, settings, false);
}

double MaximumCornerDistance(std::array<Vector2D, 4> const& first,
	std::array<Vector2D, 4> const& second) {
	double distance = 0;
	for (size_t index = 0; index < first.size(); ++index)
		distance = std::max(distance, static_cast<double>((first[index] - second[index]).Len()));
	return distance;
}

bool Advance(GrayFrame const& previous, GrayFrame const& next,
	GrayFrame const& reference, std::array<Vector2D, 4> const& reference_quad,
	std::vector<Vector2D> const& reference_points, std::array<Vector2D, 4>& quad,
	AutoTrackSettings const& settings) {
	auto points = FeaturePoints(previous, quad, settings);
	std::vector<Match> matches;
	for (auto point : points)
		if (auto matched = MatchPatch(previous, next, point, point,
			settings.search_radius, settings)) matches.push_back({point, *matched});
	auto incremental = EstimateQuad(std::move(matches), quad, settings, true);
	if (!incremental) {
		auto recovered = RelockToReference(reference, next, reference_quad,
			reference_points, quad, settings, settings.search_radius);
		if (!recovered) return false;
		quad = *recovered;
		return true;
	}
	auto relocked = RelockToReference(reference, next, reference_quad,
		reference_points, *incremental, settings, settings.reference_relock_radius);
	if (relocked && MaximumCornerDistance(*relocked, *incremental) <=
		settings.reference_relock_radius * 1.5)
		quad = *relocked;
	else
		quad = *incremental;
	return true;
}

Vector2D Centre(std::array<Vector2D, 4> const& quad) {
	Vector2D centre;
	for (auto corner : quad) centre = centre + corner;
	return centre / 4.f;
}

Vector2D Rotate(Vector2D point, double angle) {
	double cosine = std::cos(angle), sine = std::sin(angle);
	return Vector2D(static_cast<float>(point.X() * cosine - point.Y() * sine),
		static_cast<float>(point.X() * sine + point.Y() * cosine));
}

std::array<Vector2D, 4> FilterComponents(std::array<Vector2D, 4> const& reference,
	std::array<Vector2D, 4> const& tracked, AutoTrackSettings const& settings) {
	Vector2D reference_centre = Centre(reference);
	Vector2D tracked_centre = Centre(tracked);
	std::vector<Match> corner_matches;
	std::vector<size_t> indices;
	for (size_t index = 0; index < reference.size(); ++index) {
		corner_matches.push_back({reference[index], tracked[index]});
		indices.push_back(index);
	}
	Similarity overall = Fit(corner_matches, indices);
	double tracked_scale = std::hypot(overall.a, overall.b);
	double tracked_angle = std::atan2(overall.b, overall.a);
	if (!std::isfinite(tracked_scale) || tracked_scale < 1e-6) tracked_scale = 1;
	Vector2D target_centre(settings.track_x ? tracked_centre.X() : reference_centre.X(),
		settings.track_y ? tracked_centre.Y() : reference_centre.Y());
	double output_scale = settings.scale ? tracked_scale : 1;
	double output_angle = settings.rotate ? tracked_angle : 0;
	std::array<Vector2D, 4> out;
	for (size_t i = 0; i < 4; ++i) {
		Vector2D local = settings.perspective ?
			Rotate(tracked[i] - tracked_centre, -tracked_angle) /
				static_cast<float>(tracked_scale) : reference[i] - reference_centre;
		out[i] = target_centre + Rotate(local * static_cast<float>(output_scale), output_angle);
	}
	return out;
}

} // namespace

std::optional<Track> TrackRegion(agi::Context *context, Vector2D top_left,
	Vector2D bottom_right, int first_frame, int last_frame, int reference_frame,
	AutoTrackSettings const& settings, std::function<bool(int, int)> progress,
	std::string& error) {
	error.clear();
	if (!context || !context->videoController || first_frame > last_frame ||
		reference_frame < first_frame || reference_frame > last_frame) {
		error = "The Auto Motion frame range is invalid.";
		return std::nullopt;
	}
	if (!settings.track_x && !settings.track_y && !settings.scale &&
		!settings.rotate && !settings.perspective) {
		error = "Select at least one motion component.";
		return std::nullopt;
	}
	auto reference_video = context->videoController->GetFrame(reference_frame, true);
	if (!reference_video) { error = "The reference video frame could not be read."; return std::nullopt; }
	int script_width = 0, script_height = 0;
	context->ass->GetResolution(script_width, script_height);
	if (script_width <= 0 || script_height <= 0) {
		error = "The subtitle resolution is invalid.";
		return std::nullopt;
	}
	double x_scale = static_cast<double>(reference_video->width) / script_width;
	double y_scale = static_cast<double>(reference_video->height) / script_height;
	std::array<Vector2D, 4> reference_quad = {
		Vector2D(top_left.X() * x_scale, top_left.Y() * y_scale),
		Vector2D(bottom_right.X() * x_scale, top_left.Y() * y_scale),
		Vector2D(bottom_right.X() * x_scale, bottom_right.Y() * y_scale),
		Vector2D(top_left.X() * x_scale, bottom_right.Y() * y_scale)
	};
	if ((reference_quad[1] - reference_quad[0]).Len() < 14.f ||
		(reference_quad[3] - reference_quad[0]).Len() < 14.f) {
		error = "The tracking region is too small; select at least 14 x 14 video pixels.";
		return std::nullopt;
	}

	int frame_count = last_frame - first_frame + 1;
	std::vector<std::array<Vector2D, 4>> quads(static_cast<size_t>(frame_count), reference_quad);
	int completed = 0;
	auto report = [&]() { return !progress || progress(completed, std::max(1, frame_count - 1)); };

	GrayFrame reference = MakeGray(*reference_video);
	auto reference_points = FeaturePoints(reference, reference_quad, settings);
	int minimum_features = settings.perspective ? 6 : 4;
	if (static_cast<int>(reference_points.size()) < minimum_features) {
		error = "The selected region does not contain enough distinct, stable corners to track.";
		return std::nullopt;
	}
	GrayFrame previous = reference;
	auto quad = reference_quad;
	for (int frame = reference_frame + 1; frame <= last_frame; ++frame) {
		auto video = context->videoController->GetFrame(frame, true);
		if (!video) { error = "A video frame could not be read."; return std::nullopt; }
		GrayFrame next = MakeGray(*video);
		if (!Advance(previous, next, reference, reference_quad, reference_points,
			quad, settings)) {
			error = "Auto Motion lost the selected region at video frame " +
				std::to_string(frame + 1) +
				". Select a larger region with stable corners.";
			return std::nullopt;
		}
		quads[static_cast<size_t>(frame - first_frame)] = quad;
		previous = std::move(next);
		++completed;
		if (!report()) { error = "Auto motion cancelled."; return std::nullopt; }
	}
	previous = reference;
	quad = reference_quad;
	for (int frame = reference_frame - 1; frame >= first_frame; --frame) {
		auto video = context->videoController->GetFrame(frame, true);
		if (!video) { error = "A video frame could not be read."; return std::nullopt; }
		GrayFrame next = MakeGray(*video);
		if (!Advance(previous, next, reference, reference_quad, reference_points,
			quad, settings)) {
			error = "Auto Motion lost the selected region at video frame " +
				std::to_string(frame + 1) +
				". Select a larger region with stable corners.";
			return std::nullopt;
		}
		quads[static_cast<size_t>(frame - first_frame)] = quad;
		previous = std::move(next);
		++completed;
		if (!report()) { error = "Auto motion cancelled."; return std::nullopt; }
	}
	Track track;
	track.kind = TrackKind::CornerPin;
	track.source_width = reference_video->width;
	track.source_height = reference_video->height;
	track.adapter = settings.perspective ? "native-auto-projective-stabilized" :
		"native-auto-similarity-stabilized";
	track.samples.reserve(quads.size());
	for (int frame = first_frame; frame <= last_frame; ++frame) {
		Sample sample;
		sample.source_frame = frame;
		auto native = FilterComponents(reference_quad,
			quads[static_cast<size_t>(frame - first_frame)], settings);
		for (size_t corner = 0; corner < 4; ++corner)
			sample.corners[corner] = Vector2D(native[corner].X() / x_scale,
				native[corner].Y() / y_scale);
		track.samples.push_back(sample);
	}
	return track;
}

} // namespace typesetting::motion
