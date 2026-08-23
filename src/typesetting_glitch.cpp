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
	std::string animations;
	for (auto animation : settings.animations) {
		animation.from = Clamp(animation.from);
		animation.to = Clamp(animation.to);
		if (!animations.empty()) animations += ';';
		animations += agi::format("%d,%d,%d,%s,%s", animation.enabled,
			animation.start_time, animation.end_time,
			SerializeValues(animation.from), SerializeValues(animation.to));
	}
	return agi::format("4|%d|%u|%d|%s|%s", static_cast<int>(settings.mode),
		settings.seed, settings.show_base, SerializeValues(settings.base), animations);
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
	bool legacy = fields.size() == 5 && fields[0] == "1";
	bool version_two = fields.size() == 6 && fields[0] == "2";
	bool version_three = fields.size() == 6 && fields[0] == "3";
	bool version_four = fields.size() == 6 && fields[0] == "4";
	if (!legacy && !version_two && !version_three && !version_four) return std::nullopt;
	try {
		Settings settings;
		settings.mode = static_cast<Mode>(std::clamp(std::stoi(fields[1]), 0, 16));
		settings.seed = static_cast<uint32_t>(std::stoul(fields[2]));
		settings.show_base = legacy || std::stoi(fields[3]) != 0;
		size_t base_field = legacy ? 3 : 4;
		size_t animations_field = legacy ? 4 : 5;
		auto base = DeserializeValues(fields[base_field]);
		if (!base) return std::nullopt;
		settings.base = *base;
		size_t item_at = 0;
		while (item_at < fields[animations_field].size()) {
			size_t item_end = fields[animations_field].find(';', item_at);
			std::string item = fields[animations_field].substr(item_at,
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
			if (parts.size() != 9 && parts.size() != 11 && parts.size() != 14 &&
				parts.size() != 15)
				return std::nullopt;
			Animation animation;
			animation.enabled = std::stoi(parts[0]) != 0;
			animation.start_time = std::clamp(std::stoi(parts[1]), 0, 3600000);
			animation.end_time = std::clamp(std::stoi(parts[2]), 0, 3600000);
			std::string from_text;
			std::string to_text;
			if (parts.size() == 9) {
				from_text = parts[3] + "," + parts[4] + "," + parts[5];
				to_text = parts[6] + "," + parts[7] + "," + parts[8];
			}
			else if (parts.size() == 11) {
				from_text = parts[3] + "," + parts[4] + "," + parts[5] + "," + parts[6];
				to_text = parts[7] + "," + parts[8] + "," + parts[9] + "," + parts[10];
			}
			else if (parts.size() == 14) {
				from_text = parts[4] + "," + parts[5] + "," + parts[6] + "," +
					parts[7] + "," + parts[8];
				to_text = parts[9] + "," + parts[10] + "," + parts[11] + "," +
					parts[12] + "," + parts[13];
			}
			else {
				from_text = parts[3] + "," + parts[4] + "," + parts[5] + "," +
					parts[6] + "," + parts[7] + "," + parts[8];
				to_text = parts[9] + "," + parts[10] + "," + parts[11] + "," +
					parts[12] + "," + parts[13] + "," + parts[14];
			}
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

std::vector<SourceGroup> CollectGroups(agi::Context *c) {
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
			for (auto const& line : group.stored) group.sources.push_back(line.get());
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

std::string ApplyTint(std::string text, char const *bgr) {
	if (!bgr) return text;
	static std::regex colour(R"(\\(?:1c|c)&H[0-9A-Fa-f]{6}&)");
	bool matched = false;
	text = ReplaceMatches(text, colour, [&](std::smatch const&) {
		return agi::format("\\1c&H%s&", bgr);
	}, &matched);
	if (!matched) text = InjectTags(std::move(text), agi::format("\\1c&H%s&", bgr));
	return text;
}

std::string OffsetPosition(agi::Context *c, AssDialogue const& source, std::string text,
		double from_x, double to_x, int duration) {
	static std::regex position(
		R"(\\pos\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*\))");
	std::smatch found;
	if (std::regex_search(text, found, position)) {
		double x = std::stod(found[1].str());
		double y = std::stod(found[2].str());
		std::string tag = std::abs(from_x - to_x) < .001 ?
			agi::format("\\pos(%s,%s)", Number(x + from_x), Number(y)) :
			agi::format("\\move(%s,%s,%s,%s,0,%d)", Number(x + from_x), Number(y),
				Number(x + to_x), Number(y), std::max(1, duration));
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
			Number(y1), Number(x2 + to_x), Number(y2), timing);
		text.replace(static_cast<size_t>(found.position()), static_cast<size_t>(found.length()), tag);
		return text;
	}

	Vector2D at = LinePosition(c, source);
	std::string tag = std::abs(from_x - to_x) < .001 ?
		agi::format("\\pos(%s,%s)", Number(at.X() + from_x), Number(at.Y())) :
		agi::format("\\move(%s,%s,%s,%s,0,%d)", Number(at.X() + from_x), Number(at.Y()),
			Number(at.X() + to_x), Number(at.Y()), std::max(1, duration));
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
	char const *bgr = nullptr;
	double opacity = 1.0;
	double direction = 1.0;
};

std::vector<Paint> Paints(Mode mode) {
	switch (mode) {
		case Mode::Difference: return {{"FFFF00", .62, 1}, {"FF00FF", .48, -1}};
		case Mode::SourceAtop: return {{nullptr, 1.0, 1}};
		case Mode::DestinationOut: return {{"202020", .72, 1}};
		case Mode::Lighter: return {{"FFFFFF", .62, 1}, {"FFFF00", .34, -1}};
		case Mode::Multiply: return {{"503020", .72, 1}};
		case Mode::Screen: return {{"FFFFFF", .48, 1}, {"FFFF00", .28, -1}};
		case Mode::Overlay: return {{"FFFF00", .48, 1}, {"FF00FF", .42, -1}};
		case Mode::Darken: return {{"402010", .72, 1}};
		case Mode::Lighten: return {{"FFFFFF", .52, 1}};
		case Mode::ColorDodge: return {{"FFFFAA", .58, 1}};
		case Mode::ColorBurn: return {{"401040", .68, 1}};
		case Mode::HardLight: return {{"FFFF00", .55, 1}, {"FF0080", .45, -1}};
		case Mode::SoftLight: return {{"FFFFFF", .32, 1}};
		case Mode::Exclusion: return {{"FFFF00", .46, 1}, {"FF00FF", .38, -1}};
		case Mode::Hue: return {{"FFFF00", .52, 1}, {"FF00FF", .42, -1}};
		case Mode::Color: return {{"FF7000", .55, 1}, {"7000FF", .42, -1}};
		case Mode::Luminosity: return {{"FFFFFF", .5, 1}};
	}
	return {{nullptr, 1.0, 1}};
}

struct Phase {
	int start = 0;
	int end = 0;
	Values from;
	Values to;
	bool animated = false;
	Mode mode = Mode::SourceAtop;
};

std::vector<Phase> Phases(Settings const& settings, int duration) {
	std::vector<Animation> animations;
	for (auto animation : settings.animations) {
		if (!animation.enabled) continue;
		animation.start_time = std::clamp(animation.start_time, 0, duration);
		animation.end_time = std::clamp(animation.end_time, 0, duration);
		if (animation.end_time <= animation.start_time) continue;
		animation.from = Clamp(animation.from);
		animation.to = Clamp(animation.to);
		animations.push_back(animation);
	}
	std::stable_sort(animations.begin(), animations.end(), [](Animation const& left,
		Animation const& right) { return left.start_time < right.start_time; });

	std::vector<Phase> phases;
	Values current = Clamp(settings.base);
	int cursor = 0;
	for (auto animation : animations) {
		int start = std::max(cursor, animation.start_time);
		if (start >= animation.end_time) continue;
		if (start > cursor)
			phases.push_back({cursor, start, current, current, false, settings.mode});
		phases.push_back({start, animation.end_time, animation.from, animation.to, true,
			settings.mode});
		current = animation.to;
		cursor = animation.end_time;
	}
	if (cursor < duration)
		phases.push_back({cursor, duration, current, current, false, settings.mode});
	if (phases.empty() && duration > 0)
		phases.push_back({0, duration, current, current, false, settings.mode});
	return phases;
}

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
		frames.push_back({start, end, sampled, sampled, false, phase.mode});
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
		bool had_clip) {
	Polygon combined = band;
	if (had_clip) {
		auto existing = ParsePositiveClip(text);
		if (!existing) return text;
		combined = IntersectConvex(std::move(*existing), band);
		if (combined.size() < 3) return std::nullopt;
		static std::regex clips(R"(\\i?clip\([^)]*\))");
		text = std::regex_replace(text, clips, "");
	}
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
		double from_x, double to_x, double from_opacity, double to_opacity,
		char const *bgr, std::optional<Polygon> clip) {
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
	text = OffsetPosition(c, sampled, std::move(text), from_x, to_x, end - start);
	if (clip) {
		auto clipped = ApplyBandClip(std::move(text), *clip, had_clip);
		if (!clipped) return;
		text = std::move(*clipped);
	}
	text = ApplyTint(std::move(text), bgr);
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
	auto paints = Paints(phase.mode);
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
	int regular_count = std::max(from_regular_count, to_regular_count);
	int from_extra_count = detail_count(from.amount, from_regular_count);
	int to_extra_count = detail_count(to.amount, to_regular_count);
	int extra_count = std::max(from_extra_count, to_extra_count);
	for (size_t source_index = 0; source_index < sources.size(); ++source_index) {
		auto source = sources[source_index];
		bool atomic = imagemask::IsLine(source) || source->Effect.get() == "gradient-fx";
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
				double random_from = Offset(seed, source_index, band,
					static_cast<int>(paint_index), from.offset) * paint.direction *
					displacement_scale;
				double random_to = Offset(seed, source_index, band,
					static_cast<int>(paint_index), to.offset) * paint.direction *
					displacement_scale;
				AddOverlay(out, c, *source,
					static_cast<int>(source->Start) + phase.start,
					static_cast<int>(source->Start) + phase.end,
					random_from, random_to,
					from.opacity * paint.opacity * from_fade * from_opacity_scale,
					to.opacity * paint.opacity * to_fade * to_opacity_scale,
					paint.bgr, clip);
			};
			if (atomic) {
				auto [from_rows, from_columns] = grid(from);
				auto [to_rows, to_columns] = grid(to);
				(void)from_rows; (void)to_rows;
				int columns = std::max(from_columns, to_columns);
				for (int column = 0; column < columns; ++column) {
					double from_active = column < from_columns && from.amount > 0.0 &&
						Noise01(phase_seed, source_index, source_band * 131 + column, 31) *
							100.0 < from.amount;
					double to_active = column < to_columns && to.amount > 0.0 &&
						Noise01(phase_seed, source_index, source_band * 131 + column, 31) *
							100.0 < to.amount;
					Values const& geometry = from_active ? from : to;
					auto normal = from_active ? from_range : to_range;
					auto tangent = from_active ? from_tangent : to_tangent;
					int geometry_columns = from_active ? from_columns : to_columns;
					double segment = (tangent.second - tangent.first) * geometry.width / 100.0;
					double tangent_low = tangent.first + column * segment;
					double tangent_high = std::min(tangent.second, tangent_low + segment);
					if (column >= geometry_columns || tangent_high <= tangent_low) continue;
					auto clip = SlicePolygon(geometry.angle, normal.first, normal.second,
						tangent_low, tangent_high);
					int band = source_band * columns + column;
					add(band, clip, 1.0, from_active, to_active);
					if (peak_amount > 30.0)
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
					double tangent_high = std::min(tangent.second, tangent_low + segment);
					auto clip = SlicePolygon(geometry.angle, centre - geometry.height * .5,
						centre + geometry.height * .5, tangent_low, tangent_high);
					add(band, std::move(clip), 1.0,
						from_active ? 1.0 : 0.0, to_active ? 1.0 : 0.0);
				}

				// The site's second loop draws random 5-25 px strips from the canvas after
				// the regular bands have already modified it. ASS cannot sample its own
				// rendered result, so two accumulated echoes approximate that feedback.
				for (int strip = 0; strip < extra_count; ++strip) {
					double y_unit = Noise01(phase_seed, source_index, strip,
						static_cast<int>(paint_index) * 7 + 1);
					double height_unit = Noise01(phase_seed, source_index, strip,
						static_cast<int>(paint_index) * 7 + 2);
					double strip_height = std::max(1.0, static_cast<double>(std::lround(
						height * (.45 + 1.65 * height_unit))));
					double low = from_range.first + y_unit * std::max(0.0,
						from_range.second - from_range.first - strip_height);
					double tangent_span = from_tangent.second - from_tangent.first;
					double segment = tangent_span * from.width / 100.0;
					double tangent_unit = Noise01(phase_seed, source_index, strip,
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
	if (settings.show_base)
		for (auto source : sources) {
			AssDialogue base(*source);
			base.Comment = false;
			base.Effect = effect_name;
			out.push_back(std::move(base));
		}

	auto phases = Phases(settings, duration);
	for (size_t phase_index = 0; phase_index < phases.size(); ++phase_index) {
		auto const& phase = phases[phase_index];
		uint32_t phase_seed = settings.seed ^ static_cast<uint32_t>(phase_index * 0x9e3779b9U);
		auto frames = FramePhases(c, *sources.front(), phase);
		for (size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
			auto const& frame = frames[frame_index];
			GeneratePass(out, c, sources, bounds, frame, frame.from, frame.to,
				frame.from.height, 1.0, 1.0,
				phase_seed ^ static_cast<uint32_t>(frame_index * 0x85ebca6bU));
		}
	}
	if (out.empty() && !settings.show_base) {
		AssDialogue empty(*sources.front());
		empty.Comment = false;
		empty.Effect = effect_name;
		empty.Text = InjectTags(empty.Text.get(), "\\alpha&HFF&");
		out.push_back(std::move(empty));
	}
	return out;
}

} // namespace

std::vector<std::string> ModeNames() {
	return {"difference", "source-atop", "destination-out", "lighter", "multiply",
		"screen", "overlay", "darken", "lighten", "color-dodge", "color-burn",
		"hard-light", "soft-light", "exclusion", "hue", "color", "luminosity"};
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
	auto names = ModeNames();
	bool animated = std::any_of(settings->animations.begin(), settings->animations.end(),
		[](Animation const& animation) { return animation.enabled; });
	return from_wx(wxString::Format(animated ? _("%s, animation") : _("%s, no animation"),
		to_wx(names[static_cast<size_t>(settings->mode)])));
}

Settings LoadSettingsForSelection(agi::Context *c) {
	for (auto line : c->selectionController->GetSortedSelection()) {
		if (!IsSource(*c->ass, line)) continue;
		auto encoded = Extra(*c->ass, *line, data_key);
		if (encoded) {
			auto parsed = DeserializeSettings(*encoded);
			if (parsed) return *parsed;
		}
	}
	return {};
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
