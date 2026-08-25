// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

/// @file typesetting_glitch.cpp
/// @brief Deterministic, editable ASS slice glitches for typesetting objects

#include "typesetting_glitch.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "compat.h"
#include "imagemask_codec.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "selection_controller.h"
#include "subtitle_line_combiner.h"
#include "typesetting_motion.h"
#include "typesetting_transform.h"
#include "video_controller.h"

#include <libaegisub/format.h>
#include <libaegisub/ass/uuencode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace typesetting::glitch {
namespace {

constexpr char const *effect_name = "glitch-fx";
constexpr char const *data_key = "aegisub/glitch-fx";
constexpr char const *source_key = "aegisub/glitch-fx-source";
constexpr std::string_view clipboard_settings = "{:Aegisub Glitch Settings:";
constexpr std::string_view clipboard_sources = "{:Aegisub Glitch Sources:";

Values Clamp(Values value) {
	value.amount = std::clamp(value.amount, 0.0, 100.0);
	value.offset = std::clamp(value.offset, 0.0, 100.0);
	value.opacity = std::clamp(value.opacity, 0.0, 1.0);
	value.height = std::clamp(value.height, 1, 200);
	value.width = std::clamp(value.width, 1.0, 100.0);
	value.angle = std::fmod(value.angle, 360.0);
	if (value.angle < 0.0) value.angle += 360.0;
	return value;
}

std::string SerializeValues(Values value) {
	value = Clamp(value);
	return agi::format("%.8g,%.8g,%.8g,%d,%.8g,%.8g", value.amount, value.offset,
		value.opacity, value.height, value.width, value.angle);
}

std::optional<Values> DeserializeValues(std::string const& encoded) {
	try {
		std::vector<std::string> parts;
		size_t at = 0;
		for (;;) {
			size_t comma = encoded.find(',', at);
			parts.push_back(encoded.substr(at,
				comma == std::string::npos ? std::string::npos : comma - at));
			if (comma == std::string::npos) break;
			at = comma + 1;
		}
		if (parts.size() == 3) {
			double amount = std::stod(parts[0]);
			return Clamp({amount, amount, std::stod(parts[1]), std::stoi(parts[2]), 100.0, 90.0});
		}
		if (parts.size() == 4)
			return Clamp({std::stod(parts[0]), std::stod(parts[1]), std::stod(parts[2]),
				std::stoi(parts[3]), 100.0, 90.0});
		if (parts.size() == 5)
			return Clamp({std::stod(parts[0]), std::stod(parts[1]), std::stod(parts[2]),
				std::stoi(parts[3]), 100.0, std::stod(parts[4])});
		if (parts.size() != 6) return std::nullopt;
		return Clamp({std::stod(parts[0]), std::stod(parts[1]), std::stod(parts[2]),
			std::stoi(parts[3]), std::stod(parts[4]), std::stod(parts[5])});
	}
	catch (...) { return std::nullopt; }
}

std::string SerializeSettings(Settings settings) {
	settings.base = Clamp(settings.base);
	auto color_text = [](std::optional<agi::Color> const& color) {
		return color ? color->GetHexFormatted() : std::string();
	};
	std::string animations;
	for (auto animation : settings.animations) {
		animation.from = Clamp(animation.from);
		animation.to = Clamp(animation.to);
		if (!animations.empty()) animations += ';';
		animations += agi::format("%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s,%s,%d,%s,%s",
			animation.enabled,
			static_cast<int>(animation.timing), animation.start_time, animation.end_time,
			animation.frame, animation.use_default_effect_type,
			static_cast<int>(animation.effect_type), animation.use_default_color_style,
			static_cast<int>(animation.color_style), color_text(animation.custom_colors[0]),
			color_text(animation.custom_colors[1]), color_text(animation.custom_colors[2]),
			animation.show_base, SerializeValues(animation.from), SerializeValues(animation.to));
	}
	return agi::format("9|%d|%d|%u|%d|%s,%s,%s|%s|%s",
		static_cast<int>(settings.effect_type), static_cast<int>(settings.color_style),
		settings.seed, settings.show_base, color_text(settings.custom_colors[0]),
		color_text(settings.custom_colors[1]), color_text(settings.custom_colors[2]),
		SerializeValues(settings.base), animations);
}

std::optional<Settings> DeserializeSettings(std::string const& encoded) {
	std::vector<std::string> fields;
	size_t at = 0;
	for (;;) {
		size_t end = encoded.find('|', at);
		fields.push_back(encoded.substr(at,
			end == std::string::npos ? std::string::npos : end - at));
		if (end == std::string::npos) break;
		at = end + 1;
	}
	if (fields.size() != 8 || fields[0] != "9") return std::nullopt;
	try {
		Settings settings;
		settings.effect_type = static_cast<EffectType>(std::clamp(std::stoi(fields[1]), 0, 8));
		settings.color_style = static_cast<ColorStyle>(std::clamp(std::stoi(fields[2]), 0, 6));
		settings.seed = static_cast<uint32_t>(std::stoul(fields[3]));
		settings.show_base = std::stoi(fields[4]) != 0;
		std::vector<std::string> colors;
		size_t color_at = 0;
		for (;;) {
			size_t comma = fields[5].find(',', color_at);
			colors.push_back(fields[5].substr(color_at,
				comma == std::string::npos ? std::string::npos : comma - color_at));
			if (comma == std::string::npos) break;
			color_at = comma + 1;
		}
		if (colors.size() != 3) return std::nullopt;
		for (size_t i = 0; i < settings.custom_colors.size(); ++i)
			if (!colors[i].empty()) settings.custom_colors[i] = agi::Color(colors[i]);
		auto base = DeserializeValues(fields[6]);
		if (!base) return std::nullopt;
		settings.base = *base;
		size_t item_at = 0;
		while (item_at < fields[7].size()) {
			size_t item_end = fields[7].find(';', item_at);
			std::string item = fields[7].substr(item_at,
				item_end == std::string::npos ? std::string::npos : item_end - item_at);
			std::vector<std::string> parts;
			size_t part_at = 0;
			for (;;) {
				size_t comma = item.find(',', part_at);
				parts.push_back(item.substr(part_at,
					comma == std::string::npos ? std::string::npos : comma - part_at));
				if (comma == std::string::npos) break;
				part_at = comma + 1;
			}
			if (parts.size() != 25) return std::nullopt;
			Animation animation;
			animation.enabled = std::stoi(parts[0]) != 0;
			animation.show_base = settings.show_base;
			animation.timing = std::stoi(parts[1]) == 1 ?
				AnimationTiming::Frame : AnimationTiming::Range;
			animation.start_time = std::clamp(std::stoi(parts[2]), 0, 3600000);
			animation.end_time = std::clamp(std::stoi(parts[3]), 0, 3600000);
			animation.frame = std::clamp(std::stoi(parts[4]), 0, 1000000);
			animation.use_default_effect_type = std::stoi(parts[5]) != 0;
			animation.effect_type = static_cast<EffectType>(
				std::clamp(std::stoi(parts[6]), 0, 8));
			animation.use_default_color_style = std::stoi(parts[7]) != 0;
			animation.color_style = static_cast<ColorStyle>(
				std::clamp(std::stoi(parts[8]), 0, 6));
			for (size_t i = 0; i < animation.custom_colors.size(); ++i)
				if (!parts[9 + i].empty()) animation.custom_colors[i] = agi::Color(parts[9 + i]);
			animation.show_base = std::stoi(parts[12]) != 0;
			std::string from_text = parts[13] + "," + parts[14] + "," + parts[15] + "," +
				parts[16] + "," + parts[17] + "," + parts[18];
			std::string to_text = parts[19] + "," + parts[20] + "," + parts[21] + "," +
				parts[22] + "," + parts[23] + "," + parts[24];
			auto from = DeserializeValues(from_text);
			auto to = DeserializeValues(to_text);
			if (!from || !to) return std::nullopt;
			animation.from = *from;
			animation.to = *to;
			settings.animations.push_back(animation);
			if (item_end == std::string::npos) break;
			item_at = item_end + 1;
		}
		return settings;
	}
	catch (...) { return std::nullopt; }
}

std::optional<std::string> Extra(AssFile const& file, AssDialogue const& line,
		char const *key) {
	for (auto const& extra : file.GetExtradata(line.ExtradataIds))
		if (extra.key == key) return extra.value;
	return std::nullopt;
}

std::string ClipboardMarker(std::string_view prefix, std::string const& value) {
	return std::string(prefix) + agi::ass::UUEncode(value.data(),
		value.data() + value.size(), false) + "}";
}

std::optional<std::string> TakeClipboardMarker(std::string& text,
		std::string_view prefix) {
	auto start = text.find(prefix);
	if (start == std::string::npos) return std::nullopt;
	auto end = text.find('}', start + prefix.size());
	if (end == std::string::npos) return std::nullopt;
	auto encoded = std::string_view(text).substr(start + prefix.size(),
		end - start - prefix.size());
	auto decoded = agi::ass::UUDecode(encoded.data(), encoded.data() + encoded.size());
	text.erase(start, end - start + 1);
	return std::string(decoded.begin(), decoded.end());
}

std::string SerializeSources(std::vector<AssDialogue *> const& sources) {
	std::string out;
	for (auto source : sources) {
		std::string entry = source->GetEntryData();
		out += std::to_string(entry.size()) + ':' + entry;
	}
	return out;
}

std::vector<std::unique_ptr<AssDialogue>> DeserializeSources(std::string const& encoded) {
	std::vector<std::unique_ptr<AssDialogue>> out;
	size_t at = 0;
	while (at < encoded.size()) {
		size_t colon = encoded.find(':', at);
		if (colon == std::string::npos) return {};
		size_t size = 0;
		try { size = static_cast<size_t>(std::stoull(encoded.substr(at, colon - at))); }
		catch (...) { return {}; }
		at = colon + 1;
		if (size > encoded.size() - at) return {};
		try { out.push_back(std::make_unique<AssDialogue>(encoded.substr(at, size))); }
		catch (...) { return {}; }
		at += size;
	}
	return out;
}

struct SourceGroup {
	AssDialogue *anchor = nullptr;
	std::vector<std::unique_ptr<AssDialogue>> stored;
	std::vector<AssDialogue *> sources;
	std::vector<AssDialogue *> existing;
	bool editing = false;
};

std::vector<SourceGroup> CollectGroups(agi::Context *c, bool combine = true) {
	auto selected = c->selectionController->GetSelectedSet();
	std::vector<AssDialogue *> rows;
	for (auto& line : c->ass->Events) rows.push_back(&line);

	std::set<AssDialogue *> claimed;
	std::vector<SourceGroup> pieces;
	for (size_t i = 0; i < rows.size();) {
		auto anchor = rows[i];
		if (!IsSource(*c->ass, anchor)) { ++i; continue; }
		SourceGroup group;
		group.anchor = anchor;
		group.editing = true;
		group.existing.push_back(anchor);
		size_t j = i + 1;
		while (j < rows.size() && IsEffect(rows[j]) &&
			!IsSource(*c->ass, rows[j]))
			group.existing.push_back(rows[j++]);
		bool wanted = std::any_of(group.existing.begin(), group.existing.end(),
			[&](AssDialogue *line) { return selected.count(line) != 0; });
		claimed.insert(group.existing.begin(), group.existing.end());
		if (wanted) {
			auto encoded = Extra(*c->ass, *anchor, source_key);
			if (encoded) group.stored = DeserializeSources(*encoded);
			for (auto const& line : group.stored) {
				// Entry-data serialization does not retain Row. Preview-only lines use it
				// to preserve ASS event ordering, so place every restored source at the
				// generated group's anchor just as Apply does for committed output.
				line->Row = anchor->Row;
				group.sources.push_back(line.get());
			}
			if (!group.sources.empty()) pieces.push_back(std::move(group));
		}
		i = j;
	}

	for (auto line : c->selectionController->GetSortedSelection()) {
		if (claimed.count(line) || IsEffect(line)) continue;
		SourceGroup group;
		group.anchor = line;
		std::vector<AssDialogue *> members;
		if (c->imageMask) {
			auto const& combined = c->imageMask->GetGroupLines(line);
			if (!combined.empty()) members.assign(combined.begin(), combined.end());
		}
		if (members.empty()) members.push_back(line);
		if (std::any_of(members.begin(), members.end(),
			[&](AssDialogue *member) { return claimed.count(member) != 0; })) continue;
		group.anchor = members.front();
		group.sources = members;
		group.existing = members;
		claimed.insert(members.begin(), members.end());
		pieces.push_back(std::move(group));
	}
	std::stable_sort(pieces.begin(), pieces.end(), [](SourceGroup const& left,
		SourceGroup const& right) { return left.anchor->Row < right.anchor->Row; });
	if (pieces.empty()) return {};
	if (!combine) return pieces;

	// One command invocation creates one logical glitch object. This is important both for
	// editing the settings later and for displaying every selected source as one collapsed row.
	SourceGroup combined;
	combined.anchor = pieces.front().anchor;
	for (auto& piece : pieces) {
		combined.editing = combined.editing || piece.editing;
		combined.sources.insert(combined.sources.end(), piece.sources.begin(),
			piece.sources.end());
		combined.existing.insert(combined.existing.end(), piece.existing.begin(),
			piece.existing.end());
		for (auto& stored : piece.stored)
			combined.stored.push_back(std::move(stored));
	}
	std::vector<SourceGroup> groups;
	groups.push_back(std::move(combined));
	return groups;
}

struct Bounds {
	double left = 0;
	double top = 0;
	double right = 1;
	double bottom = 1;
};

Bounds GroupBounds(agi::Context *c, std::vector<AssDialogue *> const& sources) {
	int script_w = 0, script_h = 0;
	c->ass->GetResolution(script_w, script_h);
	Bounds bounds{0, 0, static_cast<double>(script_w), static_cast<double>(script_h)};
	if (auto raster = imagemask::Decode(sources); raster && raster->IsOk())
		return {static_cast<double>(raster->x), static_cast<double>(raster->y),
			static_cast<double>(raster->x + raster->width),
			static_cast<double>(raster->y + raster->height)};

	typesetting::ShapeEditor editor(c, sources);
	if (!editor.ok()) return bounds;
	editor.Build([](Vector2D point) { return point; }, false, false, false, true);
	Vector2D corners[4];
	editor.Box().Corners(corners);
	bounds = {corners[0].X(), corners[0].Y(), corners[0].X(), corners[0].Y()};
	for (auto point : corners) {
		bounds.left = std::min(bounds.left, static_cast<double>(point.X()));
		bounds.top = std::min(bounds.top, static_cast<double>(point.Y()));
		bounds.right = std::max(bounds.right, static_cast<double>(point.X()));
		bounds.bottom = std::max(bounds.bottom, static_cast<double>(point.Y()));
	}
	bounds.left = std::clamp(bounds.left - 2.0, 0.0, static_cast<double>(script_w));
	bounds.top = std::clamp(bounds.top - 2.0, 0.0, static_cast<double>(script_h));
	bounds.right = std::clamp(bounds.right + 2.0, 0.0, static_cast<double>(script_w));
	bounds.bottom = std::clamp(bounds.bottom + 2.0, 0.0, static_cast<double>(script_h));
	return bounds;
}

using ParamVector = std::vector<AssOverrideParameter> const *;

Vector2D LinePosition(agi::Context *c, AssDialogue const& line) {
	AssDialogue reading(line);
	auto blocks = reading.ParseTags();
	auto find = [&](char const *name) -> ParamVector {
		for (auto const& block : blocks) {
			if (block->GetType() != AssBlockType::OVERRIDE) continue;
			for (auto const& tag : static_cast<AssDialogueBlockOverride *>(block.get())->Tags)
				if (tag.Name == name) return &tag.Params;
		}
		return nullptr;
	};
	auto vector = [](ParamVector params) -> Vector2D {
		if (!params || params->size() < 2 || (*params)[0].omitted || (*params)[1].omitted)
			return {};
		return {(*params)[0].Get<float>(), (*params)[1].Get<float>()};
	};
	if (auto position = vector(find("\\pos"))) return position;
	if (auto position = vector(find("\\move"))) return position;

	int script_w = 0, script_h = 0;
	c->ass->GetResolution(script_w, script_h);
	auto margin = line.Margin;
	int align = 2;
	if (auto style = c->ass->GetStyle(line.Style)) {
		align = style->alignment;
		for (int i = 0; i < 3; ++i)
			if (!margin[i]) margin[i] = style->Margin[i];
	}
	if (auto align_tag = find("\\an"))
		align = std::clamp((*align_tag)[0].Get<int>(align), 1, 9);
	else if (auto legacy_align_tag = find("\\a"))
		align = std::clamp(AssStyle::SsaToAss((*legacy_align_tag)[0].Get<int>(2)), 1, 9);
	int horizontal = (align - 1) % 3;
	int vertical = (align - 1) / 3;
	float x = horizontal == 0 ? margin[0] : horizontal == 1 ?
		(script_w + margin[0] - margin[1]) / 2.f : script_w - margin[1];
	float y = vertical == 0 ? script_h - margin[2] : vertical == 1 ?
		script_h / 2.f : static_cast<float>(margin[2]);
	return {x, y};
}

std::string Number(double value) {
	std::ostringstream stream;
	stream << std::fixed << std::setprecision(3) << value;
	std::string out = stream.str();
	while (out.size() > 1 && out.back() == '0') out.pop_back();
	if (!out.empty() && out.back() == '.') out.pop_back();
	return out;
}

std::string InjectTags(std::string text, std::string const& tags) {
	if (tags.empty()) return text;
	if (!text.empty() && text.front() == '{') text.insert(1, tags);
	else text.insert(0, "{" + tags + "}");
	return text;
}

template<typename Replacer>
std::string ReplaceMatches(std::string const& text, std::regex const& pattern,
		Replacer replace, bool *matched = nullptr) {
	std::string out;
	size_t at = 0;
	bool any = false;
	for (auto found = std::sregex_iterator(text.begin(), text.end(), pattern);
		found != std::sregex_iterator(); ++found) {
		auto const& match = *found;
		out.append(text, at, static_cast<size_t>(match.position()) - at);
		out += replace(match);
		at = static_cast<size_t>(match.position() + match.length());
		any = true;
	}
	out.append(text, at, std::string::npos);
	if (matched) *matched = any;
	return out;
}

std::string ApplyOpacity(std::string text, double opacity) {
	opacity = std::clamp(opacity, 0.0, 1.0);
	static std::regex alpha(R"(\\(alpha|[1-4]a)&H([0-9A-Fa-f]{2})&)");
	bool matched = false;
	text = ReplaceMatches(text, alpha, [&](std::smatch const& match) {
		int old_alpha = std::stoi(match[2].str(), nullptr, 16);
		double old_opacity = 1.0 - old_alpha / 255.0;
		int next = std::clamp(static_cast<int>(std::lround(
			255.0 * (1.0 - old_opacity * opacity))), 0, 255);
		return agi::format("\\%s&H%02X&", match[1].str(), next);
	}, &matched);
	if (!matched) {
		int alpha_value = std::clamp(static_cast<int>(std::lround(255.0 * (1.0 - opacity))),
			0, 255);
		text = InjectTags(std::move(text), agi::format("\\alpha&H%02X&", alpha_value));
	}
	return text;
}

std::string ApplyTint(std::string text, std::optional<agi::Color> const& colour) {
	if (!colour) return text;
	std::string ass = colour->GetAssOverrideFormatted();
	static std::regex colour_tag(R"(\\(?:1c|c)&H[0-9A-Fa-f]{6}&)");
	bool matched = false;
	text = ReplaceMatches(text, colour_tag, [&](std::smatch const&) {
		return "\\1c" + ass;
	}, &matched);
	if (!matched) text = InjectTags(std::move(text), "\\1c" + ass);
	return text;
}

std::string OffsetPosition(agi::Context *c, AssDialogue const& source, std::string text,
		double from_x, double from_y, double to_x, double to_y, int duration) {
	static std::regex position(
		R"(\\pos\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*\))");
	std::smatch found;
	if (std::regex_search(text, found, position)) {
		double x = std::stod(found[1].str());
		double y = std::stod(found[2].str());
		std::string tag = std::abs(from_x - to_x) < .001 &&
			std::abs(from_y - to_y) < .001 ?
			agi::format("\\pos(%s,%s)", Number(x + from_x), Number(y + from_y)) :
			agi::format("\\move(%s,%s,%s,%s,0,%d)", Number(x + from_x),
				Number(y + from_y), Number(x + to_x), Number(y + to_y),
				std::max(1, duration));
		text.replace(static_cast<size_t>(found.position()), static_cast<size_t>(found.length()), tag);
		return text;
	}

	static std::regex move(
		R"(\\move\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)(\s*,\s*\d+\s*,\s*\d+\s*)?\))");
	if (std::regex_search(text, found, move)) {
		double x1 = std::stod(found[1].str());
		double y1 = std::stod(found[2].str());
		double x2 = std::stod(found[3].str());
		double y2 = std::stod(found[4].str());
		std::string timing = found[5].matched ? found[5].str() : std::string();
		std::string tag = agi::format("\\move(%s,%s,%s,%s%s)", Number(x1 + from_x),
			Number(y1 + from_y), Number(x2 + to_x), Number(y2 + to_y), timing);
		text.replace(static_cast<size_t>(found.position()), static_cast<size_t>(found.length()), tag);
		return text;
	}

	Vector2D at = LinePosition(c, source);
	std::string tag = std::abs(from_x - to_x) < .001 &&
		std::abs(from_y - to_y) < .001 ?
		agi::format("\\pos(%s,%s)", Number(at.X() + from_x), Number(at.Y() + from_y)) :
		agi::format("\\move(%s,%s,%s,%s,0,%d)", Number(at.X() + from_x),
			Number(at.Y() + from_y), Number(at.X() + to_x), Number(at.Y() + to_y),
			std::max(1, duration));
	return InjectTags(std::move(text), tag);
}

uint32_t Mix(uint32_t value) {
	value ^= value >> 16;
	value *= 0x7feb352dU;
	value ^= value >> 15;
	value *= 0x846ca68bU;
	return value ^ (value >> 16);
}

double Offset(uint32_t seed, size_t source, int band, int palette, double amount) {
	uint32_t value = Mix(seed ^ static_cast<uint32_t>(source * 0x9e3779b9U) ^
		static_cast<uint32_t>((band + 2048) * 0x85ebca6bU) ^
		static_cast<uint32_t>(palette * 0xc2b2ae35U));
	double unit = (value & 0xffffU) / 65535.0 * 2.0 - 1.0;
	return unit * amount;
}

double Noise01(uint32_t seed, size_t source, int strip, int salt) {
	uint32_t value = Mix(seed ^ static_cast<uint32_t>(source * 0x9e3779b9U) ^
		static_cast<uint32_t>((strip + 4096) * 0x85ebca6bU) ^
		static_cast<uint32_t>((salt + 8192) * 0xc2b2ae35U));
	return value / 4294967295.0;
}

struct Paint {
	std::optional<agi::Color> colour;
	double opacity = 1.0;
	double direction = 1.0;
};

std::vector<Paint> Paints(ColorStyle style,
		std::array<std::optional<agi::Color>, 3> const& custom_colors) {
	std::vector<Paint> custom;
	constexpr std::array<double, 3> custom_opacity = {.65, .52, .42};
	constexpr std::array<double, 3> custom_direction = {1.0, -1.0, 0.0};
	if (style == ColorStyle::Custom) {
		for (auto const& color : custom_colors) {
			if (!color) continue;
			size_t at = custom.size();
			custom.push_back({color, custom_opacity[at], custom_direction[at]});
		}
		if (!custom.empty()) return custom;
	}
	switch (style) {
		case ColorStyle::Original: return {{std::nullopt, 1.0, 1}};
		case ColorStyle::CyanMagenta:
			return {{agi::Color(0, 255, 255), .58, 1},
				{agi::Color(255, 0, 255), .46, -1}};
		case ColorStyle::Rgb:
			return {{agi::Color(255, 40, 40), .58, 1},
				{agi::Color(40, 255, 80), .42, 0},
				{agi::Color(40, 80, 255), .52, -1}};
		case ColorStyle::BluePink:
			return {{agi::Color(0, 112, 255), .58, 1},
				{agi::Color(255, 0, 112), .46, -1}};
		case ColorStyle::Light:
			return {{agi::Color(255, 255, 255), .56, 1},
				{agi::Color(170, 255, 255), .3, -1}};
		case ColorStyle::Dark:
			return {{agi::Color(16, 32, 64), .72, 1},
				{agi::Color(64, 16, 64), .36, -1}};
		case ColorStyle::Custom: return {{std::nullopt, 1.0, 1}};
	}
	return {{std::nullopt, 1.0, 1}};
}

struct Phase {
	int start = 0;
	int end = 0;
	Values from;
	Values to;
	bool animated = false;
	EffectType effect_type = EffectType::SliceShift;
	ColorStyle color_style = ColorStyle::Original;
	std::array<std::optional<agi::Color>, 3> custom_colors;
	bool show_base = true;
};

Values Interpolate(Values from, Values to, double factor) {
	factor = std::clamp(factor, 0.0, 1.0);
	auto mix = [factor](double left, double right) {
		return left + (right - left) * factor;
	};
	double angle_delta = std::fmod(to.angle - from.angle + 540.0, 360.0) - 180.0;
	return Clamp({mix(from.amount, to.amount), mix(from.offset, to.offset),
		mix(from.opacity, to.opacity),
		static_cast<int>(std::lround(mix(from.height, to.height))),
		mix(from.width, to.width), from.angle + angle_delta * factor});
}

Phase SlicePhase(Phase const& phase, int start, int end) {
	if (!phase.animated || phase.end <= phase.start)
		return {start, end, phase.from, phase.to, phase.animated, phase.effect_type,
			phase.color_style, phase.custom_colors,
			phase.show_base};
	double duration = static_cast<double>(phase.end - phase.start);
	return {start, end,
		Interpolate(phase.from, phase.to, (start - phase.start) / duration),
		Interpolate(phase.from, phase.to, (end - phase.start) / duration),
		true, phase.effect_type, phase.color_style, phase.custom_colors, phase.show_base};
}

struct ResolvedAnimation {
	Animation animation;
	int start = 0;
	int end = 0;
};

std::optional<std::pair<int, int>> AnimationInterval(agi::Context *c,
		AssDialogue const& source, Animation const& animation, int duration) {
	if (animation.timing == AnimationTiming::Range) {
		int start = std::clamp(animation.start_time, 0, duration);
		int end = std::clamp(animation.end_time, 0, duration);
		if (end <= start) return std::nullopt;
		return std::pair{start, end};
	}

	int start = 0;
	int end = 0;
	auto const& fps = c->project->Timecodes();
	if (fps.IsLoaded()) {
		int line_start = static_cast<int>(source.Start);
		int first = fps.FrameAtTime(line_start, agi::vfr::START);
		start = animation.frame == 0 ? 0 :
			fps.TimeAtFrame(first + animation.frame, agi::vfr::START) - line_start;
		end = fps.TimeAtFrame(first + animation.frame + 1, agi::vfr::START) - line_start;
	}
	else {
		start = animation.frame * 40;
		end = start + 40;
	}
	start = std::clamp(start, 0, duration);
	end = std::clamp(end, 0, duration);
	if (end <= start) return std::nullopt;
	return std::pair{start, end};
}

std::vector<Phase> Phases(agi::Context *c, Settings const& settings,
		AssDialogue const& source, int duration) {
	std::vector<ResolvedAnimation> ranges;
	std::vector<ResolvedAnimation> frames;
	for (auto animation : settings.animations) {
		if (!animation.enabled) continue;
		animation.from = Clamp(animation.from);
		animation.to = Clamp(animation.to);
		animation.from.angle = settings.base.angle;
		animation.to.angle = settings.base.angle;
		auto interval = AnimationInterval(c, source, animation, duration);
		if (!interval) continue;
		ResolvedAnimation resolved{animation, interval->first, interval->second};
		(animation.timing == AnimationTiming::Frame ? frames : ranges).push_back(resolved);
	}
	auto by_start = [](ResolvedAnimation const& left, ResolvedAnimation const& right) {
		return left.start < right.start;
	};
	std::stable_sort(ranges.begin(), ranges.end(), by_start);
	std::stable_sort(frames.begin(), frames.end(), by_start);

	std::vector<Phase> phases;
	Values current = Clamp(settings.base);
	int cursor = 0;
	for (auto const& resolved : ranges) {
		auto const& animation = resolved.animation;
		int start = std::max(cursor, resolved.start);
		if (start >= resolved.end) continue;
		if (start > cursor)
			phases.push_back({cursor, start, current, current, false,
				settings.effect_type, settings.color_style, settings.custom_colors,
				settings.show_base});
		EffectType animation_effect = animation.use_default_effect_type ?
			settings.effect_type : animation.effect_type;
		ColorStyle animation_color = animation.use_default_color_style ?
			settings.color_style : animation.color_style;
		auto animation_colors = animation.use_default_color_style ?
			settings.custom_colors : animation.custom_colors;
		Phase range{resolved.start, resolved.end, animation.from, animation.to, true,
			animation_effect, animation_color, animation_colors, animation.show_base};
		phases.push_back(SlicePhase(range, start, resolved.end));
		current = animation.to;
		cursor = resolved.end;
	}
	if (cursor < duration)
		phases.push_back({cursor, duration, current, current, false,
			settings.effect_type, settings.color_style, settings.custom_colors,
			settings.show_base});
	if (phases.empty() && duration > 0)
		phases.push_back({0, duration, current, current, false,
			settings.effect_type, settings.color_style, settings.custom_colors,
			settings.show_base});

	// A single-frame animation temporarily replaces the underlying range/base phase,
	// then the underlying phase continues exactly where its interpolation would be.
	for (auto const& resolved : frames) {
		std::vector<Phase> replaced;
		for (auto const& phase : phases) {
			int overlap_start = std::max(phase.start, resolved.start);
			int overlap_end = std::min(phase.end, resolved.end);
			if (overlap_end <= overlap_start) {
				replaced.push_back(phase);
				continue;
			}
			if (phase.start < overlap_start)
				replaced.push_back(SlicePhase(phase, phase.start, overlap_start));
			EffectType animation_effect = resolved.animation.use_default_effect_type ?
				settings.effect_type : resolved.animation.effect_type;
			ColorStyle animation_color = resolved.animation.use_default_color_style ?
				settings.color_style : resolved.animation.color_style;
			auto animation_colors = resolved.animation.use_default_color_style ?
				settings.custom_colors : resolved.animation.custom_colors;
			replaced.push_back({overlap_start, overlap_end, resolved.animation.to,
				resolved.animation.to, false, animation_effect, animation_color,
				animation_colors, resolved.animation.show_base});
			if (overlap_end < phase.end)
				replaced.push_back(SlicePhase(phase, overlap_end, phase.end));
		}
		phases = std::move(replaced);
	}
	return phases;
}

std::vector<Phase> FramePhases(agi::Context *c, AssDialogue const& source,
		Phase const& phase) {
	std::string source_text = source.Text.get();
	bool source_animated = source_text.find("\\t(") != std::string::npos ||
		source_text.find("\\move(") != std::string::npos ||
		source_text.find("\\fad(") != std::string::npos ||
		source_text.find("\\fade(") != std::string::npos;
	if ((!phase.animated && !source_animated) || phase.end <= phase.start) return {phase};
	std::vector<int> times = {phase.start, phase.end};
	auto const& fps = c->project->Timecodes();
	if (fps.IsLoaded()) {
		int absolute_start = static_cast<int>(source.Start) + phase.start;
		int absolute_end = static_cast<int>(source.Start) + phase.end;
		int first = fps.FrameAtTime(absolute_start, agi::vfr::START);
		int last = fps.FrameAtTime(absolute_end, agi::vfr::END) + 1;
		for (int frame = first; frame <= last; ++frame) {
			int relative = fps.TimeAtFrame(frame, agi::vfr::START) -
				static_cast<int>(source.Start);
			if (relative > phase.start && relative < phase.end) times.push_back(relative);
		}
	}
	else {
		for (int at = phase.start + 40; at < phase.end; at += 40) times.push_back(at);
	}
	std::sort(times.begin(), times.end());
	times.erase(std::unique(times.begin(), times.end()), times.end());
	std::vector<Phase> frames;
	frames.reserve(times.size() - 1);
	for (size_t i = 1; i < times.size(); ++i) {
		int start = times[i - 1], end = times[i];
		double factor = ((start + end) * .5 - phase.start) /
			std::max(1.0, static_cast<double>(phase.end - phase.start));
		Values sampled = Interpolate(phase.from, phase.to, factor);
		frames.push_back({start, end, sampled, sampled, false, phase.effect_type,
			phase.color_style, phase.custom_colors, phase.show_base});
	}
	return frames;
}

bool HasClip(std::string const& text) {
	return text.find("\\clip(") != std::string::npos ||
		text.find("\\iclip(") != std::string::npos;
}

using Polygon = std::vector<Vector2D>;

double Cross(Vector2D a, Vector2D b, Vector2D c) {
	return (b.X() - a.X()) * (c.Y() - a.Y()) -
		(b.Y() - a.Y()) * (c.X() - a.X());
}

double SignedArea(Polygon const& polygon) {
	double area = 0.0;
	for (size_t i = 0; i < polygon.size(); ++i) {
		auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
		area += a.X() * b.Y() - b.X() * a.Y();
	}
	return area * .5;
}

Polygon IntersectConvex(Polygon subject, Polygon const& clip) {
	if (subject.size() < 3 || clip.size() < 3) return {};
	double orientation = SignedArea(clip) >= 0.0 ? 1.0 : -1.0;
	for (size_t edge = 0; edge < clip.size() && subject.size() >= 3; ++edge) {
		Vector2D a = clip[edge], b = clip[(edge + 1) % clip.size()];
		auto inside = [&](Vector2D point) { return Cross(a, b, point) * orientation >= -1e-5; };
		auto crossing = [&](Vector2D from, Vector2D to) {
			double from_side = Cross(a, b, from), to_side = Cross(a, b, to);
			double along = std::abs(from_side - to_side) < 1e-9 ? 0.0 :
				from_side / (from_side - to_side);
			return from + (to - from) * static_cast<float>(along);
		};
		Polygon kept;
		for (size_t i = 0; i < subject.size(); ++i) {
			Vector2D current = subject[i], next = subject[(i + 1) % subject.size()];
			bool current_in = inside(current), next_in = inside(next);
			if (current_in) kept.push_back(current);
			if (current_in != next_in) kept.push_back(crossing(current, next));
		}
		subject = std::move(kept);
	}
	return subject;
}

std::string PolygonClip(Polygon const& polygon) {
	if (polygon.size() < 3) return {};
	double left = std::numeric_limits<double>::max();
	double top = std::numeric_limits<double>::max();
	double right = std::numeric_limits<double>::lowest();
	double bottom = std::numeric_limits<double>::lowest();
	for (auto point : polygon) {
		left = std::min(left, static_cast<double>(point.X()));
		top = std::min(top, static_cast<double>(point.Y()));
		right = std::max(right, static_cast<double>(point.X()));
		bottom = std::max(bottom, static_cast<double>(point.Y()));
	}
	bool rectangle = polygon.size() == 4 && std::all_of(polygon.begin(), polygon.end(),
		[&](Vector2D point) {
			bool x_edge = std::abs(point.X() - left) < .01 ||
				std::abs(point.X() - right) < .01;
			bool y_edge = std::abs(point.Y() - top) < .01 ||
				std::abs(point.Y() - bottom) < .01;
			return x_edge && y_edge;
		});
	if (rectangle)
		return agi::format("%d,%d,%d,%d", static_cast<int>(std::floor(left)),
			static_cast<int>(std::floor(top)), static_cast<int>(std::ceil(right)),
			static_cast<int>(std::ceil(bottom)));
	std::string body = "m " + Number(polygon[0].X()) + " " + Number(polygon[0].Y());
	for (size_t i = 1; i < polygon.size(); ++i)
		body += " l " + Number(polygon[i].X()) + " " + Number(polygon[i].Y());
	return body;
}

std::optional<Polygon> ParsePositiveClip(std::string const& text) {
	static std::regex pattern(R"(\\(clip|iclip)\(([^)]*)\))");
	std::smatch last;
	for (auto found = std::sregex_iterator(text.begin(), text.end(), pattern);
		found != std::sregex_iterator(); ++found) last = *found;
	if (last.empty() || last[1].str() == "iclip") return std::nullopt;
	std::string body = last[2].str();
	std::vector<double> numbers;
	static std::regex number(R"([-+]?\d+(?:\.\d+)?)");
	for (auto found = std::sregex_iterator(body.begin(), body.end(), number);
		found != std::sregex_iterator(); ++found)
		numbers.push_back(std::stod(found->str()));
	if (body.find_first_of("mnlbspc") == std::string::npos && numbers.size() == 4) {
		double left = std::min(numbers[0], numbers[2]);
		double right = std::max(numbers[0], numbers[2]);
		double top = std::min(numbers[1], numbers[3]);
		double bottom = std::max(numbers[1], numbers[3]);
		return Polygon{{static_cast<float>(left), static_cast<float>(top)},
			{static_cast<float>(right), static_cast<float>(top)},
			{static_cast<float>(right), static_cast<float>(bottom)},
			{static_cast<float>(left), static_cast<float>(bottom)}};
	}
	// Vector clips made by Aegisub use m/l polygons. The optional leading scale is
	// deliberately honoured; Bezier and inverse clips stay untouched as a safe fallback.
	if (body.find('b') != std::string::npos || body.find('s') != std::string::npos ||
		body.find('p') != std::string::npos || body.find('c') != std::string::npos)
		return std::nullopt;
	double divisor = 1.0;
	size_t comma = body.find(',');
	if (comma != std::string::npos) {
		try { divisor = static_cast<double>(1 << std::max(0, std::stoi(body.substr(0, comma)) - 1)); }
		catch (...) { return std::nullopt; }
		numbers.clear();
		std::string drawing = body.substr(comma + 1);
		for (auto found = std::sregex_iterator(drawing.begin(), drawing.end(), number);
			found != std::sregex_iterator(); ++found)
			numbers.push_back(std::stod(found->str()));
	}
	if (numbers.size() < 6 || numbers.size() % 2) return std::nullopt;
	Polygon polygon;
	for (size_t i = 0; i < numbers.size(); i += 2)
		polygon.emplace_back(static_cast<float>(numbers[i] / divisor),
			static_cast<float>(numbers[i + 1] / divisor));
	return polygon;
}

std::optional<std::string> ApplyBandClip(std::string text, Polygon const& band,
		bool had_clip, double translate_x = 0.0, double translate_y = 0.0) {
	Polygon combined = band;
	if (had_clip) {
		auto existing = ParsePositiveClip(text);
		if (!existing) return text;
		combined = IntersectConvex(std::move(*existing), band);
		if (combined.size() < 3) return std::nullopt;
	}
	if (had_clip) {
		static std::regex clips(R"(\\i?clip\([^)]*\))");
		text = std::regex_replace(text, clips, "");
	}
	if (std::abs(translate_x) > .0001 || std::abs(translate_y) > .0001)
		for (auto& point : combined)
			point = point + Vector2D(static_cast<float>(translate_x),
				static_cast<float>(translate_y));
	std::string body = PolygonClip(combined);
	if (body.empty()) return std::nullopt;
	return InjectTags(std::move(text), "\\clip(" + body + ")");
}

std::pair<double, double> ProjectionRange(Bounds const& bounds, double angle) {
	double radians = angle * 3.14159265358979323846 / 180.0;
	double nx = std::cos(radians), ny = std::sin(radians);
	std::array<std::pair<double, double>, 4> corners = {{{bounds.left, bounds.top},
		{bounds.right, bounds.top}, {bounds.right, bounds.bottom}, {bounds.left, bounds.bottom}}};
	double low = std::numeric_limits<double>::max();
	double high = std::numeric_limits<double>::lowest();
	for (auto [x, y] : corners) {
		double projection = x * nx + y * ny;
		low = std::min(low, projection);
		high = std::max(high, projection);
	}
	return {low, high};
}

Polygon SlicePolygon(double angle, double normal_low, double normal_high,
		double tangent_low, double tangent_high) {
	double radians = angle * 3.14159265358979323846 / 180.0;
	double nx = std::cos(radians), ny = std::sin(radians);
	double tx = -ny, ty = nx;
	auto point = [&](double projection, double tangent) {
		return Vector2D(static_cast<float>(nx * projection + tx * tangent),
			static_cast<float>(ny * projection + ty * tangent));
	};
	return {point(normal_low, tangent_low), point(normal_low, tangent_high),
		point(normal_high, tangent_high), point(normal_high, tangent_low)};
}

void AddOverlay(std::vector<AssDialogue>& out, agi::Context *c,
		AssDialogue const& source, int start, int end,
		double from_x, double from_y, double to_x, double to_y,
		double from_opacity, double to_opacity,
		std::optional<agi::Color> const& colour, std::optional<Polygon> clip) {
	if (end <= start || (from_opacity <= .0001 && to_opacity <= .0001)) return;
	AssDialogue sampled(source);
	typesetting::motion::SnapshotAnimations(c, sampled, (start + end) / 2, start);
	AssDialogue generated(sampled);
	generated.Comment = false;
	generated.Effect = effect_name;
	generated.Start = start;
	generated.End = end;
	std::string text = sampled.Text.get();
	bool had_clip = HasClip(text);
	text = OffsetPosition(c, sampled, std::move(text), from_x, from_y, to_x, to_y,
		end - start);
	if (clip) {
		// Intersect an existing source clip with the requested glitch slice first, then
		// move that complete pixel region with the displaced content. Leaving the new
		// clip behind at the source position cuts every shifted slice at the old bounds.
		double clip_shift = (from_x + to_x) * .5;
		double clip_shift_y = (from_y + to_y) * .5;
		auto clipped = ApplyBandClip(std::move(text), *clip, had_clip,
			clip_shift, clip_shift_y);
		if (!clipped) return;
		text = std::move(*clipped);
	}
	text = ApplyTint(std::move(text), colour);
	text = ApplyOpacity(std::move(text), from_opacity);
	if (std::abs(from_opacity - to_opacity) > .001) {
		int target_alpha = std::clamp(static_cast<int>(std::lround(
			255.0 * (1.0 - std::clamp(to_opacity, 0.0, 1.0)))), 0, 255);
		text = InjectTags(std::move(text),
			agi::format("\\t(0,%d,\\alpha&H%02X&)", std::max(1, end - start), target_alpha));
	}
	generated.Text = std::move(text);
	out.push_back(std::move(generated));
}

void GeneratePass(std::vector<AssDialogue>& out, agi::Context *c,
		std::vector<AssDialogue *> const& sources, Bounds const& bounds,
		Phase const& phase, Values from, Values to,
		int height, double from_fade, double to_fade, uint32_t phase_seed) {
	auto paints = Paints(phase.color_style, phase.custom_colors);
	switch (phase.effect_type) {
		case EffectType::ScanlineTear:
			from.angle = to.angle = 90.0;
			from.height = std::min(from.height, 3);
			to.height = std::min(to.height, 3);
			from.width = to.width = 100.0;
			break;
		case EffectType::Macroblock:
		case EffectType::BlockShuffle:
			from.angle = to.angle = 90.0;
			from.height = std::max(from.height, 8);
			to.height = std::max(to.height, 8);
			from.width = std::max(from.width, 12.0);
			to.width = std::max(to.width, 12.0);
			break;
		case EffectType::VhsTracking:
			from.angle = to.angle = 90.0;
			from.height = std::min(from.height, 4);
			to.height = std::min(to.height, 4);
			from.width = to.width = 100.0;
			break;
		case EffectType::Dropout:
			from.angle = to.angle = 90.0;
			from.height = std::min(from.height, 8);
			to.height = std::min(to.height, 8);
			break;
		case EffectType::PixelStretch:
			from.height = std::min(from.height, 5);
			to.height = std::min(to.height, 5);
			break;
		default: break;
	}
	double peak_amount = std::max(from.amount, to.amount);
	auto from_range = ProjectionRange(bounds, from.angle);
	auto to_range = ProjectionRange(bounds, to.angle);
	auto from_tangent = ProjectionRange(bounds, from.angle + 90.0);
	auto to_tangent = ProjectionRange(bounds, to.angle + 90.0);
	double projection_span = std::max(from_range.second - from_range.first,
		to_range.second - to_range.first);
	// Height and width form a two-dimensional cell grid over the object. Amount is
	// the percentage of that grid which is emitted, so narrowing a slice creates
	// more cells (including several at the same height) rather than reducing coverage.
	auto grid = [projection_span](Values const& value) {
		int rows = std::max(1, static_cast<int>(std::ceil(
			projection_span / std::max(1, value.height))));
		int columns = std::max(1, static_cast<int>(std::ceil(100.0 / value.width)));
		return std::pair<int, int>{rows, columns};
	};
	auto strip_count = [&](Values const& value) {
		if (value.amount <= 0.0) return 0;
		auto [rows, columns] = grid(value);
		int wanted = static_cast<int>(std::ceil(rows * value.amount / value.width));
		return std::clamp(wanted, 1, rows * columns);
	};
	auto detail_count = [](double amount, int regular) {
		return amount > 30.0 ? std::max(1, static_cast<int>(std::ceil(regular * .35))) : 0;
	};
	int from_regular_count = strip_count(from);
	int to_regular_count = strip_count(to);
	if (phase.effect_type == EffectType::Dropout) {
		auto [from_rows, from_columns] = grid(from);
		auto [to_rows, to_columns] = grid(to);
		from_regular_count = from_rows * from_columns;
		to_regular_count = to_rows * to_columns;
	}
	int regular_count = std::max(from_regular_count, to_regular_count);
	int from_extra_count = phase.effect_type == EffectType::Dropout ? 0 :
		detail_count(from.amount, from_regular_count);
	int to_extra_count = phase.effect_type == EffectType::Dropout ? 0 :
		detail_count(to.amount, to_regular_count);
	int extra_count = std::max(from_extra_count, to_extra_count);
	for (size_t source_index = 0; source_index < sources.size(); ++source_index) {
		auto source = sources[source_index];
		bool gradient = source->Effect.get() == "gradient-fx";
		bool atomic = imagemask::IsLine(source);
		size_t noise_source = gradient ? sources.size() : source_index;
		int source_band = static_cast<int>(source_index);
		if (imagemask::IsLine(source)) {
			Vector2D position = LinePosition(c, *source);
			source_band = static_cast<int>(std::floor((position.Y() - bounds.top) /
				std::max(1, height)));
		}
		for (size_t paint_index = 0; paint_index < paints.size(); ++paint_index) {
			auto const& paint = paints[paint_index];
			auto add = [&](int band, std::optional<Polygon> clip,
				double displacement_scale = 1.0, double from_opacity_scale = 1.0,
				double to_opacity_scale = 1.0,
				uint32_t seed = 0) {
				seed = seed ? seed : phase_seed;
				auto emit = [&](double scale, double from_alpha, double to_alpha,
						uint32_t emit_seed) {
					double displacement_jitter = .85 + .3 * Noise01(emit_seed, noise_source,
						band, 211 + static_cast<int>(paint_index));
					double from_x = Offset(emit_seed, noise_source, band,
						static_cast<int>(paint_index), from.offset) * paint.direction *
						scale * displacement_jitter;
					double to_x = Offset(emit_seed, noise_source, band,
						static_cast<int>(paint_index), to.offset) * paint.direction *
						scale * displacement_jitter;
					double from_y = 0.0, to_y = 0.0;
					double y_scale = 0.0;
					if (phase.effect_type == EffectType::Macroblock) y_scale = .35;
					else if (phase.effect_type == EffectType::BlockShuffle) y_scale = .8;
					else if (phase.effect_type == EffectType::VhsTracking) y_scale = .08;
					else if (phase.effect_type == EffectType::GhostTrail) y_scale = .16;
					if (y_scale > 0.0) {
						from_y = Offset(emit_seed ^ 0x7f4a7c15U, noise_source, band,
							static_cast<int>(paint_index) + 17, from.offset * y_scale) *
							paint.direction * scale;
						to_y = Offset(emit_seed ^ 0x7f4a7c15U, noise_source, band,
							static_cast<int>(paint_index) + 17, to.offset * y_scale) *
							paint.direction * scale;
					}
					if (phase.effect_type == EffectType::Macroblock ||
						phase.effect_type == EffectType::BlockShuffle) {
						double quantum = std::max(4.0, static_cast<double>(
							std::max(from.height, to.height)));
						from_x = std::round(from_x / quantum) * quantum;
						to_x = std::round(to_x / quantum) * quantum;
						from_y = std::round(from_y / quantum) * quantum;
						to_y = std::round(to_y / quantum) * quantum;
					}
					if (phase.effect_type == EffectType::Dropout)
						from_x = from_y = to_x = to_y = 0.0;
					AddOverlay(out, c, *source,
						static_cast<int>(source->Start) + phase.start,
						static_cast<int>(source->Start) + phase.end,
						from_x, from_y, to_x, to_y,
						from.opacity * paint.opacity * from_fade * from_alpha,
						to.opacity * paint.opacity * to_fade * to_alpha,
						paint.colour, clip);
				};
				if (phase.effect_type == EffectType::PixelStretch) {
					for (int echo = 0; echo < 6; ++echo) {
						double echo_scale = displacement_scale * (.35 + echo * .42);
						double echo_alpha = 1.0 / (1.0 + echo * .65);
						emit(echo_scale, from_opacity_scale * echo_alpha,
							to_opacity_scale * echo_alpha,
							seed ^ static_cast<uint32_t>(echo * 0x45d9f3bU));
					}
				}
				else emit(displacement_scale, from_opacity_scale, to_opacity_scale, seed);
			};
			if (phase.effect_type == EffectType::ChromaticSplit) {
				double from_strength = from.amount / 100.0;
				double to_strength = to.amount / 100.0;
				add(source_band, std::nullopt, 1.0, from_strength, to_strength);
				continue;
			}
			if (phase.effect_type == EffectType::GhostTrail) {
				for (int echo = 1; echo <= 4; ++echo) {
					double from_strength = from.amount / 100.0 / echo;
					double to_strength = to.amount / 100.0 / echo;
					add(source_band - echo, std::nullopt, .55 * echo,
						from_strength, to_strength,
						phase_seed ^ static_cast<uint32_t>(echo * 0x9e3779b9U));
				}
				continue;
			}
			if (atomic) {
				auto [from_rows, from_columns] = grid(from);
				auto [to_rows, to_columns] = grid(to);
				(void)from_rows; (void)to_rows;
				int columns = std::max(from_columns, to_columns);
				for (int column = 0; column < columns; ++column) {
					double from_noise = Noise01(phase_seed, source_index,
						source_band * 131 + column, 31) * 100.0;
					double to_noise = Noise01(phase_seed, source_index,
						source_band * 131 + column, 31) * 100.0;
					bool dropout = phase.effect_type == EffectType::Dropout;
					double from_active = column < from_columns &&
						(dropout ? from_noise >= from.amount * .85 :
							from.amount > 0.0 && from_noise < from.amount);
					double to_active = column < to_columns &&
						(dropout ? to_noise >= to.amount * .85 :
							to.amount > 0.0 && to_noise < to.amount);
					Values const& geometry = from_active ? from : to;
					auto normal = from_active ? from_range : to_range;
					auto tangent = from_active ? from_tangent : to_tangent;
					int geometry_columns = from_active ? from_columns : to_columns;
					double segment = (tangent.second - tangent.first) * geometry.width / 100.0;
					double tangent_low = tangent.first + column * segment;
					if (geometry.width < 99.999) {
						double jitter = (Noise01(phase_seed, source_index,
							source_band * 131 + column, 113 + static_cast<int>(paint_index)) *
							.6 - .3) * (tangent.second - tangent.first);
						tangent_low = std::clamp(tangent_low + jitter, tangent.first,
							std::max(tangent.first, tangent.second - segment));
					}
					double tangent_high = std::min(tangent.second, tangent_low + segment);
					if (column >= geometry_columns || tangent_high <= tangent_low) continue;
					auto clip = SlicePolygon(geometry.angle, normal.first, normal.second,
						tangent_low, tangent_high);
					int band = source_band * columns + column;
					add(band, clip, 1.0, from_active, to_active);
					if (peak_amount > 30.0 && !dropout)
						add(band, clip, 1.5,
							from.amount > 30.0 ? .48 * from_active : 0.0,
							to.amount > 30.0 ? .48 * to_active : 0.0,
							phase_seed ^ 0x6d2b79f5U);
				}
			}
			else {
				for (int band = 0; band < regular_count; ++band) {
					bool from_active = band < from_regular_count;
					bool to_active = band < to_regular_count;
					if (phase.effect_type == EffectType::Dropout) {
						double activity = Noise01(phase_seed, noise_source, band, 41) * 100.0;
						from_active = from_active && activity >= from.amount * .85;
						to_active = to_active && activity >= to.amount * .85;
					}
					if (!from_active && !to_active) continue;
					Values const& geometry = from_active ? from : to;
					auto normal = from_active ? from_range : to_range;
					auto tangent = from_active ? from_tangent : to_tangent;
					auto [rows, columns] = grid(geometry);
					int active = from_active ? from_regular_count : to_regular_count;
					int cell = std::min(rows * columns - 1, static_cast<int>(std::floor(
						(band + .5) * rows * columns / std::max(1, active))));
					int row = cell / columns, column = cell % columns;
					double centre = normal.first + (row + .5) *
						(normal.second - normal.first) / rows;
					double segment = (tangent.second - tangent.first) * geometry.width / 100.0;
					double tangent_low = tangent.first + column * segment;
					if (geometry.width < 99.999) {
						double jitter = (Noise01(phase_seed, noise_source, band,
							127 + static_cast<int>(paint_index)) * .6 - .3) *
							(tangent.second - tangent.first);
						tangent_low = std::clamp(tangent_low + jitter, tangent.first,
							std::max(tangent.first, tangent.second - segment));
					}
					double tangent_high = std::min(tangent.second, tangent_low + segment);
					if (gradient && geometry.width >= 99.999) {
						tangent_low -= 32.0;
						tangent_high += 32.0;
					}
					auto clip = SlicePolygon(geometry.angle, centre - geometry.height * .5,
						centre + geometry.height * .5, tangent_low, tangent_high);
					double displacement_scale = phase.effect_type == EffectType::BlockShuffle ?
						1.55 : 1.0;
					add(band, std::move(clip), displacement_scale,
						from_active ? 1.0 : 0.0, to_active ? 1.0 : 0.0);
				}

				// The site's second loop draws random 5-25 px strips from the canvas after
				// the regular bands have already modified it. ASS cannot sample its own
				// rendered result, so two accumulated echoes approximate that feedback.
				for (int strip = 0; strip < extra_count; ++strip) {
					double y_unit = Noise01(phase_seed, noise_source, strip,
						static_cast<int>(paint_index) * 7 + 1);
					double height_unit = Noise01(phase_seed, noise_source, strip,
						static_cast<int>(paint_index) * 7 + 2);
					double strip_height = std::max(1.0, static_cast<double>(std::lround(
						height * (.45 + 1.65 * height_unit))));
					double low = from_range.first + y_unit * std::max(0.0,
						from_range.second - from_range.first - strip_height);
					double tangent_span = from_tangent.second - from_tangent.first;
					double segment = tangent_span * from.width / 100.0;
					double tangent_unit = Noise01(phase_seed, noise_source, strip,
						static_cast<int>(paint_index) * 7 + 3);
					double tangent_low = from_tangent.first + tangent_unit *
						std::max(0.0, tangent_span - segment);
					auto clip = SlicePolygon(from.angle, low, low + strip_height,
						tangent_low, tangent_low + segment);
					int band = regular_count + strip;
					double from_active = strip < from_extra_count ? 1.0 : 0.0;
					double to_active = strip < to_extra_count ? 1.0 : 0.0;
					add(band, clip, 1.5, .78 * from_active, .78 * to_active,
						phase_seed ^ 0xa511e9b3U);
					add(band, clip, 2.15, .36 * from_active, .36 * to_active,
						phase_seed ^ 0x63d83595U);
				}
			}
		}
	}
}

std::vector<AssDialogue> GenerateGroup(agi::Context *c,
		std::vector<AssDialogue *> const& sources, Settings const& settings) {
	std::vector<AssDialogue> out;
	if (sources.empty()) return out;
	int duration = std::numeric_limits<int>::max();
	for (auto source : sources)
		duration = std::min(duration, std::max(0,
			static_cast<int>(source->End) - static_cast<int>(source->Start)));
	if (duration <= 0) return out;
	Bounds bounds = GroupBounds(c, sources);
	out.reserve(sources.size() * 2);

	auto phases = Phases(c, settings, *sources.front(), duration);
	for (size_t phase_index = 0; phase_index < phases.size(); ++phase_index) {
		auto const& phase = phases[phase_index];
		uint32_t phase_seed = settings.seed ^ static_cast<uint32_t>(phase_index * 0x9e3779b9U);
		auto frames = FramePhases(c, *sources.front(), phase);
		for (size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
			auto const& frame = frames[frame_index];
			if (frame.show_base && frame.effect_type != EffectType::Dropout) {
				for (auto source : sources) {
					int start = static_cast<int>(source->Start) + frame.start;
					int end = static_cast<int>(source->Start) + frame.end;
					AssDialogue base(*source);
					typesetting::motion::SnapshotAnimations(c, base, (start + end) / 2, start);
					base.Comment = false;
					base.Effect = effect_name;
					base.Start = start;
					base.End = end;
					out.push_back(std::move(base));
				}
			}
			GeneratePass(out, c, sources, bounds, frame, frame.from, frame.to,
				frame.from.height, 1.0, 1.0,
				phase_seed ^ static_cast<uint32_t>(frame_index * 0x85ebca6bU));
		}
	}
	if (out.empty()) {
		AssDialogue empty(*sources.front());
		empty.Comment = false;
		empty.Effect = effect_name;
		empty.Text = InjectTags(empty.Text.get(), "\\alpha&HFF&");
		out.push_back(std::move(empty));
	}
	return out;
}

} // namespace

