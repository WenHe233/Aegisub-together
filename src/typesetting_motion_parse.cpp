// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_motion.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <regex>
#include <sstream>

namespace typesetting::motion {
namespace {

std::string Trim(std::string text) {
	auto space = [](unsigned char c) { return std::isspace(c); };
	text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), space));
	text.erase(std::find_if_not(text.rbegin(), text.rend(), space).base(), text.end());
	return text;
}

std::string Lower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

std::vector<double> Numbers(std::string const& line) {
	static std::regex number(R"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)");
	std::vector<double> values;
	for (auto at = std::sregex_iterator(line.begin(), line.end(), number);
		at != std::sregex_iterator(); ++at) {
		try { values.push_back(std::stod(at->str())); }
		catch (...) { }
	}
	return values;
}

} // namespace

std::optional<Track> ParseMocha(std::string const& text, int script_width,
	int script_height, TrackKind expected_kind, std::string& error) {
	error.clear();
	if (text.find("Adobe After Effects") == std::string::npos) {
		error = "A bemenet nem After Effects/Mocha keyframe data.";
		return std::nullopt;
	}
	std::istringstream input(text);
	std::vector<std::string> lines;
	std::string line;
	int source_width = 0, source_height = 0;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		lines.push_back(line);
		auto lower = Lower(line);
		auto values = Numbers(line);
		if (lower.find("source width") != std::string::npos && !values.empty())
			source_width = static_cast<int>(values.back());
		if (lower.find("source height") != std::string::npos && !values.empty())
			source_height = static_cast<int>(values.back());
	}
	if (source_width <= 0 || source_height <= 0) {
		error = "A Mocha-adatból hiányzik a Source Width vagy Source Height.";
		return std::nullopt;
	}
	double scale_x = static_cast<double>(script_width) / source_width;
	double scale_y = static_cast<double>(script_height) / source_height;
	enum class Section { None, Position, Scale, Rotation, UpperLeft, UpperRight, LowerRight, LowerLeft };
	Section section = Section::None;
	std::map<int, Vector2D> position, scale;
	std::map<int, double> rotation;
	std::array<std::map<int, Vector2D>, 4> corners;
	for (auto const& raw : lines) {
		std::string stripped = Trim(raw);
		std::string lower = Lower(stripped);
		bool indented = !raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()));
		if (!indented && !stripped.empty()) {
			if (lower.find("cc power pin-0002") != std::string::npos) section = Section::UpperLeft;
			else if (lower.find("cc power pin-0003") != std::string::npos) section = Section::UpperRight;
			else if (lower.find("cc power pin-0005") != std::string::npos) section = Section::LowerRight;
			else if (lower.find("cc power pin-0004") != std::string::npos) section = Section::LowerLeft;
			else if (lower == "position") section = Section::Position;
			else if (lower == "scale") section = Section::Scale;
			else if (lower == "rotation") section = Section::Rotation;
			else if (lower.find("upper left") != std::string::npos) section = Section::UpperLeft;
			else if (lower.find("upper right") != std::string::npos) section = Section::UpperRight;
			else if (lower.find("lower right") != std::string::npos) section = Section::LowerRight;
			else if (lower.find("lower left") != std::string::npos) section = Section::LowerLeft;
			else section = Section::None;
			continue;
		}
		auto values = Numbers(raw);
		if (values.size() < 2 || lower.find("frame") != std::string::npos) continue;
		int frame = static_cast<int>(std::lround(values[0]));
		switch (section) {
			case Section::Position:
				if (values.size() >= 3) position[frame] = Vector2D(values[1] * scale_x, values[2] * scale_y);
				break;
			case Section::Scale:
				// Keep both axes when Mocha exports them. One-value exports remain
				// uniform, while newer/non-uniform tracks retain their full scale.
				scale[frame] = Vector2D(values[1],
					values.size() >= 3 ? values[2] : values[1]);
				break;
			case Section::Rotation: rotation[frame] = values[1]; break;
			case Section::UpperLeft: case Section::UpperRight:
			case Section::LowerRight: case Section::LowerLeft:
				if (values.size() >= 3) {
					int index = static_cast<int>(section) - static_cast<int>(Section::UpperLeft);
					corners[index][frame] = Vector2D(values[1] * scale_x, values[2] * scale_y);
				}
				break;
			default: break;
		}
	}
	Track track;
	track.source_width = source_width;
	track.source_height = source_height;
	bool corner_pin = std::all_of(corners.begin(), corners.end(),
		[](auto const& values) { return !values.empty(); });
	if (expected_kind == TrackKind::CornerPin && corner_pin) {
		track.kind = TrackKind::CornerPin;
		track.adapter = "mocha-corner-pin";
		for (auto const& [frame, point] : corners[0]) {
			Sample sample; sample.source_frame = frame; sample.corners[0] = point;
			bool complete = true;
			for (int index = 1; index < 4; ++index) {
				auto found = corners[index].find(frame);
				if (found == corners[index].end()) { complete = false; break; }
				sample.corners[index] = found->second;
			}
			if (complete) track.samples.push_back(sample);
		}
	}
	else if (expected_kind == TrackKind::Transform && !position.empty()) {
		track.kind = TrackKind::Transform;
		track.adapter = "mocha-transform";
		Vector2D last_scale(100, 100);
		double last_rotation = 0;
		for (auto const& [frame, point] : position) {
			if (auto found = scale.find(frame); found != scale.end()) last_scale = found->second;
			if (auto found = rotation.find(frame); found != rotation.end()) last_rotation = found->second;
			Sample sample; sample.source_frame = frame; sample.position = point;
			sample.scale = last_scale; sample.rotation = last_rotation;
			track.samples.push_back(sample);
		}
	}
	if (track.samples.empty()) {
		error = expected_kind == TrackKind::CornerPin ?
			"Nem találtam teljes négysarkos Corner Pin adatsort." :
			"Nem találtam Position/Scale/Rotation transzformációs adatsort.";
		return std::nullopt;
	}
	return track;
}

} // namespace typesetting::motion
