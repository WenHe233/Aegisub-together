// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "visual_color_segment.h"

#include "ai_client.h"
#include "compat.h"
#include "dialog_progress.h"
#include "format.h"
#include "options.h"
#include "video_frame.h"

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/boykov_kolmogorov_max_flow.hpp>
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include <utility>

#include <wx/base64.h>
#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/cursor.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/pen.h>
#include <wx/settings.h>

namespace {
	struct GridPoint {
		int x;
		int y;
	};

	struct Edge {
		GridPoint from;
		GridPoint to;
		bool used = false;
	};

	long long point_key(GridPoint point) {
		return (static_cast<long long>(point.y) << 32) |
			static_cast<unsigned int>(point.x);
	}

	int direction(GridPoint from, GridPoint to) {
		if (to.x > from.x) return 0;
		if (to.y > from.y) return 1;
		if (to.x < from.x) return 2;
		return 3;
	}

	double point_segment_distance(Vector2D point, Vector2D a, Vector2D b) {
		Vector2D delta = b - a;
		double length_squared = delta.SquareLen();
		if (length_squared <= 1e-8)
			return (point - a).Len();
		double t = std::clamp(static_cast<double>((point - a).Dot(delta)) / length_squared, 0.0, 1.0);
		return (point - (a + delta * static_cast<float>(t))).Len();
	}

	void simplify_open(std::vector<Vector2D> const& source, size_t first, size_t last,
		double epsilon, std::vector<unsigned char>& keep) {
		if (last <= first + 1) return;
		double largest = 0.0;
		size_t largest_index = first;
		for (size_t i = first + 1; i < last; ++i) {
			double distance = point_segment_distance(source[i], source[first], source[last]);
			if (distance > largest) {
				largest = distance;
				largest_index = i;
			}
		}
		if (largest <= epsilon) return;
		keep[largest_index] = 1;
		simplify_open(source, first, largest_index, epsilon, keep);
		simplify_open(source, largest_index, last, epsilon, keep);
	}

	std::vector<Vector2D> simplify_closed(std::vector<Vector2D> points, double epsilon) {
		if (points.size() < 4) return points;

		// Removing exact grid-line runs before RDP makes large flat colour areas
		// very cheap while retaining every real corner.
		std::vector<Vector2D> corners;
		corners.reserve(points.size());
		for (size_t i = 0; i < points.size(); ++i) {
			Vector2D previous = points[(i + points.size() - 1) % points.size()];
			Vector2D current = points[i];
			Vector2D next = points[(i + 1) % points.size()];
			Vector2D a = current - previous;
			Vector2D b = next - current;
			if (std::abs(a.Cross(b)) > 1e-5f)
				corners.push_back(current);
		}
		if (corners.size() < 4) return corners;

		size_t opposite = 1;
		float farthest = 0.f;
		for (size_t i = 1; i < corners.size(); ++i) {
			float distance = (corners[i] - corners[0]).SquareLen();
			if (distance > farthest) {
				farthest = distance;
				opposite = i;
			}
		}

		std::vector<Vector2D> opened;
		opened.reserve(corners.size() + 1);
		for (size_t i = 0; i <= corners.size(); ++i)
			opened.push_back(corners[i % corners.size()]);
		std::vector<unsigned char> keep(opened.size(), 0);
		keep[0] = keep[opposite] = keep.back() = 1;
		simplify_open(opened, 0, opposite, epsilon, keep);
		simplify_open(opened, opposite, opened.size() - 1, epsilon, keep);

		std::vector<Vector2D> result;
		for (size_t i = 0; i + 1 < opened.size(); ++i)
			if (keep[i]) result.push_back(opened[i]);
		return result.size() >= 3 ? result : corners;
	}

	double polygon_area(std::vector<Vector2D> const& polygon) {
		double area = 0.0;
		for (size_t i = 0; i < polygon.size(); ++i) {
			auto const& a = polygon[i];
			auto const& b = polygon[(i + 1) % polygon.size()];
			area += static_cast<double>(a.X()) * b.Y() - static_cast<double>(b.X()) * a.Y();
		}
		return area * .5;
	}

	bool polygon_contains_point(std::vector<Vector2D> const& polygon, Vector2D point) {
		bool inside = false;
		for (size_t i = 0, previous = polygon.size() - 1;
			i < polygon.size(); previous = i++) {
			auto const& a = polygon[previous];
			auto const& b = polygon[i];
			if ((a.Y() > point.Y()) == (b.Y() > point.Y())) continue;
			float crossing_x = a.X() + (point.Y() - a.Y()) *
				(b.X() - a.X()) / (b.Y() - a.Y());
			if (point.X() < crossing_x) inside = !inside;
		}
		return inside;
	}

