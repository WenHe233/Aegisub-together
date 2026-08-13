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
#include <numeric>
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

Vector2D QuadPoint(std::array<Vector2D, 4> const& quad, double u, double v) {
	Vector2D top = quad[0] * static_cast<float>(1.0 - u) + quad[1] * static_cast<float>(u);
	Vector2D bottom = quad[3] * static_cast<float>(1.0 - u) + quad[2] * static_cast<float>(u);
	return top * static_cast<float>(1.0 - v) + bottom * static_cast<float>(v);
}

int TextureScore(GrayFrame const& frame, int x, int y, int radius) {
	if (x - radius < 0 || y - radius < 0 || x + radius >= frame.width ||
		y + radius >= frame.height) return 0;
	int sum = 0, squared = 0, count = 0;
	for (int py = -radius; py <= radius; ++py)
		for (int px = -radius; px <= radius; ++px) {
			int value = frame.At(x + px, y + py);
			sum += value;
			squared += value * value;
			++count;
		}
	return squared * count - sum * sum;
}

std::vector<Vector2D> FeaturePoints(GrayFrame const& frame,
	std::array<Vector2D, 4> const& quad, AutoTrackSettings const& settings) {
	std::vector<std::pair<int, Vector2D>> candidates;
	for (int row = 0; row < settings.grid_rows; ++row) {
		double v = (row + 1.0) / (settings.grid_rows + 1.0);
		for (int column = 0; column < settings.grid_columns; ++column) {
			double u = (column + 1.0) / (settings.grid_columns + 1.0);
			Vector2D point = QuadPoint(quad, u, v);
			int x = static_cast<int>(std::lround(point.X()));
			int y = static_cast<int>(std::lround(point.Y()));
			int score = TextureScore(frame, x, y, settings.patch_radius);
			if (score > 0) candidates.emplace_back(score, Vector2D(x, y));
		}
	}
	std::stable_sort(candidates.begin(), candidates.end(),
		[](auto const& left, auto const& right) { return left.first > right.first; });
	if (candidates.size() > static_cast<size_t>(settings.maximum_features))
		candidates.resize(settings.maximum_features);
	std::vector<Vector2D> out;
	out.reserve(candidates.size());
	for (auto const& candidate : candidates) out.push_back(candidate.second);
	return out;
}

