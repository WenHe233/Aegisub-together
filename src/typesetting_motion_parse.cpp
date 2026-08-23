// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_motion.h"

#include <algorithm>
#include <optional>
#include <array>
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

/// One of the tracker's channels: the keys it was written with, read where they are asked for.
///
/// The file declares these Linear, and says a plateau by putting the same value at both of its
/// ends - so between two keys the value is interpolated, and outside the outermost keys it is
/// held. Which is also why a channel that never moved has no keys at all.
struct Channel {
	std::map<int, double> keys;

	double At(int frame, double neutral) const {
		if (keys.empty()) return neutral;
		auto after = keys.lower_bound(frame);
		if (after == keys.end()) return std::prev(after)->second;
		if (after->first == frame) return after->second;
		if (after == keys.begin()) return after->second;
		auto before = std::prev(after);
		double span = static_cast<double>(after->first - before->first);
		if (span <= 0) return before->second;
		double along = (frame - before->first) / span;
		return before->second + (after->second - before->second) * along;
	}

	double Span() const {
		if (keys.empty()) return 0;
		double low = keys.begin()->second, high = low;
		for (auto const& [_, value] : keys) {
			low = std::min(low, value);
			high = std::max(high, value);
		}
		return high - low;
	}
};

struct Matrix3 {
	double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

Matrix3 Times(Matrix3 const& left, Matrix3 const& right) {
	Matrix3 out;
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j) {
			double sum = 0;
			for (int k = 0; k < 3; ++k) sum += left.m[i][k] * right.m[k][j];
			out.m[i][j] = sum;
		}
	return out;
}

} // namespace