	std::vector<std::vector<Vector2D>> trace_binary_mask(
		std::vector<unsigned char> const& selected, int width, int height,
		int origin_x, int origin_y, double simplify_epsilon = .6) {
		if (width < 1 || height < 1 ||
			selected.size() != static_cast<size_t>(width) * height)
			return {};

		std::vector<Edge> edges;
		edges.reserve(selected.size());
		auto matches = [&](int x, int y) {
			return x >= 0 && y >= 0 && x < width && y < height &&
				selected[static_cast<size_t>(y) * width + x];
		};
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				if (!matches(x, y)) continue;
				if (!matches(x, y - 1)) edges.push_back({{x, y}, {x + 1, y}});
				if (!matches(x + 1, y)) edges.push_back({{x + 1, y}, {x + 1, y + 1}});
				if (!matches(x, y + 1)) edges.push_back({{x + 1, y + 1}, {x, y + 1}});
				if (!matches(x - 1, y)) edges.push_back({{x, y + 1}, {x, y}});
			}
		}

		std::unordered_map<long long, std::vector<size_t>> outgoing;
		outgoing.reserve(edges.size());
		for (size_t i = 0; i < edges.size(); ++i)
			outgoing[point_key(edges[i].from)].push_back(i);

		std::vector<std::vector<Vector2D>> contours;
		for (size_t seed = 0; seed < edges.size(); ++seed) {
			if (edges[seed].used) continue;
			std::vector<Vector2D> contour;
			size_t current = seed;
			GridPoint start = edges[seed].from;
			for (size_t guard = 0; guard <= edges.size(); ++guard) {
				Edge& edge = edges[current];
				if (edge.used) break;
				edge.used = true;
				contour.emplace_back(static_cast<float>(origin_x + edge.from.x),
					static_cast<float>(origin_y + edge.from.y));
				if (edge.to.x == start.x && edge.to.y == start.y) break;
				auto found = outgoing.find(point_key(edge.to));
				if (found == outgoing.end()) break;
				int incoming = direction(edge.from, edge.to);
				int best_rank = std::numeric_limits<int>::max();
				size_t next_edge = edges.size();
				for (size_t candidate : found->second) {
					if (edges[candidate].used) continue;
					int turn = (direction(edges[candidate].from, edges[candidate].to) - incoming + 4) % 4;
					int rank = turn == 1 ? 0 : turn == 0 ? 1 : turn == 3 ? 2 : 3;
					if (rank < best_rank) {
						best_rank = rank;
						next_edge = candidate;
					}
				}
				if (next_edge == edges.size()) break;
				current = next_edge;
			}
			if (contour.size() < 4 || std::abs(polygon_area(contour)) < 1.5) continue;
			auto simplified = simplify_closed(std::move(contour), simplify_epsilon);
			if (simplified.size() >= 3) contours.push_back(std::move(simplified));
		}
		return contours;
	}

	struct RasterContour {
		bool add = true;
		std::vector<Vector2D> points;
	};

	struct RasterizedSemanticMask {
		std::vector<uint32_t> owners;
		std::vector<unsigned char> add_union;
		std::vector<unsigned char> subtract;
	};

	void apply_raster_polygon(std::vector<unsigned char>& mask, int width, int height,
		std::vector<Vector2D> const& polygon, unsigned char value) {
		if (polygon.size() < 3) return;
		float minimum_y = polygon.front().Y();
		float maximum_y = minimum_y;
		for (auto const& point : polygon) {
			minimum_y = std::min(minimum_y, point.Y());
			maximum_y = std::max(maximum_y, point.Y());
		}
		int first_y = std::max(0, static_cast<int>(std::ceil(minimum_y - .5f)));
		int last_y = std::min(height - 1, static_cast<int>(std::floor(maximum_y - .5f)));
		std::vector<float> crossings;
		for (int y = first_y; y <= last_y; ++y) {
			float scan_y = y + .5f;
			crossings.clear();
			for (size_t i = 0, previous = polygon.size() - 1; i < polygon.size(); previous = i++) {
				auto const& a = polygon[previous];
				auto const& b = polygon[i];
				if ((a.Y() > scan_y) == (b.Y() > scan_y)) continue;
				crossings.push_back(a.X() + (scan_y - a.Y()) *
					(b.X() - a.X()) / (b.Y() - a.Y()));
			}
			std::sort(crossings.begin(), crossings.end());
			for (size_t i = 1; i < crossings.size(); i += 2) {
				int first_x = std::max(0, static_cast<int>(std::ceil(crossings[i - 1] - .5f)));
				int last_x = std::min(width - 1, static_cast<int>(std::floor(crossings[i] - .5f)));
				for (int x = first_x; x <= last_x; ++x)
					mask[static_cast<size_t>(y) * width + x] = value;
			}
		}
	}

	void apply_raster_polygon_owner(std::vector<uint32_t>& owners, int width, int height,
		std::vector<Vector2D> const& polygon, uint32_t owner) {
		if (polygon.size() < 3 || !owner) return;
		float minimum_y = polygon.front().Y();
		float maximum_y = minimum_y;
		for (auto const& point : polygon) {
			minimum_y = std::min(minimum_y, point.Y());
			maximum_y = std::max(maximum_y, point.Y());
		}
		int first_y = std::max(0, static_cast<int>(std::ceil(minimum_y - .5f)));
		int last_y = std::min(height - 1, static_cast<int>(std::floor(maximum_y - .5f)));
		std::vector<float> crossings;
		for (int y = first_y; y <= last_y; ++y) {
			float scan_y = y + .5f;
			crossings.clear();
			for (size_t i = 0, previous = polygon.size() - 1; i < polygon.size(); previous = i++) {
				auto const& a = polygon[previous];
				auto const& b = polygon[i];
				if ((a.Y() > scan_y) == (b.Y() > scan_y)) continue;
				crossings.push_back(a.X() + (scan_y - a.Y()) *
					(b.X() - a.X()) / (b.Y() - a.Y()));
			}
			std::sort(crossings.begin(), crossings.end());
			for (size_t i = 1; i < crossings.size(); i += 2) {
				int first_x = std::max(0, static_cast<int>(std::ceil(crossings[i - 1] - .5f)));
				int last_x = std::min(width - 1, static_cast<int>(std::floor(crossings[i] - .5f)));
				for (int x = first_x; x <= last_x; ++x)
					owners[static_cast<size_t>(y) * width + x] |= owner;
			}
		}
	}

	RasterizedSemanticMask rasterize_contour_operations(
		std::vector<RasterContour> const& contours, int width, int height) {
		RasterizedSemanticMask result;
		result.owners.resize(static_cast<size_t>(width) * height);
		result.add_union.resize(result.owners.size());
		result.subtract.resize(result.owners.size());
		size_t addition_index = 0;
		for (auto const& contour : contours) {
			if (!contour.add) continue;
			uint32_t owner = uint32_t{1} << std::min<size_t>(addition_index, 31);
			apply_raster_polygon_owner(result.owners, width, height, contour.points, owner);
			++addition_index;
		}
		for (size_t i = 0; i < result.owners.size(); ++i)
			result.add_union[i] = result.owners[i] != 0;
		for (auto const& contour : contours)
			if (!contour.add)
				apply_raster_polygon(result.subtract, width, height, contour.points, 1);
		return result;
	}

	struct BlurredPixel {
		float red = 0.f;
		float green = 0.f;
		float blue = 0.f;
	};

	std::vector<BlurredPixel> blur_selection_image(wxImage const& image) {
		int width = image.GetWidth();
		int height = image.GetHeight();
		auto source = image.GetData();
		std::vector<BlurredPixel> horizontal(static_cast<size_t>(width) * height);
		std::vector<BlurredPixel> result(horizontal.size());
		auto source_channel = [&](int x, int y, int channel) {
			x = std::clamp(x, 0, width - 1);
			y = std::clamp(y, 0, height - 1);
			return static_cast<float>(source[(static_cast<size_t>(y) * width + x) * 3 + channel]);
		};
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				auto& pixel = horizontal[static_cast<size_t>(y) * width + x];
				pixel.red = (source_channel(x - 1, y, 0) + source_channel(x, y, 0) * 2.f +
					source_channel(x + 1, y, 0)) * .25f;
				pixel.green = (source_channel(x - 1, y, 1) + source_channel(x, y, 1) * 2.f +
					source_channel(x + 1, y, 1)) * .25f;
				pixel.blue = (source_channel(x - 1, y, 2) + source_channel(x, y, 2) * 2.f +
					source_channel(x + 1, y, 2)) * .25f;
			}
		}
		auto horizontal_at = [&](int x, int y) -> BlurredPixel const& {
			y = std::clamp(y, 0, height - 1);
			return horizontal[static_cast<size_t>(y) * width + x];
		};
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				auto const& above = horizontal_at(x, y - 1);
				auto const& centre = horizontal_at(x, y);
				auto const& below = horizontal_at(x, y + 1);
				auto& pixel = result[static_cast<size_t>(y) * width + x];
				pixel.red = (above.red + centre.red * 2.f + below.red) * .25f;
				pixel.green = (above.green + centre.green * 2.f + below.green) * .25f;
				pixel.blue = (above.blue + centre.blue * 2.f + below.blue) * .25f;
			}
		}
		return result;
	}

	int colour_bin(BlurredPixel const& pixel) {
		int red = std::clamp(static_cast<int>(pixel.red), 0, 255) >> 4;
		int green = std::clamp(static_cast<int>(pixel.green), 0, 255) >> 4;
		int blue = std::clamp(static_cast<int>(pixel.blue), 0, 255) >> 4;
		return (red << 8) | (green << 4) | blue;
	}

	float colour_edge(BlurredPixel const& a, BlurredPixel const& b) {
		float red = a.red - b.red;
		float green = a.green - b.green;
		float blue = a.blue - b.blue;
		return std::sqrt(red * red * .25f + green * green * .5f + blue * blue * .25f);
	}

	std::vector<unsigned short> inside_boundary_distance(
		std::vector<unsigned char> const& mask, int width, int height) {
		std::vector<unsigned short> distance(mask.size(), 0);
		std::deque<size_t> queue;
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				size_t index = static_cast<size_t>(y) * width + x;
				if (!mask[index]) continue;
				bool boundary = x == 0 || y == 0 || x + 1 == width || y + 1 == height;
				for (auto [nx, ny] : {std::pair{x - 1, y}, {x + 1, y},
					{ x, y - 1 }, { x, y + 1 }}) {
					if (nx >= 0 && ny >= 0 && nx < width && ny < height &&
						!mask[static_cast<size_t>(ny) * width + nx])
						boundary = true;
				}
				if (boundary) {
					distance[index] = 1;
					queue.push_back(index);
				}
			}
		}
		while (!queue.empty()) {
			size_t current = queue.front();
			queue.pop_front();
			int x = static_cast<int>(current % width);
			int y = static_cast<int>(current / width);
			for (auto [nx, ny] : {std::pair{x - 1, y}, {x + 1, y},
				{ x, y - 1 }, { x, y + 1 }}) {
				if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
				size_t next = static_cast<size_t>(ny) * width + nx;
				if (!mask[next] || distance[next]) continue;
				distance[next] = static_cast<unsigned short>(
					std::min<int>(distance[current] + 1, std::numeric_limits<unsigned short>::max()));
				queue.push_back(next);
			}
		}
		return distance;
	}

	std::vector<unsigned char> exterior_background(
		std::vector<unsigned char> const& mask, int width, int height) {
		std::vector<unsigned char> exterior(mask.size(), 0);
		std::deque<size_t> queue;
		auto enqueue = [&](int x, int y) {
			if (x < 0 || y < 0 || x >= width || y >= height) return;
			size_t index = static_cast<size_t>(y) * width + x;
			if (mask[index] || exterior[index]) return;
			exterior[index] = 1;
			queue.push_back(index);
		};
		for (int x = 0; x < width; ++x) {
			enqueue(x, 0);
			enqueue(x, height - 1);
		}
		for (int y = 0; y < height; ++y) {
			enqueue(0, y);
			enqueue(width - 1, y);
		}
		while (!queue.empty()) {
			size_t current = queue.front();
			queue.pop_front();
			int x = static_cast<int>(current % width);
			int y = static_cast<int>(current / width);
			enqueue(x - 1, y);
			enqueue(x + 1, y);
			enqueue(x, y - 1);
			enqueue(x, y + 1);
		}
		return exterior;
	}

	std::vector<unsigned short> distance_from_mask(
		std::vector<unsigned char> const& mask, int width, int height) {
		constexpr auto infinity = std::numeric_limits<unsigned short>::max();
		std::vector<unsigned short> distance(mask.size(), infinity);
		std::deque<size_t> queue;
		for (size_t i = 0; i < mask.size(); ++i) {
			if (!mask[i]) continue;
			distance[i] = 0;
			queue.push_back(i);
		}
		while (!queue.empty()) {
			size_t current = queue.front();
			queue.pop_front();
			int x = static_cast<int>(current % width);
			int y = static_cast<int>(current / width);
			unsigned short next_distance = static_cast<unsigned short>(
				std::min<int>(distance[current] + 1, infinity));
			for (auto [nx, ny] : {std::pair{x - 1, y}, {x + 1, y},
				{ x, y - 1 }, { x, y + 1 }}) {
				if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
				size_t next = static_cast<size_t>(ny) * width + nx;
				if (distance[next] <= next_distance) continue;
				distance[next] = next_distance;
				queue.push_back(next);
			}
		}
		return distance;
	}

	int owner_count(uint32_t owners) {
		int count = 0;
		while (owners) {
			owners &= owners - 1;
			++count;
		}
		return count;
	}

	float edge_percentile(std::vector<BlurredPixel> const& pixels,
		int width, int height, double percentile) {
		std::array<size_t, 256> histogram{};
		size_t total = 0;
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				size_t index = static_cast<size_t>(y) * width + x;
				if (x + 1 < width) {
					int value = std::clamp(static_cast<int>(std::lround(
						colour_edge(pixels[index], pixels[index + 1]))), 0, 255);
					++histogram[value];
					++total;
				}
				if (y + 1 < height) {
					int value = std::clamp(static_cast<int>(std::lround(
						colour_edge(pixels[index], pixels[index + width]))), 0, 255);
					++histogram[value];
					++total;
				}
			}
		}
		if (!total) return 0.f;
		size_t target = static_cast<size_t>(std::clamp(percentile, 0.0, 1.0) * (total - 1));
		size_t accumulated = 0;
		for (size_t value = 0; value < histogram.size(); ++value) {
			accumulated += histogram[value];
			if (accumulated > target) return static_cast<float>(value);
		}
		return 255.f;
	}

	using SemanticFeature = std::array<float, 5>;

	double srgb_to_linear(double value) {
		value /= 255.0;
		return value <= .04045 ? value / 12.92 :
			std::pow((value + .055) / 1.055, 2.4);
	}

	SemanticFeature make_semantic_feature(unsigned char const* image_data,
		int width, int height, int index, double xy_scale) {
		auto pixel = image_data + static_cast<size_t>(index) * 3;
		double red = srgb_to_linear(pixel[0]);
		double green = srgb_to_linear(pixel[1]);
		double blue = srgb_to_linear(pixel[2]);
		double x = (0.4124564 * red + 0.3575761 * green + 0.1804375 * blue) / 0.95047;
		double y = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue;
		double z = (0.0193339 * red + 0.1191920 * green + 0.9503041 * blue) / 1.08883;
		auto pivot = [](double value) {
			return value > .008856451679 ? std::cbrt(value) :
				7.787037037 * value + 16.0 / 116.0;
		};
		double fx = pivot(x);
		double fy = pivot(y);
		double fz = pivot(z);
		int pixel_x = index % width;
		int pixel_y = index / width;
		return {
			static_cast<float>(116.0 * fy - 16.0),
			static_cast<float>(500.0 * (fx - fy)),
			static_cast<float>(200.0 * (fy - fz)),
			static_cast<float>(xy_scale * (pixel_x + .5) / width),
			static_cast<float>(xy_scale * (pixel_y + .5) / height)
		};
	}

	double semantic_colour_distance_squared(SemanticFeature const& first,
		SemanticFeature const& second) {
		double result = 0.0;
		for (int channel = 0; channel < 3; ++channel) {
			double difference = first[channel] - second[channel];
			result += difference * difference;
		}
		return result;
	}

	struct SemanticGaussian {
		std::array<double, 5> mean{};
		std::array<double, 5> variance{};
		double prior = 0.0;
	};

	struct SemanticGmm {
		std::vector<SemanticGaussian> components;

		double negative_log_likelihood(SemanticFeature const& sample) const {
			double maximum = -std::numeric_limits<double>::infinity();
			std::array<double, 24> terms{};
			size_t count = 0;
			for (auto const& component : components) {
				if (component.prior <= 0.0) continue;
				double value = std::log(component.prior);
				for (int dimension = 0; dimension < 5; ++dimension) {
					double variance = component.variance[dimension];
					double difference = sample[dimension] - component.mean[dimension];
					value -= .5 * (std::log(2.0 * 3.14159265358979323846 * variance) +
						difference * difference / variance);
				}
				terms[count++] = value;
				maximum = std::max(maximum, value);
			}
			if (!count) return 1e6;
			double sum = 0.0;
			for (size_t i = 0; i < count; ++i)
				sum += std::exp(terms[i] - maximum);
			return -(maximum + std::log(sum));
		}
	};

	SemanticGmm fit_semantic_gmm(std::vector<SemanticFeature> const& features,
		std::vector<int> samples, int requested_components) {
		// Dense crops contain large runs of almost identical anime colours. A
		// deterministic 65k sample preserves those distributions while avoiding
		// needless k-means work on every repeated pixel.
		constexpr size_t maximum_samples = 65000;
		if (samples.size() > maximum_samples) {
			std::vector<int> reduced;
			reduced.reserve(maximum_samples);
			double step = static_cast<double>(samples.size()) / maximum_samples;
			for (size_t i = 0; i < maximum_samples; ++i)
				reduced.push_back(samples[static_cast<size_t>(i * step)]);
			samples = std::move(reduced);
		}
		if (samples.empty()) return {};
		int component_count = std::min<int>(requested_components,
			static_cast<int>(samples.size()));
		std::vector<std::array<double, 5>> means;
		means.reserve(component_count);
		std::array<double, 5> global{};
		for (int index : samples)
			for (int dimension = 0; dimension < 5; ++dimension)
				global[dimension] += features[index][dimension];
		for (double& value : global) value /= samples.size();
		auto squared = [](auto const& first, auto const& second) {
			double value = 0.0;
			for (int dimension = 0; dimension < 5; ++dimension) {
				double difference = first[dimension] - second[dimension];
				value += difference * difference;
			}
			return value;
		};
		int first_mean = *std::max_element(samples.begin(), samples.end(),
			[&](int first, int second) {
				return squared(features[first], global) < squared(features[second], global);
			});
		std::array<double, 5> first{};
		std::copy(features[first_mean].begin(), features[first_mean].end(), first.begin());
		means.push_back(first);
		std::vector<double> nearest(samples.size(), std::numeric_limits<double>::infinity());
		while (static_cast<int>(means.size()) < component_count) {
			size_t best = 0;
			for (size_t i = 0; i < samples.size(); ++i) {
				nearest[i] = std::min(nearest[i], squared(features[samples[i]], means.back()));
				if (nearest[i] > nearest[best]) best = i;
			}
			std::array<double, 5> mean{};
			std::copy(features[samples[best]].begin(), features[samples[best]].end(), mean.begin());
			means.push_back(mean);
		}

		std::vector<int> assignment(samples.size());
		for (int iteration = 0; iteration < 6; ++iteration) {
			std::vector<std::array<double, 5>> sums(component_count);
			std::vector<size_t> counts(component_count);
			for (size_t i = 0; i < samples.size(); ++i) {
				int best = 0;
				double best_distance = squared(features[samples[i]], means[0]);
				for (int component = 1; component < component_count; ++component) {
					double distance = squared(features[samples[i]], means[component]);
					if (distance < best_distance) {
						best = component;
						best_distance = distance;
					}
				}
				assignment[i] = best;
				++counts[best];
				for (int dimension = 0; dimension < 5; ++dimension)
					sums[best][dimension] += features[samples[i]][dimension];
			}
			for (int component = 0; component < component_count; ++component) {
				if (!counts[component]) continue;
				for (int dimension = 0; dimension < 5; ++dimension)
					means[component][dimension] = sums[component][dimension] / counts[component];
			}
		}

		std::vector<SemanticGaussian> result(component_count);
		std::vector<size_t> counts(component_count);
		for (size_t i = 0; i < samples.size(); ++i) {
			int component = assignment[i];
			++counts[component];
			for (int dimension = 0; dimension < 5; ++dimension) {
				double value = features[samples[i]][dimension];
				result[component].mean[dimension] += value;
				result[component].variance[dimension] += value * value;
			}
		}
		for (int component = 0; component < component_count; ++component) {
			if (!counts[component]) continue;
			result[component].prior = static_cast<double>(counts[component]) / samples.size();
			for (int dimension = 0; dimension < 5; ++dimension) {
				result[component].mean[dimension] /= counts[component];
				result[component].variance[dimension] =
					result[component].variance[dimension] / counts[component] -
					result[component].mean[dimension] * result[component].mean[dimension];
				double floor = dimension < 3 ? 9.0 : 2.25;
				result[component].variance[dimension] =
					std::max(result[component].variance[dimension], floor);
			}
		}
		result.erase(std::remove_if(result.begin(), result.end(),
			[](SemanticGaussian const& value) { return value.prior == 0.0; }), result.end());
		return {std::move(result)};
	}

	// Distance is measured only from an actual foreground/background transition.
	// The edge of the crop is deliberately not treated as background: a subject
	// which is legitimately cut by the selected range must remain locked to it.
	std::vector<unsigned short> semantic_boundary_distance(
		std::vector<unsigned char> const& mask, int width, int height) {
		constexpr auto infinity = std::numeric_limits<unsigned short>::max();
		std::vector<unsigned short> distance(mask.size(), infinity);
		std::deque<int> queue;
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				int index = y * width + x;
				bool boundary = (x && mask[index - 1] != mask[index]) ||
					(x + 1 < width && mask[index + 1] != mask[index]) ||
					(y && mask[index - width] != mask[index]) ||
					(y + 1 < height && mask[index + width] != mask[index]);
				if (!boundary) continue;
				distance[index] = 0;
				queue.push_back(index);
			}
		}
		while (!queue.empty()) {
			int index = queue.front();
			queue.pop_front();
			int x = index % width;
			int y = index / width;
			unsigned short next_distance = static_cast<unsigned short>(
				std::min<int>(distance[index] + 1, infinity));
			for (auto [next_x, next_y] : {std::pair{x - 1, y}, {x + 1, y},
				{x, y - 1}, {x, y + 1}}) {
				if (next_x < 0 || next_y < 0 || next_x >= width || next_y >= height) continue;
				int next = next_y * width + next_x;
				if (distance[next] <= next_distance) continue;
				distance[next] = next_distance;
				queue.push_back(next);
			}
		}
		return distance;
	}

	void nearest_semantic_seed(std::vector<unsigned char> const& hard_label,
		unsigned char wanted, int width, int height, std::vector<int>& nearest) {
		constexpr auto infinity = std::numeric_limits<unsigned short>::max();
		nearest.assign(hard_label.size(), -1);
		std::vector<unsigned short> distance(hard_label.size(), infinity);
		std::deque<int> queue;
		for (int index = 0; index < static_cast<int>(hard_label.size()); ++index) {
			if (hard_label[index] != wanted) continue;
			nearest[index] = index;
			distance[index] = 0;
			queue.push_back(index);
		}
		while (!queue.empty()) {
			int index = queue.front();
			queue.pop_front();
			int x = index % width;
			int y = index / width;
			unsigned short next_distance = static_cast<unsigned short>(
				std::min<int>(distance[index] + 1, infinity));
			for (auto [next_x, next_y] : {std::pair{x - 1, y}, {x + 1, y},
				{x, y - 1}, {x, y + 1}}) {
				if (next_x < 0 || next_y < 0 || next_x >= width || next_y >= height) continue;
				int next = next_y * width + next_x;
				if (distance[next] <= next_distance) continue;
				distance[next] = next_distance;
				nearest[next] = nearest[index];
				queue.push_back(next);
			}
		}
	}

	double semantic_softplus(double value) {
		if (value > 30.0) return value;
		if (value < -30.0) return std::exp(value);
		return std::log1p(std::exp(value));
	}

	std::vector<unsigned char> graph_cut_semantic_mask(wxImage const& image,
		RasterizedSemanticMask const& semantic,
		std::vector<unsigned char> const& accepted_subtract) try {
		constexpr int initial_band = 30;
		constexpr int minimum_band = 8;
		constexpr int maximum_unknown_nodes = 160000;
		constexpr int gmm_components = 12;
		constexpr double xy_scale = 38.0;
		constexpr double global_weight = .55;
		constexpr double local_weight = 1.0;
		constexpr double local_temperature = 22.0;
		constexpr double shape_weight = 1.4;
		constexpr double shape_sigma = 10.0;
		constexpr double smoothness = 14.0;
		constexpr double smooth_floor = .12;

		int width = image.GetWidth();
		int height = image.GetHeight();
		size_t pixel_count = static_cast<size_t>(width) * height;
		if (!image.IsOk() || !image.GetData() || width < 3 || height < 3 ||
			pixel_count > static_cast<size_t>(std::numeric_limits<int>::max()) ||
			semantic.add_union.size() != pixel_count || accepted_subtract.size() != pixel_count)
			return {};

		std::vector<unsigned char> coarse = semantic.add_union;
		for (size_t i = 0; i < pixel_count; ++i)
			if (accepted_subtract[i]) coarse[i] = 0;
		auto boundary_distance = semantic_boundary_distance(coarse, width, height);
		std::vector<unsigned char> hard(pixel_count);
		std::vector<int> foreground_samples;
		std::vector<int> background_samples;
		int unknown_count = 0;
		int band = initial_band;
		for (;;) {
			foreground_samples.clear();
			background_samples.clear();
			unknown_count = 0;
			for (int index = 0; index < static_cast<int>(pixel_count); ++index) {
				if (accepted_subtract[index]) hard[index] = 2;
				else if (boundary_distance[index] >= band)
					hard[index] = coarse[index] ? 1 : 2;
				else {
					hard[index] = 0;
					++unknown_count;
				}
				if (hard[index] == 1) foreground_samples.push_back(index);
				else if (hard[index] == 2) background_samples.push_back(index);
			}
			if (unknown_count <= maximum_unknown_nodes && foreground_samples.size() >= 64 &&
				background_samples.size() >= 64)
				break;
			if (band == minimum_band) return {};
			band = std::max(minimum_band, band - std::max(2, band / 5));
		}
		if (!unknown_count) return coarse;

		std::vector<SemanticFeature> features(pixel_count);
		auto image_data = image.GetData();
		for (int index = 0; index < static_cast<int>(pixel_count); ++index)
			features[index] = make_semantic_feature(image_data, width, height, index, xy_scale);
		auto foreground = fit_semantic_gmm(features, std::move(foreground_samples), gmm_components);
		auto background = fit_semantic_gmm(features, std::move(background_samples), gmm_components);
		if (foreground.components.empty() || background.components.empty()) return {};

		std::vector<int> nearest_foreground;
		std::vector<int> nearest_background;
		nearest_semantic_seed(hard, 1, width, height, nearest_foreground);
		nearest_semantic_seed(hard, 2, width, height, nearest_background);
		std::vector<int> node(pixel_count, -1);
		int next_node = 0;
		for (int index = 0; index < static_cast<int>(pixel_count); ++index)
			if (!hard[index]) node[index] = next_node++;

		using Traits = boost::adjacency_list_traits<boost::vecS, boost::vecS,
			boost::directedS>;
		using EdgeProperties = boost::property<boost::edge_capacity_t, double,
			boost::property<boost::edge_residual_capacity_t, double,
			boost::property<boost::edge_reverse_t, Traits::edge_descriptor>>>;
		using Graph = boost::adjacency_list<boost::vecS, boost::vecS,
			boost::directedS, boost::no_property, EdgeProperties>;
		using Vertex = Traits::vertex_descriptor;
		Graph graph(static_cast<size_t>(unknown_count) + 2);
		Vertex source = static_cast<Vertex>(unknown_count);
		Vertex sink = static_cast<Vertex>(unknown_count + 1);
		auto capacity = get(boost::edge_capacity, graph);
		auto residual = get(boost::edge_residual_capacity, graph);
		auto reverse = get(boost::edge_reverse, graph);
		auto add_pair = [&](Vertex from, Vertex to, double forward, double backward) {
			auto first = add_edge(from, to, graph).first;
			auto second = add_edge(to, from, graph).first;
			capacity[first] = forward;
			capacity[second] = backward;
			residual[first] = 0.0;
			residual[second] = 0.0;
			reverse[first] = second;
			reverse[second] = first;
		};

		double sum_difference = 0.0;
		size_t difference_count = 0;
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				int index = y * width + x;
				if (x + 1 < width) {
					sum_difference += semantic_colour_distance_squared(features[index], features[index + 1]);
					++difference_count;
				}
				if (y + 1 < height) {
					sum_difference += semantic_colour_distance_squared(features[index], features[index + width]);
					++difference_count;
				}
			}
		}
		double beta = difference_count && sum_difference > 0.0 ?
			difference_count / (2.0 * sum_difference) : 0.0;
		auto pairwise = [&](int first, int second) {
			return smooth_floor + smoothness * std::exp(-beta *
				semantic_colour_distance_squared(features[first], features[second]));
		};

		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				int index = y * width + x;
				if (hard[index]) continue;
				double global_logit = background.negative_log_likelihood(features[index]) -
					foreground.negative_log_likelihood(features[index]);
				global_logit = std::clamp(global_logit / 8.0, -8.0, 8.0);
				double local_logit = 0.0;
				if (nearest_foreground[index] >= 0 && nearest_background[index] >= 0) {
					double foreground_distance = std::sqrt(semantic_colour_distance_squared(
						features[index], features[nearest_foreground[index]]));
					double background_distance = std::sqrt(semantic_colour_distance_squared(
						features[index], features[nearest_background[index]]));
					local_logit = std::clamp((background_distance - foreground_distance) /
						local_temperature, -6.0, 6.0);
				}
				double signed_distance = (coarse[index] ? 1.0 : -1.0) *
					(boundary_distance[index] + .5);
				double logit = global_weight * global_logit + local_weight * local_logit +
					shape_weight * signed_distance / shape_sigma;
				double foreground_cost = semantic_softplus(-logit);
				double background_cost = semantic_softplus(logit);
				for (auto [next_x, next_y] : {std::pair{x - 1, y}, {x + 1, y},
					{x, y - 1}, {x, y + 1}}) {
					if (next_x < 0 || next_y < 0 || next_x >= width || next_y >= height) continue;
					int next = next_y * width + next_x;
					if (!hard[next]) continue;
					double weight = pairwise(index, next);
					if (hard[next] == 1) background_cost += weight;
					else foreground_cost += weight;
				}
				Vertex vertex = static_cast<Vertex>(node[index]);
				add_pair(source, vertex, background_cost, 0.0);
				add_pair(vertex, sink, foreground_cost, 0.0);
				if (x + 1 < width && !hard[index + 1]) {
					double weight = pairwise(index, index + 1);
					add_pair(vertex, static_cast<Vertex>(node[index + 1]), weight, weight);
				}
				if (y + 1 < height && !hard[index + width]) {
					double weight = pairwise(index, index + width);
					add_pair(vertex, static_cast<Vertex>(node[index + width]), weight, weight);
				}
			}
		}

		std::vector<boost::default_color_type> colours(num_vertices(graph));
		auto index_map = get(boost::vertex_index, graph);
		auto colour_map = boost::make_iterator_property_map(colours.begin(), index_map);
		boost::boykov_kolmogorov_max_flow(graph, capacity, residual, reverse,
			colour_map, index_map, source, sink);
		auto foreground_colour = boost::color_traits<boost::default_color_type>::black();
		std::vector<unsigned char> result(pixel_count);
		for (int index = 0; index < static_cast<int>(pixel_count); ++index) {
			bool selected = hard[index] == 1;
			if (!hard[index])
				selected = colours[static_cast<Vertex>(node[index])] == foreground_colour;
			result[index] = selected && !accepted_subtract[index];
		}
		return result;
	}
	catch (...) {
		// The legacy refinement below is intentionally retained as a safe fallback
		// for an allocation failure or an unexpectedly degenerate graph.
		return {};
	}

	std::vector<unsigned char> refine_semantic_mask(wxImage const& image,
		RasterizedSemanticMask const& semantic,
		std::vector<unsigned char>& accepted_subtract) {
		int width = image.GetWidth();
		int height = image.GetHeight();
		auto const& coarse_mask = semantic.add_union;
		accepted_subtract.assign(coarse_mask.size(), 0);
		if (!image.IsOk() || !image.GetData() || width < 3 || height < 3 ||
			coarse_mask.size() != static_cast<size_t>(width) * height ||
			semantic.owners.size() != coarse_mask.size() ||
			semantic.subtract.size() != coarse_mask.size())
			return coarse_mask;

		size_t selected_count = static_cast<size_t>(std::count(
			coarse_mask.begin(), coarse_mask.end(), static_cast<unsigned char>(1)));
		if (selected_count < 16)
			return coarse_mask;

		auto blurred = blur_selection_image(image);
		auto boundary_distance = inside_boundary_distance(coarse_mask, width, height);
		auto exterior = exterior_background(coarse_mask, width, height);
		auto distance_from_subject = distance_from_mask(coarse_mask, width, height);
		int core_distance = std::clamp(std::min(width, height) / 100, 3, 10);
		std::array<double, 4096> foreground_histogram{};
		std::array<double, 4096> background_histogram{};
		double foreground_total = 0.0;
		double background_total = 0.0;
		for (size_t i = 0; i < coarse_mask.size(); ++i) {
			int bin = colour_bin(blurred[i]);
			if (exterior[i] && distance_from_subject[i] >= 3) {
				background_histogram[bin] += 1.0;
				background_total += 1.0;
			}
			else if (boundary_distance[i] >= core_distance) {
				foreground_histogram[bin] += 1.0;
				foreground_total += 1.0;
			}
		}
		if (background_total < 64.0) {
			background_histogram.fill(0.0);
			background_total = 0.0;
			for (size_t i = 0; i < coarse_mask.size(); ++i) {
				if (!exterior[i]) continue;
				background_histogram[colour_bin(blurred[i])] += 1.0;
				background_total += 1.0;
			}
		}
		if (foreground_total < 64.0) {
			foreground_histogram.fill(0.0);
			foreground_total = 0.0;
			for (size_t i = 0; i < coarse_mask.size(); ++i) {
				if (!coarse_mask[i]) continue;
				foreground_histogram[colour_bin(blurred[i])] += 1.0;
				foreground_total += 1.0;
			}
		}

		constexpr double smoothing = .5;
		constexpr double bins = 4096.0;
		std::array<float, 4096> background_log_odds{};
		bool has_colour_model = foreground_total >= 16.0 && background_total >= 16.0;
		if (has_colour_model) {
			for (size_t i = 0; i < background_log_odds.size(); ++i) {
				double foreground_probability = (foreground_histogram[i] + smoothing) /
					(foreground_total + smoothing * bins);
				double background_probability = (background_histogram[i] + smoothing) /
					(background_total + smoothing * bins);
				background_log_odds[i] = static_cast<float>(
					std::log(background_probability / foreground_probability));
			}
		}

		float ordinary_edge = edge_percentile(blurred, width, height, .72);
		float soft_edge_limit = std::clamp(ordinary_edge * 1.6f + 3.f, 10.f, 28.f);
		float hard_edge_limit = std::clamp(soft_edge_limit * 1.8f, 20.f, 52.f);
		float supported_edge_limit = std::clamp(ordinary_edge * 1.6f + 3.f, 14.f, 44.f);
		std::vector<unsigned char> carved(coarse_mask.size(), 0);
		std::deque<size_t> queue;
		auto try_carve = [&](size_t from, int nx, int ny) {
			if (nx < 0 || ny < 0 || nx >= width || ny >= height) return;
			size_t next = static_cast<size_t>(ny) * width + nx;
			if (!coarse_mask[next] || carved[next]) return;
			float odds = background_log_odds[colour_bin(blurred[next])];
			float depth = static_cast<float>(boundary_distance[next]);
			float required_odds = depth <= 2.f ? -.1f :
				.2f + std::min(.9f, (depth - 2.f) * .04f);
			float edge = colour_edge(blurred[from], blurred[next]);
			bool clearly_background = odds >= required_odds + .7f && edge <= hard_edge_limit;
			bool compatible_background = odds >= required_odds && edge <= soft_edge_limit;
			if (!clearly_background && !compatible_background) return;
			carved[next] = 1;
			queue.push_back(next);
		};

		// Only crop-edge-connected background may seed carving. Internal gaps and
		// explicit subtract regions must never teach the model that subject pixels
		// are background or grow into false seams.
		if (has_colour_model) {
			for (int y = 0; y < height; ++y) {
				for (int x = 0; x < width; ++x) {
					size_t index = static_cast<size_t>(y) * width + x;
					if (!exterior[index]) continue;
					try_carve(index, x - 1, y);
					try_carve(index, x + 1, y);
					try_carve(index, x, y - 1);
					try_carve(index, x, y + 1);
				}
			}
			while (!queue.empty()) {
				size_t current = queue.front();
				queue.pop_front();
				int x = static_cast<int>(current % width);
				int y = static_cast<int>(current / width);
				try_carve(current, x - 1, y);
				try_carve(current, x + 1, y);
				try_carve(current, x, y - 1);
				try_carve(current, x, y + 1);
			}
		}

		std::vector<unsigned char> refined = coarse_mask;
		for (size_t i = 0; i < refined.size(); ++i)
			if (carved[i]) refined[i] = 0;

		size_t refined_count = static_cast<size_t>(std::count(
			refined.begin(), refined.end(), static_cast<unsigned char>(1)));
		if (refined_count < std::max<size_t>(16, selected_count / 4))
			refined = coarse_mask;

		// Validate explicit subtract polygons as topology hints rather than
		// blindly cutting every AI-produced interior detail out of the subject.
		auto refined_distance = distance_from_mask(refined, width, height);
		auto refined_exterior = exterior_background(refined, width, height);
		auto distance_from_exterior = distance_from_mask(refined_exterior, width, height);
		std::vector<unsigned char> visited(semantic.subtract.size(), 0);
		for (size_t seed = 0; seed < semantic.subtract.size(); ++seed) {
			if (!semantic.subtract[seed] || visited[seed]) continue;
			std::vector<size_t> component;
			queue.clear();
			queue.push_back(seed);
			visited[seed] = 1;
			while (!queue.empty()) {
				size_t current = queue.front();
				queue.pop_front();
				component.push_back(current);
				int x = static_cast<int>(current % width);
				int y = static_cast<int>(current / width);
				for (auto [nx, ny] : {std::pair{x - 1, y}, {x + 1, y},
					{ x, y - 1 }, { x, y + 1 }}) {
					if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
					size_t next = static_cast<size_t>(ny) * width + nx;
					if (!semantic.subtract[next] || visited[next]) continue;
					visited[next] = 1;
					queue.push_back(next);
				}
			}

			size_t relevant = 0;
			size_t background_pixels = 0;
			size_t foreground_pixels = 0;
			size_t boundary_edges = 0;
			size_t supported_edges = 0;
			bool near_exterior = false;
			for (size_t index : component) {
				if (refined_distance[index] > 2) continue;
				++relevant;
				if (has_colour_model) {
					float odds = background_log_odds[colour_bin(blurred[index])];
					background_pixels += odds > .45f;
					foreground_pixels += odds < -.35f;
				}
				near_exterior = near_exterior || distance_from_exterior[index] <= 2;
				int x = static_cast<int>(index % width);
				int y = static_cast<int>(index / width);
				for (auto [nx, ny] : {std::pair{x - 1, y}, {x + 1, y},
					{ x, y - 1 }, { x, y + 1 }}) {
					if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
					size_t next = static_cast<size_t>(ny) * width + nx;
					if (semantic.subtract[next] || !refined[next]) continue;
					++boundary_edges;
					supported_edges += colour_edge(blurred[index], blurred[next]) >= supported_edge_limit;
				}
			}
			if (relevant < 6) continue;
			double background_ratio = static_cast<double>(background_pixels) / relevant;
			double foreground_ratio = static_cast<double>(foreground_pixels) / relevant;
			double edge_ratio = boundary_edges ?
				static_cast<double>(supported_edges) / boundary_edges : 0.0;
			bool accept = !has_colour_model || background_ratio >= .55 ||
				(background_ratio >= .35 && edge_ratio >= .45) ||
				(near_exterior && background_ratio >= .25);
			if (has_colour_model && foreground_ratio > .60 && edge_ratio < .60)
				accept = false;
			if (accept)
				for (size_t index : component) accepted_subtract[index] = 1;
		}

		// The semantic polygons remain the topology/ownership prior, while the
		// graph cut is allowed to replace their imprecise pixel boundary. Explicit
		// subtract regions have already been validated above and remain hard BG.
		// Run this before the legacy topology repair: that repair is retained only
		// as the low-confidence/allocation fallback and is otherwise wasted work.
		auto graph_cut = graph_cut_semantic_mask(image, semantic, accepted_subtract);
		if (graph_cut.size() == refined.size()) {
			size_t expected_count = 0;
			size_t graph_count = 0;
			size_t intersection = 0;
			size_t union_count = 0;
			bool subtract_preserved = true;
			for (size_t i = 0; i < graph_cut.size(); ++i) {
				bool expected = coarse_mask[i] && !accepted_subtract[i];
				expected_count += expected;
				graph_count += graph_cut[i] != 0;
				intersection += graph_cut[i] && expected;
				union_count += graph_cut[i] || expected;
				subtract_preserved = subtract_preserved &&
					(!accepted_subtract[i] || !graph_cut[i]);
			}
			double area_ratio = expected_count ?
				static_cast<double>(graph_count) / expected_count : 0.0;
			double coarse_iou = union_count ?
				static_cast<double>(intersection) / union_count : 0.0;
			if (graph_count && area_ratio >= .55 && area_ratio <= 1.45 &&
				coarse_iou >= .65 && subtract_preserved)
				return graph_cut;
		}

		// Expand compact owner bitsets into nearby background. A subsequent
		// erosion turns this into a true closing (rather than an exterior halo),
		// while accepted subtract regions remain impermeable barriers.
		std::vector<unsigned char> bridge_subject = refined;
		std::vector<uint32_t> reach = semantic.owners;
		for (size_t i = 0; i < reach.size(); ++i) {
			if (!bridge_subject[i] || accepted_subtract[i]) reach[i] = 0;
			if (accepted_subtract[i]) bridge_subject[i] = 0;
		}
		int bridge_radius = std::clamp(static_cast<int>(std::lround(
			std::min(width, height) / 160.0)), 2, 8);
		for (int pass = 0; pass < bridge_radius; ++pass) {
			auto next_reach = reach;
			for (int y = 0; y < height; ++y) {
				for (int x = 0; x < width; ++x) {
					size_t index = static_cast<size_t>(y) * width + x;
					if (bridge_subject[index] || accepted_subtract[index]) continue;
					uint32_t owners = reach[index];
					for (int dy = -1; dy <= 1; ++dy) {
						for (int dx = -1; dx <= 1; ++dx) {
							if ((!dx && !dy) || x + dx < 0 || y + dy < 0 ||
								x + dx >= width || y + dy >= height) continue;
							owners |= reach[static_cast<size_t>(y + dy) * width + x + dx];
						}
					}
					next_reach[index] = owners;
				}
			}
			reach.swap(next_reach);
		}
		std::vector<unsigned char> closed(reach.size());
		for (size_t i = 0; i < reach.size(); ++i) closed[i] = reach[i] != 0;
		for (int pass = 0; pass < bridge_radius; ++pass) {
			auto before = closed;
			for (int y = 0; y < height; ++y) {
				for (int x = 0; x < width; ++x) {
					size_t index = static_cast<size_t>(y) * width + x;
					if (!before[index]) continue;
					for (int dy = -1; dy <= 1 && closed[index]; ++dy) {
						for (int dx = -1; dx <= 1; ++dx) {
							if (!dx && !dy) continue;
							int nx = x + dx;
							int ny = y + dy;
							// Outside the crop is treated as selected so a legitimately
							// cropped subject is never pulled away from the range edge.
							if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
							if (!before[static_cast<size_t>(ny) * width + nx]) {
								closed[index] = 0;
								break;
							}
						}
					}
				}
			}
		}

		std::vector<unsigned char> bridge_candidate(closed.size(), 0);
		for (size_t i = 0; i < closed.size(); ++i)
			bridge_candidate[i] = closed[i] && !bridge_subject[i] &&
				!accepted_subtract[i] && owner_count(reach[i]) >= 2;
		visited.assign(bridge_candidate.size(), 0);
		for (size_t seed = 0; seed < bridge_candidate.size(); ++seed) {
			if (!bridge_candidate[seed] || visited[seed]) continue;
			std::vector<size_t> component;
			queue.clear();
			queue.push_back(seed);
			visited[seed] = 1;
			while (!queue.empty()) {
				size_t current = queue.front();
				queue.pop_front();
				component.push_back(current);
				int x = static_cast<int>(current % width);
				int y = static_cast<int>(current / width);
				for (auto [nx, ny] : {std::pair{x - 1, y}, {x + 1, y},
					{ x, y - 1 }, { x, y + 1 }}) {
					if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
					size_t next = static_cast<size_t>(ny) * width + nx;
					if (!bridge_candidate[next] || visited[next]) continue;
					visited[next] = 1;
					queue.push_back(next);
				}
			}

			size_t background_pixels = 0;
			size_t boundary_edges = 0;
			size_t supported_edges = 0;
			size_t protected_pixels = 0;
			for (size_t index : component) {
				if (has_colour_model)
					background_pixels += background_log_odds[colour_bin(blurred[index])] > .45f;
				int x = static_cast<int>(index % width);
				int y = static_cast<int>(index / width);
				bool protected_pixel = false;
				for (int dy = -1; dy <= 1; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						if (!dx && !dy) continue;
						int nx = x + dx;
						int ny = y + dy;
						if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
						size_t next = static_cast<size_t>(ny) * width + nx;
						protected_pixel = protected_pixel || accepted_subtract[next];
						if (std::abs(dx) + std::abs(dy) != 1 || !bridge_subject[next]) continue;
						++boundary_edges;
						supported_edges += colour_edge(blurred[index], blurred[next]) >= supported_edge_limit;
					}
				}
				protected_pixels += protected_pixel;
			}
			double background_ratio = static_cast<double>(background_pixels) / component.size();
			double edge_ratio = boundary_edges ?
				static_cast<double>(supported_edges) / boundary_edges : 0.0;
			double protected_ratio = static_cast<double>(protected_pixels) / component.size();
			bool preserve_gap = protected_ratio >= .15 ||
				(has_colour_model && background_ratio >= .55 && edge_ratio >= .35);
			if (!preserve_gap)
				for (size_t index : component)
					if (!accepted_subtract[index]) refined[index] = 1;
		}

		// Accepted subtract pixels are authoritative and applied after every
		// topology repair. A tiny isolated crack may be restored only elsewhere.
		for (size_t i = 0; i < refined.size(); ++i)
			if (accepted_subtract[i]) refined[i] = 0;
		auto before_cleanup = refined;
		for (int y = 1; y + 1 < height; ++y) {
			for (int x = 1; x + 1 < width; ++x) {
				size_t index = static_cast<size_t>(y) * width + x;
				if (refined[index] || accepted_subtract[index]) continue;
				int neighbours = 0;
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
						if (dx || dy) neighbours += before_cleanup[
							static_cast<size_t>(y + dy) * width + x + dx] != 0;
				if (neighbours >= 7) refined[index] = 1;
			}
		}
		return refined;
	}

	std::vector<unsigned char> retain_semantic_components(
		std::vector<unsigned char> const& selected,
		std::vector<uint32_t> const& owners,
		std::vector<unsigned char> const& protected_background,
		int width, int height) {
		if (width < 1 || height < 1 || selected.size() != owners.size() ||
			selected.size() != protected_background.size() ||
			selected.size() != static_cast<size_t>(width) * height)
			return selected;

		struct Component {
			std::vector<size_t> pixels;
			std::array<size_t, 32> owner_overlap{};
		};
		std::vector<Component> components;
		std::vector<unsigned char> visited(selected.size(), 0);
		std::deque<size_t> queue;
		for (size_t seed = 0; seed < selected.size(); ++seed) {
			if (!selected[seed] || visited[seed]) continue;
			components.emplace_back();
			auto& component = components.back();
			visited[seed] = 1;
			queue.push_back(seed);
			while (!queue.empty()) {
				size_t current = queue.front();
				queue.pop_front();
				component.pixels.push_back(current);
				uint32_t owner_bits = owners[current];
				while (owner_bits) {
					unsigned long bit = 0;
#ifdef _MSC_VER
					_BitScanForward(&bit, owner_bits);
#else
					bit = static_cast<unsigned long>(__builtin_ctz(owner_bits));
#endif
					++component.owner_overlap[bit];
					owner_bits &= owner_bits - 1;
				}
				int x = static_cast<int>(current % width);
				int y = static_cast<int>(current / width);
				for (auto [nx, ny] : {std::pair{x - 1, y}, {x + 1, y},
					{ x, y - 1 }, { x, y + 1 }}) {
					if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
					size_t next = static_cast<size_t>(ny) * width + nx;
					if (!selected[next] || visited[next]) continue;
					visited[next] = 1;
					queue.push_back(next);
				}
			}
		}
		if (components.empty()) return selected;

		std::vector<unsigned char> keep(components.size(), 0);
		for (size_t owner = 0; owner < 32; ++owner) {
			size_t best = components.size();
			size_t best_overlap = 0;
			for (size_t i = 0; i < components.size(); ++i) {
				size_t overlap = components[i].owner_overlap[owner];
				if (overlap > best_overlap ||
					(overlap && overlap == best_overlap && best < components.size() &&
						components[i].pixels.size() > components[best].pixels.size())) {
					best = i;
					best_overlap = overlap;
				}
			}
			if (best < components.size()) keep[best] = 1;
		}
		if (std::none_of(keep.begin(), keep.end(), [](unsigned char value) { return value != 0; })) {
			auto largest = std::max_element(components.begin(), components.end(),
				[](Component const& first, Component const& second) {
					return first.pixels.size() < second.pixels.size();
				});
			keep[static_cast<size_t>(largest - components.begin())] = 1;
		}

		std::vector<unsigned char> result(selected.size(), 0);
		for (size_t i = 0; i < components.size(); ++i) {
			if (!keep[i]) continue;
			for (size_t pixel : components[i].pixels)
				if (!protected_background[pixel]) result[pixel] = 1;
		}

		// One symmetric cleanup pass removes raster pinpricks without rounding
		// genuine hair tips, fingers, corners, or AI-confirmed background gaps.
		auto before = result;
		for (int y = 1; y + 1 < height; ++y) {
			for (int x = 1; x + 1 < width; ++x) {
				size_t index = static_cast<size_t>(y) * width + x;
				int neighbours = 0;
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
						if (dx || dy) neighbours += before[
							static_cast<size_t>(y + dy) * width + x + dx] != 0;
				if (before[index] && neighbours <= 1)
					result[index] = 0;
				else if (!before[index] && !protected_background[index] && neighbours >= 7)
					result[index] = 1;
			}
		}
		for (size_t i = 0; i < result.size(); ++i)
			if (protected_background[i]) result[i] = 0;
		return result;
	}

	bool sample_bilinear(std::vector<BlurredPixel> const& pixels, int width, int height,
		Vector2D point, BlurredPixel& result) {
		if (point.X() < 0.f || point.Y() < 0.f ||
			point.X() > width - 1.f || point.Y() > height - 1.f)
			return false;
		int x0 = static_cast<int>(std::floor(point.X()));
		int y0 = static_cast<int>(std::floor(point.Y()));
		int x1 = std::min(x0 + 1, width - 1);
		int y1 = std::min(y0 + 1, height - 1);
		float tx = point.X() - x0;
		float ty = point.Y() - y0;
		auto const& p00 = pixels[static_cast<size_t>(y0) * width + x0];
		auto const& p10 = pixels[static_cast<size_t>(y0) * width + x1];
		auto const& p01 = pixels[static_cast<size_t>(y1) * width + x0];
		auto const& p11 = pixels[static_cast<size_t>(y1) * width + x1];
		auto blend = [&](float BlurredPixel::*channel) {
			float top_value = p00.*channel + (p10.*channel - p00.*channel) * tx;
			float bottom_value = p01.*channel + (p11.*channel - p01.*channel) * tx;
			return top_value + (bottom_value - top_value) * ty;
		};
		result = {blend(&BlurredPixel::red), blend(&BlurredPixel::green),
			blend(&BlurredPixel::blue)};
		return true;
	}

	template <size_t MaximumSamples>
	bool median_colour(std::array<BlurredPixel, MaximumSamples> const& samples,
		size_t sample_count, BlurredPixel& result) {
		if (sample_count < 4) return false;
		auto median = [&](float BlurredPixel::*channel) {
			std::array<float, MaximumSamples> values{};
			for (size_t i = 0; i < sample_count; ++i)
				values[i] = samples[i].*channel;
			auto middle = values.begin() + sample_count / 2;
			std::nth_element(values.begin(), middle, values.begin() + sample_count);
			if (sample_count % 2) return *middle;
			float lower = *std::max_element(values.begin(), middle);
			return (lower + *middle) * .5f;
		};
		result = {median(&BlurredPixel::red), median(&BlurredPixel::green),
			median(&BlurredPixel::blue)};
		return true;
	}

	template <size_t DistanceCount>
	bool sample_strip_median(std::vector<BlurredPixel> const& pixels, int width, int height,
		Vector2D point, Vector2D normal, Vector2D tangent,
		std::array<float, DistanceCount> const& distances, float side,
		BlurredPixel& result) {
		constexpr std::array<float, 5> tangent_offsets{{-3.f, -1.5f, 0.f, 1.5f, 3.f}};
		std::array<BlurredPixel, DistanceCount * tangent_offsets.size()> samples{};
		size_t sample_count = 0;
		for (float distance : distances) {
			for (float tangent_offset : tangent_offsets) {
				BlurredPixel sample;
				if (sample_bilinear(pixels, width, height,
					point + normal * (distance * side) + tangent * tangent_offset, sample))
					samples[sample_count++] = sample;
			}
		}
		return median_colour(samples, sample_count, result);
	}

	float robust_colour_distance(BlurredPixel const& first, BlurredPixel const& second) {
		float red = first.red - second.red;
		float green = first.green - second.green;
		float blue = first.blue - second.blue;
		return std::sqrt(red * red * .25f + green * green * .5f + blue * blue * .25f) / 100.f;
	}

	float contour_perimeter(std::vector<Vector2D> const& contour) {
		float perimeter = 0.f;
		for (size_t i = 0; i < contour.size(); ++i)
			perimeter += (contour[(i + 1) % contour.size()] - contour[i]).Len();
		return perimeter;
	}

	bool point_on_segment(Vector2D point, Vector2D first, Vector2D second,
		float epsilon = 1e-3f) {
		Vector2D segment = second - first;
		Vector2D relative = point - first;
		if (std::abs(segment.Cross(relative)) > epsilon * std::max(1.f, segment.Len()))
			return false;
		return relative.Dot(segment) >= -epsilon &&
			relative.Dot(segment) <= segment.SquareLen() + epsilon;
	}

	bool segments_intersect(Vector2D a, Vector2D b, Vector2D c, Vector2D d) {
		auto orientation = [](Vector2D first, Vector2D second, Vector2D third) {
			return (second - first).Cross(third - first);
		};
		float ab_c = orientation(a, b, c);
		float ab_d = orientation(a, b, d);
		float cd_a = orientation(c, d, a);
		float cd_b = orientation(c, d, b);
		constexpr float epsilon = 1e-3f;
		if (((ab_c > epsilon && ab_d < -epsilon) || (ab_c < -epsilon && ab_d > epsilon)) &&
			((cd_a > epsilon && cd_b < -epsilon) || (cd_a < -epsilon && cd_b > epsilon)))
			return true;
		return (std::abs(ab_c) <= epsilon && point_on_segment(c, a, b)) ||
			(std::abs(ab_d) <= epsilon && point_on_segment(d, a, b)) ||
			(std::abs(cd_a) <= epsilon && point_on_segment(a, c, d)) ||
			(std::abs(cd_b) <= epsilon && point_on_segment(b, c, d));
	}

	bool contour_self_intersects(std::vector<Vector2D> const& contour) {
		if (contour.size() < 4) return false;
		for (size_t i = 0; i < contour.size(); ++i) {
			size_t i_next = (i + 1) % contour.size();
			for (size_t j = i + 1; j < contour.size(); ++j) {
				size_t j_next = (j + 1) % contour.size();
				if (i == j || i_next == j || j_next == i) continue;
				if (segments_intersect(contour[i], contour[i_next],
					contour[j], contour[j_next]))
					return true;
			}
		}
		return false;
	}

	bool contours_intersect(std::vector<Vector2D> const& first,
		std::vector<Vector2D> const& second) {
		for (size_t i = 0; i < first.size(); ++i) {
			for (size_t j = 0; j < second.size(); ++j) {
				if (segments_intersect(first[i], first[(i + 1) % first.size()],
					second[j], second[(j + 1) % second.size()]))
					return true;
			}
		}
		return false;
	}

	bool contour_inside(std::vector<Vector2D> const& inner,
		std::vector<Vector2D> const& outer) {
		if (inner.size() < 3 || outer.size() < 3 || contours_intersect(inner, outer))
			return false;
		for (size_t i = 0; i < inner.size(); ++i) {
			Vector2D first = inner[i];
			Vector2D second = inner[(i + 1) % inner.size()];
			int samples = std::max(1, static_cast<int>(std::ceil((second - first).Len() / 4.f)));
			for (int sample = 0; sample < samples; ++sample) {
				Vector2D point = first + (second - first) *
					(static_cast<float>(sample) / samples);
				if (!polygon_contains_point(outer, point)) return false;
			}
		}
		return true;
	}

	struct DenseContourPoint {
		Vector2D point;
		bool crop_locked = false;
		bool sharp = false;
	};

	std::vector<Vector2D> snap_closed_contour(std::vector<BlurredPixel> const& pixels,
		int width, int height, std::vector<Vector2D> source, bool add) {
		if (source.size() < 3) return {};
		// Never try to make a malformed AI traversal simple with 2-opt. Reversing
		// a long point span can remove a crossing while inventing large diagonal
		// cuts through an otherwise correct subject.
		if (contour_self_intersects(source)) return {};
		bool wants_positive_area = add;
		if ((polygon_area(source) > 0.0) != wants_positive_area)
			std::reverse(source.begin(), source.end());
		double original_area = std::abs(polygon_area(source));
		float original_perimeter = contour_perimeter(source);
		if (original_area < 8.0 || original_perimeter < 8.f) return {};

		float spacing = std::clamp(std::min(width, height) / 320.f, 1.5f, 2.5f);
		std::vector<DenseContourPoint> dense;
		for (size_t i = 0; i < source.size(); ++i) {
			Vector2D previous = source[(i + source.size() - 1) % source.size()];
			Vector2D current = source[i];
			Vector2D next = source[(i + 1) % source.size()];
			Vector2D incoming = current - previous;
			Vector2D outgoing = next - current;
			if (outgoing.SquareLen() <= 1e-5f) continue;
			bool sharp = incoming.SquareLen() > 1e-5f &&
				incoming.Unit().Dot(outgoing.Unit()) < .5f;
			int steps = std::max(1, static_cast<int>(std::ceil(outgoing.Len() / spacing)));
			for (int step = 0; step < steps; ++step) {
				Vector2D point = current + outgoing * (static_cast<float>(step) / steps);
				constexpr float crop_lock_distance = .75f;
				bool lock_left = point.X() <= crop_lock_distance;
				bool lock_top = point.Y() <= crop_lock_distance;
				bool lock_right = point.X() >= width - crop_lock_distance;
				bool lock_bottom = point.Y() >= height - crop_lock_distance;
				if (lock_left) point = Vector2D(0.f, point.Y());
				else if (lock_right) point = Vector2D(static_cast<float>(width), point.Y());
				if (lock_top) point = Vector2D(point.X(), 0.f);
				else if (lock_bottom) point = Vector2D(point.X(), static_cast<float>(height));
				bool crop_locked = lock_left || lock_top || lock_right || lock_bottom;
				dense.push_back({point, crop_locked, step == 0 && sharp});
			}
		}
		if (dense.size() < 3) return source;
		auto first_locked = std::find_if(dense.begin(), dense.end(),
			[](DenseContourPoint const& point) { return point.crop_locked; });
		if (first_locked != dense.end())
			std::rotate(dense.begin(), first_locked, dense.end());

		size_t point_count = dense.size();
		std::vector<Vector2D> normals(point_count);
		std::vector<Vector2D> tangents(point_count);
		for (size_t i = 0; i < point_count; ++i) {
			Vector2D tangent = -dense[(i + point_count - 2) % point_count].point -
				dense[(i + point_count - 1) % point_count].point * 2.f +
				dense[(i + 1) % point_count].point * 2.f +
				dense[(i + 2) % point_count].point;
			if (tangent.SquareLen() <= 1e-5f)
				tangent = dense[(i + 1) % point_count].point -
					dense[(i + point_count - 1) % point_count].point;
			tangents[i] = tangent.SquareLen() > 1e-5f ? tangent.Unit() : Vector2D(1.f, 0.f);
			normals[i] = tangents[i].Perpendicular();
		}

		int radius = std::clamp(static_cast<int>(std::lround(
			std::min(width, height) * .01)), 4, add ? 8 : 5);
		constexpr float state_step = .5f;
		int state_count = radius * 4 + 1;
		int zero_state = radius * 2;
		auto state_offset = [&](int state) { return (state - zero_state) * state_step; };
		constexpr float infinity = 1e20f;
		std::vector<float> unary(point_count * state_count, infinity);
		std::array<float, 4> reference_distances{{
			static_cast<float>(radius + 2), static_cast<float>(radius + 3),
			static_cast<float>(radius + 4), static_cast<float>(radius + 5)}};
		constexpr std::array<float, 3> candidate_distances{{1.25f, 2.25f, 3.25f}};
		for (size_t i = 0; i < point_count; ++i) {
			BlurredPixel foreground_reference;
			BlurredPixel background_reference;
			bool has_foreground = sample_strip_median(pixels, width, height,
				dense[i].point, normals[i], tangents[i], reference_distances, 1.f,
				foreground_reference);
			bool has_background = sample_strip_median(pixels, width, height,
				dense[i].point, normals[i], tangents[i], reference_distances, -1.f,
				background_reference);
			bool confident = has_foreground && has_background &&
				robust_colour_distance(foreground_reference, background_reference) >= .08f;
			for (int state = 0; state < state_count; ++state) {
				float offset = state_offset(state);
				if ((dense[i].crop_locked && state != zero_state) ||
					(dense[i].sharp && std::abs(offset) > 2.f))
					continue;
				float prior = (dense[i].sharp ? .08f : .028f) * offset * offset;
				if (!confident) {
					unary[i * state_count + state] = prior + std::abs(offset) * .12f;
					continue;
				}
				Vector2D candidate = dense[i].point + normals[i] * offset;
				BlurredPixel inside;
				BlurredPixel outside;
				if (!sample_strip_median(pixels, width, height, candidate, normals[i],
					tangents[i], candidate_distances, 1.f, inside) ||
					!sample_strip_median(pixels, width, height, candidate, normals[i],
						tangents[i], candidate_distances, -1.f, outside))
					continue;
				float match = robust_colour_distance(inside, foreground_reference) +
					robust_colour_distance(outside, background_reference);
				float reversed = robust_colour_distance(inside, background_reference) +
					robust_colour_distance(outside, foreground_reference);
				float contrast = robust_colour_distance(inside, outside);
				unary[i * state_count + state] = 1.25f * match - 1.65f * contrast +
					std::max(0.f, match - reversed) + prior;
			}
		}

		std::vector<float> transition(static_cast<size_t>(state_count) * state_count, infinity);
		for (int previous = 0; previous < state_count; ++previous) {
			for (int state = 0; state < state_count; ++state) {
				float delta = state_offset(state) - state_offset(previous);
				if (std::abs(delta) > 3.f) continue;
				transition[static_cast<size_t>(previous) * state_count + state] =
					.32f * std::min(delta * delta, 4.f) +
					.65f * std::max(std::abs(delta) - 2.f, 0.f);
			}
		}

		auto solve = [&](int start_state, std::vector<int16_t> *backtrack,
			int *end_state) {
			std::vector<float> previous(state_count, infinity);
			std::vector<float> current(state_count, infinity);
			previous[start_state] = unary[start_state];
			for (size_t i = 1; i < point_count; ++i) {
				std::fill(current.begin(), current.end(), infinity);
				for (int state = 0; state < state_count; ++state) {
					float data_cost = unary[i * state_count + state];
					if (data_cost >= infinity) continue;
					int best_previous = 0;
					float best = infinity;
					constexpr int maximum_state_delta = 6;
					int first_predecessor = std::max(0, state - maximum_state_delta);
					int last_predecessor = std::min(state_count - 1,
						state + maximum_state_delta);
					for (int predecessor = first_predecessor;
						predecessor <= last_predecessor; ++predecessor) {
						float candidate = previous[predecessor] + transition[
							static_cast<size_t>(predecessor) * state_count + state];
						if (candidate < best) {
							best = candidate;
							best_previous = predecessor;
						}
					}
					current[state] = best + data_cost;
					if (backtrack)
						(*backtrack)[i * state_count + state] =
							static_cast<int16_t>(best_previous);
				}
				previous.swap(current);
			}
			float best = infinity;
			int best_end = 0;
			for (int state = 0; state < state_count; ++state) {
				float candidate = previous[state] + transition[
					static_cast<size_t>(state) * state_count + start_state];
				if (candidate < best) {
					best = candidate;
					best_end = state;
				}
			}
			if (end_state) *end_state = best_end;
			return best;
		};

		int best_start = zero_state;
		float best_score = infinity;
		if (dense.front().crop_locked) {
			best_score = solve(zero_state, nullptr, nullptr);
		}
		else {
			for (int start = 0; start < state_count; ++start) {
				float score = solve(start, nullptr, nullptr);
				if (score < best_score) {
					best_score = score;
					best_start = start;
				}
			}
		}
		if (best_score >= infinity * .5f) return source;
		std::vector<int16_t> backtrack(point_count * state_count, 0);
		int end_state = best_start;
		solve(best_start, &backtrack, &end_state);
		std::vector<float> offsets(point_count);
		int state = end_state;
		for (size_t reverse = point_count; reverse-- > 0;) {
			offsets[reverse] = state_offset(state);
			if (reverse)
				state = backtrack[reverse * state_count + state];
		}
		for (int pass = 0; pass < 2; ++pass) {
			auto before = offsets;
			for (size_t i = 0; i < point_count; ++i) {
				if (dense[i].crop_locked || dense[i].sharp) {
					if (dense[i].crop_locked) offsets[i] = 0.f;
					continue;
				}
				offsets[i] = (before[(i + point_count - 1) % point_count] +
					before[i] * 2.f + before[(i + 1) % point_count]) * .25f;
			}
		}

		std::vector<Vector2D> result;
		result.reserve(point_count);
		for (size_t i = 0; i < point_count; ++i) {
			Vector2D point = dense[i].point + normals[i] * offsets[i];
			result.emplace_back(std::clamp(point.X(), 0.f, static_cast<float>(width)),
				std::clamp(point.Y(), 0.f, static_cast<float>(height)));
		}
		result = simplify_closed(std::move(result), .65);
		if (result.size() < 3 || contour_self_intersects(result) ||
			(polygon_area(result) > 0.0) != wants_positive_area)
			return source;
		double area_ratio = std::abs(polygon_area(result)) / original_area;
		float perimeter_ratio = contour_perimeter(result) / original_perimeter;
		double minimum_area_ratio = add ? .92 : .90;
		double maximum_area_ratio = add ? 1.08 : 1.10;
		if (area_ratio < minimum_area_ratio || area_ratio > maximum_area_ratio ||
			perimeter_ratio < .8f || perimeter_ratio > 1.2f)
			return source;
		return result;
	}

	std::vector<std::vector<Vector2D>> snap_semantic_contours(wxImage const& image,
		std::vector<RasterContour> const& semantic_contours) {
		if (!image.IsOk() || !image.GetData()) return {};
		auto pixels = blur_selection_image(image);
		std::vector<std::vector<Vector2D>> addition_candidates;
		for (auto const& semantic : semantic_contours) {
			if (!semantic.add || semantic.points.size() < 3)
				continue;
			auto contour = snap_closed_contour(pixels, image.GetWidth(), image.GetHeight(),
				semantic.points, true);
			if (contour.size() < 3) continue;
			addition_candidates.push_back(std::move(contour));
		}
		std::sort(addition_candidates.begin(), addition_candidates.end(),
			[](std::vector<Vector2D> const& first, std::vector<Vector2D> const& second) {
				return std::abs(polygon_area(first)) > std::abs(polygon_area(second));
			});
		std::vector<std::vector<Vector2D>> additions;
		for (auto& contour : addition_candidates) {
			bool overlaps = std::any_of(additions.begin(), additions.end(),
				[&](std::vector<Vector2D> const& existing) {
					return contours_intersect(existing, contour) ||
						polygon_contains_point(existing, contour.front()) ||
						polygon_contains_point(contour, existing.front());
				});
			if (!overlaps) additions.push_back(std::move(contour));
		}
		if (additions.empty()) return {};
		std::vector<std::vector<Vector2D>> result = additions;
		std::vector<std::vector<Vector2D>> accepted_holes;
		for (auto const& semantic : semantic_contours) {
			if (semantic.add || semantic.points.size() < 3)
				continue;
			auto raw_hole = semantic.points;
			if (contour_self_intersects(raw_hole)) continue;
			if (polygon_area(raw_hole) > 0.0) std::reverse(raw_hole.begin(), raw_hole.end());
			auto outer = std::find_if(additions.begin(), additions.end(),
				[&](std::vector<Vector2D> const& candidate) {
					return contour_inside(raw_hole, candidate);
				});
			if (outer == additions.end()) continue;
			auto hole = snap_closed_contour(pixels, image.GetWidth(), image.GetHeight(),
				raw_hole, false);
			if (!contour_inside(hole, *outer)) hole = std::move(raw_hole);
			if (!contour_inside(hole, *outer)) continue;
			bool overlaps = std::any_of(accepted_holes.begin(), accepted_holes.end(),
				[&](std::vector<Vector2D> const& existing) {
					return contours_intersect(existing, hole) ||
						polygon_contains_point(existing, hole.front()) ||
						polygon_contains_point(hole, existing.front());
				});
			if (!overlaps) accepted_holes.push_back(std::move(hole));
		}
		for (auto& hole : accepted_holes) result.push_back(std::move(hole));
		return result;
	}
}

