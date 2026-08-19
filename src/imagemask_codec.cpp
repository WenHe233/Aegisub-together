// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "imagemask_codec.h"

#include "ass_dialogue.h"

#include <libaegisub/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <regex>

namespace imagemask {
namespace {

constexpr char effect_name[] = "imagemask-fx";
constexpr size_t maximum_drawing_bytes = 48000;

struct Colour {
	unsigned char red = 0;
	unsigned char green = 0;
	unsigned char blue = 0;
	unsigned char alpha = 0;

	uint32_t Key() const {
		return (static_cast<uint32_t>(red) << 24) |
			(static_cast<uint32_t>(green) << 16) |
			(static_cast<uint32_t>(blue) << 8) | alpha;
	}
};

struct Run {
	int start = 0;
	int end = 0;
	Colour colour;

};

int Hex(std::string const& value, int fallback) {
	try { return std::stoi(value, nullptr, 16); }
	catch (...) { return fallback; }
}

Colour TagsColour(std::string const& tags, Colour fallback = {255, 255, 255, 255}) {
	static std::regex colour_pattern(R"(\\(?:1c|c)&H([0-9A-Fa-f]{6})&)");
	static std::regex alpha_pattern(R"(\\1a&H([0-9A-Fa-f]{2})&)");
	std::smatch match;
	if (std::regex_search(tags, match, colour_pattern)) {
		int bgr = Hex(match[1].str(), 0xffffff);
		fallback.blue = static_cast<unsigned char>((bgr >> 16) & 0xff);
		fallback.green = static_cast<unsigned char>((bgr >> 8) & 0xff);
		fallback.red = static_cast<unsigned char>(bgr & 0xff);
	}
	if (std::regex_search(tags, match, alpha_pattern))
		fallback.alpha = static_cast<unsigned char>(255 - Hex(match[1].str(), 0));
	return fallback;
}

std::pair<int, int> Position(std::string const& tags) {
	static std::regex pattern(R"(\\pos\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*\))");
	std::smatch match;
	if (!std::regex_search(tags, match, pattern)) return {0, 0};
	try {
		return {static_cast<int>(std::lround(std::stod(match[1].str()))),
			static_cast<int>(std::lround(std::stod(match[2].str())))};
	}
	catch (...) { return {0, 0}; }
}

std::string StyleTags(int x, int y, Colour colour) {
	return agi::format("{\\an7\\pos(%d,%d)\\bord0\\shad0\\blur0\\be0"
		"\\fscx100\\fscy100\\frz0\\frx0\\fry0\\fax0\\fay0"
		"\\1c&H%02X%02X%02X&\\1a&H%02X&\\p1}",
		x, y, colour.blue, colour.green, colour.red, 255 - colour.alpha);
}

std::string Rectangle(int left, int top, int right, int bottom) {
	return agi::format("m %d %d l %d %d %d %d %d %d", left, top,
		left, bottom, right, bottom, right, top);
}

AssDialogue MakeLine(AssDialogue const& prototype, int start_ms, int end_ms,
	std::string text) {
	AssDialogue line(prototype);
	line.Comment = false;
	line.Start = start_ms;
	line.End = end_ms;
	line.Effect = effect_name;
	line.ExtradataIds = std::vector<uint32_t>();
	line.Text = std::move(text);
	return line;
}

Raster Crop(Raster const& source) {
	if (!source.IsOk()) return {};
	int left = source.width, top = source.height, right = 0, bottom = 0;
	for (int y = 0; y < source.height; ++y) {
		for (int x = 0; x < source.width; ++x) {
			size_t offset = (static_cast<size_t>(y) * source.width + x) * 4;
			if (!source.rgba[offset + 3]) continue;
			left = std::min(left, x);
			top = std::min(top, y);
			right = std::max(right, x + 1);
			bottom = std::max(bottom, y + 1);
		}
	}
	if (left >= right || top >= bottom) return {};
	Raster out;
	out.x = source.x + left;
	out.y = source.y + top;
	out.width = right - left;
	out.height = bottom - top;
	out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
	for (int y = 0; y < out.height; ++y) {
		auto from = source.rgba.begin() +
			(static_cast<size_t>(y + top) * source.width + left) * 4;
		auto to = out.rgba.begin() + static_cast<size_t>(y) * out.width * 4;
		std::copy_n(from, static_cast<size_t>(out.width) * 4, to);
	}
	return out;
}

std::vector<std::vector<Run>> MakeRuns(Raster const& image,
	ProgressCallback const& progress, size_t progress_total) {
	std::vector<std::vector<Run>> rows(static_cast<size_t>(image.height));
	for (int y = 0; y < image.height; ++y) {
		auto& runs = rows[y];
		for (int x = 0; x < image.width; ++x) {
			size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
			Colour colour;
			colour.alpha = image.rgba[offset + 3];
			if (colour.alpha) {
				colour.red = image.rgba[offset];
				colour.green = image.rgba[offset + 1];
				colour.blue = image.rgba[offset + 2];
			}
			if (!runs.empty() && runs.back().colour.Key() == colour.Key())
				runs.back().end = x + 1;
			else
				runs.push_back({x, x + 1, colour});
		}
		if (progress) progress(static_cast<size_t>(y) + 1, progress_total);
	}
	return rows;
}

std::vector<AssDialogue> EncodeRows(Raster const& image,
	std::vector<std::vector<Run>> const& rows, AssDialogue const& prototype,
	int start_ms, int end_ms, ProgressCallback const& progress,
	size_t progress_offset, size_t progress_total) {
	std::vector<AssDialogue> lines;
	for (int y = 0; y < image.height;) {
		int band_end = y + 1;
		auto first = std::find_if(rows[y].begin(), rows[y].end(),
			[](Run const& run) { return run.colour.alpha != 0; });
		auto last = std::find_if(rows[y].rbegin(), rows[y].rend(),
			[](Run const& run) { return run.colour.alpha != 0; });
		if (first == rows[y].end()) {
			y = band_end;
			if (progress) progress(progress_offset + y, progress_total);
			continue;
		}
		size_t first_index = static_cast<size_t>(first - rows[y].begin());
		size_t last_index = rows[y].size() - 1 -
			static_cast<size_t>(last - rows[y].rbegin());
		for (size_t index = first_index; index <= last_index;) {
			// A transparent run at the beginning of a split block can be represented
			// exactly by moving that block's integer position to the next visible run.
			while (index <= last_index && !rows[y][index].colour.alpha) ++index;
			if (index > last_index) break;
			int block_start = rows[y][index].start;
			Colour current = rows[y][index].colour;
			std::string text = StyleTags(image.x + block_start, image.y + y, current);
			bool has_shape = false;
			for (; index <= last_index; ++index) {
				auto const& run = rows[y][index];
				std::string addition;
				if (run.colour.Key() != current.Key()) {
					addition = agi::format("{\\1c&H%02X%02X%02X&\\1a&H%02X&}",
						run.colour.blue, run.colour.green, run.colour.red,
						255 - run.colour.alpha);
				}
				addition += Rectangle(0, 0, run.end - run.start, band_end - y);
				if (has_shape && text.size() + addition.size() > maximum_drawing_bytes)
					break;
				text += std::move(addition);
				has_shape = true;
				current = run.colour;
			}
			lines.push_back(MakeLine(prototype, start_ms, end_ms, std::move(text)));
		}
		y = band_end;
		if (progress) progress(progress_offset + y, progress_total);
	}
	return lines;
}

struct PaintedRect {
	int left;
	int top;
	int right;
	int bottom;
	Colour colour;
};

std::vector<PaintedRect> ReadLine(AssDialogue const& line) {
	std::vector<PaintedRect> out;
	std::string text = line.Text.get();
	size_t close = text.find('}');
	if (close == std::string::npos) return out;
	std::string header = text.substr(0, close + 1);
	auto [pos_x, pos_y] = Position(header);
	Colour colour = TagsColour(header);
	std::string body = text.substr(close + 1);
	bool row_layout = body.find('{') != std::string::npos;

	static std::regex token_pattern(
		R"(\{([^}]*)\}|m\s+(-?\d+)\s+(-?\d+)\s+l\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+))",
		std::regex::icase);
	int cursor_x = pos_x;
	for (auto at = std::sregex_iterator(body.begin(), body.end(), token_pattern);
		at != std::sregex_iterator(); ++at) {
		auto const& match = *at;
		if (match[1].matched) {
			colour = TagsColour(match[1].str(), colour);
			continue;
		}
		try {
			int x0 = std::stoi(match[2].str());
			int y0 = std::stoi(match[3].str());
			int y1 = std::stoi(match[5].str());
			int x1 = std::stoi(match[6].str());
			if (row_layout) {
				int width = std::abs(x1 - x0);
				int height = std::max(1, std::abs(y1 - y0));
				out.push_back({cursor_x, pos_y, cursor_x + width, pos_y + height, colour});
				cursor_x += width;
			}
			else {
				out.push_back({pos_x + std::min(x0, x1), pos_y + std::min(y0, y1),
					pos_x + std::max(x0, x1), pos_y + std::max(y0, y1), colour});
			}
		}
		catch (...) { }
	}
	return out;
}

} // namespace

bool IsLine(AssDialogue const *line) {
	if (!line) return false;
	if (line->Effect.get() == effect_name) return true;
	std::string const& text = line->Text.get();
	if (text.find("\\p1") == std::string::npos ||
		text.find("m 0 0 l 0 ") == std::string::npos) return false;
	static std::regex legacy(R"(m 0 0 l 0 \d+(?:\.\d+)? \d+(?:\.\d+)? \d+(?:\.\d+)? \d+(?:\.\d+)? 0)");
	return std::regex_search(text, legacy);
}

std::optional<Raster> Decode(std::vector<AssDialogue *> const& lines) {
	std::vector<PaintedRect> rectangles;
	for (auto line : lines) {
		if (!IsLine(line)) continue;
		auto found = ReadLine(*line);
		std::move(found.begin(), found.end(), std::back_inserter(rectangles));
	}
	if (rectangles.empty()) return std::nullopt;
	int left = std::numeric_limits<int>::max();
	int top = std::numeric_limits<int>::max();
	int right = std::numeric_limits<int>::min();
	int bottom = std::numeric_limits<int>::min();
	for (auto const& rect : rectangles) {
		left = std::min(left, rect.left);
		top = std::min(top, rect.top);
		right = std::max(right, rect.right);
		bottom = std::max(bottom, rect.bottom);
	}
	if (left >= right || top >= bottom) return std::nullopt;
	Raster raster;
	raster.x = left;
	raster.y = top;
	raster.width = right - left;
	raster.height = bottom - top;
	raster.rgba.assign(static_cast<size_t>(raster.width) * raster.height * 4, 0);
	for (auto const& rect : rectangles) {
		for (int y = std::max(top, rect.top); y < std::min(bottom, rect.bottom); ++y) {
			for (int x = std::max(left, rect.left); x < std::min(right, rect.right); ++x) {
				size_t offset = (static_cast<size_t>(y - top) * raster.width + x - left) * 4;
				raster.rgba[offset] = rect.colour.red;
				raster.rgba[offset + 1] = rect.colour.green;
				raster.rgba[offset + 2] = rect.colour.blue;
				raster.rgba[offset + 3] = rect.colour.alpha;
			}
		}
	}
	return raster;
}

std::vector<AssDialogue> Encode(Raster const& source, AssDialogue const& prototype,
	int start_ms, int end_ms, ProgressCallback progress) {
	Raster image = Crop(source);
	if (!image.IsOk()) return {};
	size_t progress_total = static_cast<size_t>(image.height) * 2;
	auto rows = MakeRuns(image, progress, progress_total);
	return EncodeRows(image, rows, prototype, start_ms, end_ms, progress,
		static_cast<size_t>(image.height), progress_total);
}

std::string Signature(std::vector<AssDialogue> const& lines) {
	std::string out;
	for (auto const& line : lines) {
		out += line.Style.get();
		out.push_back('\x1f');
		out += std::to_string(line.Layer);
		out.push_back('\x1f');
		out += line.Text.get();
		out.push_back('\x1e');
	}
	return out;
}

} // namespace imagemask
