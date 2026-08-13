// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_motion.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "auto4_base.h"
#include "imagemask_codec.h"
#include "include/aegisub/context.h"
#include "selection_controller.h"
#include "spline.h"
#include "subtitle_line_combiner.h"
#include "typesetting_perspective.h"
#include "typesetting_transform.h"
#include "video_controller.h"

#include <libaegisub/format.h>
#include <libaegisub/of_type_adaptor.h>
#include <libaegisub/split.h>

#include <boost/algorithm/string/replace.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace typesetting::motion {
namespace {

constexpr char motion_id_key[] = "aegisub/motion-id";
constexpr char motion_source_key[] = "aegisub/motion-source";
constexpr char source_separator = '\x1e';

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

bool SolveLinear(double matrix[8][9], double out[8]) {
	for (int column = 0; column < 8; ++column) {
		int pivot = column;
		for (int row = column + 1; row < 8; ++row)
			if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) pivot = row;
		if (std::abs(matrix[pivot][column]) < 1e-12) return false;
		if (pivot != column)
			for (int item = column; item <= 8; ++item)
				std::swap(matrix[pivot][item], matrix[column][item]);
		double divisor = matrix[column][column];
		for (int item = column; item <= 8; ++item) matrix[column][item] /= divisor;
		for (int row = 0; row < 8; ++row) {
			if (row == column) continue;
			double factor = matrix[row][column];
			for (int item = column; item <= 8; ++item)
				matrix[row][item] -= factor * matrix[column][item];
		}
	}
	for (int row = 0; row < 8; ++row) out[row] = matrix[row][8];
	return true;
}

Homography FromQuads(std::array<Vector2D, 4> const& from,
	std::array<Vector2D, 4> const& to) {
	double equations[8][9]{};
	for (int point = 0; point < 4; ++point) {
		double x = from[point].X(), y = from[point].Y();
		double u = to[point].X(), v = to[point].Y();
		double *first = equations[point * 2];
		first[0] = x; first[1] = y; first[2] = 1;
		first[6] = -u * x; first[7] = -u * y; first[8] = u;
		double *second = equations[point * 2 + 1];
		second[3] = x; second[4] = y; second[5] = 1;
		second[6] = -v * x; second[7] = -v * y; second[8] = v;
	}
	double solved[8];
	if (!SolveLinear(equations, solved)) return {};
	Homography out;
	for (int i = 0; i < 8; ++i) out.value[i] = solved[i];
	out.value[8] = 1;
	return out;
}

Homography TransformMap(Sample const& current, Sample const& reference) {
	double sx = std::abs(reference.scale.X()) > 1e-9 ?
		current.scale.X() / reference.scale.X() : 1.0;
	double sy = std::abs(reference.scale.Y()) > 1e-9 ?
		current.scale.Y() / reference.scale.Y() : sx;
	// AE rotation is clockwise in its y-down screen coordinates. ASS \frz is the
	// opposite sign, but the homography here works directly in screen coordinates.
	double radians = (current.rotation - reference.rotation) * 3.14159265358979 / 180.0;
	double cosine = std::cos(radians), sine = std::sin(radians);
	double a = cosine * sx, b = -sine * sy;
	double c = sine * sx, d = cosine * sy;
	Homography out;
	out.value = {a, b,
		current.position.X() - a * reference.position.X() - b * reference.position.Y(),
		c, d,
		current.position.Y() - c * reference.position.X() - d * reference.position.Y(),
		0, 0, 1};
	return out;
}

using param_vec = const std::vector<AssOverrideParameter> *;

param_vec FindTag(std::vector<std::unique_ptr<AssDialogueBlock>>& blocks,
	std::string const& name) {
	for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>())
		for (auto const& tag : block->Tags)
			if (tag.Name == name) return &tag.Params;
	return nullptr;
}

double FirstNumber(std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks,
	char const *name, double fallback) {
	for (auto const& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		for (auto const& tag : static_cast<AssDialogueBlockOverride *>(block.get())->Tags)
			if (tag.Name == name && !tag.Params.empty())
				return tag.Params[0].Get<double>(fallback);
		return fallback;
	}
	return fallback;
}