void VisualColorSegmenter::Clear() {
	left = top = width = height = source_width = source_height = 0;
	distances.clear();
	subtract_distances.clear();
	base_selected.clear();
	painted.clear();
	stored_contours.clear();
	contours_are_pristine = false;
}

VisualColorSample VisualColorSegmenter::Sample(VideoFrame const& frame, int x, int y) {
	if (frame.width <= 0 || frame.height <= 0 || frame.pitch <= 0 || frame.data.empty())
		return {};
	x = std::clamp(x, 0, frame.width - 1);
	y = std::clamp(y, 0, frame.height - 1);
	if (frame.flipped) y = frame.height - 1 - y;
	size_t offset = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
	if (offset + 2 >= frame.data.size()) return {};
	return {frame.data[offset + 2], frame.data[offset + 1], frame.data[offset]};
}

bool VisualColorSegmenter::PrepareEmpty(VideoFrame const& frame, int x1, int y1, int x2, int y2) {
	Clear();
	if (frame.width <= 0 || frame.height <= 0 || frame.pitch <= 0 || frame.data.empty())
		return false;
	left = std::clamp(std::min(x1, x2), 0, frame.width);
	top = std::clamp(std::min(y1, y2), 0, frame.height);
	int right = std::clamp(std::max(x1, x2), 0, frame.width);
	int bottom = std::clamp(std::max(y1, y2), 0, frame.height);
	width = right - left;
	height = bottom - top;
	if (width < 1 || height < 1) return false;
	source_width = frame.width;
	source_height = frame.height;

	distances.assign(static_cast<size_t>(width) * height,
		std::numeric_limits<uint16_t>::max());
	subtract_distances = distances;
	base_selected.assign(distances.size(), 0);
	painted.assign(distances.size(), 0);
	return true;
}