std::vector<std::string> EffectTypeNames() {
	return {from_wx(_("Slice shift")), from_wx(_("Chromatic split")),
		from_wx(_("Scanline tear")), from_wx(_("Macroblock")),
		from_wx(_("Block shuffle")), from_wx(_("Signal dropout")),
		from_wx(_("Ghost trail")), from_wx(_("VHS tracking")),
		from_wx(_("Pixel stretch"))};
}

std::vector<std::string> ColorStyleNames() {
	return {from_wx(_("Original colors")), from_wx(_("Cyan and magenta")),
		from_wx(_("RGB split")), from_wx(_("Blue and pink")),
		from_wx(_("Light")), from_wx(_("Dark")), from_wx(_("Custom colors"))};
}

bool IsEffect(AssDialogue const *line) {
	return line && line->Effect.get() == effect_name;
}

bool IsSource(AssFile const& file, AssDialogue const *line) {
	return IsEffect(line) && Extra(file, *line, data_key).has_value() &&
		Extra(file, *line, source_key).has_value();
}

std::string Label(AssFile const& file, AssDialogue const& line) {
	auto encoded = Extra(file, line, source_key);
	if (!encoded) return line.GetStrippedText();
	auto sources = DeserializeSources(*encoded);
	for (auto const& source : sources) {
		auto label = source->GetStrippedText();
		if (!label.empty()) return label;
	}
	return {};
}

