// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

/// @file typesetting_gradient.cpp
/// @brief Generation of clip-based and per-character ASS gradients

#include "typesetting_gradient.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "selection_controller.h"
#include "typesetting_transform.h"
#include "video_controller.h"

#include <libaegisub/format.h>
#include <libaegisub/ass/uuencode.h>
#include <libaegisub/unicode.h>

#include <unicode/uchar.h>
#include <unicode/utf8.h>

#include <wx/intl.h>

#include <boost/polygon/polygon.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace typesetting::gradient {
namespace {

constexpr char const *gradient_effect = "gradient-fx";
constexpr char const *gradient_data_key = "aegisub/gradient-fx";
constexpr char const *gradient_source_key = "aegisub/gradient-fx-source";
constexpr std::string_view gradient_clipboard_settings = "{:Aegisub Gradient Settings:";
constexpr std::string_view gradient_clipboard_source = "{:Aegisub Gradient Source:";
double VectorSeamOverlap(double requested) {
	return std::max(0.0, requested);
}

int ClampPosition(int value) {
	return std::clamp(value, 0, 100);
}

std::vector<Stop> SortedStops(std::vector<Stop> stops) {
	for (auto& stop : stops) stop.position = ClampPosition(stop.position);
	std::stable_sort(stops.begin(), stops.end(), [](Stop const& left, Stop const& right) {
		return left.position < right.position;
	});
	return stops;
}

std::vector<Stop> ParseStops(std::string const& encoded, std::vector<Stop> fallback) {
	std::vector<Stop> parsed;
	size_t at = 0;
	while (at < encoded.size()) {
		size_t end = encoded.find(';', at);
		std::string_view item(encoded.data() + at,
			end == std::string::npos ? encoded.size() - at : end - at);
		size_t colon = item.find(':');
		if (colon != std::string_view::npos) {
			try {
				int position = std::stoi(std::string(item.substr(0, colon)));
				std::string colour_text(item.substr(colon + 1));
				if (colour_text.starts_with("&H") && colour_text.size() == 10)
					parsed.push_back({ClampPosition(position), agi::Color(colour_text)});
			}
			catch (...) { }
		}
		if (end == std::string::npos) break;
		at = end + 1;
	}
	if (parsed.size() < 2) return fallback;
	return SortedStops(std::move(parsed));
}

std::string SerializeStops(std::vector<Stop> const& stops) {
	auto sorted = SortedStops(stops);
	std::string out;
	for (auto const& stop : sorted) {
		if (!out.empty()) out += ';';
		out += std::to_string(stop.position) + ':' + stop.colour.GetAssStyleFormatted();
	}
	return out;
}

std::string SerializeMotion(Motion const& motion) {
	return agi::format("%d,%d,%d,%d,%d,%d,%d,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g",
		motion.enabled, static_cast<int>(motion.mode),
		static_cast<int>(MotionOutside::Clamp),
		motion.end_at_line, motion.start_time, motion.end_time, motion.cycle_time,
		motion.accel, motion.start_position, motion.end_position,
		motion.start_width, motion.middle_width, motion.end_width);
}

std::optional<Motion> DeserializeMotion(std::string const& encoded, Motion fallback) {
	std::vector<std::string> fields;
	size_t at = 0;
	for (;;) {
		size_t end = encoded.find(',', at);
		fields.push_back(encoded.substr(at,
			end == std::string::npos ? std::string::npos : end - at));
		if (end == std::string::npos) break;
		at = end + 1;
	}
	if (fields.size() != 12 && fields.size() != 13) return std::nullopt;
	try {
		fallback.enabled = std::stoi(fields[0]) != 0;
		fallback.mode = static_cast<MotionMode>(std::clamp(std::stoi(fields[1]), 0, 3));
		fallback.outside = MotionOutside::Clamp;
		fallback.end_at_line = std::stoi(fields[3]) != 0;
		fallback.start_time = std::clamp(std::stoi(fields[4]), 0, 3600000);
		fallback.end_time = std::clamp(std::stoi(fields[5]), 0, 3600000);
		fallback.cycle_time = std::clamp(std::stoi(fields[6]), 1, 3600000);
		fallback.accel = std::clamp(std::stod(fields[7]), 0.01, 100.0);
		fallback.start_position = std::clamp(std::stod(fields[8]), -1000.0, 1000.0);
		fallback.end_position = std::clamp(std::stod(fields[9]), -1000.0, 1000.0);
		fallback.start_width = std::clamp(std::stod(fields[10]), 0.1, 1000.0);
		if (fields.size() == 13) {
			fallback.middle_width = std::clamp(std::stod(fields[11]), 0.1, 1000.0);
			fallback.end_width = std::clamp(std::stod(fields[12]), 0.1, 1000.0);
		}
		else {
			fallback.end_width = std::clamp(std::stod(fields[11]), 0.1, 1000.0);
			fallback.middle_width = (fallback.start_width + fallback.end_width) * .5;
		}
		return fallback;
	}
	catch (...) { return std::nullopt; }
}

std::string SerializeGeometry(GeometrySnapshot const& geometry) {
	return agi::format("%d,%d,%d,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g",
		geometry.valid, geometry.script_w, geometry.script_h, geometry.centre_x,
		geometry.centre_y, geometry.corners[0], geometry.corners[1], geometry.corners[2],
		geometry.corners[3], geometry.corners[4], geometry.corners[5],
		geometry.corners[6], geometry.corners[7]);
}

std::optional<GeometrySnapshot> DeserializeGeometry(std::string const& encoded) {
	std::vector<std::string> fields;
	size_t at = 0;
	for (;;) {
		size_t end = encoded.find(',', at);
		fields.push_back(encoded.substr(at,
			end == std::string::npos ? std::string::npos : end - at));
		if (end == std::string::npos) break;
		at = end + 1;
	}
	if (fields.size() != 13) return std::nullopt;
	try {
		GeometrySnapshot out;
		out.valid = std::stoi(fields[0]) != 0;
		out.script_w = std::max(1, std::stoi(fields[1]));
		out.script_h = std::max(1, std::stoi(fields[2]));
		out.centre_x = std::stod(fields[3]);
		out.centre_y = std::stod(fields[4]);
		for (size_t i = 0; i < out.corners.size(); ++i)
			out.corners[i] = std::stod(fields[i + 5]);
		return out;
	}
	catch (...) { return std::nullopt; }
}

std::string SerializeSettings(Settings const& settings) {
	return agi::format("5|%d|%d|%d|%d|%.6g|1|%s|%s|%d|%s|%s|%d|%s|%s|%d|%s|%s",
		static_cast<int>(settings.kind), static_cast<int>(settings.output), settings.angle,
		settings.pixels_per_strip, settings.anti_strip_overlap,
		SerializeMotion(settings.motion), SerializeGeometry(settings.geometry),
		settings.primary.enabled,
		SerializeStops(settings.primary.stops), SerializeMotion(settings.primary.motion),
		settings.outline.enabled, SerializeStops(settings.outline.stops),
		SerializeMotion(settings.outline.motion), settings.shadow.enabled,
		SerializeStops(settings.shadow.stops), SerializeMotion(settings.shadow.motion));
}

std::vector<std::string> SplitSettings(std::string const& encoded) {
	std::vector<std::string> fields;
	size_t at = 0;
	for (;;) {
		size_t end = encoded.find('|', at);
		fields.push_back(encoded.substr(at,
			end == std::string::npos ? std::string::npos : end - at));
		if (end == std::string::npos) break;
		at = end + 1;
	}
	return fields;
}

std::optional<Settings> DeserializeSettings(std::string const& encoded, Settings fallback) {
	auto fields = SplitSettings(encoded);
	if (fields[0] == "3" || fields[0] == "4" || fields[0] == "5") {
		bool has_geometry = fields[0] == "4" || fields[0] == "5";
		if (fields.size() != (has_geometry ? 18 : 17)) return std::nullopt;
		try {
			int kind = std::stoi(fields[1]);
			int output = std::stoi(fields[2]);
			fallback.kind = kind == 1 ? Kind::Radial : Kind::Linear;
			fallback.output = output == 1 ? Output::Characters :
				output == 2 ? Output::Shapes : Output::Clips;
			fallback.angle = std::clamp(std::stoi(fields[3]), 0, 359);
			fallback.pixels_per_strip = std::clamp(std::stoi(fields[4]), 1, 100);
			fallback.anti_strip_overlap = std::clamp(std::stod(fields[5]), 0.0, 100.0);
			bool was_shared = std::stoi(fields[6]) != 0;
			fallback.shared_motion = true;
			auto shared = DeserializeMotion(fields[7], fallback.motion);
			size_t channel = has_geometry ? 9 : 8;
			if (has_geometry) {
				auto geometry = DeserializeGeometry(fields[8]);
				if (!geometry) return std::nullopt;
				fallback.geometry = *geometry;
			}
			auto primary = DeserializeMotion(fields[channel + 2], fallback.primary.motion);
			auto outline = DeserializeMotion(fields[channel + 5], fallback.outline.motion);
			auto shadow = DeserializeMotion(fields[channel + 8], fallback.shadow.motion);
			if (!shared || !primary || !outline || !shadow) return std::nullopt;
			fallback.motion = *shared;
			fallback.motion.outside = MotionOutside::Clamp;
			fallback.primary.enabled = std::stoi(fields[channel]) != 0;
			fallback.primary.stops = ParseStops(fields[channel + 1], fallback.primary.stops);
			fallback.primary.motion = *primary;
			fallback.outline.enabled = std::stoi(fields[channel + 3]) != 0;
			fallback.outline.stops = ParseStops(fields[channel + 4], fallback.outline.stops);
			fallback.outline.motion = *outline;
			fallback.shadow.enabled = std::stoi(fields[channel + 6]) != 0;
			fallback.shadow.stops = ParseStops(fields[channel + 7], fallback.shadow.stops);
			fallback.shadow.motion = *shadow;
			if (!was_shared) {
				if (fallback.primary.enabled && primary->enabled) fallback.motion = *primary;
				else if (fallback.outline.enabled && outline->enabled) fallback.motion = *outline;
				else if (fallback.shadow.enabled && shadow->enabled) fallback.motion = *shadow;
			}
			fallback.motion.outside = MotionOutside::Clamp;
			return fallback;
		}
		catch (...) { return std::nullopt; }
	}
	if (fields.size() != 12 || (fields[0] != "1" && fields[0] != "2")) return std::nullopt;
	try {
		int kind = std::stoi(fields[1]);
		int output = std::stoi(fields[2]);
		fallback.kind = kind == 1 ? Kind::Radial : Kind::Linear;
		fallback.output = output == 1 ? Output::Characters :
			output == 2 ? Output::Shapes : Output::Clips;
		fallback.angle = std::clamp(std::stoi(fields[3]), 0, 359);
		fallback.pixels_per_strip = std::clamp(std::stoi(fields[4]), 1, 100);
		fallback.anti_strip_overlap = fields[0] == "1" ?
			(std::stoi(fields[5]) != 0 ? 0.4 : 0.0) :
			std::clamp(std::stod(fields[5]), 0.0, 100.0);
		fallback.primary.enabled = std::stoi(fields[6]) != 0;
		fallback.primary.stops = ParseStops(fields[7], fallback.primary.stops);
		fallback.outline.enabled = std::stoi(fields[8]) != 0;
		fallback.outline.stops = ParseStops(fields[9], fallback.outline.stops);
		fallback.shadow.enabled = std::stoi(fields[10]) != 0;
		fallback.shadow.stops = ParseStops(fields[11], fallback.shadow.stops);
		fallback.shared_motion = true;
		fallback.motion.outside = MotionOutside::Clamp;
		return fallback;
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<std::string> ExtradataValue(AssFile const& file, AssDialogue const& line,
	char const *key) {
	for (auto const& extra : file.GetExtradata(line.ExtradataIds))
		if (extra.key == key) return extra.value;
	return std::nullopt;
}

std::optional<std::string> GradientData(AssFile const& file, AssDialogue const& line) {
	return ExtradataValue(file, line, gradient_data_key);
}

std::optional<std::string> GradientSourceData(AssFile const& file, AssDialogue const& line) {
	return ExtradataValue(file, line, gradient_source_key);
}

bool IsGradientEffect(AssDialogue const *line) {
	return line && line->Effect.get() == gradient_effect;
}

bool IsGradientSource(AssFile const& file, AssDialogue const *line) {
	return IsGradientEffect(line) && GradientData(file, *line).has_value() &&
		(line->Comment || GradientSourceData(file, *line).has_value());
}

struct SourceGroup {
	AssDialogue *anchor = nullptr;
	std::unique_ptr<AssDialogue> stored_source;
	AssDialogue *source = nullptr;
	std::vector<AssDialogue *> existing;
	bool editing = false;
};

std::vector<SourceGroup> CollectSourceGroups(agi::Context *c) {
	auto selected = c->selectionController->GetSelectedSet();
	std::vector<AssDialogue *> rows;
	rows.reserve(c->ass->Events.size());
	for (auto& line : c->ass->Events) rows.push_back(&line);

	std::set<AssDialogue *> claimed;
	std::vector<SourceGroup> groups;
	for (size_t i = 0; i < rows.size();) {
		auto anchor = rows[i];
		if (!IsGradientSource(*c->ass, anchor)) { ++i; continue; }
		SourceGroup group;
		group.anchor = anchor;
		group.source = anchor;
		group.existing.push_back(anchor);
		group.editing = true;
		if (auto saved_source = GradientSourceData(*c->ass, *anchor)) {
			try {
				group.stored_source = std::make_unique<AssDialogue>(*saved_source);
				group.stored_source->Row = anchor->Row;
				group.source = group.stored_source.get();
			}
			catch (...) {
				group.stored_source.reset();
			}
		}
		size_t j = i + 1;
		while (j < rows.size() && IsGradientEffect(rows[j]) &&
			rows[j]->Start == anchor->Start && rows[j]->End == anchor->End &&
			!IsGradientSource(*c->ass, rows[j]))
			group.existing.push_back(rows[j++]);
		bool selected_group = std::any_of(group.existing.begin(), group.existing.end(),
			[&](AssDialogue *line) { return selected.count(line) != 0; });
		claimed.insert(group.existing.begin(), group.existing.end());
		if (selected_group) groups.push_back(std::move(group));
		i = j;
	}

	for (auto line : c->selectionController->GetSortedSelection()) {
		if (claimed.count(line) || IsGradientEffect(line)) continue;
		SourceGroup group;
		group.anchor = line;
		group.source = line;
		group.existing.push_back(line);
		groups.push_back(std::move(group));
	}
	std::stable_sort(groups.begin(), groups.end(), [](SourceGroup const& left,
		SourceGroup const& right) { return left.anchor->Row < right.anchor->Row; });
	return groups;
}

std::vector<AssDialogue *> GroupSources(std::vector<SourceGroup> const& groups) {
	std::vector<AssDialogue *> sources;
	sources.reserve(groups.size());
	for (auto const& group : groups) sources.push_back(group.source);
	return sources;
}

enum class ColourChannel {
	Primary,
	Outline,
	Shadow
};

constexpr std::array<ColourChannel, 3> paint_order = {
	ColourChannel::Shadow, ColourChannel::Outline, ColourChannel::Primary
};

size_t ChannelIndex(ColourChannel channel) {
	return static_cast<size_t>(channel);
}

struct LineColours {
	agi::Color primary;
	agi::Color outline;
	agi::Color shadow;
	double border_x = 0;
	double border_y = 0;
	double shadow_x = 0;
	double shadow_y = 0;
};

LineColours EffectiveLineColours(agi::Context *c, AssDialogue const *line) {
	AssStyle fallback;
	LineColours out;
	auto apply_style = [&](std::string const& name) {
		AssStyle const *style = c->ass->GetStyle(name);
		if (!style) style = &fallback;
		out = {style->primary, style->outline, style->shadow,
			style->outline_w, style->outline_w, style->shadow_w, style->shadow_w};
	};
	apply_style(line ? line->Style.get() : std::string());
	if (!line) return out;

	auto set_colour = [](agi::Color& target, AssOverrideTag const& tag) {
		if (tag.Params.empty() || tag.Params.front().omitted) return;
		auto colour = tag.Params.front().Get<agi::Color>(target);
		target.r = colour.r;
		target.g = colour.g;
		target.b = colour.b;
	};
	auto set_alpha = [](agi::Color& target, AssOverrideTag const& tag) {
		if (!tag.Params.empty() && !tag.Params.front().omitted)
			target.a = static_cast<unsigned char>(tag.Params.front().Get<int>(target.a));
	};

	for (auto& block : line->ParseTags()) {
		if (block->GetType() == AssBlockType::OVERRIDE) {
			for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags) {
				if (tag.Name == "\\r") {
					std::string name = tag.Params.empty() ? std::string() :
						tag.Params.front().Get<std::string>(std::string());
					apply_style(name.empty() ? line->Style.get() : name);
				}
				else if (tag.Name == "\\c" || tag.Name == "\\1c") set_colour(out.primary, tag);
				else if (tag.Name == "\\3c") set_colour(out.outline, tag);
				else if (tag.Name == "\\4c") set_colour(out.shadow, tag);
				else if (tag.Name == "\\alpha") {
					set_alpha(out.primary, tag);
					set_alpha(out.outline, tag);
					set_alpha(out.shadow, tag);
				}
				else if (tag.Name == "\\1a") set_alpha(out.primary, tag);
				else if (tag.Name == "\\3a") set_alpha(out.outline, tag);
				else if (tag.Name == "\\4a") set_alpha(out.shadow, tag);
				else if (!tag.Params.empty() && !tag.Params.front().omitted) {
					double value = tag.Params.front().Get<double>();
					if (tag.Name == "\\bord") out.border_x = out.border_y = value;
					else if (tag.Name == "\\xbord") out.border_x = value;
					else if (tag.Name == "\\ybord") out.border_y = value;
					else if (tag.Name == "\\shad") out.shadow_x = out.shadow_y = value;
					else if (tag.Name == "\\xshad") out.shadow_x = value;
					else if (tag.Name == "\\yshad") out.shadow_y = value;
				}
			}
		}
		else if ((block->GetType() == AssBlockType::PLAIN ||
			block->GetType() == AssBlockType::DRAWING) && !block->GetText().empty())
			break;
	}
	return out;
}

bool ChannelEnabled(Settings const& settings, ColourChannel channel) {
	if (channel == ColourChannel::Outline) return settings.outline.enabled;
	if (channel == ColourChannel::Shadow) return settings.shadow.enabled;
	return settings.primary.enabled;
}

bool ChannelVisible(LineColours const& line, ColourChannel channel) {
	if (channel == ColourChannel::Outline)
		return line.outline.a != 255 &&
			(std::abs(line.border_x) > 1e-9 || std::abs(line.border_y) > 1e-9);
	if (channel == ColourChannel::Shadow)
		return line.shadow.a != 255 &&
			(std::abs(line.shadow_x) > 1e-9 || std::abs(line.shadow_y) > 1e-9);
	return line.primary.a != 255;
}

Settings ChannelSettings(Settings settings, std::optional<ColourChannel> channel) {
	settings.primary.enabled = channel == ColourChannel::Primary && settings.primary.enabled;
	settings.outline.enabled = channel == ColourChannel::Outline && settings.outline.enabled;
	settings.shadow.enabled = channel == ColourChannel::Shadow && settings.shadow.enabled;
	return settings;
}

Settings DefaultSettings(agi::Context *c, AssDialogue const *line) {
	Settings out;
	auto colours = EffectiveLineColours(c, line);
	out.primary = {true, {{0, colours.primary}, {100, colours.primary}}};
	out.outline = {false, {{0, colours.outline}, {100, colours.outline}}};
	out.shadow = {false, {{0, colours.shadow}, {100, colours.shadow}}};
	return out;
}

std::string Number(double value) {
	if (std::abs(value) < .0005) value = 0;
	std::ostringstream out;
	out << std::fixed << std::setprecision(3) << value;
	std::string text = out.str();
	while (text.size() > 1 && text.back() == '0') text.pop_back();
	if (!text.empty() && text.back() == '.') text.pop_back();
	return text;
}

std::string Alpha(agi::Color const& colour) {
	return agi::format("&H%02X&", static_cast<int>(colour.a));
}

agi::Color SampleSorted(std::vector<Stop> const& stops, double position) {
	if (stops.empty()) return agi::Color();
	if (stops.size() == 1 || position * 100.0 <= stops.front().position)
		return stops.front().colour;
	if (position * 100.0 >= stops.back().position) return stops.back().colour;

	double wanted = std::clamp(position, 0.0, 1.0) * 100.0;
	for (size_t i = 1; i < stops.size(); ++i) {
		if (wanted > stops[i].position) continue;
		double span = stops[i].position - stops[i - 1].position;
		double factor = span <= 0 ? 1.0 : (wanted - stops[i - 1].position) / span;
		auto blend = [&](unsigned char left, unsigned char right) {
			return static_cast<unsigned char>(std::clamp(std::lround(
				left + (right - left) * factor), 0l, 255l));
		};
		return agi::Color(blend(stops[i - 1].colour.r, stops[i].colour.r),
			blend(stops[i - 1].colour.g, stops[i].colour.g),
			blend(stops[i - 1].colour.b, stops[i].colour.b),
			blend(stops[i - 1].colour.a, stops[i].colour.a));
	}
	return stops.back().colour;
}

struct PreparedChannel {
	bool enabled = false;
	std::vector<Stop> stops;
};

struct PreparedPaint {
	PreparedChannel primary;
	PreparedChannel outline;
	PreparedChannel shadow;

	explicit PreparedPaint(Settings const& settings)
	: primary{settings.primary.enabled, SortedStops(settings.primary.stops)}
	, outline{settings.outline.enabled, SortedStops(settings.outline.stops)}
	, shadow{settings.shadow.enabled, SortedStops(settings.shadow.stops)} {
	}
};

PreparedChannel const& PaintChannel(PreparedPaint const& paint, ColourChannel channel) {
	if (channel == ColourChannel::Outline) return paint.outline;
	if (channel == ColourChannel::Shadow) return paint.shadow;
	return paint.primary;
}

int AssChannelIndex(ColourChannel channel) {
	if (channel == ColourChannel::Outline) return 3;
	if (channel == ColourChannel::Shadow) return 4;
	return 1;
}

std::string ChannelPaintTags(PreparedChannel const& channel, int index, double factor) {
	auto colour = SampleSorted(channel.stops, factor);
	return agi::format("\\%dc%s\\%da%s", index, colour.GetAssOverrideFormatted(),
		index, Alpha(colour));
}

std::string ColourTags(int index, agi::Color const& colour) {
	return agi::format("\\%dc%s\\%da%s", index, colour.GetAssOverrideFormatted(),
		index, Alpha(colour));
}

Motion const& ChannelMotion(Settings const& settings, ColourChannel channel) {
	(void)channel;
	return settings.motion;
}

int LineDuration(AssDialogue const& line) {
	return std::max(0, static_cast<int>(line.End) - static_cast<int>(line.Start));
}

std::vector<int> MotionSampleTimes(agi::Context *c, AssDialogue const& line,
		Motion const& motion) {
	int duration = LineDuration(line);
	std::vector<int> times = {0, duration};
	if (duration <= 0) return times;

	auto const& fps = c->project->Timecodes();
	if (fps.IsLoaded()) {
		int first = fps.FrameAtTime(line.Start, agi::vfr::START);
		int last = fps.FrameAtTime(line.End, agi::vfr::END) + 1;
		int stride = std::max(1, static_cast<int>(std::ceil(
			std::max(0, last - first) / 4096.0)));
		for (int frame = first; frame <= last; frame += stride) {
			int relative = fps.TimeAtFrame(frame, agi::vfr::START) -
				static_cast<int>(line.Start);
			if (relative > 0 && relative < duration) times.push_back(relative);
		}
	}
	else {
		int step = std::max(20, static_cast<int>(std::ceil(duration / 4096.0)));
		for (int at = step; at < duration; at += step) times.push_back(at);
	}

	auto add_boundary = [&](int at) {
		if (at > 0 && at < duration) {
			times.push_back(at);
			times.push_back(at - 1);
		}
	};
	if (motion.mode == MotionMode::FitLine) {
		add_boundary(duration);
	}
	else {
		add_boundary(motion.start_time);
		if (motion.mode == MotionMode::Once)
			add_boundary(motion.end_at_line ? duration : motion.end_time);
		else {
			int cycle = std::max(1, motion.cycle_time);
			for (int at = motion.start_time + cycle, count = 0;
				at < duration && count < 4096; at += cycle, ++count)
				add_boundary(at);
		}
	}

	std::sort(times.begin(), times.end());
	times.erase(std::unique(times.begin(), times.end()), times.end());
	return times;
}

struct MotionFrame {
	double position = 0;
	double width = 1;
};

MotionFrame MotionAt(Motion const& motion, int time, int duration) {
	double progress = 0;
	if (motion.mode == MotionMode::FitLine) {
		progress = duration <= 0 ? 1.0 : static_cast<double>(time) / duration;
	}
	else if (motion.mode == MotionMode::Once) {
		int start = std::clamp(motion.start_time, 0, duration);
		int end = motion.end_at_line ? duration : std::clamp(motion.end_time, start, duration);
		progress = end <= start ? (time >= end ? 1.0 : 0.0) :
			std::clamp(static_cast<double>(time - start) / (end - start), 0.0, 1.0);
	}
	else if (time > motion.start_time) {
		double leg = static_cast<double>(time - motion.start_time) /
			std::max(1, motion.cycle_time);
		double fraction = leg - std::floor(leg);
		if (motion.mode == MotionMode::PingPong &&
			static_cast<long long>(std::floor(leg)) % 2) fraction = 1.0 - fraction;
		progress = fraction;
	}

	progress = std::pow(std::clamp(progress, 0.0, 1.0),
		std::clamp(motion.accel, 0.01, 100.0));
	double position = motion.start_position +
		(motion.end_position - motion.start_position) * progress;
	double width = progress < .5 ?
		motion.start_width + (motion.middle_width - motion.start_width) * progress * 2.0 :
		motion.middle_width + (motion.end_width - motion.middle_width) * (progress - .5) * 2.0;
	return {position / 100.0, std::max(0.001, width / 100.0)};
}

agi::Color MotionColour(PreparedChannel const& channel, Motion const& motion,
		double spatial_position, int time, int duration) {
	auto frame = MotionAt(motion, time, duration);
	double sample = 0.5 + (spatial_position - frame.position) / frame.width;
	return SampleSorted(channel.stops, sample);
}

bool SameColour(agi::Color const& left, agi::Color const& right) {
	return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
}

bool MergePrimaryAndOutline(Settings const& settings) {
	if (!settings.primary.enabled || !settings.outline.enabled) return false;
	auto primary = SortedStops(settings.primary.stops);
	auto outline = SortedStops(settings.outline.stops);
	std::set<int> positions;
	for (auto const& stop : primary) positions.insert(stop.position);
	for (auto const& stop : outline) positions.insert(stop.position);
	return std::all_of(positions.begin(), positions.end(), [&](int position) {
		return SameColour(SampleSorted(primary, position / 100.0),
			SampleSorted(outline, position / 100.0));
	});
}

struct TimedColour {
	int time = 0;
	agi::Color colour;
};

int ColourInterpolationError(TimedColour const& left, TimedColour const& middle,
		TimedColour const& right) {
	if (right.time <= left.time) return SameColour(left.colour, middle.colour) ? 0 : 255;
	double factor = static_cast<double>(middle.time - left.time) / (right.time - left.time);
	auto error = [&](unsigned char a, unsigned char b, unsigned char actual) {
		return std::abs(static_cast<int>(actual) - static_cast<int>(std::lround(
			a + (static_cast<int>(b) - a) * factor)));
	};
	return std::max({error(left.colour.r, right.colour.r, middle.colour.r),
		error(left.colour.g, right.colour.g, middle.colour.g),
		error(left.colour.b, right.colour.b, middle.colour.b),
		error(left.colour.a, right.colour.a, middle.colour.a)});
}

void KeepColourKeys(std::vector<TimedColour> const& points, size_t first, size_t last,
		std::vector<bool>& keep) {
	if (last <= first + 1) return;
	int largest = 1;
	size_t selected = first;
	for (size_t i = first + 1; i < last; ++i) {
		int error = ColourInterpolationError(points[first], points[i], points[last]);
		if (error > largest) {
			largest = error;
			selected = i;
		}
	}
	if (selected == first) return;
	keep[selected] = true;
	KeepColourKeys(points, first, selected, keep);
	KeepColourKeys(points, selected, last, keep);
}

std::string AnimatedChannelPaint(agi::Context *c, AssDialogue const& line,
		Settings const& settings, ColourChannel colour_channel,
		PreparedChannel const& channel, int index, double factor) {
	auto const& motion = ChannelMotion(settings, colour_channel);
	if (!motion.enabled)
		return ChannelPaintTags(channel, index, factor);

	int duration = LineDuration(line);
	auto times = MotionSampleTimes(c, line, motion);
	std::vector<TimedColour> points;
	points.reserve(times.size());
	for (int time : times)
		points.push_back({time, MotionColour(channel, motion, factor, time, duration)});
	if (points.empty()) return ChannelPaintTags(channel, index, factor);

	std::vector<bool> keep(points.size(), false);
	keep.front() = keep.back() = true;
	KeepColourKeys(points, 0, points.size() - 1, keep);
	std::string tags = ColourTags(index, points.front().colour);
	int previous_time = points.front().time;
	agi::Color previous_colour = points.front().colour;
	for (size_t i = 1; i < points.size(); ++i) {
		if (!keep[i]) continue;
		if (!SameColour(previous_colour, points[i].colour))
			tags += agi::format("\\t(%d,%d,1,%s)", previous_time, points[i].time,
				ColourTags(index, points[i].colour));
		previous_time = points[i].time;
		previous_colour = points[i].colour;
	}
	return tags;
}

std::string AnimatedPaintTags(agi::Context *c, AssDialogue const& line,
		Settings const& settings, PreparedPaint const& paint, double factor) {
	std::string tags;
	for (auto channel : {ColourChannel::Primary, ColourChannel::Outline, ColourChannel::Shadow}) {
		if (!ChannelEnabled(settings, channel)) continue;
		tags += AnimatedChannelPaint(c, line, settings, channel, PaintChannel(paint, channel),
			AssChannelIndex(channel), factor);
	}
	return tags;
}

std::string IsolationTags(ColourChannel visible, bool primary_with_outline = false) {
	std::string tags;
	for (auto channel : {ColourChannel::Primary, ColourChannel::Outline, ColourChannel::Shadow}) {
		bool shown = channel == visible || (primary_with_outline &&
			(channel == ColourChannel::Primary || channel == ColourChannel::Outline));
		if (!shown) tags += agi::format("\\%da&HFF&", AssChannelIndex(channel));
	}
	return tags;
}

std::string PaintTags(PreparedPaint const& paint, double factor) {
	std::string tags;
	auto add = [&](PreparedChannel const& channel, int index) {
		if (!channel.enabled) return;
		auto colour = SampleSorted(channel.stops, factor);
		tags += agi::format("\\%dc%s\\%da%s", index, colour.GetAssOverrideFormatted(),
			index, Alpha(colour));
	};
	add(paint.primary, 1);
	add(paint.outline, 3);
	add(paint.shadow, 4);
	return tags;
}

PreparedChannel const& ShapeChannel(PreparedPaint const& paint,
		typesetting::ShapeEditor::LayerKind kind) {
	if (kind == typesetting::ShapeEditor::LayerKind::Outline) return paint.outline;
	if (kind == typesetting::ShapeEditor::LayerKind::Shadow) return paint.shadow;
	return paint.primary;
}

bool IsRemovedTag(std::string const& name, Settings const& settings, bool clips) {
	if (clips && (name == "\\clip" || name == "\\iclip")) return true;
	if (settings.primary.enabled && (name == "\\c" || name == "\\1c" || name == "\\1a"))
		return true;
	if (settings.outline.enabled && (name == "\\3c" || name == "\\3a")) return true;
	if (settings.shadow.enabled && (name == "\\4c" || name == "\\4a")) return true;
	return false;
}

void CleanOverride(AssDialogueBlockOverride& block, Settings const& settings, bool clips) {
	for (auto& tag : block.Tags)
		for (auto& parameter : tag.Params)
			if (parameter.GetType() == VariableDataType::BLOCK)
				CleanOverride(*parameter.Get<AssDialogueBlockOverride*>(), settings, clips);

	block.Tags.erase(std::remove_if(block.Tags.begin(), block.Tags.end(),
		[&](AssOverrideTag const& tag) { return IsRemovedTag(tag.Name, settings, clips); }),
		block.Tags.end());
}

/// Remove colours which would override the generated values. A reset (\r) is kept, and
/// the generated values are inserted after every override block later, so it cannot make
/// the gradient disappear halfway through the line.
std::string CleanText(AssDialogue const& source, Settings const& settings, bool clips) {
	auto blocks = source.ParseTags();
	std::string out;
	for (auto& block : blocks) {
		if (block->GetType() == AssBlockType::OVERRIDE) {
			auto& override_block = *static_cast<AssDialogueBlockOverride*>(block.get());
			CleanOverride(override_block, settings, clips);
			if (override_block.Tags.empty()) continue;
		}
		out += block->GetText();
	}
	return out;
}

/// Put the paint in the line's opening override block when it has one, otherwise make
/// one. It only has to be repeated after a reset: all authored colour tags were removed
/// by CleanText, while ordinary override blocks do not clear the active gradient.
std::string InjectLineTags(std::string const& clean, std::string const& tags,
		bool after_every_override = false) {
	std::string out;
	size_t at = 0;
	if (!clean.empty() && clean.front() == '{') {
		size_t close = clean.find('}', 1);
		if (close != std::string::npos) {
			std::string_view body(clean.data() + 1, close - 1);
			if (body.find('\\') != std::string_view::npos) {
				out = clean.substr(0, close) + tags + '}';
				at = close + 1;
			}
		}
	}
	if (out.empty()) out = "{" + tags + "}";

	while (at < clean.size()) {
		size_t open = clean.find('{', at);
		if (open == std::string::npos) {
			out += clean.substr(at);
			break;
		}
		out += clean.substr(at, open - at);
		size_t close = clean.find('}', open + 1);
		if (close == std::string::npos) {
			out += clean.substr(open);
			break;
		}
		std::string_view body(clean.data() + open + 1, close - open - 1);
		out += clean.substr(open, close - open);
		if (body.find('\\') != std::string_view::npos &&
			(after_every_override || body.find("\\r") != std::string_view::npos)) out += tags;
		out += '}';
		at = close + 1;
	}
	return out;
}

/// Every clip/channel copy must share an explicit anchor, otherwise subtitle collision
/// avoidance stacks the copies instead of painting them on top of one another. Resolve
/// unpositioned lines from their alignment and effective margins, as the visual tools do.
/// Keep authored positions and moves, and leave the saved original source untouched.
std::string PositionedClipText(AssFile& file, AssDialogue const& source,
		Settings const& settings) {
	auto clean = CleanText(source, settings, true);
	AssStyle fallback;
	auto style = file.GetStyle(source.Style.get());
	if (!style) style = &fallback;
	int alignment = style->alignment;
	bool found_alignment = false;
	for (auto const& block : source.ParseTags()) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		for (auto const& tag : static_cast<AssDialogueBlockOverride const*>(block.get())->Tags) {
			size_t coordinates = tag.Name == "\\pos" ? 2 : tag.Name == "\\move" ? 4 : 0;
			if (coordinates && tag.Params.size() >= coordinates &&
				std::none_of(tag.Params.begin(), tag.Params.begin() + coordinates,
					[](AssOverrideParameter const& param) { return param.omitted; }))
				return clean;
			if (!found_alignment && (tag.Name == "\\an" || tag.Name == "\\a")) {
				found_alignment = true;
				if (tag.Params.empty() || tag.Params.front().omitted) continue;
				int value = tag.Params.front().Get<int>();
				if (tag.Name == "\\a") value = AssStyle::SsaToAss(value);
				if (value >= 1 && value <= 9) alignment = value;
			}
		}
	}

	auto margin = source.Margin;
	for (size_t i = 0; i < margin.size(); ++i)
		if (!margin[i]) margin[i] = style->Margin[i];
	int script_w = 0, script_h = 0;
	file.GetResolution(script_w, script_h);
	int horizontal = (alignment - 1) % 3;
	int vertical = (alignment - 1) / 3;
	double x = horizontal == 0 ? margin[0] : horizontal == 1 ?
		(script_w + margin[0] - margin[1]) / 2.0 : script_w - margin[1];
	double y = vertical == 0 ? script_h - margin[2] : vertical == 1 ?
		script_h / 2.0 : margin[2];
	return InjectLineTags(clean, "\\pos(" + Number(x) + ',' + Number(y) + ')');
}

std::string CanonicalTagName(std::string const& name) {
	if (name == "\\c") return "\\1c";
	if (name == "\\fr") return "\\frz";
	if (name == "\\a") return "\\an";
	return name;
}

bool NearlyEqual(double left, double right) {
	return std::abs(left - right) < 1e-9;
}

bool IsStyleDefaultTag(AssOverrideTag const& tag, AssStyle const& style,
		bool has_general_alpha) {
	if (tag.Params.empty() || tag.Params.front().omitted) return false;
	if (tag.Name == "\\an")
		return tag.Params.front().Get<int>() == style.alignment;
	if (tag.Name == "\\a")
		return AssStyle::SsaToAss(tag.Params.front().Get<int>()) == style.alignment;
	if (tag.Name == "\\fscx")
		return NearlyEqual(tag.Params.front().Get<double>(), style.scalex);
	if (tag.Name == "\\fscy")
		return NearlyEqual(tag.Params.front().Get<double>(), style.scaley);
	if (tag.Name == "\\frz" || tag.Name == "\\fr")
		return NearlyEqual(tag.Params.front().Get<double>(), style.angle);
	if (tag.Name == "\\bord")
		return NearlyEqual(tag.Params.front().Get<double>(), style.outline_w);
	if (tag.Name == "\\shad")
		return NearlyEqual(tag.Params.front().Get<double>(), style.shadow_w);
	if (tag.Name == "\\1a")
		return !has_general_alpha && tag.Params.front().Get<int>() == style.primary.a;
	return false;
}

/// Generated drawings inherit quite a few neutral tags from text_to_shape. Keep the tags
/// which actually neutralize a non-default style, but do not repeat values already supplied
/// by the style. The last duplicate wins in ASS, so earlier copies can be discarded safely.
std::string NormalizeGeneratedText(AssFile& file, AssDialogue const& source,
		std::string text, bool shape) {
	if (shape && text.ends_with("{\\p0}")) text.erase(text.size() - 5);

	AssDialogue normalized(source);
	normalized.Text = std::move(text);
	auto blocks = normalized.ParseTags();
	AssDialogueBlockOverride *first = nullptr;
	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		first = static_cast<AssDialogueBlockOverride *>(block.get());
		break;
	}
	if (!first) return normalized.Text.get();