Vector2D TagVector(param_vec params) {
	if (!params || params->size() < 2 || (*params)[0].omitted || (*params)[1].omitted)
		return {};
	return Vector2D((*params)[0].Get<float>(), (*params)[1].Get<float>());
}

int Alignment(agi::Context *context, AssDialogue& line,
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks) {
	int align = 2;
	if (auto style = context->ass->GetStyle(line.Style)) align = style->alignment;
	if (auto tag = FindTag(blocks, "\\an")) align = tag->front().Get(align);
	return std::clamp(align, 1, 9);
}

Vector2D Position(agi::Context *context, AssDialogue& line,
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks, int align, int at_ms) {
	if (auto position = TagVector(FindTag(blocks, "\\pos"))) return position;
	if (auto move = FindTag(blocks, "\\move"); move && move->size() >= 4) {
		Vector2D first((*move)[0].Get<float>(), (*move)[1].Get<float>());
		Vector2D second((*move)[2].Get<float>(), (*move)[3].Get<float>());
		int relative = at_ms - static_cast<int>(line.Start);
		int start = 0;
		int end = static_cast<int>(line.End - line.Start);
		if (move->size() >= 6 && !(*move)[4].omitted && !(*move)[5].omitted) {
			start = (*move)[4].Get<int>();
			end = (*move)[5].Get<int>();
		}
		double progress = end == start ? (relative >= end ? 1.0 : 0.0) :
			std::clamp(static_cast<double>(relative - start) / (end - start), 0.0, 1.0);
		return first * static_cast<float>(1.0 - progress) + second * static_cast<float>(progress);
	}
	int script_width = 0, script_height = 0;
	context->ass->GetResolution(script_width, script_height);
	auto margin = line.Margin;
	if (auto style = context->ass->GetStyle(line.Style))
		for (int i = 0; i < 3; ++i) if (!margin[i]) margin[i] = style->Margin[i];
	int column = (align - 1) % 3;
	int row = (align - 1) / 3;
	float x = column == 0 ? margin[0] : column == 1 ?
		(script_width + margin[0] - margin[1]) / 2.f : script_width - margin[1];
	float y = row == 0 ? script_height - margin[2] : row == 1 ?
		script_height / 2.f : margin[2];
	return Vector2D(x, y);
}