std::string Description(AssFile const& file, AssDialogue const& line) {
	auto encoded = Extra(file, line, data_key);
	if (!encoded) return {};
	auto settings = DeserializeSettings(*encoded);
	if (!settings) return {};
	auto effects = EffectTypeNames();
	auto colors = ColorStyleNames();
	bool animated = std::any_of(settings->animations.begin(), settings->animations.end(),
		[](Animation const& animation) { return animation.enabled; });
	return from_wx(wxString::Format(animated ? _("%s, %s, animation") :
		_("%s, %s, no animation"),
		to_wx(effects[static_cast<size_t>(settings->effect_type)]),
		to_wx(colors[static_cast<size_t>(settings->color_style)])));
}

bool SelectionHasEnabledAnimation(agi::Context *c) {
	for (auto const& group : CollectGroups(c, false)) {
		if (!group.editing || !group.anchor) continue;
		auto encoded = Extra(*c->ass, *group.anchor, data_key);
		if (!encoded) continue;
		auto settings = DeserializeSettings(*encoded);
		if (settings && std::any_of(settings->animations.begin(), settings->animations.end(),
				[](Animation const& animation) { return animation.enabled; }))
			return true;
	}
	return false;
}

Settings LoadSettingsForSelection(agi::Context *c) {
	// The grid may select any generated row in a collapsed glitch group, while only
	// the first row carries the settings metadata. Resolve the selected group first
	// so reopening an effect never falls back to the defaults for a child row.
	for (auto const& group : CollectGroups(c, false)) {
		if (!group.editing || !group.anchor) continue;
		auto encoded = Extra(*c->ass, *group.anchor, data_key);
		if (encoded) {
			auto parsed = DeserializeSettings(*encoded);
			if (parsed) return *parsed;
		}
	}
	return {};
}