	AssStyle fallback;
	AssStyle const *style = file.GetStyle(source.Style.get());
	if (!style) style = &fallback;

	// A reset clears every tag before it. Retaining only the final reset and what follows
	// also guarantees that there cannot be multiple reset tags in the opening block.
	auto last_reset = first->Tags.end();
	for (auto it = first->Tags.begin(); it != first->Tags.end(); ++it)
		if (it->Name == "\\r") last_reset = it;
	if (last_reset != first->Tags.end()) {
		if (!last_reset->Params.empty() && !last_reset->Params.front().omitted) {
			auto name = last_reset->Params.front().Get<std::string>();
			if (!name.empty())
				if (auto reset_style = file.GetStyle(name)) style = reset_style;
		}
		first->Tags.erase(first->Tags.begin(), last_reset);
	}

	std::set<std::string> seen;
	bool has_general_alpha = std::any_of(first->Tags.begin(), first->Tags.end(),
		[](AssOverrideTag const& tag) { return tag.Name == "\\alpha"; });
	for (auto it = first->Tags.rbegin(); it != first->Tags.rend();) {
		bool duplicate = false;
		if (it->Name != "\\t")
			duplicate = !seen.insert(CanonicalTagName(it->Name)).second;
		if (duplicate || IsStyleDefaultTag(*it, *style, has_general_alpha))
			it = decltype(it)(first->Tags.erase(std::next(it).base()));
		else
			++it;
	}