bool VisualColorSegmenter::Prepare(VideoFrame const& frame, int x1, int y1, int x2, int y2,
	VisualColorSample sample) {
	if (!PrepareEmpty(frame, x1, y1, x2, y2)) return false;
	for (int y = 0; y < height; ++y) {
		int source_y = top + y;
		if (frame.flipped) source_y = frame.height - 1 - source_y;
		for (int x = 0; x < width; ++x) {
			size_t offset = static_cast<size_t>(source_y) * frame.pitch +
				static_cast<size_t>(left + x) * 4;
			if (offset + 2 >= frame.data.size()) continue;
			int db = static_cast<int>(frame.data[offset]) - sample.blue;
			int dg = static_cast<int>(frame.data[offset + 1]) - sample.green;
			int dr = static_cast<int>(frame.data[offset + 2]) - sample.red;
			// Green carries most perceived detail, while the normalization keeps
			// the slider in the familiar 0-100 colour-difference range.
			int distance = (2 * dr * dr + 4 * dg * dg + db * db) / 7;
			distances[static_cast<size_t>(y) * width + x] =
				static_cast<uint16_t>(std::min(distance, 65535));
		}
	}
	return true;
}

bool VisualColorSegmenter::AddSample(VideoFrame const& frame, VisualColorSample sample, bool add) {
	if (distances.empty() || frame.width != source_width || frame.height != source_height ||
		frame.pitch <= 0 || frame.data.empty()) return false;
	stored_contours.clear();
	contours_are_pristine = false;
	auto& target = add ? distances : subtract_distances;
	for (int y = 0; y < height; ++y) {
		int source_y = top + y;
		if (frame.flipped) source_y = frame.height - 1 - source_y;
		for (int x = 0; x < width; ++x) {
			size_t offset = static_cast<size_t>(source_y) * frame.pitch +
				static_cast<size_t>(left + x) * 4;
			if (offset + 2 >= frame.data.size()) continue;
			int db = static_cast<int>(frame.data[offset]) - sample.blue;
			int dg = static_cast<int>(frame.data[offset + 1]) - sample.green;
			int dr = static_cast<int>(frame.data[offset + 2]) - sample.red;
			int distance = (2 * dr * dr + 4 * dg * dg + db * db) / 7;
			auto index = static_cast<size_t>(y) * width + x;
			target[index] = static_cast<uint16_t>(std::min<int>(
				target[index], std::min(distance, 65535)));
		}
	}
	return true;
}