bool SettingsFromClipboard(std::string clipboard, Settings& settings) {
	try {
		auto encoded_settings = TakeClipboardMarker(clipboard, clipboard_settings);
		auto encoded_sources = TakeClipboardMarker(clipboard, clipboard_sources);
		if (!encoded_settings || !encoded_sources ||
			DeserializeSources(*encoded_sources).empty()) return false;
		auto parsed = DeserializeSettings(*encoded_settings);
		if (!parsed) return false;
		settings = std::move(*parsed);
		return true;
	}
	catch (...) { return false; }
}

std::string ClipboardMetadata(AssFile const& file, AssDialogue const& line) {
	if (!IsSource(file, &line)) return {};
	auto settings = Extra(file, line, data_key);
	auto sources = Extra(file, line, source_key);
	if (!settings || !sources) return {};
	return ClipboardMarker(clipboard_settings, *settings) +
		ClipboardMarker(clipboard_sources, *sources);
}

bool RestoreClipboardMetadata(AssFile& file, AssDialogue& line) {
	std::string text = line.Text.get();
	auto settings = TakeClipboardMarker(text, clipboard_settings);
	auto sources = TakeClipboardMarker(text, clipboard_sources);
	line.Text = std::move(text);
	if (!settings || !sources || !IsEffect(&line) ||
		!DeserializeSettings(*settings) || DeserializeSources(*sources).empty()) return false;
	file.SetExtradataValue(line, data_key, *settings);
	file.SetExtradataValue(line, source_key, *sources);
	auto ids = line.ExtradataIds.get();
	std::sort(ids.begin(), ids.end());
	line.ExtradataIds = std::move(ids);
	return true;
}