	normalized.UpdateText(blocks);
	return normalized.Text.get();
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

struct Point {
	double x = 0;
	double y = 0;
};

double Dot(Point point, Point direction) {
	return point.x * direction.x + point.y * direction.y;
}

std::vector<Point> CutHalfPlane(std::vector<Point> shape, Point direction,
		double limit, bool keep_above) {
	if (shape.empty()) return {};
	auto inside = [&](Point point) {
		double value = Dot(point, direction);
		return keep_above ? value >= limit : value <= limit;
	};
	std::vector<Point> kept;
	for (size_t i = 0; i < shape.size(); ++i) {
		Point current = shape[i];
		Point next = shape[(i + 1) % shape.size()];
		bool current_in = inside(current), next_in = inside(next);
		if (current_in) kept.push_back(current);
		if (current_in != next_in) {
			double from = Dot(current, direction), to = Dot(next, direction);
			double along = std::abs(to - from) < 1e-9 ? 0 : (limit - from) / (to - from);
			kept.push_back({current.x + (next.x - current.x) * along,
				current.y + (next.y - current.y) * along});
		}
	}
	return kept;
}

std::string PolygonClip(std::vector<Point> const& points) {
	if (points.size() < 3) return {};
	std::string out = "m " + Number(points[0].x) + ' ' + Number(points[0].y) + " l";
	for (size_t i = 1; i < points.size(); ++i)
		out += ' ' + Number(points[i].x) + ' ' + Number(points[i].y);
	return out;
}

int SnapRectClipEdge(double value) {
	// Both bands sharing an edge must choose the same integer. floor/ceil on opposite
	// sides creates a one-pixel overlap even though a four-number clip needs no seam fix.
	return static_cast<int>(std::floor(value + .5));
}

std::string LinearClip(Point direction, double low, double high, bool first, bool last,
		int script_w, int script_h, double requested_overlap) {
	double overlap = VectorSeamOverlap(requested_overlap);
	double margin = std::max(script_w, script_h) * 0.5 + 64.0;
	if (first) low -= margin;
	if (last) high += margin;

	// Square-on clips stay rectangular. They snap to pixels and need no soft-edge overlap.
	if (std::abs(direction.y) < 1e-7) {
		double x1 = low / direction.x, x2 = high / direction.x;
		if (x1 > x2) std::swap(x1, x2);
		return agi::format("%d,%d,%d,%d", SnapRectClipEdge(x1),
			static_cast<int>(-margin), SnapRectClipEdge(x2),
			static_cast<int>(std::ceil(script_h + margin)));
	}
	if (std::abs(direction.x) < 1e-7) {
		double y1 = low / direction.y, y2 = high / direction.y;
		if (y1 > y2) std::swap(y1, y2);
		return agi::format("%d,%d,%d,%d", static_cast<int>(-margin),
			SnapRectClipEdge(y1), static_cast<int>(std::ceil(script_w + margin)),
			SnapRectClipEdge(y2));
	}

	std::vector<Point> shape = {{-margin, -margin}, {script_w + margin, -margin},
		{script_w + margin, script_h + margin}, {-margin, script_h + margin}};

	if (overlap > 0.0) {
		low -= overlap;
		high += overlap;
	}
	shape = CutHalfPlane(std::move(shape), direction, low, true);
	shape = CutHalfPlane(std::move(shape), direction, high, false);
	return PolygonClip(shape);
}

void AppendCircle(std::string& out, Point centre, double radius, bool reverse) {
	if (radius <= 0) return;
	constexpr double kappa = 0.5522847498307936;
	double k = radius * kappa;
	auto point = [&](double x, double y) {
		out += ' ' + Number(centre.x + x) + ' ' + Number(centre.y + y);
	};
	out += " m";
	point(radius, 0);
	out += " b";
	if (!reverse) {
		point(radius, k); point(k, radius); point(0, radius);
		point(-k, radius); point(-radius, k); point(-radius, 0);
		point(-radius, -k); point(-k, -radius); point(0, -radius);
		point(k, -radius); point(radius, -k); point(radius, 0);
	}
	else {
		point(radius, -k); point(k, -radius); point(0, -radius);
		point(-k, -radius); point(-radius, -k); point(-radius, 0);
		point(-radius, k); point(-k, radius); point(0, radius);
		point(k, radius); point(radius, k); point(radius, 0);
	}
}

std::string RadialClip(Point centre, double low, double high, bool first, bool last,
		int script_w, int script_h, double requested_overlap) {
	double margin = std::max(script_w, script_h) * 0.5 + 64.0;
	double overlap = VectorSeamOverlap(requested_overlap);
	double inner = first ? 0 : std::max(0.0, low - overlap);
	double outer = high + overlap + (last ? margin : 0.0);

	std::string out;
	// Opposite winding makes the inner contour a hole, so transparent colours are not
	// accumulated by a pile of overlapping discs. Only the configured seams overlap.
	AppendCircle(out, centre, outer, false);
	AppendCircle(out, centre, inner, true);
	if (!out.empty() && out.front() == ' ') out.erase(out.begin());
	return out;
}

struct Band {
	double low = 0;
	double high = 0;
	double factor = 0;
};

struct PaintedClip {
	std::string paint;
	std::string clip;
};

struct Geometry {
	std::vector<AssDialogue *> selected;
	int script_w = 1;
	int script_h = 1;
	Point centre;
	std::vector<Point> corners;
	typesetting::ShapeEditor editor;
	bool shapes_built = false;