std::optional<Vector2D> MatchPatch(GrayFrame const& previous, GrayFrame const& next,
	Vector2D centre, AutoTrackSettings const& settings) {
	int cx = static_cast<int>(std::lround(centre.X()));
	int cy = static_cast<int>(std::lround(centre.Y()));
	int radius = settings.patch_radius;
	int count = (radius * 2 + 1) * (radius * 2 + 1);
	if (cx - radius < 0 || cy - radius < 0 || cx + radius >= previous.width ||
		cy + radius >= previous.height) return std::nullopt;
	int old_sum = 0;
	for (int y = -radius; y <= radius; ++y)
		for (int x = -radius; x <= radius; ++x) old_sum += previous.At(cx + x, cy + y);
	int old_deviation = 0;
	for (int y = -radius; y <= radius; ++y)
		for (int x = -radius; x <= radius; ++x)
			old_deviation += std::abs(previous.At(cx + x, cy + y) * count - old_sum);
	if (old_deviation < count * 110) return std::nullopt;

	long long best = std::numeric_limits<long long>::max();
	int best_x = cx, best_y = cy;
	for (int dy = -settings.search_radius; dy <= settings.search_radius; ++dy) {
		int ny = cy + dy;
		if (ny - radius < 0 || ny + radius >= next.height) continue;
		for (int dx = -settings.search_radius; dx <= settings.search_radius; ++dx) {
			int nx = cx + dx;
			if (nx - radius < 0 || nx + radius >= next.width) continue;
			int new_sum = 0;
			for (int y = -radius; y <= radius; ++y)
				for (int x = -radius; x <= radius; ++x) new_sum += next.At(nx + x, ny + y);
			long long score = 0;
			for (int y = -radius; y <= radius; ++y)
				for (int x = -radius; x <= radius; ++x) {
					int old_value = previous.At(cx + x, cy + y) * count - old_sum;
					int new_value = next.At(nx + x, ny + y) * count - new_sum;
					score += std::abs(old_value - new_value);
				}
			if (score < best) { best = score; best_x = nx; best_y = ny; }
		}
	}
	if (best == std::numeric_limits<long long>::max() || best > old_deviation * 2LL)
		return std::nullopt;
	return Vector2D(best_x, best_y);
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

std::optional<Similarity> EstimateSimilarity(std::vector<Match> const& matches) {
	if (matches.size() < 3) return std::nullopt;
	std::vector<size_t> best_inliers;
	double best_error = std::numeric_limits<double>::max();
	for (size_t first = 0; first < matches.size(); ++first) {
		for (size_t second = first + 1; second < matches.size(); ++second) {
			if ((matches[first].from - matches[second].from).Len() < 5.f) continue;
			Similarity candidate = FromPair(matches[first], matches[second]);
			double scale = std::hypot(candidate.a, candidate.b);
			double angle = std::abs(std::atan2(candidate.b, candidate.a));
			if (scale < .75 || scale > 1.33 || angle > .35) continue;
			std::vector<size_t> inliers;
			double error = 0;
			for (size_t index = 0; index < matches.size(); ++index) {
				double residual = (candidate.Map(matches[index].from) - matches[index].to).Len();
				if (residual <= 2.75) { inliers.push_back(index); error += residual; }
			}
			if (inliers.size() > best_inliers.size() ||
				(inliers.size() == best_inliers.size() && error < best_error)) {
				best_inliers = std::move(inliers);
				best_error = error;
			}
		}
	}
	if (best_inliers.size() < 3) return std::nullopt;
	Similarity result = Fit(matches, best_inliers);
	double scale = std::hypot(result.a, result.b);
	if (scale < .75 || scale > 1.33) return std::nullopt;
	return result;
}

std::optional<Similarity> EstimateTranslation(std::vector<Match> const& matches) {
	if (matches.size() < 2) return std::nullopt;
	std::vector<double> dx, dy;
	dx.reserve(matches.size()); dy.reserve(matches.size());
	for (auto const& match : matches) {
		dx.push_back(match.to.X() - match.from.X());
		dy.push_back(match.to.Y() - match.from.Y());
	}
	auto median = [](std::vector<double>& values) {
		auto middle = values.begin() + values.size() / 2;
		std::nth_element(values.begin(), middle, values.end());
		return *middle;
	};
	Similarity candidate;
	candidate.tx = median(dx);
	candidate.ty = median(dy);
	double sum_x = 0, sum_y = 0;
	int inliers = 0;
	for (auto const& match : matches) {
		double mx = match.to.X() - match.from.X();
		double my = match.to.Y() - match.from.Y();
		if (std::hypot(mx - candidate.tx, my - candidate.ty) > 2.75) continue;
		sum_x += mx; sum_y += my; ++inliers;
	}
	if (inliers < 2) return std::nullopt;
	candidate.tx = sum_x / inliers;
	candidate.ty = sum_y / inliers;
	return candidate;
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
	std::array<std::array<double, 9>, 8> normal{};
	auto add_row = [&](std::array<double, 8> const& row, double value) {
		for (size_t y = 0; y < 8; ++y) {
			for (size_t x = 0; x < 8; ++x) normal[y][x] += row[y] * row[x];
			normal[y][8] += row[y] * value;
		}
	};
	for (size_t index : indices) {
		auto const& match = matches[index];
		double x = match.from.X(), y = match.from.Y();
		double u = match.to.X(), v = match.to.Y();
		add_row({x, y, 1, 0, 0, 0, -u * x, -u * y}, u);
		add_row({0, 0, 0, x, y, 1, -v * x, -v * y}, v);
	}
	std::array<double, 8> solved{};
	if (!SolveLinear(normal, solved)) return std::nullopt;
	if (!std::all_of(solved.begin(), solved.end(), [](double value) {
		return std::isfinite(value);
	})) return std::nullopt;
	Homography out;
	out.value = {solved[0], solved[1], solved[2], solved[3], solved[4], solved[5],
		solved[6], solved[7], 1};
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

bool Plausible(Homography const& map, std::array<Vector2D, 4> const& quad) {
	std::array<Vector2D, 4> mapped;
	for (size_t i = 0; i < 4; ++i) {
		mapped[i] = map.Map(quad[i]);
		if (!std::isfinite(mapped[i].X()) || !std::isfinite(mapped[i].Y())) return false;
		double before = (quad[(i + 1) % 4] - quad[i]).Len();
		double after = (mapped[(i + 1) % 4] - mapped[i]).Len();
		if (before < 1 || after / before < .55 || after / before > 1.8) return false;
	}
	double old_area = QuadArea(quad), new_area = QuadArea(mapped);
	if (old_area * new_area <= 0) return false;
	double ratio = std::abs(new_area / old_area);
	return ratio >= .4 && ratio <= 2.5;
}

std::optional<Homography> EstimateProjective(std::vector<Match> const& matches,
	std::array<Vector2D, 4> const& quad) {
	if (matches.size() < 4) return std::nullopt;
	std::vector<size_t> best_inliers;
	double best_error = std::numeric_limits<double>::max();
	for (size_t a = 0; a + 3 < matches.size(); ++a)
		for (size_t b = a + 1; b + 2 < matches.size(); ++b)
			for (size_t c = b + 1; c + 1 < matches.size(); ++c)
				for (size_t d = c + 1; d < matches.size(); ++d) {
					auto candidate = FitProjective(matches, {a, b, c, d});
					if (!candidate || !Plausible(*candidate, quad)) continue;
					std::vector<size_t> inliers;
					double error = 0;
					for (size_t index = 0; index < matches.size(); ++index) {
						double residual = (candidate->Map(matches[index].from) -
							matches[index].to).Len();
						if (residual <= 3.0) { inliers.push_back(index); error += residual; }
					}
					if (inliers.size() > best_inliers.size() ||
						(inliers.size() == best_inliers.size() && error < best_error)) {
						best_inliers = std::move(inliers);
						best_error = error;
					}
				}
	if (best_inliers.size() < 4) return std::nullopt;
	auto result = FitProjective(matches, best_inliers);
	if (!result || !Plausible(*result, quad)) return std::nullopt;
	return result;
}

bool Advance(GrayFrame const& previous, GrayFrame const& next,
	std::array<Vector2D, 4>& quad, AutoTrackSettings const& settings) {
	auto points = FeaturePoints(previous, quad, settings);
	std::vector<Match> matches;
	for (auto point : points)
		if (auto matched = MatchPatch(previous, next, point, settings))
			matches.push_back({point, *matched});
	if (settings.perspective) {
		// Four-point RANSAC grows combinatorially. The feature list is already
		// ordered by texture quality, so the best twelve retain robust coverage
		// while keeping interactive tracks quick.
		if (matches.size() > 12) matches.resize(12);
		auto map = EstimateProjective(matches, quad);
		if (!map) return false;
		for (auto& corner : quad) corner = map->Map(corner);
		return true;
	}
	auto map = settings.scale || settings.rotate ?
		EstimateSimilarity(matches) : EstimateTranslation(matches);
	if (!map) return false;
	for (auto& corner : quad) corner = map->Map(corner);
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
	Vector2D reference_edge = reference[1] - reference[0];
	Vector2D tracked_edge = tracked[1] - tracked[0];
	double reference_length = std::max(1e-6, static_cast<double>(reference_edge.Len()));
	double tracked_scale = tracked_edge.Len() / reference_length;
	double tracked_angle = std::atan2(tracked_edge.Y(), tracked_edge.X()) -
		std::atan2(reference_edge.Y(), reference_edge.X());
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
	int failed = 0;
	auto report = [&]() { return !progress || progress(completed, std::max(1, frame_count - 1)); };

	GrayFrame previous = MakeGray(*reference_video);
	auto quad = reference_quad;
	for (int frame = reference_frame + 1; frame <= last_frame; ++frame) {
		auto video = context->videoController->GetFrame(frame, true);
		if (!video) { error = "A video frame could not be read."; return std::nullopt; }
		GrayFrame next = MakeGray(*video);
		if (!Advance(previous, next, quad, settings)) ++failed;
		quads[static_cast<size_t>(frame - first_frame)] = quad;
		previous = std::move(next);
		++completed;
		if (!report()) { error = "Auto motion cancelled."; return std::nullopt; }
	}
	previous = MakeGray(*reference_video);
	quad = reference_quad;
	for (int frame = reference_frame - 1; frame >= first_frame; --frame) {
		auto video = context->videoController->GetFrame(frame, true);
		if (!video) { error = "A video frame could not be read."; return std::nullopt; }
		GrayFrame next = MakeGray(*video);
		if (!Advance(previous, next, quad, settings)) ++failed;
		quads[static_cast<size_t>(frame - first_frame)] = quad;
		previous = std::move(next);
		++completed;
		if (!report()) { error = "Auto motion cancelled."; return std::nullopt; }
	}
	if (failed > std::max(2, frame_count / 5)) {
		error = "The selected region did not contain enough stable detail to track.";
		return std::nullopt;
	}

	Track track;
	track.kind = TrackKind::CornerPin;
	track.source_width = reference_video->width;
	track.source_height = reference_video->height;
	track.adapter = settings.perspective ? "native-auto-projective" :
		(settings.scale || settings.rotate ? "native-auto-similarity" : "native-auto-translation");
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