void ClearGroupMetadata(AssFile& file, AssDialogue& line) {
	file.DeleteExtradataValue(line, data_key);
	file.DeleteExtradataValue(line, source_key);
}

struct PreviewSession::Impl {
	agi::Context *context;
	std::vector<SourceGroup> groups;

	explicit Impl(agi::Context *context)
	: context(context), groups(CollectGroups(context)) { }

	void Clear() {
		std::vector<AssDialogue const *> originals;
		for (auto const& group : groups)
			for (auto line : group.existing) originals.push_back(line);
		if (!originals.empty()) context->videoController->PreviewSubtitles(originals);
	}

	void Update(Settings const& settings) {
		std::vector<AssDialogue> silenced;
		std::vector<AssDialogue> added;
		for (auto const& group : groups) {
			for (auto line : group.existing) {
				AssDialogue quiet(*line);
				quiet.Comment = true;
				silenced.push_back(std::move(quiet));
			}
			auto generated = GenerateGroup(context, group.sources, settings);
			added.insert(added.end(), std::make_move_iterator(generated.begin()),
				std::make_move_iterator(generated.end()));
		}
		if (added.empty()) { Clear(); return; }
		std::vector<AssDialogue const *> changed, extras;
		for (auto const& line : silenced) changed.push_back(&line);
		for (auto const& line : added) extras.push_back(&line);
		context->videoController->PreviewSubtitles(changed, extras);
	}
};