void VisualColorSegmenter::SetContours(std::vector<std::vector<Vector2D>> const& contours,
	bool normalize_to_pixels) {
	if (distances.empty()) return;
	stored_contours.clear();
	for (auto const& contour : contours) {
		bool valid = contour.size() >= 3 && std::all_of(contour.begin(), contour.end(),
			[](Vector2D point) {
				return std::isfinite(point.X()) && std::isfinite(point.Y());
			});
		if (valid) stored_contours.push_back(contour);
	}
	contours_are_pristine = !stored_contours.empty();
	std::fill(distances.begin(), distances.end(), std::numeric_limits<uint16_t>::max());
	std::fill(subtract_distances.begin(), subtract_distances.end(),
		std::numeric_limits<uint16_t>::max());
	std::fill(base_selected.begin(), base_selected.end(), 0);
	std::fill(painted.begin(), painted.end(), 0);

	// Rasterize all contour loops using an even/odd scanline fill. Coordinates
	// are absolute frame pixels, matching Paint() and Extract(). Pairing all
	// crossings also preserves holes represented by inner contour loops.
	std::vector<float> crossings;
	for (int y = 0; y < height; ++y) {
		float scan_y = top + y + .5f;
		crossings.clear();
		for (auto const& contour : stored_contours) {
			if (contour.size() < 3) continue;
			for (size_t i = 0, previous = contour.size() - 1; i < contour.size(); previous = i++) {
				auto const& a = contour[previous];
				auto const& b = contour[i];
				if ((a.Y() > scan_y) == (b.Y() > scan_y)) continue;
				crossings.push_back(a.X() + (scan_y - a.Y()) *
					(b.X() - a.X()) / (b.Y() - a.Y()));
			}
		}
		std::sort(crossings.begin(), crossings.end());
		for (size_t i = 1; i < crossings.size(); i += 2) {
			int first = std::max(0, static_cast<int>(std::ceil(crossings[i - 1] - left - .5f)));
			int last = std::min(width - 1, static_cast<int>(std::floor(crossings[i] - left - .5f)));
			for (int x = first; x <= last; ++x)
				base_selected[static_cast<size_t>(y) * width + x] = 1;
		}
	}
	if (normalize_to_pixels) {
		auto normalized = trace_binary_mask(base_selected, width, height, left, top, .65);
		if (!normalized.empty()) stored_contours = std::move(normalized);
	}
	contours_are_pristine = !stored_contours.empty();
}

