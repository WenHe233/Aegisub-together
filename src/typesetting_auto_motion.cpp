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
	int grid_columns = std::max(1, settings.grid_columns);
	int grid_rows = std::max(1, settings.grid_rows);
	std::vector<bool> occupied(static_cast<size_t>(grid_columns * grid_rows));
	auto cell_for = [&](Vector2D point) {
		int column = std::clamp(static_cast<int>((point.X() - left) * grid_columns /
			std::max(1, right - left + 1)), 0, grid_columns - 1);
		int row = std::clamp(static_cast<int>((point.Y() - top) * grid_rows /
			std::max(1, bottom - top + 1)), 0, grid_rows - 1);
		return static_cast<size_t>(row * grid_columns + column);
	};
	auto select = [&](Vector2D point) {
		bool separated = std::all_of(out.begin(), out.end(), [&](Vector2D selected) {
			return (selected - point).SquareLen() >= spacing_squared;
		});
		if (!separated) return false;
		out.push_back(point);
		return true;
	};
	// First take the strongest corner from each grid cell. A globally sorted
	// list alone tends to cluster on text edges, which makes scale and rotation
	// estimates unnecessarily noisy even when position tracking is sound.
	for (auto const& candidate : candidates) {
		size_t cell = cell_for(candidate.second);
		if (occupied[cell] || !select(candidate.second)) continue;
		occupied[cell] = true;
		if (out.size() >= static_cast<size_t>(settings.maximum_features)) return out;
	}
	// Fill any remaining capacity with the best well-separated corners.
	for (auto const& candidate : candidates) {
		if (!select(candidate.second)) continue;
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

std::optional<std::array<Vector2D, 4>> EstimateQuad(std::vector<Match> matches,
	std::array<Vector2D, 4> const& source, AutoTrackSettings const& settings,
	bool incremental) {
	std::array<Vector2D, 4> out = source;
	// Output component switches must not weaken the internal tracker. Even an
	// X/Y-only result needs a similarity model to keep following a region that
	// rotates or changes size.
	auto map = EstimateSimilarity(matches, incremental);
	if (!map) return std::nullopt;
	for (auto& corner : out) corner = map->Map(corner);
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
	Similarity predictor = Fit(corner_matches, {0, 1, 2, 3});
	std::vector<Match> matches;
	for (auto point : reference_points) {
		Vector2D expected = predictor.Map(point);
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

bool RegionIsIdentical(GrayFrame const& previous, GrayFrame const& next,
	std::array<Vector2D, 4> const& quad) {
	if (previous.width != next.width || previous.height != next.height) return false;
	Vector2D low = quad[0], high = quad[0];
	for (auto point : quad) { low = low.Min(point); high = high.Max(point); }
	int left = std::clamp(static_cast<int>(std::floor(low.X())), 0, previous.width - 1);
	int top = std::clamp(static_cast<int>(std::floor(low.Y())), 0, previous.height - 1);
	int right = std::clamp(static_cast<int>(std::ceil(high.X())), left + 1, previous.width);
	int bottom = std::clamp(static_cast<int>(std::ceil(high.Y())), top + 1, previous.height);
	int compared = 0;
	for (int y = top; y < bottom; ++y) {
		for (int x = left; x < right; ++x) {
			if (!InsideQuad(quad, Vector2D(x + .5f, y + .5f))) continue;
			++compared;
			if (previous.At(x, y) != next.At(x, y)) return false;
		}
	}
	return compared > 0;
}

bool Advance(GrayFrame const& previous, GrayFrame const& next,
	GrayFrame const& reference, std::array<Vector2D, 4> const& reference_quad,
	std::vector<Vector2D> const& reference_points, std::array<Vector2D, 4>& quad,
	AutoTrackSettings const& settings) {
	// Held/duplicated source frames must reuse the exact previous transform.
	// Running sub-pixel correlation on identical pixels can otherwise introduce
	// tiny scale or position noise and prevents equal subtitle events from merging.
	if (RegionIsIdentical(previous, next, quad)) return true;
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

struct SimilarityObservation {
	Vector2D centre;
	double scale = 1.0;
	double angle = 0.0;
};

std::vector<double> Smooth(std::vector<double> const& input) {
	if (input.size() < 3) return input;
	std::vector<double> output(input.size());
	constexpr int weight[] = {1, 2, 3, 2, 1};
	for (size_t index = 0; index < input.size(); ++index) {
		double sum = 0, total = 0;
		for (int offset = -2; offset <= 2; ++offset) {
			int at = std::clamp(static_cast<int>(index) + offset, 0,
				static_cast<int>(input.size()) - 1);
			double current_weight = weight[offset + 2];
			sum += input[static_cast<size_t>(at)] * current_weight;
			total += current_weight;
		}
		output[index] = sum / total;
	}
	return output;
}

std::vector<double> MedianSmooth(std::vector<double> const& input) {
	if (input.size() < 3) return input;
	std::vector<double> output(input.size());
	for (size_t index = 0; index < input.size(); ++index) {
		std::array<double, 3> values = {
			input[index ? index - 1 : index], input[index],
			input[std::min(index + 1, input.size() - 1)]
		};
		std::sort(values.begin(), values.end());
		output[index] = values[1];
	}
	return output;
}

void Linearise(std::vector<double>& values, size_t reference_index) {
	if (values.size() < 2 || reference_index >= values.size()) return;
	auto original = values;
	if (reference_index) {
		for (size_t index = 0; index <= reference_index; ++index) {
			double progress = static_cast<double>(index) / reference_index;
			values[index] = original.front() * (1 - progress) +
				original[reference_index] * progress;
		}
	}
	if (reference_index + 1 < values.size()) {
		size_t length = values.size() - reference_index - 1;
		for (size_t index = reference_index; index < values.size(); ++index) {
			double progress = static_cast<double>(index - reference_index) / length;
			values[index] = original[reference_index] * (1 - progress) +
				original.back() * progress;
		}
	}
}

std::vector<SimilarityObservation> StabilisedSimilarity(
	std::vector<std::array<Vector2D, 4>> const& quads,
	std::array<Vector2D, 4> const& reference, size_t reference_index, bool linear) {
	std::vector<SimilarityObservation> observations;
	observations.reserve(quads.size());
	Vector2D reference_centre = Centre(reference);
	double previous_angle = 0;
	for (auto const& quad : quads) {
		Vector2D tracked_centre = Centre(quad);
		std::vector<Match> matches;
		std::vector<size_t> indices;
		for (size_t index = 0; index < reference.size(); ++index) {
			matches.push_back({reference[index], quad[index]});
			indices.push_back(index);
		}
		Similarity fitted = Fit(matches, indices);
		double scale = std::hypot(fitted.a, fitted.b);
		double angle = std::atan2(fitted.b, fitted.a);
		if (!std::isfinite(scale) || scale < 1e-6) scale = 1;
		if (!std::isfinite(angle)) angle = previous_angle;
		while (angle - previous_angle > 3.14159265358979) angle -= 2 * 3.14159265358979;
		while (angle - previous_angle < -3.14159265358979) angle += 2 * 3.14159265358979;
		previous_angle = angle;
		observations.push_back({tracked_centre, scale, angle});
	}
	if (observations.empty()) return observations;
	std::vector<double> x, y, scale, angle;
	for (auto const& value : observations) {
		x.push_back(value.centre.X());
		y.push_back(value.centre.Y());
		scale.push_back(value.scale);
		angle.push_back(value.angle);
	}
	if (linear && observations.size() > 1) {
		Linearise(x, reference_index);
		Linearise(y, reference_index);
		Linearise(scale, reference_index);
		Linearise(angle, reference_index);
	}
	else {
		x = Smooth(MedianSmooth(x));
		y = Smooth(MedianSmooth(y));
		scale = Smooth(MedianSmooth(scale));
		angle = Smooth(MedianSmooth(angle));
		if (reference_index < observations.size()) {
			x[reference_index] = reference_centre.X();
			y[reference_index] = reference_centre.Y();
			scale[reference_index] = 1.0;
			angle[reference_index] = 0.0;
		}
	}
	auto scale_bounds = std::minmax_element(scale.begin(), scale.end());
	auto angle_bounds = std::minmax_element(angle.begin(), angle.end());
	if (*scale_bounds.second - *scale_bounds.first < .012 &&
		std::max(std::abs(*scale_bounds.first - 1), std::abs(*scale_bounds.second - 1)) < .012)
		std::fill(scale.begin(), scale.end(), 1.0);
	constexpr double angle_noise_floor = .75 * 3.14159265358979 / 180.0;
	if (*angle_bounds.second - *angle_bounds.first < angle_noise_floor &&
		std::max(std::abs(*angle_bounds.first), std::abs(*angle_bounds.second)) < angle_noise_floor)
		std::fill(angle.begin(), angle.end(), 0.0);
	for (size_t index = 0; index < observations.size(); ++index)
		observations[index] = {Vector2D(static_cast<float>(x[index]), static_cast<float>(y[index])),
			scale[index], angle[index]};
	return observations;
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
	if (!settings.track_x && !settings.track_y && !settings.scale && !settings.rotate) {
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
	int minimum_features = 4;
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
	track.source_width = reference_video->width;
	track.source_height = reference_video->height;
	track.adapter = settings.linear ?
		"native-auto-similarity-linear" : "native-auto-similarity-stabilized";
	track.samples.reserve(quads.size());
	size_t reference_index = static_cast<size_t>(reference_frame - first_frame);
	track.kind = TrackKind::Transform;
	auto observations = StabilisedSimilarity(quads, reference_quad,
		reference_index, settings.linear);
	Vector2D reference_centre = Centre(reference_quad);
	for (int frame = first_frame; frame <= last_frame; ++frame) {
		auto const& observed = observations[static_cast<size_t>(frame - first_frame)];
		Sample sample;
		sample.source_frame = frame;
		sample.position = Vector2D(
			static_cast<float>((settings.track_x ? observed.centre.X() : reference_centre.X()) / x_scale),
			static_cast<float>((settings.track_y ? observed.centre.Y() : reference_centre.Y()) / y_scale));
		double output_scale = settings.scale ? observed.scale : 1.0;
		sample.scale = Vector2D(static_cast<float>(output_scale * 100),
			static_cast<float>(output_scale * 100));
		sample.rotation = settings.rotate ? observed.angle * 180.0 / 3.14159265358979 : 0.0;
		track.samples.push_back(sample);
	}
	return track;
}

} // namespace typesetting::motion