PreviewSession::PreviewSession(agi::Context *c)
: impl(std::make_unique<Impl>(c)) { }

PreviewSession::~PreviewSession() = default;

void PreviewSession::Update(Settings const& settings) { impl->Update(settings); }
void PreviewSession::Clear() { impl->Clear(); }

bool Revert(agi::Context *c) {
	// Keep selected ordinary rows independent from generated groups. Reverting an
	// effect must never consume another selected source merely because both rows
	// happened to be selected when the context-menu command was invoked.
	auto groups = CollectGroups(c, false);
	Selection selection;
	AssDialogue *active = nullptr;
	auto old_active = c->selectionController->GetActiveLine();
	std::vector<std::unique_ptr<AssDialogue>> removed;

	for (auto& group : groups) {
		if (!group.editing || group.stored.empty() || group.existing.empty()) continue;
		bool was_active = std::find(group.existing.begin(), group.existing.end(), old_active) !=
			group.existing.end();
		auto insert_at = c->ass->Events.iterator_to(*group.anchor);
		AssDialogue *first = nullptr;
		for (auto const& stored : group.stored) {
			auto original = new AssDialogue(*stored);
			c->ass->Events.insert(insert_at, *original);
			selection.insert(original);
			if (!first) first = original;
		}
		if (!active || was_active) active = first;

		for (auto line : group.existing) {
			c->ass->Events.erase(c->ass->Events.iterator_to(*line));
			removed.emplace_back(line);
		}
	}

	if (selection.empty()) return false;
	c->selectionController->SetSelectionAndActive(std::move(selection), active);
	c->ass->CleanExtradata();
	c->ass->Commit(_("remove glitch effect"), AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return true;
}

size_t Apply(agi::Context *c, Settings const& settings) {
	auto groups = CollectGroups(c);
	if (groups.empty()) return 0;
	Selection selection;
	AssDialogue *active = nullptr;
	auto old_active = c->selectionController->GetActiveLine();
	std::vector<std::unique_ptr<AssDialogue>> removed;
	size_t count = 0;
	for (auto& group : groups) {
		auto generated = GenerateGroup(c, group.sources, settings);
		if (generated.empty()) continue;
		bool was_active = std::find(group.existing.begin(), group.existing.end(), old_active) !=
			group.existing.end();
		std::string stored_sources = SerializeSources(group.sources);
		auto insert_at = c->ass->Events.iterator_to(*group.anchor);
		AssDialogue *first = nullptr;
		for (size_t i = 0; i < generated.size(); ++i) {
			auto line = new AssDialogue(generated[i]);
			c->ass->DeleteExtradataValue(*line, data_key);
			c->ass->DeleteExtradataValue(*line, source_key);
			if (!i) {
				c->ass->SetExtradataValue(*line, data_key, SerializeSettings(settings));
				c->ass->SetExtradataValue(*line, source_key, stored_sources);
				auto ids = line->ExtradataIds.get();
				std::sort(ids.begin(), ids.end());
				line->ExtradataIds = std::move(ids);
				first = line;
			}
			c->ass->Events.insert(insert_at, *line);
			selection.insert(line);
			++count;
		}
		for (auto line : group.existing) {
			c->ass->Events.erase(c->ass->Events.iterator_to(*line));
			removed.emplace_back(line);
		}
		if (!active || was_active) active = first;
	}
	if (!count) return 0;
	c->selectionController->SetSelectionAndActive(std::move(selection), active);
	c->ass->CleanExtradata();
	c->ass->Commit(_("apply glitch effect"), AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return count;
}

} // namespace typesetting::glitch