	explicit Geometry(agi::Context *c, std::vector<AssDialogue *> selected = {})
	: selected(selected.empty() ? c->selectionController->GetSortedSelection() : std::move(selected))
	, editor(c, this->selected) {
		c->ass->GetResolution(script_w, script_h);
		script_w = std::max(script_w, 1);
		script_h = std::max(script_h, 1);
		centre = {script_w / 2.0, script_h / 2.0};
		corners = {{0, 0}, {static_cast<double>(script_w), 0},
			{static_cast<double>(script_w), static_cast<double>(script_h)},
			{0, static_cast<double>(script_h)}};

		if (!editor.ok()) return;
		// ShapeEditor's ordinary box is padded to keep visual-tool handles apart on
		// short text. That padding is interaction chrome, not subtitle geometry: using
		// it here stretches the gradient far above and below the actual glyphs.
		auto box = editor.ContentBox();
		centre = {box.centre.X(), box.centre.Y()};
		Vector2D found[4];
		box.Corners(found);
		corners.clear();
		for (auto point : found) corners.push_back({point.X(), point.Y()});

	}

	void EnsureShapes() {
		if (shapes_built || !editor.ok()) return;
		editor.Build([](Vector2D point) { return point; }, false, false, true);
		shapes_built = true;
	}

	GeometrySnapshot Snapshot() const {
		GeometrySnapshot out;
		out.valid = true;
		out.script_w = script_w;
		out.script_h = script_h;
		out.centre_x = centre.x;
		out.centre_y = centre.y;
		for (size_t i = 0; i < 4 && i < corners.size(); ++i) {
			out.corners[i * 2] = corners[i].x;
			out.corners[i * 2 + 1] = corners[i].y;
		}
		return out;
	}