void VisualColorSegmenter::Paint(int frame_x, int frame_y, float radius, bool add) {
	if (painted.empty() || radius <= 0.f) return;
	float local_x = static_cast<float>(frame_x - left);
	float local_y = static_cast<float>(frame_y - top);
	int min_x = std::max(0, static_cast<int>(std::floor(local_x - radius)));
	int max_x = std::min(width - 1, static_cast<int>(std::ceil(local_x + radius)));
	int min_y = std::max(0, static_cast<int>(std::floor(local_y - radius)));
	int max_y = std::min(height - 1, static_cast<int>(std::ceil(local_y + radius)));
	float radius_squared = radius * radius;
	bool changed = false;
	signed char value = add ? 1 : -1;
	for (int y = min_y; y <= max_y; ++y) {
		for (int x = min_x; x <= max_x; ++x) {
			float dx = x + .5f - local_x;
			float dy = y + .5f - local_y;
			if (dx * dx + dy * dy <= radius_squared) {
				auto& pixel = painted[static_cast<size_t>(y) * width + x];
				changed = changed || pixel != value;
				pixel = value;
			}
		}
	}
	if (changed) {
		stored_contours.clear();
		contours_are_pristine = false;
	}
}

std::vector<std::vector<Vector2D>> VisualColorSegmenter::Extract(double tolerance, bool fill_holes,
	int offset_pixels) const {
	if (distances.empty()) return {};
	if (contours_are_pristine && !fill_holes && offset_pixels == 0)
		return stored_contours;
	double channel_delta = std::clamp(tolerance, 0.0, 100.0) * 2.55;
	uint32_t threshold = static_cast<uint32_t>(std::lround(channel_delta * channel_delta));
	std::vector<unsigned char> selected(distances.size());
	for (size_t i = 0; i < distances.size(); ++i) {
		selected[i] = base_selected[i] || distances[i] <= threshold;
		if (subtract_distances[i] <= threshold) selected[i] = 0;
	}

	// A lone matching pixel is almost always compression or antialias noise.
	// Remove only those singletons; every larger disconnected component and all
	// holes are intentionally retained.
	std::vector<unsigned char> visited(selected.size());
	std::deque<size_t> queue;
	for (size_t seed = 0; seed < selected.size(); ++seed) {
		if (!selected[seed] || visited[seed]) continue;
		queue.clear();
		queue.push_back(seed);
		visited[seed] = 1;
		std::vector<size_t> component;
		while (!queue.empty()) {
			size_t current = queue.front();
			queue.pop_front();
			component.push_back(current);
			int x = static_cast<int>(current % width);
			int y = static_cast<int>(current / width);
			for (auto [nx, ny] : {std::pair{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}}) {
				if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
				size_t next = static_cast<size_t>(ny) * width + nx;
				if (selected[next] && !visited[next]) {
					visited[next] = 1;
					queue.push_back(next);
				}
			}
		}
		if (component.size() == 1) selected[component.front()] = 0;
	}
	// Expand or contract the selection with a linear-time chamfer distance
	// field. The 3/4 weights approximate a round pixel radius without making
	// live updates depend on offset * crop size.
	offset_pixels = std::clamp(offset_pixels, -50, 50);
	if (offset_pixels != 0) {
		constexpr int infinity = 1 << 28;
		std::vector<int> distance(selected.size(), infinity);
		bool expand = offset_pixels > 0;
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				size_t index = static_cast<size_t>(y) * width + x;
				bool source = expand ? selected[index] : !selected[index];
				if (source)
					distance[index] = 0;
				else if (!expand && (x == 0 || y == 0 || x + 1 == width || y + 1 == height))
					distance[index] = 3;
			}
		}
		auto relax = [&](int x, int y, int nx, int ny, int cost) {
			if (nx < 0 || ny < 0 || nx >= width || ny >= height) return;
			size_t index = static_cast<size_t>(y) * width + x;
			size_t neighbour = static_cast<size_t>(ny) * width + nx;
			distance[index] = std::min(distance[index], distance[neighbour] + cost);
		};
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				relax(x, y, x - 1, y, 3);
				relax(x, y, x, y - 1, 3);
				relax(x, y, x - 1, y - 1, 4);
				relax(x, y, x + 1, y - 1, 4);
			}
		}
		for (int y = height - 1; y >= 0; --y) {
			for (int x = width - 1; x >= 0; --x) {
				relax(x, y, x + 1, y, 3);
				relax(x, y, x, y + 1, 3);
				relax(x, y, x + 1, y + 1, 4);
				relax(x, y, x - 1, y + 1, 4);
			}
		}
		int threshold = std::abs(offset_pixels) * 3;
		for (size_t i = 0; i < selected.size(); ++i) {
			if (expand)
				selected[i] = selected[i] || distance[i] <= threshold;
			else
				selected[i] = selected[i] && distance[i] > threshold;
		}
	}

	if (fill_holes) {
		// Background connected to the crop boundary stays transparent. Every
		// other background island is a hole enclosed by the selected colour and
		// is filled, while disconnected foreground components remain separate.
		std::vector<unsigned char> exterior(selected.size());
		queue.clear();
		auto enqueue_background = [&](int x, int y) {
			if (x < 0 || y < 0 || x >= width || y >= height) return;
			size_t index = static_cast<size_t>(y) * width + x;
			if (selected[index] || exterior[index]) return;
			exterior[index] = 1;
			queue.push_back(index);
		};
		for (int x = 0; x < width; ++x) {
			enqueue_background(x, 0);
			enqueue_background(x, height - 1);
		}
		for (int y = 0; y < height; ++y) {
			enqueue_background(0, y);
			enqueue_background(width - 1, y);
		}
		while (!queue.empty()) {
			size_t current = queue.front();
			queue.pop_front();
			int x = static_cast<int>(current % width);
			int y = static_cast<int>(current / width);
			enqueue_background(x - 1, y);
			enqueue_background(x + 1, y);
			enqueue_background(x, y - 1);
			enqueue_background(x, y + 1);
		}
		for (size_t i = 0; i < selected.size(); ++i)
			if (!selected[i] && !exterior[i]) selected[i] = 1;
	}

	// Manual brush edits are authoritative. Apply them last so Auto fill and
	// morphology cannot restore an area explicitly subtracted by the user.
	for (size_t i = 0; i < selected.size(); ++i) {
		if (painted[i] > 0) selected[i] = 1;
		else if (painted[i] < 0) selected[i] = 0;
	}

	return trace_binary_mask(selected, width, height, left, top, .85);
}

wxCursor MakeVisualColorPickerCursor() {
	wxCursor resource_cursor("eyedropper_cursor");
	if (resource_cursor.IsOk()) return resource_cursor;

	constexpr int size = 32;
	wxImage image(size, size, true);
	image.InitAlpha();
	auto rgb = image.GetData();
	auto alpha = image.GetAlpha();
	std::fill(rgb, rgb + size * size * 3, 0);
	std::fill(alpha, alpha + size * size, 0);
	auto pixel = [&](int x, int y, unsigned char red, unsigned char green,
		unsigned char blue, unsigned char opacity = 255) {
		if (x < 0 || y < 0 || x >= size || y >= size) return;
		size_t index = static_cast<size_t>(y) * size + x;
		rgb[index * 3] = red;
		rgb[index * 3 + 1] = green;
		rgb[index * 3 + 2] = blue;
		alpha[index] = opacity;
	};
	// Tip at (4, 27), handle towards the upper-right. A dark outline keeps the
	// pipette visible on both light and dark video frames.
	for (int i = 4; i <= 25; ++i) {
		int x = i;
		int y = 31 - i;
		for (int offset = -3; offset <= 3; ++offset)
			pixel(x + offset, y, 20, 20, 20);
		for (int offset = -2; offset <= 2; ++offset)
			pixel(x + offset, y, 75, 220, 255);
	}
	for (int y = 4; y <= 10; ++y)
		for (int x = 21; x <= 28; ++x)
			pixel(x, y, x == 21 || x == 28 || y == 4 || y == 10 ? 20 : 235,
				x == 21 || x == 28 || y == 4 || y == 10 ? 20 : 245,
				x == 21 || x == 28 || y == 4 || y == 10 ? 20 : 250);
	image.SetOption(wxIMAGE_OPTION_CUR_HOTSPOT_X, 4);
	image.SetOption(wxIMAGE_OPTION_CUR_HOTSPOT_Y, 27);
	return wxCursor(image);
}

bool WouldVectorBrushStrokeChange(std::vector<std::vector<Vector2D>> const& contours,
	std::vector<Vector2D> const& stroke, float radius, bool add) {
	if (stroke.empty() || radius <= 0.f) return false;
	float radius_squared = radius * radius;
	for (auto centre : stroke) {
		bool selected = false;
		for (auto const& contour : contours) {
			if (contour.size() < 3) continue;
			if (polygon_contains_point(contour, centre)) selected = !selected;
			for (size_t i = 0, previous = contour.size() - 1;
				i < contour.size(); previous = i++) {
				double distance = point_segment_distance(centre,
					contour[previous], contour[i]);
				if (distance * distance <= radius_squared) return true;
			}
		}
		if (add != selected) return true;
	}
	return false;
}