std::pair<Vector2D, Vector2D> BaseExtents(agi::Context *context, AssDialogue& line,
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks) {
	auto drawing_tag = FindTag(blocks, "\\p");
	if (drawing_tag && drawing_tag->front().Get(0)) {
		Spline spline;
		spline.SetScale(drawing_tag->front().Get(1));
		std::string drawing;
		for (auto block : blocks | agi::of_type<AssDialogueBlockDrawing>()) drawing += block->GetText();
		spline.DecodeFromAss(drawing);
		if (spline.empty()) return {Vector2D(), Vector2D(1, 1)};
		Vector2D low(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
		Vector2D high(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
		for (auto curve : spline)
			for (auto point : curve.AnchorPoints()) { low = low.Min(point); high = high.Max(point); }
		return {low, high};
	}

	AssStyle style;
	if (auto base = context->ass->GetStyle(line.Style)) style = AssStyle(base->GetEntryData());
	if (auto tag = FindTag(blocks, "\\fs")) style.fontsize = tag->front().Get(style.fontsize);
	if (auto tag = FindTag(blocks, "\\fn")) style.font = tag->front().Get(style.font);
	std::string text = line.GetStrippedText();
	boost::replace_all(text, "\\N", "\n");
	std::vector<std::string> rows;
	agi::Split(rows, text, '\n');
	double width = 0, height = 0;
	for (auto const& row : rows) {
		double row_width = 0, row_height = 0, descent = 0, lead = 0;
		if (!Automation4::CalculateTextExtents(&style, row, row_width, row_height, descent, lead)) {
			row_width = style.fontsize * row.size();
			row_height = style.fontsize;
		}
		width = std::max(width, row_width);
		height += row_height;
	}
	return {Vector2D(0, 0), Vector2D(std::max(1.0, width), std::max(1.0, height))};
}

std::string Number(double value) {
	std::string out = agi::format("%.3f", value);
	while (out.size() > 1 && out.back() == '0') out.pop_back();
	if (!out.empty() && out.back() == '.') out.pop_back();
	return out == "-0" ? "0" : out;
}

void SetFirstTag(std::string& text, std::string const& name, std::string value,
	std::vector<std::string> aliases = {}) {
	if (text.empty() || text[0] != '{') text = "{}" + text;
	size_t close = text.find('}');
	if (close == std::string::npos) { text = "{}" + text; close = 1; }
	std::string block = text.substr(0, close + 1);
	auto remove = [&](std::string const& tag) {
		std::string plain = tag.size() && tag[0] == '\\' ? tag.substr(1) : tag;
		std::regex pattern("\\\\" + plain + R"((?:\([^)]*\)|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?))");
		block = std::regex_replace(block, pattern, "");
	};
	remove(name);
	for (auto const& alias : aliases) remove(alias);
	block.insert(block.size() - 1, name + std::move(value));
	text.replace(0, close + 1, block);
}

std::string MapLine(agi::Context *context, AssDialogue& line, Homography const& matrix,
	ApplyOptions const& options, bool map_clips, int at_ms) {
	auto blocks = line.ParseTags();
	int align = Alignment(context, line, blocks);
	Vector2D position = Position(context, line, blocks, align, at_ms);
	Vector2D origin = TagVector(FindTag(blocks, "\\org"));
	if (!origin) origin = position;
	AssStyle fallback;
	auto style = context->ass->GetStyle(line.Style);
	if (!style) style = &fallback;
	Vector2D scale(
		static_cast<float>(FirstNumber(blocks, "\\fscx", style->scalex)),
		static_cast<float>(FirstNumber(blocks, "\\fscy", style->scaley)));
	typesetting::PerspectiveTags tags;
	tags.pos = position;
	tags.org = origin;
	tags.scale = scale;
	tags.shear_x = FirstNumber(blocks, "\\fax", 0);
	tags.shear_y = FirstNumber(blocks, "\\fay", 0);
	tags.angle_z = FirstNumber(blocks, "\\frz",
		FirstNumber(blocks, "\\fr", style->angle));
	tags.angle_x = FirstNumber(blocks, "\\frx", 0);
	tags.angle_y = FirstNumber(blocks, "\\fry", 0);
	auto extents = BaseExtents(context, line, blocks);
	int script_width = 0, script_height = 0, layout_width = 0, layout_height = 0;
	context->ass->GetResolution(script_width, script_height);
	context->ass->GetEffectiveLayoutResolution(context, layout_width, layout_height);
	Vector2D screen_scale(
		layout_width ? static_cast<float>(script_width) / layout_width : 1.f,
		layout_height ? static_cast<float>(script_height) / layout_height : 1.f);
	Vector2D corners[4];
	typesetting::PerspectiveQuad(tags, align, extents.first, extents.second,
		screen_scale, corners);
	for (auto& corner : corners) corner = matrix.Map(corner);
	auto solved = typesetting::SolvePerspective(corners, align, extents.first,
		extents.second, screen_scale, matrix.Map(origin));
	if (!solved.ok)
		solved = typesetting::SolvePerspective(corners, align, extents.first,
			extents.second, screen_scale, Vector2D());
	if (!solved.ok) return line.Text.get();

	std::string text = line.Text.get();
	SetFirstTag(text, "\\pos", "(" + Number(solved.pos.X()) + "," +
		Number(solved.pos.Y()) + ")", {"\\move"});
	SetFirstTag(text, "\\org", "(" + Number(solved.org.X()) + "," +
		Number(solved.org.Y()) + ")");
	SetFirstTag(text, "\\fscx", Number(solved.scale.X()));
	SetFirstTag(text, "\\fscy", Number(solved.scale.Y()));
	SetFirstTag(text, "\\fax", Number(solved.shear_x));
	SetFirstTag(text, "\\fay", Number(solved.shear_y));
	SetFirstTag(text, "\\frz", Number(solved.angle_z), {"\\fr"});
	SetFirstTag(text, "\\frx", Number(solved.angle_x));
	SetFirstTag(text, "\\fry", Number(solved.angle_y));

	Vector2D mapped = matrix.Map(position);
	double growth_x = (matrix.Map(position + Vector2D(1, 0)) - mapped).Len();
	double growth_y = (matrix.Map(position + Vector2D(0, 1)) - mapped).Len();
	double growth = std::sqrt(std::max(0.0, growth_x * growth_y));
	if (options.scale_border) {
		double border = FirstNumber(blocks, "\\bord", style->outline_w);
		if (FindTag(blocks, "\\xbord") || FindTag(blocks, "\\ybord")) {
			double xborder = FirstNumber(blocks, "\\xbord", border);
			double yborder = FirstNumber(blocks, "\\ybord", border);
			SetFirstTag(text, "\\xbord", Number(xborder * growth_x), {"\\bord"});
			SetFirstTag(text, "\\ybord", Number(yborder * growth_y), {"\\bord"});
		}
		else
			SetFirstTag(text, "\\bord", Number(border * growth));
	}
	if (options.scale_shadow) {
		double shadow = FirstNumber(blocks, "\\shad", style->shadow_w);
		if (FindTag(blocks, "\\xshad") || FindTag(blocks, "\\yshad")) {
			double xshadow = FirstNumber(blocks, "\\xshad", shadow);
			double yshadow = FirstNumber(blocks, "\\yshad", shadow);
			SetFirstTag(text, "\\xshad", Number(xshadow * growth_x), {"\\shad"});
			SetFirstTag(text, "\\yshad", Number(yshadow * growth_y), {"\\shad"});
		}
		else
			SetFirstTag(text, "\\shad", Number(shadow * growth));
	}
	if (options.scale_blur) {
		double blur = FirstNumber(blocks, "\\blur", 0);
		if (blur > 0) SetFirstTag(text, "\\blur", Number(blur * growth));
	}
	if (map_clips) {
		typesetting::OrientedBox bounds;
		bounds.centre = Vector2D(script_width / 2.f, script_height / 2.f);
		bounds.half = Vector2D(script_width / 2.f, script_height / 2.f);
		text = typesetting::TransformClips(text,
			[&matrix](Vector2D point) { return matrix.Map(point); }, bounds);
	}
	return text;
}

imagemask::Raster Warp(imagemask::Raster const& source, Homography const& map,
	int script_width, int script_height) {
	if (!source.IsOk()) return {};
	std::array<Vector2D, 4> corners = {
		Vector2D(source.x, source.y), Vector2D(source.x + source.width, source.y),
		Vector2D(source.x + source.width, source.y + source.height),
		Vector2D(source.x, source.y + source.height)
	};
	Vector2D low(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
	Vector2D high(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
	for (auto corner : corners) { corner = map.Map(corner); low = low.Min(corner); high = high.Max(corner); }
	int left = std::clamp(static_cast<int>(std::floor(low.X())), 0, script_width);
	int top = std::clamp(static_cast<int>(std::floor(low.Y())), 0, script_height);
	int right = std::clamp(static_cast<int>(std::ceil(high.X())), 0, script_width);
	int bottom = std::clamp(static_cast<int>(std::ceil(high.Y())), 0, script_height);
	if (left >= right || top >= bottom) return {};
	auto inverse = map.Inverse();
	if (!inverse) return {};
	imagemask::Raster out;
	out.x = left; out.y = top; out.width = right - left; out.height = bottom - top;
	out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
	auto sample = [&](int x, int y, int channel) -> double {
		x = std::clamp(x, 0, source.width - 1);
		y = std::clamp(y, 0, source.height - 1);
		return source.rgba[(static_cast<size_t>(y) * source.width + x) * 4 + channel];
	};
	for (int y = 0; y < out.height; ++y) {
		for (int x = 0; x < out.width; ++x) {
			Vector2D at = inverse->Map(Vector2D(left + x + .5f, top + y + .5f));
			double fx = at.X() - source.x - .5, fy = at.Y() - source.y - .5;
			if (fx < -.5 || fy < -.5 || fx > source.width - .5 || fy > source.height - .5) continue;
			int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
			double tx = fx - x0, ty = fy - y0;
			auto filtered = [&](int channel) -> double {
				double first = sample(x0, y0, channel) * (1 - tx) + sample(x0 + 1, y0, channel) * tx;
				double second = sample(x0, y0 + 1, channel) * (1 - tx) + sample(x0 + 1, y0 + 1, channel) * tx;
				return first * (1 - ty) + second * ty;
			};
			auto filtered_premultiplied = [&](int channel) -> double {
				auto premultiplied = [&](int sx, int sy) {
					return sample(sx, sy, channel) * sample(sx, sy, 3) / 255.0;
				};
				double first = premultiplied(x0, y0) * (1 - tx) +
					premultiplied(x0 + 1, y0) * tx;
				double second = premultiplied(x0, y0 + 1) * (1 - tx) +
					premultiplied(x0 + 1, y0 + 1) * tx;
				return first * (1 - ty) + second * ty;
			};
			size_t offset = (static_cast<size_t>(y) * out.width + x) * 4;
			double alpha = filtered(3);
			out.rgba[offset + 3] = static_cast<unsigned char>(
				std::clamp(std::lround(alpha), 0l, 255l));
			if (alpha > .001) {
				for (int channel = 0; channel < 3; ++channel)
					out.rgba[offset + channel] = static_cast<unsigned char>(
						std::clamp(std::lround(filtered_premultiplied(channel) * 255.0 / alpha),
							0l, 255l));
			}
		}
	}
	return out;
}

std::string NewId() {
	static std::atomic<uint64_t> sequence{0};
	auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	return agi::format("%lld-%llu", static_cast<long long>(now),
		static_cast<unsigned long long>(++sequence));
}

std::string Sources(std::vector<AssDialogue *> const& lines) {
	std::string value;
	for (auto line : lines) {
		if (!value.empty()) value.push_back(source_separator);
		value += line->GetEntryData();
	}
	return value;
}

std::string Extra(AssFile const& file, AssDialogue const& line, char const *key) {
	for (auto const& extra : file.GetExtradata(line.ExtradataIds))
		if (extra.key == key) return extra.value;
	return {};
}

std::vector<std::string> SplitSources(std::string const& source) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at <= source.size()) {
		size_t end = source.find(source_separator, at);
		out.push_back(source.substr(at, end == std::string::npos ? std::string::npos : end - at));
		if (end == std::string::npos) break;
		at = end + 1;
	}
	return out;
}

} // namespace

Vector2D Homography::Map(Vector2D point) const {
	double denominator = value[6] * point.X() + value[7] * point.Y() + value[8];
	if (std::abs(denominator) < 1e-12) return point;
	return Vector2D(
		static_cast<float>((value[0] * point.X() + value[1] * point.Y() + value[2]) / denominator),
		static_cast<float>((value[3] * point.X() + value[4] * point.Y() + value[5]) / denominator));
}

std::optional<Homography> Homography::Inverse() const {
	double a = value[0], b = value[1], c = value[2];
	double d = value[3], e = value[4], f = value[5];
	double g = value[6], h = value[7], i = value[8];
	double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (std::abs(determinant) < 1e-12) return std::nullopt;
	Homography out;
	out.value = {
		(e * i - f * h) / determinant, (c * h - b * i) / determinant,
		(b * f - c * e) / determinant,
		(f * g - d * i) / determinant, (a * i - c * g) / determinant,
		(c * d - a * f) / determinant,
		(d * h - e * g) / determinant, (b * g - a * h) / determinant,
		(a * e - b * d) / determinant
	};
	return out;
}

Homography Track::MapAt(size_t sample, size_t reference) const {
	if (samples.empty()) return {};
	sample = std::min(sample, samples.size() - 1);
	reference = std::min(reference, samples.size() - 1);
	if (kind == TrackKind::CornerPin)
		return FromQuads(samples[reference].corners, samples[sample].corners);
	return TransformMap(samples[sample], samples[reference]);
}

std::optional<Track> ParseMocha(std::string const& text, int script_width,
	int script_height, std::string& error) {
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
			// Mocha's legacy AE Corner Pin export names the four pins by the
			// CC Power Pin property ids. Newer exports use readable corner names.
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
				scale[frame] = Vector2D(values[1], values.size() >= 3 ? values[2] : values[1]);
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
	if (corner_pin) {
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
	else if (!position.empty()) {
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
		error = "Nem találtam teljes Position/Scale/Rotation vagy négysarkos Corner Pin adatsort.";
		return std::nullopt;
	}
	return track;
}

bool Apply(agi::Context *context, Track const& main_track,
	std::optional<Track> const& clip_track, ApplyOptions const& options,
	std::string& error) {
	error.clear();
	if (!context || !context->videoController || !main_track.IsOk()) {
		error = "Nincs alkalmazható motion adat vagy videó.";
		return false;
	}
	auto selected = context->selectionController->GetSelectedSet();
	if (selected.empty()) { error = "Nincs kijelölt feliratsor."; return false; }
	int selection_start = std::numeric_limits<int>::max();
	int selection_end = 0;
	for (auto line : selected)
	{
		selection_start = std::min(selection_start,
			context->videoController->FrameAtTime(line->Start, agi::vfr::START));
		selection_end = std::max(selection_end,
			context->videoController->FrameAtTime(line->End, agi::vfr::END));
	}
	if (options.relative_to_selection &&
		main_track.samples.size() < static_cast<size_t>(selection_end - selection_start + 1)) {
		error = "A motion adat rövidebb a kijelölt képkockatartománynál.";
		return false;
	}
	if (options.relative_to_selection && clip_track &&
		clip_track->samples.size() < static_cast<size_t>(selection_end - selection_start + 1)) {
		error = "A külön clip motion adat rövidebb a kijelölt képkockatartománynál.";
		return false;
	}
	auto sample_for_frame = [&](Track const& track, int frame) -> size_t {
		if (options.relative_to_selection)
			return static_cast<size_t>(frame - selection_start);
		auto found = std::lower_bound(track.samples.begin(), track.samples.end(), frame,
			[](Sample const& sample, int target) { return sample.source_frame < target; });
		if (found == track.samples.end()) return track.samples.size() - 1;
		if (found == track.samples.begin() || found->source_frame == frame)
			return static_cast<size_t>(found - track.samples.begin());
		auto previous = found - 1;
		return frame - previous->source_frame <= found->source_frame - frame ?
			static_cast<size_t>(previous - track.samples.begin()) :
			static_cast<size_t>(found - track.samples.begin());
	};

	struct Work {
		std::vector<AssDialogue *> originals;
		std::vector<AssDialogue> output;
	};
	std::vector<Work> work;
	std::set<AssDialogue *> consumed;
	for (auto line : selected) {
		if (consumed.count(line)) continue;
		Work item;
		if (IsImageMaskLine(line) && context->imageMask && context->imageMask->IsInGroup(line))
			item.originals = context->imageMask->GetGroupLines(line);
		else
			item.originals = {line};
		for (auto original : item.originals) consumed.insert(original);
		std::sort(item.originals.begin(), item.originals.end(),
			[](auto left, auto right) { return left->Row < right->Row; });

		int start_frame = context->videoController->FrameAtTime(item.originals.front()->Start,
			agi::vfr::START);
		int end_frame = context->videoController->FrameAtTime(item.originals.front()->End,
			agi::vfr::END);
		bool mask = IsImageMaskLine(item.originals.front());
		auto raster = mask ? imagemask::Decode(item.originals) : std::nullopt;
		if (mask && !raster) { error = "Az ImageMask képe nem olvasható vissza."; return false; }
		int script_width = 0, script_height = 0;
		context->ass->GetResolution(script_width, script_height);
		std::string previous_signature;
		size_t previous_begin = 0;
		for (int frame = start_frame; frame <= end_frame; ++frame) {
			size_t sample = sample_for_frame(main_track, frame);
			Homography main_map = main_track.MapAt(sample, options.reference_sample);
			int start = std::max(static_cast<int>(item.originals.front()->Start),
				context->videoController->TimeAtFrame(frame, agi::vfr::START));
			int end = std::min(static_cast<int>(item.originals.front()->End),
				context->videoController->TimeAtFrame(frame, agi::vfr::END));
			if (end <= start) continue;
			std::vector<AssDialogue> generated;
			if (mask) {
				auto warped = Warp(*raster, main_map, script_width, script_height);
				generated = imagemask::Encode(warped, *item.originals.front(), start, end);
			}
			else {
				AssDialogue generated_line(*item.originals.front());
				generated_line.Comment = false;
				generated_line.ExtradataIds = std::vector<uint32_t>();
				generated_line.Text = MapLine(context, generated_line, main_map, options,
					options.map_clips && !clip_track, (start + end) / 2);
				generated_line.Start = start;
				generated_line.End = end;
				if (options.map_clips && clip_track) {
					size_t clip_sample = sample_for_frame(*clip_track, frame);
					size_t clip_reference = options.clip_reference_sample.value_or(
						options.reference_sample);
					Homography clip_map = clip_track->MapAt(clip_sample,
						std::min(clip_reference, clip_track->samples.size() - 1));
					typesetting::OrientedBox bounds;
					bounds.centre = Vector2D(script_width / 2.f, script_height / 2.f);
					bounds.half = Vector2D(script_width / 2.f, script_height / 2.f);
					generated_line.Text = typesetting::TransformClips(generated_line.Text.get(),
						[&clip_map](Vector2D point) { return clip_map.Map(point); }, bounds);
				}
				generated.push_back(std::move(generated_line));
			}
			std::string signature = imagemask::Signature(generated);
			if (!generated.empty() && signature == previous_signature) {
				for (size_t index = previous_begin; index < item.output.size(); ++index)
					item.output[index].End = end;
			}
			else {
				previous_begin = item.output.size();
				previous_signature = std::move(signature);
				std::move(generated.begin(), generated.end(), std::back_inserter(item.output));
			}
		}
		if (item.output.empty()) { error = "A motion nem hozott létre látható eredményt."; return false; }
		work.push_back(std::move(item));
	}

	Selection new_selection;
	AssDialogue *new_active = nullptr;
	std::vector<std::unique_ptr<AssDialogue>> removed;
	for (auto& item : work) {
		auto insert_at = context->ass->Events.iterator_to(*item.originals.front());
		std::string id = NewId();
		std::string source = Sources(item.originals);
		for (auto& output : item.output) {
			auto generated = new AssDialogue(std::move(output));
			context->ass->SetExtradataValue(*generated, motion_id_key, id);
			context->ass->SetExtradataValue(*generated, motion_source_key, source);
			auto ids = generated->ExtradataIds.get();
			std::sort(ids.begin(), ids.end());
			generated->ExtradataIds = std::move(ids);
			context->ass->Events.insert(insert_at, *generated);
			new_selection.insert(generated);
			if (!new_active) new_active = generated;
		}
		for (auto original : item.originals) {
			context->ass->Events.erase(context->ass->Events.iterator_to(*original));
			removed.emplace_back(original);
		}
	}
	context->selectionController->SetSelectionAndActive(std::move(new_selection), new_active);
	context->ass->CleanExtradata();
	context->ass->Commit("apply motion", AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return true;
}

bool Revert(agi::Context *context, std::string& error) {
	error.clear();
	auto selected = context->selectionController->GetSelectedSet();
	std::set<std::string> ids;
	for (auto line : selected) {
		auto id = Extra(*context->ass, *line, motion_id_key);
		if (!id.empty()) ids.insert(std::move(id));
	}
	if (ids.empty()) { error = "A kijelölésen nincs visszaállítható motion."; return false; }
	Selection selection;
	AssDialogue *active = nullptr;
	std::vector<std::unique_ptr<AssDialogue>> removed;
	for (auto const& id : ids) {
		std::vector<AssDialogue *> generated;
		std::string source;
		for (auto& line : context->ass->Events) {
			if (Extra(*context->ass, line, motion_id_key) != id) continue;
			if (source.empty()) source = Extra(*context->ass, line, motion_source_key);
			generated.push_back(&line);
		}
		if (generated.empty() || source.empty()) continue;
		auto insert_at = context->ass->Events.iterator_to(*generated.front());
		for (auto const& entry : SplitSources(source)) {
			try {
				auto original = new AssDialogue(entry);
				context->ass->Events.insert(insert_at, *original);
				selection.insert(original);
				if (!active) active = original;
			}
			catch (...) { }
		}
		for (auto line : generated) {
			context->ass->Events.erase(context->ass->Events.iterator_to(*line));
			removed.emplace_back(line);
		}
	}
	if (selection.empty()) { error = "A motion eredeti sorai nem olvashatók."; return false; }
	context->selectionController->SetSelectionAndActive(std::move(selection), active);
	context->ass->CleanExtradata();
	context->ass->Commit("revert motion", AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return true;
}

} // namespace typesetting::motion