	void ApplySnapshot(GeometrySnapshot const& snapshot) {
		if (!snapshot.valid) return;
		script_w = std::max(1, snapshot.script_w);
		script_h = std::max(1, snapshot.script_h);
		centre = {snapshot.centre_x, snapshot.centre_y};
		corners.clear();
		for (size_t i = 0; i < 4; ++i)
			corners.push_back({snapshot.corners[i * 2], snapshot.corners[i * 2 + 1]});
	}
};

std::vector<Band> MakeBands(Settings const& settings, double low, double high) {
	double span = std::max(1.0, high - low);
	size_t count = static_cast<size_t>(std::ceil(span / std::max(1, settings.pixels_per_strip)));
	count = std::clamp<size_t>(count, 1, 4096);
	std::vector<Band> bands;
	for (size_t i = 0; i < count; ++i) {
		double factor = count == 1 ? .5 : static_cast<double>(i) / (count - 1);
		double band_low = low + i * std::max(1, settings.pixels_per_strip);
		double band_high = std::min(high,
			band_low + std::max(1, settings.pixels_per_strip));
		bands.push_back({band_low, band_high, factor});
	}
	return bands;
}

struct GradientRange {
	Point direction;
	double low = 0;
	double high = 1;
};

GradientRange FindRange(Settings const& settings, Geometry const& geometry) {
	double radians = settings.angle * 3.14159265358979323846 / 180.0;
	GradientRange range{{std::cos(radians), std::sin(radians)},
		std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()};
	if (settings.kind == Kind::Linear) {
		for (auto corner : geometry.corners) {
			double value = Dot(corner, range.direction);
			range.low = std::min(range.low, value);
			range.high = std::max(range.high, value);
		}
	}
	else {
		range.low = 0;
		range.high = 1;
		for (auto corner : geometry.corners)
			range.high = std::max(range.high, std::hypot(corner.x - geometry.centre.x,
				corner.y - geometry.centre.y));
	}
	return range;
}

std::vector<PaintedClip> MakePaintedClips(agi::Context *c, AssDialogue const& source,
		Settings const& settings, Geometry const& geometry, ColourChannel channel) {
	if (!ChannelEnabled(settings, channel)) return {};
	auto range = FindRange(settings, geometry);
	bool rectangular = settings.kind == Kind::Linear &&
		(std::abs(range.direction.x) < 1e-7 || std::abs(range.direction.y) < 1e-7);
	if (rectangular) {
		range.low = std::floor(range.low);
		range.high = std::ceil(range.high);
	}
	auto raw_bands = MakeBands(settings, range.low, range.high);
	PreparedPaint prepared(settings);
	auto const& channel_paint = PaintChannel(prepared, channel);
	int channel_index = AssChannelIndex(channel);
	bool primary_with_outline = channel == ColourChannel::Primary &&
		MergePrimaryAndOutline(settings);
	struct PaintedBand { double low, high, factor; std::string paint; };
	std::vector<PaintedBand> bands;
	for (auto const& raw : raw_bands) {
		auto paint = AnimatedChannelPaint(c, source, settings, channel, channel_paint,
			channel_index, raw.factor);
		if (primary_with_outline)
			paint += AnimatedChannelPaint(c, source, settings, ColourChannel::Outline,
				PaintChannel(prepared, ColourChannel::Outline), 3, raw.factor);
		// With 8-bit ASS colours, narrow neighbouring strips are often exactly the same.
		// Joining them loses nothing and can remove hundreds of redundant dialogue lines.
		if (!rectangular && !bands.empty() && bands.back().paint == paint)
			bands.back().high = raw.high;
		else
			bands.push_back({raw.low, raw.high, raw.factor, std::move(paint)});
	}

	std::vector<PaintedClip> painted_clips;
	painted_clips.reserve(bands.size());
	for (size_t i = 0; i < bands.size(); ++i) {
		bool first = i == 0, last = i + 1 == bands.size();
		std::string clip = settings.kind == Kind::Linear ?
			LinearClip(range.direction, bands[i].low, bands[i].high, first, last,
				geometry.script_w, geometry.script_h, settings.anti_strip_overlap) :
			RadialClip(geometry.centre, bands[i].low, bands[i].high, first, last,
				geometry.script_w, geometry.script_h, settings.anti_strip_overlap);
		if (!clip.empty()) painted_clips.push_back({bands[i].paint, std::move(clip)});
	}
	return painted_clips;
}

namespace bp = boost::polygon;
using ShapeCoordinate = std::int64_t;
using ShapePoint = bp::point_data<ShapeCoordinate>;
using ShapePolygon = bp::polygon_data<ShapeCoordinate>;
using ShapePolygonWithHoles = bp::polygon_with_holes_data<ShapeCoordinate>;
using ShapeSet = bp::polygon_set_data<ShapeCoordinate>;

// Integer polygon sets use a sweep-line implementation which tolerates touching,
// overlapping and mildly malformed font outlines far better than floating-point
// areal polygons. This scale retains the three decimal places emitted to ASS.
constexpr double shape_coordinate_scale = 1024.0;

ShapeCoordinate ShapeCoord(double value) {
	return static_cast<ShapeCoordinate>(std::llround(value * shape_coordinate_scale));
}

float ScriptCoord(ShapeCoordinate value) {
	return static_cast<float>(value / shape_coordinate_scale);
}

double ContourArea(std::vector<Vector2D> const& contour) {
	if (contour.size() < 3) return 0;
	double area = 0;
	for (size_t i = 0, previous = contour.size() - 1; i < contour.size(); previous = i++)
		area += static_cast<double>(contour[previous].X()) * contour[i].Y() -
			static_cast<double>(contour[i].X()) * contour[previous].Y();
	return area * .5;
}

bool ContourContains(std::vector<Vector2D> const& contour, Vector2D point) {
	bool inside = false;
	for (size_t i = 0, previous = contour.size() - 1; i < contour.size(); previous = i++) {
		auto a = contour[previous], b = contour[i];
		bool crosses = (a.Y() > point.Y()) != (b.Y() > point.Y());
		if (crosses && point.X() < (b.X() - a.X()) * (point.Y() - a.Y()) /
			(b.Y() - a.Y()) + a.X()) inside = !inside;
	}
	return inside;
}

struct ContourBounds {
	double left = 0;
	double top = 0;
	double right = 0;
	double bottom = 0;
};

ContourBounds BoundsOf(std::vector<Vector2D> const& contour) {
	ContourBounds bounds{contour.front().X(), contour.front().Y(),
		contour.front().X(), contour.front().Y()};
	for (auto point : contour) {
		bounds.left = std::min(bounds.left, static_cast<double>(point.X()));
		bounds.top = std::min(bounds.top, static_cast<double>(point.Y()));
		bounds.right = std::max(bounds.right, static_cast<double>(point.X()));
		bounds.bottom = std::max(bounds.bottom, static_cast<double>(point.Y()));
	}
	return bounds;
}

bool BoundsContain(ContourBounds const& outer, ContourBounds const& inner) {
	constexpr double tolerance = 1e-4;
	return outer.left <= inner.left + tolerance && outer.top <= inner.top + tolerance &&
		outer.right >= inner.right - tolerance && outer.bottom >= inner.bottom - tolerance;
}

/// A point alone is not enough to decide that one outline is a hole in another.
/// Connected handwritten glyphs routinely overlap, so the first point of the next
/// letter can sit inside the previous one even though the rest of its contour does not.
bool ContourEncloses(std::vector<Vector2D> const& outer,
		std::vector<Vector2D> const& inner) {
	for (auto point : inner)
		if (!ContourContains(outer, point)) return false;
	return true;
}

ShapePolygon PolygonOf(std::vector<Vector2D> const& contour) {
	std::vector<ShapePoint> points;
	points.reserve(contour.size());
	for (auto point : contour)
		points.emplace_back(ShapeCoord(point.X()), ShapeCoord(point.Y()));
	ShapePolygon polygon;
	bp::set_points(polygon, points.begin(), points.end());
	return polygon;
}

/// Turn ASS contours into polygons while keeping each hole with the smallest outer ring
/// around it. Containment rather than winding determines the hierarchy: real-world fonts
/// are not uniformly wound, while nesting still unambiguously distinguishes a counter
/// from a neighbouring or overlapping letter.
ShapeSet ContourPolygons(
		std::vector<std::vector<Vector2D>> const& contours) {
	std::vector<double> areas(contours.size());
	std::vector<ContourBounds> bounds(contours.size());
	for (size_t i = 0; i < contours.size(); ++i) {
		if (contours[i].size() < 3) continue;
		areas[i] = std::abs(ContourArea(contours[i]));
		bounds[i] = BoundsOf(contours[i]);
	}

	std::vector<int> parent(contours.size(), -1);
	for (size_t i = 0; i < contours.size(); ++i) {
		if (areas[i] < 1e-8) continue;
		double smallest = std::numeric_limits<double>::max();
		for (size_t candidate = 0; candidate < contours.size(); ++candidate) {
			if (candidate == i || areas[candidate] <= areas[i] ||
				areas[candidate] >= smallest ||
				!BoundsContain(bounds[candidate], bounds[i])) continue;
			if (ContourEncloses(contours[candidate], contours[i])) {
				smallest = areas[candidate];
				parent[i] = static_cast<int>(candidate);
			}
		}
	}

	std::vector<int> depth(contours.size(), 0);
	for (size_t i = 0; i < contours.size(); ++i) {
		int at = parent[i];
		while (at >= 0 && depth[i] <= static_cast<int>(contours.size())) {
			++depth[i];
			at = parent[at];
		}
	}

	ShapeSet shapes;
	for (size_t i = 0; i < contours.size(); ++i) {
		if (areas[i] < 1e-8) continue;
		shapes.insert(PolygonOf(contours[i]), depth[i] % 2 != 0);
	}
	return shapes;
}

template<typename Polygon>
void AppendShapeRing(std::vector<std::vector<Vector2D>>& contours,
		Polygon const& polygon) {
	std::vector<Vector2D> contour;
	for (auto point = bp::begin_points(polygon); point != bp::end_points(polygon); ++point)
		contour.emplace_back(ScriptCoord(bp::x(*point)), ScriptCoord(bp::y(*point)));
	if (contour.size() >= 3) contours.push_back(std::move(contour));
}

std::vector<std::vector<Vector2D>> IntersectShapes(
		ShapeSet const& shapes, ShapeSet const& boundary) {
	using namespace boost::polygon::operators;
	ShapeSet clipped = shapes & boundary;
	std::vector<ShapePolygonWithHoles> pieces;
	clipped.get(pieces);
	std::vector<std::vector<Vector2D>> contours;
	for (auto const& piece : pieces) {
		AppendShapeRing(contours, piece);
		for (auto hole = bp::begin_holes(piece); hole != bp::end_holes(piece); ++hole)
			AppendShapeRing(contours, *hole);
	}
	return contours;
}

ShapePolygon PolygonOf(std::vector<Point> const& points) {
	std::vector<ShapePoint> converted;
	converted.reserve(points.size());
	for (auto point : points)
		converted.emplace_back(ShapeCoord(point.x), ShapeCoord(point.y));
	ShapePolygon polygon;
	bp::set_points(polygon, converted.begin(), converted.end());
	return polygon;
}

ShapeSet PolygonBoundary(std::vector<Point> const& points) {
	ShapeSet boundary;
	if (points.size() >= 3) boundary.insert(PolygonOf(points));
	return boundary;
}

ShapeSet LinearBoundary(Point direction, double low, double high, bool first, bool last,
		int script_w, int script_h, double requested_overlap) {
	double overlap = VectorSeamOverlap(requested_overlap);
	double margin = std::max(script_w, script_h) * .5 + 64.0;
	if (first) low -= margin;
	if (last) high += margin;
	// These intersections are emitted as drawing fragments, so every orientation has
	// antialiased edges. The rectangular-clip exception does not apply to shape output.
	if (overlap > 0.0) {
		low -= overlap;
		high += overlap;
	}
	std::vector<Point> shape = {{-margin, -margin}, {script_w + margin, -margin},
		{script_w + margin, script_h + margin}, {-margin, script_h + margin}};
	shape = CutHalfPlane(std::move(shape), direction, low, true);
	shape = CutHalfPlane(std::move(shape), direction, high, false);
	return PolygonBoundary(shape);
}

std::vector<Point> CircleRing(Point centre, double radius, int segments) {
	std::vector<Point> ring;
	ring.reserve(segments);
	for (int i = 0; i < segments; ++i) {
		double radians = i * 2.0 * 3.14159265358979323846 / segments;
		ring.push_back({centre.x + std::cos(radians) * radius,
			centre.y + std::sin(radians) * radius});
	}
	return ring;
}

ShapeSet RadialBoundary(Point centre, double low, double high, bool first, bool last,
		int script_w, int script_h, double requested_overlap) {
	double margin = std::max(script_w, script_h) * .5 + 64.0;
	double overlap = VectorSeamOverlap(requested_overlap);
	double inner = first ? 0 : std::max(0.0, low - overlap);
	double outer = high + overlap + (last ? margin : 0.0);
	int segments = std::clamp(static_cast<int>(std::ceil(
		3.14159265358979323846 * std::sqrt(std::max(outer, 1.0) / .5))), 32, 256);
	ShapeSet boundary;
	boundary.insert(PolygonOf(CircleRing(centre, outer, segments)));
	if (inner > 1e-6)
		boundary.insert(PolygonOf(CircleRing(centre, inner, segments)), true);
	return boundary;
}

struct ShapeSliceTemplate {
	double factor = 0;
	std::string text;
};

struct ShapeLayerTemplate {
	AssDialogue *source = nullptr;
	typesetting::ShapeEditor::LayerKind kind = typesetting::ShapeEditor::LayerKind::Primary;
	std::string whole_text;
	std::vector<ShapeSliceTemplate> slices;
	bool covered = false;
};

struct PreparedShapes {
	Kind kind = Kind::Linear;
	int angle = 0;
	int pixels_per_strip = 3;
	double anti_strip_overlap = 0.4;
	std::vector<ShapeLayerTemplate> layers;
};

PreparedShapes PrepareShapes(Settings const& settings, Geometry& geometry) {
	PreparedShapes out{settings.kind, settings.angle, settings.pixels_per_strip,
		settings.anti_strip_overlap};
	geometry.EnsureShapes();
	if (!geometry.shapes_built) return out;
	auto range = FindRange(settings, geometry);
	auto bands = MakeBands(settings, range.low, range.high);
	std::vector<ShapeSet> boundaries;
	boundaries.reserve(bands.size());
	for (size_t i = 0; i < bands.size(); ++i) {
		bool first = i == 0, last = i + 1 == bands.size();
		boundaries.push_back(settings.kind == Kind::Linear ?
			LinearBoundary(range.direction, bands[i].low, bands[i].high, first, last,
				geometry.script_w, geometry.script_h, settings.anti_strip_overlap) :
			RadialBoundary(geometry.centre, bands[i].low, bands[i].high, first, last,
				geometry.script_w, geometry.script_h, settings.anti_strip_overlap));
	}

	for (auto const& layer : geometry.editor.layers()) {
		ShapeLayerTemplate prepared{layer.source, layer.kind, layer.text, {}, layer.covered};
		auto polygons = ContourPolygons(layer.contours);
		for (size_t i = 0; i < bands.size() && i < boundaries.size(); ++i) {
			if (polygons.empty() || boundaries[i].empty()) continue;
			auto clipped = IntersectShapes(polygons, boundaries[i]);
			auto text = geometry.editor.TextForContours(layer, clipped);
			if (!text.empty()) prepared.slices.push_back({bands[i].factor, std::move(text)});
		}
		out.layers.push_back(std::move(prepared));
	}
	return out;
}

struct PaintedShape {
	AssDialogue *source = nullptr;
	std::string text;
};

std::vector<PaintedShape> PaintShapes(agi::Context *c, Settings const& settings,
		PreparedShapes const& prepared) {
	PreparedPaint paint(settings);
	std::set<AssDialogue *> eligible;
	for (auto const& layer : prepared.layers) {
		auto const& channel = ShapeChannel(paint, layer.kind);
		if (channel.enabled && !layer.slices.empty()) eligible.insert(layer.source);
	}

	std::vector<PaintedShape> output;
	for (auto const& layer : prepared.layers) {
		if (!eligible.count(layer.source)) continue;
		auto const& channel = ShapeChannel(paint, layer.kind);
		if (channel.enabled) {
			for (auto const& slice : layer.slices)
				output.push_back({layer.source, InjectLineTags(slice.text,
					AnimatedChannelPaint(c, *layer.source, settings,
						layer.kind == typesetting::ShapeEditor::LayerKind::Outline ?
							ColourChannel::Outline :
						layer.kind == typesetting::ShapeEditor::LayerKind::Shadow ?
							ColourChannel::Shadow : ColourChannel::Primary,
						channel, 1, slice.factor))});
		}
		else if (!(layer.kind == typesetting::ShapeEditor::LayerKind::Primary && layer.covered))
			output.push_back({layer.source, layer.whole_text});
	}
	return output;
}

std::vector<std::string> PlainUnits(std::string const& text) {
	std::vector<std::string> units;
	agi::BreakIterator iterator;
	iterator.set_text(text);
	for (; !iterator.done(); iterator.next())
		units.emplace_back(iterator.current());

	std::vector<std::string> joined;
	for (size_t i = 0; i < units.size(); ++i) {
		if (units[i] == "\\" && i + 1 < units.size() &&
			(units[i + 1] == "N" || units[i + 1] == "n" || units[i + 1] == "h")) {
			joined.push_back(units[i] + units[i + 1]);
			++i;
		}
		else joined.push_back(std::move(units[i]));
	}
	return joined;
}

bool IsWhitespaceUnit(std::string const& unit) {
	if (unit == "\\N" || unit == "\\n" || unit == "\\h") return true;
	UChar32 codepoint = 0;
	int32_t at = 0;
	U8_NEXT(unit.data(), at, static_cast<int32_t>(unit.size()), codepoint);
	return codepoint >= 0 && u_isUWhiteSpace(codepoint);
}

bool CharacterGradient(agi::Context *c, AssDialogue& line, Settings const& settings) {
	AssDialogue clean(line);
	clean.Text = CleanText(line, settings, false);
	auto blocks = clean.ParseTags();
	PreparedPaint prepared(settings);

	size_t total = 0;
	for (auto const& block : blocks)
		if (block->GetType() == AssBlockType::PLAIN)
			total += PlainUnits(block->GetText()).size();
	if (!total) return false;

	std::string out;
	size_t index = 0;
	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::PLAIN) {
			out += block->GetText();
			continue;
		}
		for (auto const& unit : PlainUnits(block->GetText())) {
			double factor = total == 1 ? .5 : static_cast<double>(index) / (total - 1);
			if (!IsWhitespaceUnit(unit))
				out += "{" + AnimatedPaintTags(c, line, settings, prepared, factor) + "}";
			out += unit;
			++index;
		}
	}
	line.Text = std::move(out);
	return true;
}