std::optional<ProjectTrack> ParseMochaProject(std::string const& text, int script_width,
	int script_height, std::string& error) {
	error.clear();
	if (text.find("Imagineer project script") == std::string::npos &&
		text.find("GeneralProjectProperties") == std::string::npos) {
		error = "A fájl nem Mocha projekt.";
		return std::nullopt;
	}

	auto number = [&](char const *key) -> std::optional<double> {
		std::string wanted = std::string(key) + ".value=";
		auto at = text.find(wanted);
		if (at == std::string::npos) return std::nullopt;
		auto values = Numbers(text.substr(at + wanted.size(),
			text.find('\n', at) - at - wanted.size()));
		if (values.empty()) return std::nullopt;
		return values.front();
	};

	auto width = number("GeneralProjectProperties.ImageWidth");
	auto height = number("GeneralProjectProperties.ImageHeight");
	auto film_back = number("GeneralProjectProperties.FilmBackWidth");
	auto length = number("GeneralProjectProperties.ProjectLength");
	if (!width || !height || !film_back || !length || *width <= 0 || *height <= 0 ||
		*film_back <= 0 || *length < 1) {
		error = "A Mocha projekt fejléce hiányos.";
		return std::nullopt;
	}

	// How far one unit of the tracker's own coordinates reaches on the picture. The tracker works
	// in the camera's units, and these three are what turn those into pixels.
	double focal = 25;
	if (auto at = text.find(".Camera.FocalDistance.value="); at != std::string::npos) {
		auto values = Numbers(text.substr(at, text.find('\n', at) - at));
		if (!values.empty() && values.front() != 0) focal = values.front();
	}
	double reach = focal * *width / *film_back;

	// Every layer's channels, in the order the layers are written.
	std::vector<std::string> layer_order;
	std::map<std::string, std::map<std::string, Channel>> layers;
	std::istringstream input(text);
	std::string line;
	static std::regex key(R"(^(Layer_\d+)\.Track\.(\w+)\.keyframes\.append)");
	while (std::getline(input, line)) {
		std::smatch found;
		if (!std::regex_search(line, found, key)) continue;
		auto values = Numbers(line);
		if (values.size() < 2) continue;
		std::string const& which = found[1];
		if (!layers.count(which)) layer_order.push_back(which);
		layers[which][found[2]].keys[static_cast<int>(std::lround(values[0]))] = values[1];
	}

	// The layer the motion belongs to: the last one written that has anything tracked on it. A
	// chain of layers ends at the one that carries the whole of the motion, and the ones before it
	// are the steps it was built from.
	std::string chosen;
	for (auto const& which : layer_order) {
		auto const& channels = layers[which];
		bool tracked = false;
		for (char const *name : {"Translation_X", "Translation_Y", "Scale", "Rotation",
			"Shear_X", "Shear_Y", "Perspective_X", "Perspective_Y"})
			if (channels.count(name) && !channels.at(name).keys.empty()) tracked = true;
		if (tracked) chosen = which;
	}
	// Nothing tracked at all, which is a shot that was opened and left alone. Not an error.
	if (chosen.empty()) return std::nullopt;

	auto const& channels = layers[chosen];
	auto channel = [&](char const *name) -> Channel const& {
		static Channel const empty;
		auto found = channels.find(name);
		return found == channels.end() ? empty : found->second;
	};
	Channel const& tx = channel("Translation_X");
	Channel const& ty = channel("Translation_Y");
	Channel const& scale = channel("Scale");
	Channel const& rotation = channel("Rotation");
	Channel const& shear_x = channel("Shear_X");
	Channel const& shear_y = channel("Shear_Y");
	Channel const& perspective_x = channel("Perspective_X");
	Channel const& perspective_y = channel("Perspective_Y");

	// The tracker's own transform for one frame, in units of `reach` about the middle of the
	// picture. Worked out against the exported Position, Scale, Rotation and Corner Pin of the
	// same shot: this order and this frame land every corner within a hundredth of a pixel, which
	// is the rounding of the export itself.
	auto transform = [&](int frame) {
		double turn = rotation.At(frame, 0);
		double size = scale.At(frame, 1);
		Matrix3 move, spin, lean, grow, tilt;
		move.m[0][2] = tx.At(frame, 0);
		move.m[1][2] = ty.At(frame, 0);
		spin.m[0][0] = std::cos(turn);  spin.m[0][1] = -std::sin(turn);
		spin.m[1][0] = std::sin(turn);  spin.m[1][1] = std::cos(turn);
		lean.m[0][1] = shear_x.At(frame, 0);
		lean.m[1][1] = shear_y.At(frame, 1);
		grow.m[0][0] = size;            grow.m[1][1] = size;
		tilt.m[2][0] = perspective_x.At(frame, 0);
		tilt.m[2][1] = perspective_y.At(frame, 0);
		return Times(move, Times(spin, Times(lean, Times(grow, tilt))));
	};

	// The picture's own four corners are what the samples are written as. Any four points would
	// do - the map between two frames is what is ever asked for - but the whole picture keeps the
	// arithmetic behind that map well conditioned.
	double half_width = *width / 2, half_height = *height / 2;
	double across = static_cast<double>(script_width) / *width;
	double down = static_cast<double>(script_height) / *height;
	// In the order the samples are written in: upper left, upper right, lower right, lower left -
	// upper being the larger y here, because the tracker counts y up from the bottom.
	std::array<std::pair<double, double>, 4> picture{{
		{0, *height}, {*width, *height}, {*width, 0}, {0, 0}}};

	Track track;
	track.kind = TrackKind::CornerPin;
	track.source_width = static_cast<int>(*width);
	track.source_height = static_cast<int>(*height);
	track.coordinate_width = script_width;
	track.coordinate_height = script_height;
	track.adapter = "mocha-project";
	int frames = static_cast<int>(*length);
	for (int frame = 0; frame < frames; ++frame) {
		Matrix3 map = transform(frame);
		Sample sample;
		sample.source_frame = frame;
		bool usable = true;
		for (int corner = 0; corner < 4; ++corner) {
			double u = (picture[corner].first - half_width) / reach;
			double v = (picture[corner].second - half_height) / reach;
			double w = map.m[2][0] * u + map.m[2][1] * v + map.m[2][2];
			if (!(std::abs(w) > 1e-9)) { usable = false; break; }
			double x = half_width + reach * (map.m[0][0] * u + map.m[0][1] * v + map.m[0][2]) / w;
			double y = half_height + reach * (map.m[1][0] * u + map.m[1][1] * v + map.m[1][2]) / w;
			// And into the script's own coordinates, which count y down from the top.
			sample.corners[corner] = Vector2D(static_cast<float>(x * across),
				static_cast<float>((*height - y) * down));
		}
		if (!usable) {
			error = "A Mocha projekt egyik képkockája nem értelmezhető.";
			return std::nullopt;
		}
		track.samples.push_back(sample);
	}
	if (track.samples.empty()) return std::nullopt;

	ProjectTrack out;
	out.layer = chosen;
	out.has_scale = scale.Span() > 1e-9;
	out.has_rotation = rotation.Span() > 1e-9;
	out.has_perspective = perspective_x.Span() > 1e-9 || perspective_y.Span() > 1e-9;
	out.has_shear = shear_x.Span() > 1e-9 || shear_y.Span() > 1e-9;
	out.track = std::move(track);
	return out;
}

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
	track.coordinate_width = script_width;
	track.coordinate_height = script_height;
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