void ApplyVectorBrushStamp(std::vector<std::vector<Vector2D>>& contours,
	Vector2D centre, float radius, bool add) {
	namespace geometry = boost::geometry;
	using Point = geometry::model::d2::point_xy<double>;
	using Polygon = geometry::model::polygon<Point>;
	using MultiPolygon = geometry::model::multi_polygon<Polygon>;
	if (radius <= 0.f) return;

	auto touches = [&](std::vector<Vector2D> const& contour) {
		if (contour.size() < 3) return false;
		if (polygon_contains_point(contour, centre)) return true;
		for (size_t i = 0, previous = contour.size() - 1;
			i < contour.size(); previous = i++)
			if (point_segment_distance(centre, contour[previous], contour[i]) <= radius)
				return true;
		return false;
	};
	auto make_polygon = [&](size_t outer_index, std::vector<size_t> const& hole_indices) {
		Polygon polygon;
		auto& ring = polygon.outer();
		auto const& contour = contours[outer_index];
		ring.reserve(contour.size() + 1);
		for (auto point : contour) ring.emplace_back(point.X(), point.Y());
		if (!ring.empty()) ring.push_back(ring.front());
		for (size_t hole_index : hole_indices) {
			auto& inner = polygon.inners().emplace_back();
			auto const& hole = contours[hole_index];
			inner.reserve(hole.size() + 1);
			for (auto point : hole) inner.emplace_back(point.X(), point.Y());
			if (!inner.empty()) inner.push_back(inner.front());
		}
		geometry::correct(polygon);
		return polygon;
	};
	auto make_circle = [&] {
		constexpr int segments = 64;
		Polygon circle;
		auto& ring = circle.outer();
		ring.reserve(segments + 1);
		for (int i = 0; i < segments; ++i) {
			double angle = i * 2.0 * M_PI / segments;
			ring.emplace_back(centre.X() + std::cos(angle) * radius,
				centre.Y() + std::sin(angle) * radius);
		}
		ring.push_back(ring.front());
		geometry::correct(circle);
		return circle;
	};
	auto append_ring = [](std::vector<std::vector<Vector2D>>& output, auto const& ring) {
		if (ring.size() < 4) return;
		std::vector<Vector2D> contour;
		contour.reserve(ring.size() - 1);
		for (size_t i = 0; i + 1 < ring.size(); ++i)
			contour.emplace_back(static_cast<float>(ring[i].x()),
				static_cast<float>(ring[i].y()));
		output.push_back(std::move(contour));
	};

	std::vector<unsigned char> touched(contours.size());
	for (size_t i = 0; i < contours.size(); ++i)
		if (touches(contours[i])) touched[i] = 1;
	if (std::none_of(touched.begin(), touched.end(), [](unsigned char value) { return value != 0; })) {
		if (add) {
			auto circle = make_circle();
			append_ring(contours, circle.outer());
		}
		return;
	}

	// Treat outer rings and their inner rings as one polygon. Feeding holes to
	// union_ as independent filled polygons would fill the complete hole when
	// only a nearby part of the outer boundary was brushed.
	double reference_area = 0.0;
	for (auto const& contour : contours)
		if (std::abs(polygon_area(contour)) > std::abs(reference_area))
			reference_area = polygon_area(contour);
	if (reference_area == 0.0) reference_area = 1.0;
	std::vector<size_t> outers;
	std::vector<int> parent(contours.size(), -1);
	for (size_t i = 0; i < contours.size(); ++i)
		if (polygon_area(contours[i]) * reference_area > 0.0) outers.push_back(i);
	for (size_t i = 0; i < contours.size(); ++i) {
		if (polygon_area(contours[i]) * reference_area >= 0.0 || contours[i].empty()) continue;
		double best_area = std::numeric_limits<double>::max();
		for (size_t outer : outers) {
			double area = std::abs(polygon_area(contours[outer]));
			if (area < best_area && polygon_contains_point(contours[outer], contours[i].front())) {
				best_area = area;
				parent[i] = static_cast<int>(outer);
			}
		}
	}

	std::vector<size_t> affected;
	std::vector<Polygon> source;
	for (size_t outer : outers) {
		std::vector<size_t> holes;
		bool group_touched = touched[outer] != 0;
		for (size_t i = 0; i < parent.size(); ++i) {
			if (parent[i] != static_cast<int>(outer)) continue;
			holes.push_back(i);
			group_touched = group_touched || touched[i] != 0;
		}
		if (!group_touched) continue;
		affected.push_back(outer);
		affected.insert(affected.end(), holes.begin(), holes.end());
		auto polygon = make_polygon(outer, holes);
		if (!polygon.outer().empty()) source.push_back(std::move(polygon));
	}
	for (size_t i = 0; i < contours.size(); ++i) {
		if (!touched[i] || parent[i] >= 0 ||
			std::find(outers.begin(), outers.end(), i) != outers.end()) continue;
		affected.push_back(i);
		auto polygon = make_polygon(i, {});
		if (!polygon.outer().empty()) source.push_back(std::move(polygon));
	}
	Polygon circle = make_circle();
	MultiPolygon result;
	if (add) {
		result.push_back(circle);
		for (auto const& polygon : source) {
			MultiPolygon merged;
			geometry::union_(result, polygon, merged);
			result = std::move(merged);
		}
	}
	else {
		for (auto const& polygon : source) {
			MultiPolygon cut;
			geometry::difference(polygon, circle, cut);
			result.insert(result.end(), std::make_move_iterator(cut.begin()),
				std::make_move_iterator(cut.end()));
		}
	}
	std::sort(affected.begin(), affected.end());
	affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
	for (auto it = affected.rbegin(); it != affected.rend(); ++it)
		contours.erase(contours.begin() + *it);
	for (auto const& polygon : result) {
		append_ring(contours, polygon.outer());
		for (auto const& inner : polygon.inners()) append_ring(contours, inner);
	}
}

std::vector<std::vector<Vector2D>> ApplyVectorBrushStroke(
	std::vector<std::vector<Vector2D>> contours, std::vector<Vector2D> const& stroke,
	float radius, bool add) {
	constexpr int circle_segments = 64;
	if (radius <= 0.f || stroke.empty()) return contours;

	auto contains = [](std::vector<Vector2D> const& polygon, Vector2D point) {
		bool inside = false;
		for (size_t i = 0, previous = polygon.size() - 1; i < polygon.size(); previous = i) {
			auto const& a = polygon[previous];
			auto const& b = polygon[i];
			if ((a.Y() > point.Y()) != (b.Y() > point.Y()) &&
				point.X() < (b.X() - a.X()) * (point.Y() - a.Y()) /
					(b.Y() - a.Y()) + a.X()) inside = !inside;
		}
		return inside;
	};
	auto boundary_distance_squared = [](std::vector<Vector2D> const& polygon, Vector2D point) {
		float best = std::numeric_limits<float>::max();
		for (size_t i = 0, previous = polygon.size() - 1; i < polygon.size(); previous = i) {
			Vector2D start = polygon[previous];
			Vector2D delta = polygon[i] - start;
			float length_squared = delta.SquareLen();
			float factor = length_squared <= .0001f ? 0.f :
				std::clamp((point - start).Dot(delta) / length_squared, 0.f, 1.f);
			best = std::min(best, (point - (start + delta * factor)).SquareLen());
		}
		return best;
	};
	auto make_circle = [&](Vector2D centre, bool positive_winding) {
		std::vector<Vector2D> points;
		points.reserve(circle_segments);
		for (int i = 0; i < circle_segments; ++i) {
			double angle = (positive_winding ? i : -i) * 2.0 * M_PI / circle_segments;
			points.emplace_back(centre.X() + static_cast<float>(std::cos(angle) * radius),
				centre.Y() + static_cast<float>(std::sin(angle) * radius));
		}
		return points;
	};

	float spacing = std::max(1.f, radius * .3f);
	std::vector<Vector2D> centres{stroke.front()};
	for (size_t i = 1; i < stroke.size(); ++i)
		if ((stroke[i] - centres.back()).Len() >= spacing) centres.push_back(stroke[i]);
	if ((stroke.back() - centres.back()).Len() >= 1.f) centres.push_back(stroke.back());

	float radius_squared = radius * radius;
	std::vector<size_t> affected;
	bool boundary_touched = false;
	for (size_t index = 0; index < contours.size(); ++index) {
		if (contours[index].size() < 3) continue;
		for (auto centre : centres) {
			bool touches_boundary = boundary_distance_squared(contours[index], centre) <= radius_squared;
			if (contains(contours[index], centre) || touches_boundary) {
				affected.push_back(index);
				boundary_touched = boundary_touched || touches_boundary;
				break;
			}
		}
	}

	// A click wholly inside or outside the selection never needs a general
	// polygon operation. This is the common correction workflow and preserves
	// every existing point exactly.
	if (centres.size() == 1 && !boundary_touched) {
		bool selected = false;
		for (auto const& contour : contours)
			if (contour.size() >= 3 && contains(contour, centres.front())) selected = !selected;
		if (add == selected) return contours;
		double reference_area = 1.0;
		for (auto const& contour : contours)
			if (std::abs(polygon_area(contour)) > std::abs(reference_area))
				reference_area = polygon_area(contour);
		bool positive_winding = add ? reference_area > 0.0 : reference_area < 0.0;
		contours.push_back(make_circle(centres.front(), positive_winding));
		return contours;
	}

	// Boundary-crossing strokes use a bounded local bitmap. Unlike the previous
	// polygon boolean implementations this has a strict memory/time ceiling and
	// cannot loop on self-intersecting AI contours. Unaffected contours are kept
	// byte-for-byte.
	if (affected.empty()) {
		if (add)
			for (auto centre : centres) contours.push_back(make_circle(centre, true));
		return contours;
	}
	float minimum_x = centres.front().X() - radius - 2.f;
	float maximum_x = centres.front().X() + radius + 2.f;
	float minimum_y = centres.front().Y() - radius - 2.f;
	float maximum_y = centres.front().Y() + radius + 2.f;
	for (size_t index : affected) {
		for (auto point : contours[index]) {
			minimum_x = std::min(minimum_x, point.X() - 2.f);
			maximum_x = std::max(maximum_x, point.X() + 2.f);
			minimum_y = std::min(minimum_y, point.Y() - 2.f);
			maximum_y = std::max(maximum_y, point.Y() + 2.f);
		}
	}
	float source_width = std::max(1.f, maximum_x - minimum_x);
	float source_height = std::max(1.f, maximum_y - minimum_y);
	constexpr double maximum_pixels = 8000000.0;
	float raster_scale = std::min(2.f, static_cast<float>(
		std::sqrt(maximum_pixels / (source_width * source_height))));
	raster_scale = std::max(.5f, raster_scale);
	int width = std::max(1, static_cast<int>(std::ceil(source_width * raster_scale)) + 2);
	int height = std::max(1, static_cast<int>(std::ceil(source_height * raster_scale)) + 2);
	std::vector<unsigned char> selected(static_cast<size_t>(width) * height);
	std::vector<float> crossings;
	for (int y = 0; y < height; ++y) {
		float scan_y = minimum_y + (y + .5f) / raster_scale;
		crossings.clear();
		for (size_t index : affected) {
			auto const& contour = contours[index];
			for (size_t i = 0, previous = contour.size() - 1; i < contour.size(); previous = i++) {
				auto const& a = contour[previous];
				auto const& b = contour[i];
				if ((a.Y() > scan_y) == (b.Y() > scan_y)) continue;
				crossings.push_back((a.X() + (scan_y - a.Y()) *
					(b.X() - a.X()) / (b.Y() - a.Y()) - minimum_x) * raster_scale);
			}
		}
		std::sort(crossings.begin(), crossings.end());
		for (size_t i = 1; i < crossings.size(); i += 2) {
			int first = std::max(0, static_cast<int>(std::ceil(crossings[i - 1] - .5f)));
			int last = std::min(width - 1, static_cast<int>(std::floor(crossings[i] - .5f)));
			for (int x = first; x <= last; ++x) selected[static_cast<size_t>(y) * width + x] = 1;
		}
	}
	float scaled_radius = radius * raster_scale;
	float scaled_radius_squared = scaled_radius * scaled_radius;
	for (size_t segment = 0; segment < centres.size(); ++segment) {
		Vector2D from = segment ? centres[segment - 1] : centres.front();
		Vector2D to = centres[segment];
		float distance = (to - from).Len();
		int steps = distance > .01f ? std::max(1, static_cast<int>(std::ceil(distance / spacing))) : 0;
		for (int step = 0; step <= steps; ++step) {
			Vector2D centre = from + (to - from) *
				(steps ? static_cast<float>(step) / steps : 0.f);
			float local_x = (centre.X() - minimum_x) * raster_scale;
			float local_y = (centre.Y() - minimum_y) * raster_scale;
			int first_x = std::max(0, static_cast<int>(std::floor(local_x - scaled_radius)));
			int last_x = std::min(width - 1, static_cast<int>(std::ceil(local_x + scaled_radius)));
			int first_y = std::max(0, static_cast<int>(std::floor(local_y - scaled_radius)));
			int last_y = std::min(height - 1, static_cast<int>(std::ceil(local_y + scaled_radius)));
			for (int y = first_y; y <= last_y; ++y) {
				for (int x = first_x; x <= last_x; ++x) {
					float dx = x + .5f - local_x;
					float dy = y + .5f - local_y;
					if (dx * dx + dy * dy <= scaled_radius_squared)
						selected[static_cast<size_t>(y) * width + x] = add ? 1 : 0;
				}
			}
		}
	}

	for (auto it = affected.rbegin(); it != affected.rend(); ++it)
		contours.erase(contours.begin() + *it);
	auto rebuilt = trace_binary_mask(selected, width, height, 0, 0, .75);
	for (auto& contour : rebuilt) {
		for (auto& point : contour)
			point = Vector2D(minimum_x + point.X() / raster_scale,
				minimum_y + point.Y() / raster_scale);
		contours.push_back(std::move(contour));
	}
	return contours;
}

wxBitmap MakeVisualSelectionModeBitmap(VisualSelectionMode mode, int size) {
	size = std::max(size, 16);
	wxBitmap bitmap(size, size, 24);
	wxMemoryDC dc(bitmap);
	dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
	dc.Clear();
	int stroke = std::max(2, size / 8);
	dc.SetPen(wxPen(wxColour(20, 20, 20), stroke));
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	int centre = size / 2;
	int a = std::max(2, size / 8);
	int b = size - a - 1;
	bool add = mode == VisualSelectionMode::PipetteAdd ||
		mode == VisualSelectionMode::BrushAdd;
	if (mode == VisualSelectionMode::PipetteAdd ||
		mode == VisualSelectionMode::PipetteSubtract) {
		dc.DrawLine(a, b, b - size / 4, a + size / 4);
		dc.DrawLine(a, b, a + size / 5, b);
		dc.DrawLine(b - size / 4, a + size / 4, b, a);
	}
	else if (mode == VisualSelectionMode::BrushAdd) {
		dc.DrawEllipse(a, centre - size / 6, size / 2, size / 3);
		dc.DrawLine(centre, centre - size / 6, b - size / 8, a);
	}
	else {
		wxPoint eraser[]{{a, centre + size / 5}, {centre, a},
			{b, centre - size / 8}, {centre, b}};
		dc.DrawPolygon(4, eraser);
	}
	int symbol_x = b - size / 8;
	int symbol_y = b - size / 8;
	int arm = std::max(2, size / 7);
	dc.DrawLine(symbol_x - arm, symbol_y, symbol_x + arm, symbol_y);
	if (add) dc.DrawLine(symbol_x, symbol_y - arm, symbol_x, symbol_y + arm);
	dc.SelectObject(wxNullBitmap);
	return bitmap;
}

wxBitmap MakeVisualVectorClipBrushBitmap(bool add, int size, bool dropdown) {
	size = std::max(size, 16);
	wxBitmap bitmap(size, size, 24);
	wxMemoryDC dc(bitmap);
	dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
	dc.Clear();
	int margin = std::max(2, size / 10);
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	dc.SetPen(wxPen(wxColour(220, 45, 55), std::max(2, size / 10)));
	dc.DrawCircle(size / 2, size / 2, size / 2 - margin - 1);
	if (!add) {
		dc.SetPen(wxPen(wxColour(220, 45, 55), std::max(2, size / 10)));
		int low = size * 3 / 10;
		int high = size * 7 / 10;
		dc.DrawLine(low, high, high, low);
	}

	if (dropdown) {
		int corner = std::max(5, size / 4);
		wxColour background = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(background));
		dc.DrawRectangle(size - corner, size - corner, corner, corner);
		wxPoint triangle[]{{size - corner + 1, size - corner + 1},
			{size - 1, size - corner + 1}, {size - 1, size - 1}};
		dc.SetBrush(wxBrush(wxColour(25, 25, 25)));
		dc.DrawPolygon(3, triangle);
	}
	dc.SelectObject(wxNullBitmap);
	return bitmap;
}

wxBitmap MakeVisualAISelectionBitmap(int size) {
	size = std::max(size, 16);
	wxBitmap bitmap(size, size, 24);
	wxMemoryDC dc(bitmap);
	dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
	dc.Clear();
	dc.SetPen(wxPen(wxColour(20, 20, 20), std::max(2, size / 8)));
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	int centre = size / 2;
	int radius = std::max(3, size / 3);
	dc.DrawCircle(centre, centre, radius);
	dc.DrawCircle(centre, centre, std::max(1, radius / 3));
	dc.SelectObject(wxNullBitmap);
	return bitmap;
}