std::vector<std::vector<std::string>> GenerateOutputs(agi::Context *c,
		std::vector<AssDialogue *> const& sources, Settings const& settings,
		Geometry *given_geometry = nullptr) {
	std::unique_ptr<Geometry> owned_geometry;
	if (!given_geometry) {
		owned_geometry = std::make_unique<Geometry>(c, sources);
		given_geometry = owned_geometry.get();
	}
	auto& geometry = *given_geometry;
	std::vector<std::vector<std::string>> output(sources.size());
	if (settings.output == Output::Characters) {
		for (size_t i = 0; i < sources.size(); ++i) {
			AssDialogue generated(*sources[i]);
			if (CharacterGradient(c, generated, settings))
				output[i].push_back(NormalizeGeneratedText(*c->ass, *sources[i],
					generated.Text.get(), false));
		}
	}
	else if (settings.output == Output::Shapes) {
		auto painted = PaintShapes(c, settings, PrepareShapes(settings, geometry));
		for (auto& shape : painted) {
			auto found = std::find(sources.begin(), sources.end(), shape.source);
			if (found != sources.end())
				output[std::distance(sources.begin(), found)].push_back(
					NormalizeGeneratedText(*c->ass, *shape.source, std::move(shape.text), true));
		}
	}
	else {
		for (size_t i = 0; i < sources.size(); ++i) {
			bool primary_with_outline = MergePrimaryAndOutline(settings);
			std::array<std::vector<PaintedClip>, 3> painted;
			for (auto channel : paint_order)
				if (ChannelEnabled(settings, channel) &&
					!(primary_with_outline && channel == ColourChannel::Outline))
					painted[ChannelIndex(channel)] =
						MakePaintedClips(c, *sources[i], settings, geometry, channel);
			auto appearance = EffectiveLineColours(c, sources[i]);
			for (auto channel : paint_order) {
				if (primary_with_outline && channel == ColourChannel::Outline) continue;
				bool merged = primary_with_outline && channel == ColourChannel::Primary;
				std::string isolation = IsolationTags(channel, merged);
				if (ChannelEnabled(settings, channel)) {
					auto channel_settings = merged ? settings : ChannelSettings(settings, channel);
					if (merged) channel_settings.shadow.enabled = false;
					auto clean = PositionedClipText(*c->ass, *sources[i], channel_settings);
					for (auto const& band : painted[ChannelIndex(channel)])
						output[i].push_back(NormalizeGeneratedText(*c->ass, *sources[i],
							InjectLineTags(clean, band.paint + isolation +
								"\\clip(" + band.clip + ")", true), false));
				}
				else if (ChannelVisible(appearance, channel)) {
					auto no_channels = ChannelSettings(settings, std::nullopt);
					auto clean = PositionedClipText(*c->ass, *sources[i], no_channels);
					output[i].push_back(NormalizeGeneratedText(*c->ass, *sources[i],
						InjectLineTags(clean, isolation, true), false));
				}
			}
		}
	}
	return output;
}

} // namespace

agi::Color Sample(std::vector<Stop> const& given, double position) {
	return SampleSorted(SortedStops(given), position);
}

std::string ClipboardMetadata(AssFile const& file, AssDialogue const& line) {
	if (!IsGradientSource(file, &line)) return {};
	auto settings = GradientData(file, line);
	auto source = GradientSourceData(file, line);
	if (!settings || !source) return {};
	return ClipboardMarker(gradient_clipboard_settings, *settings) +
		ClipboardMarker(gradient_clipboard_source, *source);
}

bool RestoreClipboardMetadata(AssFile& file, AssDialogue& line) {
	std::string text = line.Text.get();
	auto settings = TakeClipboardMarker(text, gradient_clipboard_settings);
	auto source = TakeClipboardMarker(text, gradient_clipboard_source);
	line.Text = std::move(text);
	if (!settings || !source || line.Effect.get() != gradient_effect) return false;

	try {
		AssDialogue source_line(*source);
		*source = source_line.GetEntryData(false);
	}
	catch (...) {
		return false;
	}
	file.SetExtradataValue(line, gradient_data_key, *settings);
	file.SetExtradataValue(line, gradient_source_key, *source);
	auto ids = line.ExtradataIds.get();
	std::sort(ids.begin(), ids.end());
	line.ExtradataIds = std::move(ids);
	return true;
}

bool TransferGroupMetadata(AssFile& file, AssDialogue const& from, AssDialogue& to) {
	auto settings = GradientData(file, from);
	auto source = GradientSourceData(file, from);
	if (!settings || !source) return false;

	file.SetExtradataValue(to, gradient_data_key, *settings);
	file.SetExtradataValue(to, gradient_source_key, *source);
	auto ids = to.ExtradataIds.get();
	std::sort(ids.begin(), ids.end());
	to.ExtradataIds = std::move(ids);
	return true;
}

std::string GroupSourceEntry(AssFile const& file, AssDialogue const& line) {
	auto source = GradientSourceData(file, line);
	return source ? *source : std::string();
}

std::string GroupDescription(AssFile const& file, AssDialogue const& line) {
	auto encoded = GradientData(file, line);
	if (!encoded) return {};
	auto parsed = DeserializeSettings(*encoded, Settings{});
	if (!parsed) return {};

	std::vector<std::string> tags;
	auto add = [&](Channel const& channel, char const *colour_tag, char const *alpha_tag) {
		if (!channel.enabled || channel.stops.empty()) return;
		auto const& first = channel.stops.front().colour;
		bool colour_varies = false;
		bool alpha_varies = false;
		for (auto const& stop : channel.stops) {
			colour_varies |= stop.colour.r != first.r || stop.colour.g != first.g ||
				stop.colour.b != first.b;
			alpha_varies |= stop.colour.a != first.a;
		}
		// An enabled constant channel is still deliberately painted by the Gradient tool,
		// so identify its colour tag even though only one value is present.
		if (colour_varies || !alpha_varies) tags.emplace_back(colour_tag);
		if (alpha_varies) tags.emplace_back(alpha_tag);
	};
	add(parsed->primary, "\\c", "\\1a");
	add(parsed->outline, "\\3c", "\\3a");
	add(parsed->shadow, "\\4c", "\\4a");
	if (parsed->motion.enabled) tags.emplace_back(from_wx(_("Animation")));

	std::string out;
	for (auto const& tag : tags) {
		if (!out.empty()) out += ' ';
		out += tag;
	}
	return out;
}

Settings LoadSettingsForSelection(agi::Context *c) {
	auto groups = CollectSourceGroups(c);
	auto fallback = DefaultSettings(c, groups.empty() ? nullptr : groups.front().source);
	for (auto const& group : groups) {
		if (!group.editing) continue;
		auto encoded = GradientData(*c->ass, *group.anchor);
		if (encoded) {
			auto saved = DeserializeSettings(*encoded, fallback);
			if (saved) return *saved;
		}
	}
	return fallback;
}

bool SettingsFromClipboard(std::string clipboard, Settings& settings) {
	try {
		auto encoded = TakeClipboardMarker(clipboard, gradient_clipboard_settings);
		auto source = TakeClipboardMarker(clipboard, gradient_clipboard_source);
		if (!encoded || !source) return false;
		AssDialogue source_line(*source);
		(void)source_line;
		auto parsed = DeserializeSettings(*encoded, Settings{});
		if (!parsed) return false;
		// The copied geometry belongs to the source row. Preview and apply the settings using
		// the geometry of the rows currently selected in the gradient dialog.
		parsed->geometry.valid = false;
		settings = std::move(*parsed);
		return true;
	}
	catch (...) {
		return false;
	}
}

struct PreviewSession::Impl {
	agi::Context *context;
	std::vector<SourceGroup> groups;
	Geometry geometry;
	std::array<std::vector<std::string>, 8> clean_text;
	std::array<bool, 8> clean_text_ready{};
	std::optional<PreparedShapes> prepared_shapes;

	explicit Impl(agi::Context *context)
	: context(context)
	, groups(CollectSourceGroups(context))
	, geometry(context, GroupSources(groups)) {
	}

	int ChannelMask(Settings const& settings) const {
		return (settings.primary.enabled ? 1 : 0) |
			(settings.outline.enabled ? 2 : 0) |
			(settings.shadow.enabled ? 4 : 0);
	}

	std::vector<std::string> const& CleanClipText(Settings const& settings) {
		int mask = ChannelMask(settings);
		if (!clean_text_ready[mask]) {
			auto& cached = clean_text[mask];
			cached.reserve(geometry.selected.size());
			for (auto source : geometry.selected)
				cached.push_back(PositionedClipText(*context->ass, *source, settings));
			clean_text_ready[mask] = true;
		}
		return clean_text[mask];
	}

	PreparedShapes const& Shapes(Settings const& settings) {
		if (!prepared_shapes || prepared_shapes->kind != settings.kind ||
			prepared_shapes->angle != settings.angle ||
			prepared_shapes->pixels_per_strip != settings.pixels_per_strip ||
			prepared_shapes->anti_strip_overlap != settings.anti_strip_overlap)
			prepared_shapes = PrepareShapes(settings, geometry);
		return *prepared_shapes;
	}

	void Clear() {
		std::vector<AssDialogue const *> originals;
		for (auto const& group : groups)
			for (auto line : group.existing) originals.push_back(line);
		if (!originals.empty()) context->videoController->PreviewSubtitles(originals);
	}

	void Update(Settings const& settings) {
		if (geometry.selected.empty() ||
			(!settings.primary.enabled && !settings.outline.enabled && !settings.shadow.enabled)) {
			Clear();
			return;
		}
		if (settings.geometry.valid && std::all_of(groups.begin(), groups.end(),
			[](SourceGroup const& group) { return group.editing; }))
			geometry.ApplySnapshot(settings.geometry);

		std::vector<AssDialogue> changed;
		std::vector<AssDialogue> added;
		for (auto const& group : groups)
			for (auto line : group.existing) {
				AssDialogue silenced(*line);
				silenced.Comment = true;
				changed.push_back(std::move(silenced));
			}
		if (settings.output == Output::Characters) {
			for (auto source : geometry.selected) {
				AssDialogue generated(*source);
				if (!CharacterGradient(context, generated, settings)) continue;
				generated.Comment = false;
				generated.Effect = gradient_effect;
				generated.Text = NormalizeGeneratedText(*context->ass, *source,
					generated.Text.get(), false);
				added.push_back(std::move(generated));
			}
			if (added.empty()) { Clear(); return; }
		}
		else if (settings.output == Output::Shapes) {
			auto painted_shapes = PaintShapes(context, settings, Shapes(settings));
			if (painted_shapes.empty()) {
				Clear();
				return;
			}
			added.reserve(painted_shapes.size());
			for (auto& shape : painted_shapes) {
				AssDialogue generated(*shape.source);
				generated.Comment = false;
				generated.Effect = gradient_effect;
				generated.Text = NormalizeGeneratedText(*context->ass, *shape.source,
					std::move(shape.text), true);
				added.push_back(std::move(generated));
			}
		}
		else {
			for (size_t source_index = 0; source_index < geometry.selected.size(); ++source_index) {
				auto source = geometry.selected[source_index];
				bool primary_with_outline = MergePrimaryAndOutline(settings);
				std::array<std::vector<PaintedClip>, 3> painted_clips;
				for (auto channel : paint_order)
					if (ChannelEnabled(settings, channel) &&
						!(primary_with_outline && channel == ColourChannel::Outline))
						painted_clips[ChannelIndex(channel)] =
							MakePaintedClips(context, *source, settings, geometry, channel);
				auto appearance = EffectiveLineColours(context, source);
				for (auto channel : paint_order) {
					if (primary_with_outline && channel == ColourChannel::Outline) continue;
					bool merged = primary_with_outline && channel == ColourChannel::Primary;
					std::string isolation = IsolationTags(channel, merged);
					if (ChannelEnabled(settings, channel)) {
						auto channel_settings = merged ? settings : ChannelSettings(settings, channel);
						if (merged) channel_settings.shadow.enabled = false;
						auto const& clean = CleanClipText(channel_settings)[source_index];
						for (auto const& band : painted_clips[ChannelIndex(channel)]) {
							AssDialogue generated(*source);
							generated.Comment = false;
							generated.Effect = gradient_effect;
							generated.Text = NormalizeGeneratedText(*context->ass, *source,
								InjectLineTags(clean, band.paint + isolation +
									"\\clip(" + band.clip + ")", true), false);
							added.push_back(std::move(generated));
						}
					}
					else if (ChannelVisible(appearance, channel)) {
						auto no_channels = ChannelSettings(settings, std::nullopt);
						auto const& clean = CleanClipText(no_channels)[source_index];
						AssDialogue generated(*source);
						generated.Comment = false;
						generated.Effect = gradient_effect;
						generated.Text = NormalizeGeneratedText(*context->ass, *source,
							InjectLineTags(clean, isolation, true), false);
						added.push_back(std::move(generated));
					}
				}
			}
			if (added.empty()) { Clear(); return; }
		}

		std::vector<AssDialogue const *> changed_pointers, added_pointers;
		changed_pointers.reserve(changed.size());
		for (auto const& line : changed) changed_pointers.push_back(&line);
		added_pointers.reserve(added.size());
		for (auto const& line : added) added_pointers.push_back(&line);
		context->videoController->PreviewSubtitles(changed_pointers, added_pointers);
	}
};