std::vector<std::vector<Vector2D>> GenerateVisualAISelection(
	wxWindow *parent, wxImage const& crop, std::string const& subject_prompt) {
	if (!crop.IsOk() || crop.GetWidth() < 3 || crop.GetHeight() < 3)
		throw ai::Error("The selected image range is empty.");
	if (ai::GetApiKey().empty())
		throw ai::Error("No working AI connection is configured.");

	auto encode_png = [](wxImage const& image) {
		wxMemoryOutputStream stream;
		if (!image.SaveFile(stream, wxBITMAP_TYPE_PNG))
			throw ai::Error("The AI selection image could not be encoded.");
		std::vector<unsigned char> data(stream.GetSize());
		if (!data.empty()) stream.CopyTo(data.data(), data.size());
		return data;
	};
	auto image_png = encode_png(crop);
	auto selection_model = OPT_GET("AI/OpenAI/Selection Model")->GetString();
	uint64_t fingerprint = 1469598103934665603ULL;
	for (unsigned char byte : image_png) {
		fingerprint ^= byte;
		fingerprint *= 1099511628211ULL;
	}
	struct CacheEntry {
		uint64_t fingerprint;
		size_t png_size;
		int width;
		int height;
		std::string model;
		std::string prompt;
		std::vector<ai::ImageContour> contours;
	};
	static std::deque<CacheEntry> cache;
	static std::mutex cache_mutex;
	std::vector<ai::ImageContour> ai_contours;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		auto cached = std::find_if(cache.begin(), cache.end(), [&](CacheEntry const& entry) {
			return entry.fingerprint == fingerprint && entry.png_size == image_png.size() &&
				entry.width == crop.GetWidth() && entry.height == crop.GetHeight() &&
				entry.model == selection_model && entry.prompt == subject_prompt;
		});
		if (cached != cache.end()) {
			CacheEntry entry = std::move(*cached);
			cache.erase(cached);
			cache.push_front(std::move(entry));
			ai_contours = cache.front().contours;
		}
	}
	bool request_cancelled = false;
	auto request_contours = [&] {
		std::vector<ai::ImageContour> requested;
		std::string image_url = "data:image/png;base64," +
			from_wx(wxBase64Encode(image_png.data(), image_png.size()));
		std::string error_message;
		DialogProgress progress(parent, _("AI selection"), _("Selecting the requested object..."));
		try {
			progress.Run([&](agi::ProgressSink *sink) {
				sink->SetIndeterminate();
				try {
					auto cancel = [sink] { return sink->IsCancelled(); };
					requested = ai::SelectImageContours(ai::GetApiKey(),
						selection_model, image_url, subject_prompt, cancel);
				}
				catch (std::exception const& error) {
					error_message = error.what();
				}
			});
		}
		catch (agi::UserCancelException const&) {
			request_cancelled = true;
			return std::vector<ai::ImageContour>{};
		}
		if (!error_message.empty()) throw ai::Error(error_message);
		return requested;
	};
	if (ai_contours.empty()) {
		ai_contours = request_contours();
		if (request_cancelled) return {};
	}

	auto render_contours = [&](std::vector<ai::ImageContour> const& source_contours) {
		std::vector<RasterContour> semantic_contours;
		semantic_contours.reserve(source_contours.size());
		for (auto const& source_contour : source_contours) {
			RasterContour contour;
			contour.add = source_contour.operation == ai::ImageContourOperation::Add;
			contour.points.reserve(source_contour.points.size());
			for (auto const& source_point : source_contour.points) {
				contour.points.emplace_back(
					static_cast<float>(source_point.x * crop.GetWidth() / 1000.0),
					static_cast<float>(source_point.y * crop.GetHeight() / 1000.0));
			}
			if (contour.points.size() >= 3)
				semantic_contours.push_back(std::move(contour));
		}
		if (semantic_contours.empty())
			return std::vector<std::vector<Vector2D>>{};
		// The AI outline is a semantic prior. Graph cut aligns a valid prior to
		// image pixels; owner filtering removes detached text/line-art islands.
		auto semantic_mask = rasterize_contour_operations(semantic_contours,
			crop.GetWidth(), crop.GetHeight());
		std::vector<unsigned char> accepted_subtract;
		auto refined_mask = refine_semantic_mask(crop, semantic_mask, accepted_subtract);
		auto cleaned_mask = retain_semantic_components(refined_mask, semantic_mask.owners,
			accepted_subtract, crop.GetWidth(), crop.GetHeight());
		// Pixel contours are axis-aligned by construction. Scale RDP very mildly
		// with the crop so one-pixel staircase noise is removed at HD sizes while
		// small hair tips, fingers and corners stay within a 1.25 px error bound.
		double contour_epsilon = std::clamp(.55 +
			std::min(crop.GetWidth(), crop.GetHeight()) / 3000.0, .6, .9);
		auto contours = trace_binary_mask(cleaned_mask,
			crop.GetWidth(), crop.GetHeight(), 0, 0, contour_epsilon);
		if (!contours.empty()) return contours;
		return snap_semantic_contours(crop, semantic_contours);
	};

	auto contours = render_contours(ai_contours);
	if (contours.empty())
		throw ai::Error("The AI selection could not be aligned to a visible object boundary.");

	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		cache.erase(std::remove_if(cache.begin(), cache.end(), [&](CacheEntry const& entry) {
			return entry.fingerprint == fingerprint && entry.png_size == image_png.size() &&
				entry.width == crop.GetWidth() && entry.height == crop.GetHeight() &&
				entry.model == selection_model && entry.prompt == subject_prompt;
		}), cache.end());
		cache.push_front({fingerprint, image_png.size(), crop.GetWidth(), crop.GetHeight(),
			selection_model, subject_prompt, ai_contours});
		while (cache.size() > 16) cache.pop_back();
	}
	return contours;
#if 0
	auto semantic_mask = rasterize_contour_operations(semantic_contours,
		crop.GetWidth(), crop.GetHeight());
	std::vector<unsigned char> accepted_subtract;
	auto refined_mask = refine_semantic_mask(crop, semantic_mask, accepted_subtract);
	auto topology_contours = trace_binary_mask(refined_mask,
		crop.GetWidth(), crop.GetHeight(), 0, 0, .7);
	if (topology_contours.empty()) return {};

	auto pixels = crop.GetData();
	auto sample = [&](Vector2D position, int channel) {
			int x = std::clamp(static_cast<int>(std::lround(position.X())), 0, crop.GetWidth() - 1);
			int y = std::clamp(static_cast<int>(std::lround(position.Y())), 0, crop.GetHeight() - 1);
			return static_cast<int>(pixels[(static_cast<size_t>(y) * crop.GetWidth() + x) * 3 + channel]);
	};
	auto edge_strength = [&](Vector2D point, Vector2D normal) {
		Vector2D outside = point + normal * 2.f;
		Vector2D inside = point - normal * 2.f;
		int strength = 0;
		for (int channel = 0; channel < 3; ++channel)
			strength += std::abs(sample(outside, channel) - sample(inside, channel));
		return strength;
	};
	auto band_edge_strength = [&](Vector2D point, Vector2D normal) {
		Vector2D tangent = normal.Perpendicular();
		double positive[3]{}, negative[3]{};
		int samples = 0;
		for (int distance = 2; distance <= 5; ++distance) {
			for (int tangent_offset = -1; tangent_offset <= 1; ++tangent_offset) {
				Vector2D along = tangent * static_cast<float>(tangent_offset);
				for (int channel = 0; channel < 3; ++channel) {
					positive[channel] += sample(point + normal * static_cast<float>(distance) + along, channel);
					negative[channel] += sample(point - normal * static_cast<float>(distance) + along, channel);
				}
				++samples;
			}
		}
		double score = 0.0;
		for (int channel = 0; channel < 3; ++channel) {
			double difference = (positive[channel] - negative[channel]) / samples;
			score += difference * difference;
		}
		return std::sqrt(score);
	};
	auto local_edge_strength = [&](int x, int y) {
		x = std::clamp(x, 1, crop.GetWidth() - 2);
		y = std::clamp(y, 1, crop.GetHeight() - 2);
		auto offset = [&](int px, int py) {
			return (static_cast<size_t>(py) * crop.GetWidth() + px) * 3;
		};
		int strength = 0;
		for (int channel = 0; channel < 3; ++channel) {
			strength += std::abs(static_cast<int>(pixels[offset(x + 1, y) + channel]) -
				static_cast<int>(pixels[offset(x - 1, y) + channel]));
			strength += std::abs(static_cast<int>(pixels[offset(x, y + 1) + channel]) -
				static_cast<int>(pixels[offset(x, y - 1) + channel]));
		}
		return strength;
	};
	auto snap_contours_to_image = [&](std::vector<std::vector<Vector2D>> const& sources,
		std::vector<unsigned char> const& boundary_mask, int radius,
		double simplify_epsilon) {
		std::vector<std::vector<Vector2D>> snapped;
		for (auto base : sources) {
			if (base.size() < 3) continue;
			bool positive_winding = polygon_area(base) > 0.0;
			std::vector<Vector2D> dense;
			float point_spacing = std::clamp(
				std::min(crop.GetWidth(), crop.GetHeight()) / 160.f, 1.5f, 4.f);
			for (size_t i = 0; i < base.size(); ++i) {
				Vector2D a = base[i];
				Vector2D b = base[(i + 1) % base.size()];
				int steps = std::max(1, static_cast<int>(std::ceil((b - a).Len() / point_spacing)));
				for (int step = 0; step < steps; ++step)
					dense.push_back(a + (b - a) * (static_cast<float>(step) / steps));
			}
			base = std::move(dense);
			std::vector<Vector2D> normals(base.size());
			std::vector<float> offsets(base.size());
			std::vector<unsigned char> crop_edge(base.size());
			for (size_t i = 0; i < base.size(); ++i) {
				Vector2D tangent = base[(i + 1) % base.size()] -
					base[(i + base.size() - 1) % base.size()];
				normals[i] = tangent.SquareLen() > 1e-4f ?
					tangent.Perpendicular().Unit() : Vector2D(0.f, 1.f);
				// A subject cut by the selected range must stay exactly on that
				// range edge. Sampling a clamped image outside it otherwise pulls
				// the contour inward and creates a ragged crop boundary.
				bool on_crop_edge = base[i].X() <= .01f || base[i].Y() <= .01f ||
					base[i].X() >= crop.GetWidth() - .01f ||
					base[i].Y() >= crop.GetHeight() - .01f;
				crop_edge[i] = on_crop_edge;
				if (on_crop_edge) continue;
				float best_offset = 0.f;
				double best_score = -1e9;
				for (int offset = -radius; offset <= radius; ++offset) {
					Vector2D candidate = base[i] + normals[i] * static_cast<float>(offset);
					int x = std::clamp(static_cast<int>(std::lround(candidate.X())),
						1, crop.GetWidth() - 2);
					int y = std::clamp(static_cast<int>(std::lround(candidate.Y())),
						1, crop.GetHeight() - 2);
					auto selected_at = [&](Vector2D position) {
						int mask_x = std::clamp(static_cast<int>(std::floor(position.X())),
							0, crop.GetWidth() - 1);
						int mask_y = std::clamp(static_cast<int>(std::floor(position.Y())),
							0, crop.GetHeight() - 1);
						return boundary_mask[static_cast<size_t>(mask_y) * crop.GetWidth() + mask_x] != 0;
					};
					bool separates_mask = selected_at(candidate + normals[i] * 1.5f) !=
						selected_at(candidate - normals[i] * 1.5f);
					double score = band_edge_strength(candidate, normals[i]) * 3.0 +
						edge_strength(candidate, normals[i]) * .6 +
						local_edge_strength(x, y) * .08 - std::abs(offset) * 6.0 -
						(separates_mask ? 0.0 : 1000.0);
					if (score > best_score) {
						best_score = score;
						best_offset = static_cast<float>(offset);
					}
				}
				offsets[i] = best_offset;
			}
			std::vector<Vector2D> contour;
			contour.reserve(base.size());
			for (size_t i = 0; i < base.size(); ++i) {
				float smooth_offset = (offsets[(i + base.size() - 2) % base.size()] +
					offsets[(i + base.size() - 1) % base.size()] * 2.f + offsets[i] * 4.f +
					offsets[(i + 1) % base.size()] * 2.f + offsets[(i + 2) % base.size()]) * .1f;
				Vector2D point = crop_edge[i] ? base[i] :
					base[i] + normals[i] * smooth_offset;
				contour.emplace_back(std::clamp(point.X(), 0.f, static_cast<float>(crop.GetWidth())),
					std::clamp(point.Y(), 0.f, static_cast<float>(crop.GetHeight())));
			}
			contour = simplify_closed(std::move(contour), simplify_epsilon);
			if (contour.size() < 3) continue;
			if ((polygon_area(contour) > 0.0) != positive_winding)
				std::reverse(contour.begin(), contour.end());
			snapped.push_back(std::move(contour));
		}
		return snapped;
	};
	int snap_radius = std::clamp(static_cast<int>(std::lround(
		std::min(crop.GetWidth(), crop.GetHeight()) * .015)), 2, 8);
	auto contours = snap_contours_to_image(topology_contours, refined_mask,
		snap_radius, .45);
	if (contours.empty()) return {};
	// Build every outer component with only its own holes, then union the
	// components. Unlike global even/odd or union-minus-all-holes, this preserves
	// both overlapping foreground components and positive islands inside holes.
	std::vector<unsigned char> canonical_mask(
		static_cast<size_t>(crop.GetWidth()) * crop.GetHeight(), 0);
	bool has_outer = false;
	for (auto const& outer : contours) {
		if (polygon_area(outer) <= 0.0) continue;
		has_outer = true;
		std::vector<unsigned char> component(canonical_mask.size(), 0);
		apply_raster_polygon(component, crop.GetWidth(), crop.GetHeight(), outer, 1);
		for (auto const& hole : contours) {
			if (polygon_area(hole) >= 0.0 || hole.empty()) continue;
			if (polygon_contains_point(outer, hole.front()))
				apply_raster_polygon(component, crop.GetWidth(), crop.GetHeight(), hole, 0);
		}
		for (size_t i = 0; i < canonical_mask.size(); ++i)
			canonical_mask[i] = canonical_mask[i] || component[i];
	}
	if (!has_outer) return {};
	for (size_t i = 0; i < canonical_mask.size() && i < accepted_subtract.size(); ++i)
		if (accepted_subtract[i]) canonical_mask[i] = 0;
	// Reassert crop-edge pixels from the topology-safe refined mask. This keeps
	// a subject cut by the selected range attached to the exact range boundary,
	// even if simplification removed an intermediate edge anchor.
	auto restore_crop_pixel = [&](int x, int y) {
		size_t index = static_cast<size_t>(y) * crop.GetWidth() + x;
		if (refined_mask[index] && !accepted_subtract[index]) canonical_mask[index] = 1;
	};
	for (int x = 0; x < crop.GetWidth(); ++x) {
		restore_crop_pixel(x, 0);
		restore_crop_pixel(x, crop.GetHeight() - 1);
	}
	for (int y = 0; y < crop.GetHeight(); ++y) {
		restore_crop_pixel(0, y);
		restore_crop_pixel(crop.GetWidth() - 1, y);
	}

	// Remove only single-pixel teeth and pinholes. This is deliberately one
	// symmetric pass: it cleans raster artefacts without rounding hair tips,
	// fingers, corners, or any protected background opening.
	auto before_cleanup = canonical_mask;
	for (int y = 1; y + 1 < crop.GetHeight(); ++y) {
		for (int x = 1; x + 1 < crop.GetWidth(); ++x) {
			size_t index = static_cast<size_t>(y) * crop.GetWidth() + x;
			int neighbours = 0;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
					if (dx || dy) neighbours += before_cleanup[
						static_cast<size_t>(y + dy) * crop.GetWidth() + x + dx] != 0;
			if (before_cleanup[index] && neighbours == 0)
				canonical_mask[index] = 0;
			else if (!before_cleanup[index] && !accepted_subtract[index] && neighbours >= 7)
				canonical_mask[index] = 1;
		}
	}
	for (size_t i = 0; i < canonical_mask.size() && i < accepted_subtract.size(); ++i)
		if (accepted_subtract[i]) canonical_mask[i] = 0;

	// The topology pass has to rasterize overlapping semantic regions. Remove
	// sub-pixel staircase deviations while keeping the canonical mask's exact
	// component/hole relationships; another independent snap could cross a thin
	// gap or make an outer loop intersect its hole.
	return trace_binary_mask(canonical_mask,
		crop.GetWidth(), crop.GetHeight(), 0, 0, .85);
#endif
}