PreviewSession::PreviewSession(agi::Context *c)
: impl(std::make_unique<Impl>(c)) {
}

PreviewSession::~PreviewSession() = default;

void PreviewSession::Update(Settings const& settings) {
	impl->Update(settings);
}

void PreviewSession::Clear() {
	impl->Clear();
}

bool RegenerateMotionForTiming(agi::Context *c, int type,
		AssDialogue const *changed_line) {
	if (!(type & AssFile::COMMIT_DIAG_TIME)) return false;

	std::vector<AssDialogue *> rows;
	rows.reserve(c->ass->Events.size());
	for (auto& line : c->ass->Events) rows.push_back(&line);

	bool updated = false;
	for (size_t i = 0; i < rows.size();) {
		AssDialogue *anchor = rows[i];
		if (!IsGradientSource(*c->ass, anchor)) {
			++i;
			continue;
		}

		std::vector<AssDialogue *> existing = {anchor};
		size_t next = i + 1;
		while (next < rows.size() && IsGradientEffect(rows[next]) &&
			!IsGradientSource(*c->ass, rows[next]))
			existing.push_back(rows[next++]);
		i = next;

		auto changed = changed_line && std::find(existing.begin(), existing.end(),
			changed_line) != existing.end();
		if (changed_line && !changed) continue;
		auto encoded_settings = GradientData(*c->ass, *anchor);
		auto encoded_source = GradientSourceData(*c->ass, *anchor);
		if (!encoded_settings || !encoded_source) continue;

		try {
			AssDialogue source(*encoded_source);
			AssDialogue const *timing = changed ? changed_line : anchor;
			if (!changed && source.Start == timing->Start && source.End == timing->End)
				continue;
			auto parsed = DeserializeSettings(*encoded_settings, DefaultSettings(c, &source));
			if (!parsed) continue;
			auto const& settings = *parsed;
			bool moving = false;
			for (auto channel : {ColourChannel::Primary, ColourChannel::Outline,
					ColourChannel::Shadow})
				moving |= ChannelEnabled(settings, channel) &&
					ChannelMotion(settings, channel).enabled;
			if (!moving) continue;

			source.Start = timing->Start;
			source.End = timing->End;
			std::vector<AssDialogue *> sources = {&source};
			Geometry geometry(c, sources);
			geometry.ApplySnapshot(settings.geometry);
			auto output = GenerateOutputs(c, sources, settings, &geometry);
			if (output.empty() || output.front().size() != existing.size()) continue;

			for (size_t row = 0; row < existing.size(); ++row) {
				existing[row]->Start = source.Start;
				existing[row]->End = source.End;
				existing[row]->Text = std::move(output.front()[row]);
			}
			c->ass->SetExtradataValue(*anchor, gradient_source_key, source.GetEntryData());
			auto ids = anchor->ExtradataIds.get();
			std::sort(ids.begin(), ids.end());
			anchor->ExtradataIds = std::move(ids);
			updated = true;
		}
		catch (...) {
			// Damaged or obsolete metadata must not make an ordinary timing edit fail.
		}
	}
	if (updated) c->ass->CleanExtradata();
	return updated;
}

bool RegenerateGroupText(agi::Context *c, AssDialogue const& given_anchor,
		AssDialogue const& edited_source) {
	auto anchor = const_cast<AssDialogue *>(&given_anchor);
	if (!IsGradientSource(*c->ass, anchor)) return false;
	auto encoded = GradientData(*c->ass, *anchor);
	if (!encoded) return false;

	AssDialogue source(edited_source);
	source.Row = anchor->Row;
	auto parsed = DeserializeSettings(*encoded, DefaultSettings(c, &source));
	if (!parsed) return false;
	Settings settings = *parsed;
	// A changed string can have entirely different bounds and outlines. The saved snapshot
	// describes the old text, so measure the edited source again before repainting it.
	settings.geometry.valid = false;
	std::vector<AssDialogue *> sources = {&source};
	Geometry geometry(c, sources);
	auto output = GenerateOutputs(c, sources, settings, &geometry);
	Settings stored_settings = settings;
	stored_settings.geometry = geometry.Snapshot();

	std::vector<AssDialogue *> existing = {anchor};
	auto after = c->ass->Events.iterator_to(*anchor);
	++after;
	while (after != c->ass->Events.end() && IsGradientEffect(&*after) &&
		after->Start == anchor->Start && after->End == anchor->End &&
		!IsGradientSource(*c->ass, &*after))
		existing.push_back(&*after++);

	c->ass->DeleteExtradataValue(source, gradient_data_key);
	c->ass->DeleteExtradataValue(source, gradient_source_key);
	std::string source_entry = source.GetEntryData();
	auto insert_at = c->ass->Events.iterator_to(*anchor);
	Selection new_selection;
	AssDialogue *new_active = nullptr;

	if (!output.empty()) {
		for (size_t index = 0; index < output.front().size(); ++index) {
			auto generated = new AssDialogue(source);
			generated->Comment = false;
			generated->Effect = gradient_effect;
			generated->Text = std::move(output.front()[index]);
			if (index == 0) {
				c->ass->SetExtradataValue(*generated, gradient_data_key,
					SerializeSettings(stored_settings));
				c->ass->SetExtradataValue(*generated, gradient_source_key, source_entry);
				auto ids = generated->ExtradataIds.get();
				std::sort(ids.begin(), ids.end());
				generated->ExtradataIds = std::move(ids);
				new_active = generated;
			}
			c->ass->Events.insert(insert_at, *generated);
			new_selection.insert(generated);
		}
	}

	// An empty result becomes an ordinary empty source row rather than leaving behind
	// hundreds of stale gradient fragments which no longer describe its text.
	if (!new_active) {
		auto replacement = new AssDialogue(source);
		if (replacement->Effect.get() == gradient_effect) replacement->Effect = "";
		c->ass->Events.insert(insert_at, *replacement);
		new_selection.insert(replacement);
		new_active = replacement;
	}

	std::vector<std::unique_ptr<AssDialogue>> removed;
	for (auto line : existing) {
		c->ass->Events.erase(c->ass->Events.iterator_to(*line));
		removed.emplace_back(line);
	}
	c->selectionController->SetSelectionAndActive(std::move(new_selection), new_active);
	c->ass->CleanExtradata();
	c->ass->Commit(_("change text"), AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return true;
}

bool Revert(agi::Context *c) {
	auto groups = CollectSourceGroups(c);
	Selection selection;
	AssDialogue *active = nullptr;
	auto old_active = c->selectionController->GetActiveLine();
	std::vector<std::unique_ptr<AssDialogue>> removed;

	for (auto& group : groups) {
		if (!group.editing || !group.stored_source || group.existing.empty()) continue;
		bool was_active = std::find(group.existing.begin(), group.existing.end(), old_active) !=
			group.existing.end();
		auto insert_at = c->ass->Events.iterator_to(*group.anchor);
		auto original = new AssDialogue(*group.stored_source);
		c->ass->DeleteExtradataValue(*original, gradient_data_key);
		c->ass->DeleteExtradataValue(*original, gradient_source_key);
		c->ass->Events.insert(insert_at, *original);
		selection.insert(original);
		if (!active || was_active) active = original;

		for (auto line : group.existing) {
			c->ass->Events.erase(c->ass->Events.iterator_to(*line));
			removed.emplace_back(line);
		}
	}

	if (selection.empty()) return false;
	c->selectionController->SetSelectionAndActive(std::move(selection), active);
	c->ass->CleanExtradata();
	c->ass->Commit(_("remove gradient effect"), AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return true;
}

size_t Apply(agi::Context *c, Settings const& settings) {
	auto groups = CollectSourceGroups(c);
	if (groups.empty()) return 0;
	auto sources = GroupSources(groups);
	Geometry geometry(c, sources);
	if (settings.geometry.valid && std::all_of(groups.begin(), groups.end(),
		[](SourceGroup const& group) { return group.editing; }))
		geometry.ApplySnapshot(settings.geometry);
	auto output = GenerateOutputs(c, sources, settings, &geometry);
	Settings stored_settings = settings;
	stored_settings.geometry = geometry.Snapshot();

	Selection new_selection;
	AssDialogue *new_active = nullptr;
	auto old_active = c->selectionController->GetActiveLine();
	std::vector<std::unique_ptr<AssDialogue>> removed;
	size_t generated_count = 0;
	for (size_t i = 0; i < groups.size(); ++i) {
		if (output[i].empty()) continue;
		auto& group = groups[i];
		bool was_active = std::find(group.existing.begin(), group.existing.end(), old_active) !=
			group.existing.end();

		AssDialogue original(*group.source);
		if (group.editing && !GradientSourceData(*c->ass, *group.anchor)) {
			original.Comment = false;
			if (original.Effect.get() == gradient_effect) original.Effect = "";
		}
		c->ass->DeleteExtradataValue(original, gradient_data_key);
		c->ass->DeleteExtradataValue(original, gradient_source_key);
		auto source_entry = original.GetEntryData();

		auto insert_at = c->ass->Events.iterator_to(*group.anchor);
		AssDialogue *first_generated = nullptr;
		for (size_t text_index = 0; text_index < output[i].size(); ++text_index) {
			auto& text = output[i][text_index];
			auto generated = new AssDialogue(*group.source);
			generated->Comment = false;
			generated->Effect = gradient_effect;
			generated->Text = std::move(text);
			c->ass->DeleteExtradataValue(*generated, gradient_data_key);
			c->ass->DeleteExtradataValue(*generated, gradient_source_key);
			if (text_index == 0) {
				c->ass->SetExtradataValue(*generated, gradient_data_key,
					SerializeSettings(stored_settings));
				c->ass->SetExtradataValue(*generated, gradient_source_key, source_entry);
				auto ids = generated->ExtradataIds.get();
				std::sort(ids.begin(), ids.end());
				generated->ExtradataIds = std::move(ids);
				first_generated = generated;
			}
			c->ass->Events.insert(insert_at, *generated);
			new_selection.insert(generated);
			++generated_count;
		}
		for (auto line : group.existing) {
			c->ass->Events.erase(c->ass->Events.iterator_to(*line));
			removed.emplace_back(line);
		}
		if (!new_active || was_active) new_active = first_generated;
	}

	if (!generated_count) return 0;
	c->selectionController->SetSelectionAndActive(std::move(new_selection), new_active);
	c->ass->CleanExtradata();
	c->ass->Commit(_("apply gradient"), AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return generated_count;
}

} // namespace typesetting::gradient
