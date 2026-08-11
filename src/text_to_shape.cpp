// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file text_to_shape.cpp
/// @brief Turning the text of a line into an ASS drawing
///
/// The outline comes from the platform's own text engine rather than from a font
/// file read directly, because the hard part of this is not the glyph curves but the
/// layout around them - advances, kerning, the shaping of scripts that need it. On
/// Windows GDI does all of that in one call and hands back the path, which is also
/// what the Automation script this replaces does on Windows. FreeType is in the tree
/// as a libass dependency and would give the curves on the other platforms, but it
/// would not give the layout, so it is a separate job rather than a switch here.

#include "text_to_shape.h"

#include "ass_dialogue.h"
#include "vector2d.h"
#include "ass_file.h"
#include "ass_style.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"

#include <libaegisub/format.h>
#include <libaegisub/of_type_adaptor.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <wx/msgdlg.h>
#include <wx/string.h>

#ifdef __WXMSW__
#include <windows.h>
#endif

namespace text_to_shape {
namespace {

/// The parameter list of an override tag, as the visual tools spell it.
typedef const std::vector<AssOverrideParameter> * param_vec;

/// The font is built this many times larger than asked for and the path divided back
/// down, so the integer coordinates GDI returns still carry sub-pixel detail.
constexpr int UPSCALE = 64;

/// What one line's text should be drawn with. Gathered from the style and from the
/// line's own override tags.
struct TextStyle {
	std::string face;
	double size = 0;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strikeout = false;
	double spacing = 0;
	int alignment = 2;
	double scale_x = 100;
	double scale_y = 100;
	double angle = 0;             ///< \frz, in degrees
	double rot_x = 0;             ///< \frx, in degrees
	double rot_y = 0;             ///< \fry, in degrees
	double shear_x = 0;           ///< \fax
	double shear_y = 0;           ///< \fay
	Vector2D border;              ///< \bord, or \xbord and \ybord
	Vector2D shadow;              ///< \shad, or \xshad and \yshad
	bool has_origin = false;      ///< whether \org moved the centre of the rotation
	Vector2D origin;              ///< where \org put it, in script coordinates
	Vector2D position;            ///< where the line is drawn, in script coordinates
};

/// What ASS does to a glyph outline between the font and the screen.
///
/// Everything but the scale, which is already multiplied into the coordinates: the two
/// shears, the three rotations, and the perspective projection about \org. Kept as a
/// projective matrix because that is what the chain collapses to, and because the
/// perspective needs the third row.
struct AssTransform {
	double a[3] = {1, 0, 0};
	double b[3] = {0, 1, 0};
	double c[3] = {0, 0, 1};
	bool needed = false;

	Vector2D Apply(double x, double y) const {
		if (!needed) return Vector2D((float)x, (float)y);
		double px = a[0] * x + a[1] * y + a[2];
		double py = b[0] * x + b[1] * y + b[2];
		double pz = c[0] * x + c[1] * y + c[2];
		double w = 1.0 / std::max(pz, 0.1);
		return Vector2D((float)(px * w), (float)(py * w));
	}
};

/// Build that matrix.
///
/// `ascent` and `left` are how far the anchor of the old alignment sits below the top of
/// the text block and to the right of its left edge, in the same units as the outline. The
/// two shears are measured from the top and the left of the block - libass shears x by the
/// distance down the block and y by the distance along the row - while the outline is
/// measured from the anchor, so each shear needs the offset between the two put back.
AssTransform BuildTransform(TextStyle const& style, double ascent, double left) {
	AssTransform out;
	out.needed = std::abs(style.angle) > 1e-9 || std::abs(style.rot_x) > 1e-9 ||
		std::abs(style.rot_y) > 1e-9 || std::abs(style.shear_x) > 1e-9 ||
		std::abs(style.shear_y) > 1e-9;
	if (!out.needed) return out;

	// The distance the projection is done from. Not a free parameter: it is the number
	// the renderers use, and a different one would put a rotated line somewhere else.
	const double dist = 312.5;
	const double pi = 3.14159265358979;

	double frx = style.rot_x * pi / 180, fry = style.rot_y * pi / 180;
	double frz = style.angle * pi / 180;
	double sx = -std::sin(frx), cx = std::cos(frx);
	double sy = std::sin(fry), cy = std::cos(fry);
	double sz = -std::sin(frz), cz = std::cos(frz);

	// The outline is already scaled, and a shear in scaled space is not the same number
	// as a shear before it.
	double scale_x = style.scale_x / 100.0, scale_y = style.scale_y / 100.0;
	double fax = style.shear_x * (std::abs(scale_y) > 1e-9 ? scale_x / scale_y : 1.0);
	double fay = style.shear_y * (std::abs(scale_x) > 1e-9 ? scale_y / scale_x : 1.0);

	// Where the rotation turns about, relative to where the line is drawn.
	double offs_x = 0, offs_y = 0;
	if (style.has_origin) {
		offs_x = style.origin.X() - style.position.X();
		offs_y = style.origin.Y() - style.position.Y();
	}

	double x1[3] = {1, fax, -offs_x + ascent * fax};
	double y1[3] = {fay, 1, -offs_y + left * fay};

	for (int i = 0; i < 3; ++i) {
		double x2 = x1[i] * cz - y1[i] * sz;
		double y2 = x1[i] * sz + y1[i] * cz;
		double y3 = y2 * cx;
		double z3 = y2 * sx;
		double x4 = x2 * cy - z3 * sy;
		double z4 = x2 * sy + z3 * cy;
		if (i == 2) z4 += dist;

		out.a[i] = z4 * offs_x + x4 * dist;
		out.b[i] = z4 * offs_y + y3 * dist;
		out.c[i] = z4;
	}

	return out;
}

/// Where a line without \pos sits, by the same rules the renderer uses: the alignment
/// picks a corner of the margins, and that is what the text is hung from.
Vector2D LayoutPosition(const agi::Context *c, AssDialogue *line, int alignment) {
	int script_w = 0, script_h = 0;
	c->ass->GetResolution(script_w, script_h);

	auto margin = line->Margin;
	if (AssStyle *style = c->ass->GetStyle(line->Style))
		for (int i = 0; i < 3; ++i)
			if (margin[i] == 0) margin[i] = style->Margin[i];

	int horizontal = (alignment - 1) % 3;
	int vertical = (alignment - 1) / 3;

	float x = horizontal == 0 ? (float)margin[0] :
		horizontal == 1 ? (script_w + margin[0] - margin[1]) / 2.f :
		(float)(script_w - margin[1]);
	// ASS counts the rows from the bottom: 1-3 sit on the bottom margin, 7-9 on the top.
	float y = vertical == 0 ? (float)(script_h - margin[2]) :
		vertical == 1 ? script_h / 2.f : (float)margin[2];

	return Vector2D(x, y);
}

/// How wide the renderer would let a row get before breaking it.
double LayoutWidth(const agi::Context *c, AssDialogue *line) {
	int script_w = 0, script_h = 0;
	c->ass->GetResolution(script_w, script_h);

	auto margin = line->Margin;
	if (AssStyle *style = c->ass->GetStyle(line->Style))
		for (int i = 0; i < 3; ++i)
			if (margin[i] == 0) margin[i] = style->Margin[i];

	return std::max(0, script_w - margin[0] - margin[1]);
}

param_vec FindTag(std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks,
                  const char *name) {
	for (auto ovr : blocks | agi::of_type<AssDialogueBlockOverride>())
		for (auto const& tag : ovr->Tags)
			if (tag.Name == name) return &tag.Params;
	return nullptr;
}

TextStyle GatherStyle(const agi::Context *c, AssDialogue *line) {
	TextStyle out;
	AssStyle const default_style;
	AssStyle const *style = c->ass->GetStyle(line->Style);
	if (!style) style = &default_style;

	out.face = style->font;
	out.size = style->fontsize;
	out.bold = style->bold;
	out.italic = style->italic;
	out.underline = style->underline;
	out.strikeout = style->strikeout;
	out.spacing = style->spacing;
	out.alignment = style->alignment;
	out.scale_x = style->scalex;
	out.scale_y = style->scaley;

	auto blocks = line->ParseTags();
	if (auto tag = FindTag(blocks, "\\fn")) out.face = (*tag)[0].Get<std::string>(out.face);
	if (auto tag = FindTag(blocks, "\\fs")) out.size = (*tag)[0].Get<double>(out.size);
	if (auto tag = FindTag(blocks, "\\b")) out.bold = (*tag)[0].Get<int>(out.bold ? 1 : 0) != 0;
	if (auto tag = FindTag(blocks, "\\i")) out.italic = (*tag)[0].Get<int>(out.italic ? 1 : 0) != 0;
	if (auto tag = FindTag(blocks, "\\u")) out.underline = (*tag)[0].Get<int>(out.underline ? 1 : 0) != 0;
	if (auto tag = FindTag(blocks, "\\s")) out.strikeout = (*tag)[0].Get<int>(out.strikeout ? 1 : 0) != 0;
	if (auto tag = FindTag(blocks, "\\fsp")) out.spacing = (*tag)[0].Get<double>(out.spacing);
	out.border = Vector2D((float)style->outline_w, (float)style->outline_w);
	out.shadow = Vector2D((float)style->shadow_w, (float)style->shadow_w);
	if (auto tag = FindTag(blocks, "\\bord")) {
		float both = (*tag)[0].Get<float>(out.border.X());
		out.border = Vector2D(both, both);
	}
	if (auto tag = FindTag(blocks, "\\shad")) {
		float both = (*tag)[0].Get<float>(out.shadow.X());
		out.shadow = Vector2D(both, both);
	}
	if (auto tag = FindTag(blocks, "\\xbord"))
		out.border = Vector2D((*tag)[0].Get<float>(out.border.X()), out.border.Y());
	if (auto tag = FindTag(blocks, "\\ybord"))
		out.border = Vector2D(out.border.X(), (*tag)[0].Get<float>(out.border.Y()));
	if (auto tag = FindTag(blocks, "\\xshad"))
		out.shadow = Vector2D((*tag)[0].Get<float>(out.shadow.X()), out.shadow.Y());
	if (auto tag = FindTag(blocks, "\\yshad"))
		out.shadow = Vector2D(out.shadow.X(), (*tag)[0].Get<float>(out.shadow.Y()));
	if (auto tag = FindTag(blocks, "\\fscx")) out.scale_x = (*tag)[0].Get<double>(out.scale_x);
	if (auto tag = FindTag(blocks, "\\fscy")) out.scale_y = (*tag)[0].Get<double>(out.scale_y);
	out.angle = style->angle;
	if (auto tag = FindTag(blocks, "\\frz")) out.angle = (*tag)[0].Get<double>(out.angle);
	else if (auto tag = FindTag(blocks, "\\fr")) out.angle = (*tag)[0].Get<double>(out.angle);
	if (auto tag = FindTag(blocks, "\\frx")) out.rot_x = (*tag)[0].Get<double>(out.rot_x);
	if (auto tag = FindTag(blocks, "\\fry")) out.rot_y = (*tag)[0].Get<double>(out.rot_y);
	if (auto tag = FindTag(blocks, "\\fax")) out.shear_x = (*tag)[0].Get<double>(out.shear_x);
	if (auto tag = FindTag(blocks, "\\fay")) out.shear_y = (*tag)[0].Get<double>(out.shear_y);
	if (auto tag = FindTag(blocks, "\\org")) {
		if (tag->size() >= 2 && !(*tag)[0].omitted && !(*tag)[1].omitted) {
			out.has_origin = true;
			out.origin = Vector2D((*tag)[0].Get<float>(), (*tag)[1].Get<float>());
		}
	}
	if (auto tag = FindTag(blocks, "\\an")) {
		int value = (*tag)[0].Get<int>(out.alignment);
		if (value > 0 && value <= 9) out.alignment = value;
	}
	else if (auto tag = FindTag(blocks, "\\a")) {
		int value = AssStyle::SsaToAss((*tag)[0].Get<int>(2));
		if (value > 0 && value <= 9) out.alignment = value;
	}
	return out;
}

/// What the renderer's chain does to the shadow's offset and to the border's pen.
///
/// The shadow's offset is added to the glyph's shift, which rides in the translation column of the
/// transform: so it is turned by \frz, and neither scaled nor sheared. The border is stroked with
/// an upright pen of the asked-for width and then sheared and turned with the letters, so what is
/// left of it once the letters have none of that is the pen's own extents after the same lean and
/// turn - which is as near as an upright pen can come to a leaning one.
struct Decorations {
	Vector2D shadow;
	Vector2D border;
};

Decorations TransformDecorations(TextStyle const& style, Vector2D shadow, Vector2D border) {
	const double pi = 3.14159265358979;
	double radians = style.angle * pi / 180.0;
	double sine = -std::sin(radians), cosine = std::cos(radians);

	double scale_x = style.scale_x / 100.0, scale_y = style.scale_y / 100.0;
	double fax = style.shear_x * (std::abs(scale_y) > 1e-9 ? scale_x / scale_y : 1.0);
	double fay = style.shear_y * (std::abs(scale_x) > 1e-9 ? scale_y / scale_x : 1.0);

	Decorations out;
	out.shadow = Vector2D(
		(float)(shadow.X() * cosine - shadow.Y() * sine),
		(float)(shadow.X() * sine + shadow.Y() * cosine));

	// The lean and the turn as one, then the pen through it: an upright pen becomes a slanted
	// ellipse, and what an upright pen can say of it is how far that ellipse reaches each way.
	double a = cosine - fay * sine, b = fax * cosine - sine;
	double c = sine + fay * cosine, d = fax * sine + cosine;
	out.border = Vector2D(
		(float)std::hypot(a * border.X(), b * border.Y()),
		(float)std::hypot(c * border.X(), d * border.Y()));
	return out;
}

std::string FormatNumber(double value) {
	std::string text = agi::format("%.3f", value);
	auto dot = text.find('.');
	if (dot != std::string::npos) {
		while (!text.empty() && text.back() == '0') text.pop_back();
		if (!text.empty() && text.back() == '.') text.pop_back();
	}
	return text.empty() ? "0" : text;
}

/// Which outlines a measuring run reads out, and in which frame.
enum class Outlines {
	None,   ///< none at all: the measurements and the layout, which is the cheap half
	Baked,  ///< in script coordinates, with everything the line does to them already in the numbers
	Loose   ///< in the units the line's own \fscx and \fscy still act on, from the piece's own
	        ///< corner - so the piece's own tags put it back - and widened by its border as well
};

/// One stretch of text with one set of tags in force: what a line has to be broken into,
/// because a conversion can only bake one font and one transform at a time.
struct Piece {
	TextStyle style;
	std::string tags;      ///< what goes on the line this piece becomes
	/// Only the tags that came into force just before this piece, rather than everything in
	/// force by then. What a piece needs when it is written after another one on the same
	/// line: saying the lot again would restart an animation that is already running.
	std::string added_tags;
	std::wstring text;
	/// The same text with its hard spaces still hard. A piece written on a line of its own
	/// has both its ends at the end of a row, where the renderer drops ordinary spaces - so
	/// which of them may not be dropped has to survive the measuring.
	std::wstring source_text;
	/// Whether the stretch held anything before the ends of its row were trimmed. A row that
	/// held only spaces is a row of full height; one that held nothing at all is half of one.
	bool had_char = false;
	size_t row = 0;

	/// Measured, in script units.
	double width = 0;
	double ascent = 0;
	double descent = 0;
	/// Placed, in script units, from the top left of the whole block.
	double x = 0;
	double top = 0;

	std::string drawing;
	/// The same letters in the frame the line's own tags act on, and the same again widened by the
	/// border the line asked for. Both empty unless they were asked for.
	std::string loose;
	std::string wide;
};

/// Whether the conversion swallows this tag, so that writing it out again would apply it
/// twice.
bool IsBakedTag(std::string const& name) {
	static const char *baked[] = {
		"\\fn", "\\fs", "\\b", "\\i", "\\u", "\\s", "\\fsp", "\\fscx", "\\fscy",
		"\\frz", "\\fr", "\\org", "\\frx", "\\fry", "\\fax", "\\fay",
		"\\pos", "\\move", "\\an", "\\a"
	};
	for (auto candidate : baked)
		if (name == candidate) return true;
	return false;
}

/// The part of the state that comes from a style rather than from the line's tags.
TextStyle StyleOnly(const agi::Context *c, std::string const& name) {
	AssStyle const default_style;
	AssStyle const *style = c->ass->GetStyle(name);
	if (!style) style = &default_style;

	TextStyle out;
	out.face = style->font;
	out.size = style->fontsize;
	out.bold = style->bold;
	out.italic = style->italic;
	out.underline = style->underline;
	out.strikeout = style->strikeout;
	out.spacing = style->spacing;
	out.alignment = style->alignment;
	out.scale_x = style->scalex;
	out.scale_y = style->scaley;
	out.angle = style->angle;
	// Not baked into anything, but a stretch still has to know them: a shape that stands in for the
	// border is widened by the pen the stretch itself asked for.
	out.border = Vector2D((float)style->outline_w, (float)style->outline_w);
	out.shadow = Vector2D((float)style->shadow_w, (float)style->shadow_w);
	return out;
}

/// Apply one tag to the running state of a line.
void ApplyTag(const agi::Context *c, AssDialogue *line, AssOverrideTag const& tag,
              TextStyle& style, std::string& carried) {
	std::string const& name = tag.Name;
	auto const& params = tag.Params;
	auto number = [&](double fallback) {
		return params.empty() ? fallback : params[0].Get<double>(fallback);
	};
	auto flag = [&](bool fallback) {
		return params.empty() ? fallback : params[0].Get<int>(fallback ? 1 : 0) != 0;
	};

	if (name == "\\r") {
		// A reset goes back to a style and drops everything said before it. The alignment
		// belongs to the line as a whole, so it is not part of what gets reset here.
		std::string named = params.empty() ? std::string() :
			params[0].Get<std::string>(std::string());
		int alignment = style.alignment;
		style = StyleOnly(c, named.empty() ? line->Style.get() : named);
		style.alignment = alignment;
		carried.clear();
		return;
	}

	if (name == "\\fn")
		style.face = params.empty() ? style.face : params[0].Get<std::string>(style.face);
	else if (name == "\\fs") style.size = number(style.size);
	else if (name == "\\b") style.bold = flag(style.bold);
	else if (name == "\\i") style.italic = flag(style.italic);
	else if (name == "\\u") style.underline = flag(style.underline);
	else if (name == "\\s") style.strikeout = flag(style.strikeout);
	else if (name == "\\fsp") style.spacing = number(style.spacing);
	else if (name == "\\fscx") style.scale_x = number(style.scale_x);
	else if (name == "\\fscy") style.scale_y = number(style.scale_y);
	else if (name == "\\frz" || name == "\\fr") style.angle = number(style.angle);
	else if (name == "\\frx") style.rot_x = number(style.rot_x);
	else if (name == "\\fry") style.rot_y = number(style.rot_y);
	else if (name == "\\bord") {
		float both = (float)number(style.border.X());
		style.border = Vector2D(both, both);
	}
	else if (name == "\\xbord")
		style.border = Vector2D((float)number(style.border.X()), style.border.Y());
	else if (name == "\\ybord")
		style.border = Vector2D(style.border.X(), (float)number(style.border.Y()));
	else if (name == "\\shad") {
		float both = (float)number(style.shadow.X());
		style.shadow = Vector2D(both, both);
	}
	else if (name == "\\xshad")
		style.shadow = Vector2D((float)number(style.shadow.X()), style.shadow.Y());
	else if (name == "\\yshad")
		style.shadow = Vector2D(style.shadow.X(), (float)number(style.shadow.Y()));
	else if (name == "\\fax") style.shear_x = number(style.shear_x);
	else if (name == "\\fay") style.shear_y = number(style.shear_y);
	else if (name == "\\org") {
		if (params.size() >= 2 && !params[0].omitted && !params[1].omitted) {
			style.has_origin = true;
			style.origin = Vector2D(params[0].Get<float>(), params[1].Get<float>());
		}
	}

	if (!IsBakedTag(name)) carried += (std::string)tag;
}

/// Break a line into pieces, in reading order. Every row gets at least one piece, even an
/// empty one, so that a row with nothing on it still has a height.
std::vector<Piece> SplitPieces(const agi::Context *c, AssDialogue *line, int alignment) {
	TextStyle running = StyleOnly(c, line->Style);
	running.alignment = alignment;
	std::string carried;

	std::vector<Piece> pieces;
	size_t row = 0;

	// How much of `carried` the last piece already said, so that each one can be given only
	// what is new. A reset empties it, and then there is nothing to carry.
	size_t carried_said = 0;

	auto add = [&](std::string const& raw) {
		Piece piece;
		piece.style = running;
		piece.tags = carried;
		piece.added_tags = carried.size() >= carried_said ?
			carried.substr(carried_said) : carried;
		carried_said = carried.size();
		piece.row = row;
		piece.text = wxString::FromUTF8(raw).ToStdWstring();
		piece.source_text = piece.text;
		// Whether there was anything here at all, before the ends of the row are trimmed. A
		// stretch that held only spaces is still a stretch as far as the height of the row
		// goes; one that held nothing is not.
		piece.had_char = !piece.text.empty();
		pieces.push_back(std::move(piece));
	};

	// \n is a row break only where nothing wraps by itself; everywhere else the renderers
	// read it as a space, and breaking there would put every row in the wrong place.
	int wrap_style = c->ass->GetScriptInfoAsInt("WrapStyle");
	auto blocks = line->ParseTags();
	if (auto tag = FindTag(blocks, "\\q")) {
		int value = (*tag)[0].Get<int>(wrap_style);
		if (value >= 0 && value <= 3) wrap_style = value;
	}

	std::string current;
	for (auto& block : blocks) {
		if (block->GetType() == AssBlockType::OVERRIDE) {
			// The tags take effect where they are written, so the text before them is a
			// piece of its own.
			if (!current.empty()) { add(current); current.clear(); }
			for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags)
				ApplyTag(c, line, tag, running, carried);
			continue;
		}
		if (block->GetType() != AssBlockType::PLAIN) continue;

		std::string const& raw = block->GetText();
		for (size_t at = 0; at < raw.size(); ++at) {
			// A tab is a space to the renderers, and measuring the tab itself would ask the
			// font for a glyph that is not there.
			if (raw[at] == '\t') { current += ' '; continue; }
			if (raw[at] == '\\' && at + 1 < raw.size()) {
				char next = raw[at + 1];
				if (next == 'N' || (next == 'n' && wrap_style == 2)) {
					add(current);
					current.clear();
					++row;
					++at;
					continue;
				}
				if (next == 'n') { current += ' '; ++at; continue; }
				if (next == 'h') { current += "\xC2\xA0"; ++at; continue; }
			}
			current += raw[at];
		}
	}
	add(current);

	// The renderers drop the spaces at the two ends of a row - only ordinary ones, and only
	// at the ends: \h is there to say "a space that stays", and what sits between the pieces
	// of a row is text like any other. A run of spaces can cross from one piece into the
	// next, so each end of the row is walked until something else turns up rather than each
	// piece being trimmed on its own.
	for (size_t at = 0; at < pieces.size();) {
		size_t past = at;
		while (past < pieces.size() && pieces[past].row == pieces[at].row) ++past;

		// The two say the same thing in the same places, so what is trimmed off one is
		// trimmed off the other.
		for (size_t i = at; i < past; ++i) {
			auto& piece = pieces[i];
			size_t start = piece.text.find_first_not_of(L' ');
			if (start == std::wstring::npos) {
				piece.text.clear();
				piece.source_text.clear();
				continue;
			}
			piece.text = piece.text.substr(start);
			piece.source_text = piece.source_text.substr(start);
			break;
		}
		for (size_t i = past; i-- > at;) {
			auto& piece = pieces[i];
			size_t end = piece.text.find_last_not_of(L' ');
			if (end == std::wstring::npos) {
				piece.text.clear();
				piece.source_text.clear();
				continue;
			}
			piece.text = piece.text.substr(0, end + 1);
			piece.source_text = piece.source_text.substr(0, end + 1);
			break;
		}

		at = past;
	}

	// A hard space was carried as U+00A0 so that the trimming would leave it alone; from
	// here on it is an ordinary space, which is what it is measured and drawn as.
	for (auto& piece : pieces)
		std::replace(piece.text.begin(), piece.text.end(), L'\u00A0', L' ');

	return pieces;
}

/// A ring of an outline, flattened to straight steps.
typedef std::vector<Vector2D> Contour;

/// One cubic curve, flattened to as many steps as it takes for the corners of them not to show.
void AddCurve(Contour& out, Vector2D a, Vector2D b, Vector2D c, Vector2D d, double fine) {
	// The control polygon is longer than the curve and bows further from the chord than the curve
	// does, so the number of steps it asks for is on the safe side of enough.
	double bow = (b - a).Len() + (c - b).Len() + (d - c).Len();
	int steps = (int)std::ceil(std::sqrt(bow / std::max(fine, 1e-3)));
	steps = std::min(std::max(steps, 2), 24);
	for (int i = 1; i <= steps; ++i) {
		double t = (double)i / steps, u = 1 - t;
		double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
		out.push_back(Vector2D(
			(float)(w0 * a.X() + w1 * b.X() + w2 * c.X() + w3 * d.X()),
			(float)(w0 * a.Y() + w1 * b.Y() + w2 * c.Y() + w3 * d.Y())));
	}
}

/// Wind a ring the way the letters are wound, so that the non-zero rule adds it to them rather
/// than cutting into them.
void WindRing(Contour& ring, double sense) {
	double area = 0;
	for (size_t i = 0; i < ring.size(); ++i) {
		auto const& p = ring[i];
		auto const& q = ring[(i + 1) % ring.size()];
		area += (double)p.X() * q.Y() - (double)q.X() * p.Y();
	}
	if (area * sense < 0) std::reverse(ring.begin(), ring.end());
}

/// The same outline widened by the pen the renderer would have stroked it with - the Minkowski sum
/// of the letters and an upright ellipse - as the rings that have to be added to the letters.
///
/// A band along every edge, and the pen's own shape at every corner that turns away from the ink.
/// All of them wound the way the letters are, which is what makes the non-zero rule union them:
/// where a band crosses a letter the winding only grows, and a hole stays a hole because the band
/// that cancels its winding reaches no further into it than the pen does.
///
/// `sense` says which way the letters are wound. It is one sign for all of the rings, holes
/// included: the fill rule needs them wound against each other, and that is exactly what leaves
/// the ink on the same side of every ring - so one sign is all it takes to face away from it.
std::vector<Contour> Widen(std::vector<Contour> const& rings, double rx, double ry, double sense) {
	std::vector<Contour> out;
	if (rx < 1e-3 && ry < 1e-3) return out;
	rx = std::max(rx, 1e-3);
	ry = std::max(ry, 1e-3);
	const double pi = 3.14159265358979;

	// How far the pen reaches in the direction an edge faces. An ellipse touches a line where its
	// own normal is the line's, which is not the direction of the line unless the radii agree.
	auto reach = [&](Vector2D face) {
		double len = std::hypot(rx * face.X(), ry * face.Y());
		if (len < 1e-9) return Vector2D(0, 0);
		return Vector2D((float)(rx * rx * face.X() / len), (float)(ry * ry * face.Y() / len));
	};
	auto add = [&](Contour ring) {
		WindRing(ring, sense);
		out.push_back(std::move(ring));
	};

	for (auto const& ring : rings) {
		size_t count = ring.size();
		for (size_t i = 0; i < count; ++i) {
			Vector2D p = ring[i], q = ring[(i + 1) % count];
			Vector2D along = q - p;
			double len = along.Len();
			if (len < 1e-6) continue;

			Vector2D face((float)(along.Y() / len * sense), (float)(-along.X() / len * sense));
			Vector2D off = reach(face);
			add({p, q, q + off, p + off});

			// And the pen at the corner, where the two bands leave a wedge open. Only where the
			// ring turns away from the ink: turning the other way the bands overlap instead.
			Vector2D next = ring[(i + 2) % count] - q;
			double after = next.Len();
			if (after < 1e-6) continue;
			double turn = ((double)along.X() * next.Y() - (double)along.Y() * next.X()) /
				(len * after);
			// Away from the ink, and far enough round to leave a gap worth filling: the wedge a
			// corner opens is the pen times the angle, so below a fifth of a degree it is thinner
			// than the outline was drawn to begin with. On a smooth curve that is most corners.
			if (turn * sense <= 3e-3) continue;

			Vector2D face2((float)(next.Y() / after * sense), (float)(-next.X() / after * sense));
			Vector2D off2 = reach(face2);
			double from = std::atan2(off.Y() / ry, off.X() / rx);
			double to = std::atan2(off2.Y() / ry, off2.X() / rx);
			double sweep = to - from;
			while (sweep > pi) sweep -= 2 * pi;
			while (sweep < -pi) sweep += 2 * pi;

			// As many steps as the pen's own size asks for: what shows is the sagitta, and a tenth
			// of the pen is already below what anyone can see. A wide pen still gets its steps; a
			// narrow one no longer pays for four of them.
			double fine = std::acos(std::max(1.0 - 0.2 / std::max(std::max(rx, ry), 1e-3), -1.0)) * 2;
			int steps = std::max((int)std::ceil(std::abs(sweep) / std::max(fine, pi / 12)), 1);
			Contour wedge;
			wedge.push_back(q);
			for (int step = 0; step <= steps; ++step) {
				double angle = from + sweep * step / steps;
				wedge.push_back(q + Vector2D((float)(rx * std::cos(angle)),
				                             (float)(ry * std::sin(angle))));
			}
			add(std::move(wedge));
		}
	}
	return out;
}

/// Rings, written the way a drawing writes them.
std::string DrawContours(std::vector<Contour> const& rings) {
	std::string out;
	for (auto const& ring : rings) {
		if (ring.size() < 3) continue;
		if (!out.empty()) out += ' ';
		out += "m " + FormatNumber(ring[0].X()) + " " + FormatNumber(ring[0].Y()) + " l";
		for (size_t i = 1; i < ring.size(); ++i) {
			out += ' ';
			out += FormatNumber(ring[i].X());
			out += ' ';
			out += FormatNumber(ring[i].Y());
		}
	}
	return out;
}

#ifdef __WXMSW__

/// The weight and the style bits of the font selected into the device, read out of its own OS/2
/// table. GDI answers questions about weight with the weight that was asked for, so the only way
/// to know whether it found a bold face or is making one up is to look in the font itself.
bool ReadFontStyle(HDC dc, unsigned short& weight, unsigned short& selection) {
	// The two fields wanted, at their places in the table, and big-endian as everything in a
	// font file is.
	unsigned char header[80]{};
	DWORD got = GetFontData(dc, 0x322F534F /* 'OS/2' */, 0, header, sizeof(header));
	if (got == GDI_ERROR || got < 64) return false;
	auto number = [&](size_t at) {
		return (unsigned short)((header[at] << 8) | header[at + 1]);
	};
	weight = number(4);
	selection = number(62);
	return true;
}

/// Whether the face selected into the device keeps its outlines as PostScript curves rather than
/// TrueType ones. It decides who slants a made-up italic: libass matches GDI for a TrueType face and
/// uses tan(10 degrees) for a PostScript one, which is nearly half as much.
bool FontIsPostScript(HDC dc) {
	return GetFontData(dc, 0x20464643 /* 'CFF ' */, 0, nullptr, 0) != GDI_ERROR;
}

/// What the renderers make of that weight. Small numbers are the old one-to-nine scale, and a
/// zero says the font did not answer, in which case the style bit is all there is to go on.
int FaceWeight(unsigned short weight, bool bold_bit) {
	switch (weight) {
		case 0: return bold_bit ? 700 : 400;
		case 1: return 100;
		case 2: return 200;
		case 3: return 300;
		case 4: return 350;
		case 5: return 400;
		case 6: return 600;
		case 7: return 700;
		case 8: return 800;
		case 9: return 900;
		default: return weight;
	}
}

/// The path GDI gives back, as closed rings of points. `units` is what its integers have to be
/// multiplied by to become the units the rings are wanted in.
std::vector<Contour> FlattenPath(std::vector<POINT> const& points, std::vector<BYTE> const& types,
                                 double units, double fine) {
	std::vector<Contour> out;
	Contour ring;
	Vector2D at;
	auto read = [&](size_t i) {
		return Vector2D((float)(points[i].x * units), (float)(points[i].y * units));
	};
	auto finish = [&]() {
		// A point that says the same as the one before it says nothing about which way an edge
		// faces, and the ring closes on its own, so a last one repeating the first goes as well.
		//
		// The same for one that says nearly the same: a font draws finer than a border can show, and
		// every point here costs a band and a wedge - and costs them again for every mapping a drag
		// applies. Only where the line through the point is nearly straight, so that dropping it
		// moves the outline by less than it is drawn to.
		for (size_t i = ring.size(); i-- > 1;) {
			Vector2D step = ring[i] - ring[i - 1];
			if (step.Len() < 1e-4) { ring.erase(ring.begin() + i); continue; }
			if (step.Len() > .2f || i + 1 >= ring.size()) continue;
			Vector2D onward = ring[i + 1] - ring[i];
			double along = onward.Len();
			if (along < 1e-6) continue;
			double turn = std::abs((double)step.X() * onward.Y() - (double)step.Y() * onward.X()) /
				(step.Len() * along);
			if (turn >= .05) continue;
			ring.erase(ring.begin() + i);
			// The neighbour stays, whatever it looks like: a run of these would move the outline by
			// the sum of the steps rather than by one of them.
			if (i > 1) --i;
		}
		while (ring.size() > 2 && (ring.back() - ring.front()).Len() < 1e-4) ring.pop_back();
		if (ring.size() > 2) out.push_back(ring);
		ring.clear();
	};
	for (size_t i = 0; i < points.size() && i < types.size(); ++i) {
		switch (types[i] & ~PT_CLOSEFIGURE) {
		case PT_MOVETO:
			finish();
			at = read(i);
			ring.push_back(at);
			break;
		case PT_LINETO:
			at = read(i);
			ring.push_back(at);
			break;
		case PT_BEZIERTO:
			if (i + 2 >= points.size()) break;
			AddCurve(ring, at, read(i), read(i + 1), read(i + 2), fine);
			at = read(i + 2);
			i += 2;
			break;
		default: break;
		}
	}
	finish();
	return out;
}

/// Measure every piece, lay them out into rows, and turn each one into a drawing.
///
/// All of them end up in the same frame: the origin is the point the line's alignment
/// anchored, so every piece can be written with the line's own \pos and they still read as
/// one line. Returns false when nothing could be drawn.
bool BuildPieces(std::vector<Piece>& pieces, int alignment,
                 double& box_width, double& box_height, Outlines want = Outlines::Baked) {
	if (pieces.empty()) return false;

	HDC screen = GetDC(nullptr);
	HDC dc = CreateCompatibleDC(screen);
	ReleaseDC(nullptr, screen);
	if (!dc) return false;

	SetBkMode(dc, TRANSPARENT);
	// The top of the cell, not the baseline, so that a piece's own (0,0) is its top left.
	SetTextAlign(dc, TA_LEFT | TA_TOP);
	// Left at zero: the letter spacing goes into the advances below, and having GDI add it
	// as well would count it twice.
	SetTextCharacterExtra(dc, 0);

	/// Where a bar goes under or through a letter, in the font's own units at the size asked for.
	/// Both taken from the font, as the renderers take them, and both nothing at all when the font
	/// does not say.
	struct Decoration {
		double top = 0, bottom = 0;
		bool wanted = false;
	};

	struct Prepared {
		HFONT font = nullptr;
		/// The same font without the bold the face itself does not have. GDI makes one up, and its
		/// made-up bold walks the letters further apart than the real face would; the renderers
		/// thicken the outline and leave the advances alone. So the letters are measured and drawn
		/// with this one where GDI was inventing.
		///
		/// The italic stays, made up or not: the renderers make one up too, by slanting the outline,
		/// so taking it off would stand the letters back up - and a slant changes no advance, so
		/// there is nothing to correct for either.
		HFONT plain = nullptr;
		/// How far the outline has to be slanted by hand, and where the baseline it slants about is,
		/// in the font's own units. Both nought unless the italic was taken off GDI above.
		double slant = 0;
		double baseline = 0;
		std::vector<INT> advances;
		/// The same advances without the letter spacing, which is the width of a letter's own bar.
		std::vector<INT> bars;
		Decoration underline, strikeout;
		double dx = 1, dy = 1;
	};
	std::vector<Prepared> prepared(pieces.size());
	bool ok = true;

	for (size_t at = 0; at < pieces.size() && ok; ++at) {
		auto& piece = pieces[at];
		auto& ready = prepared[at];

		LOGFONTW lf{};
		// Positive, so it is the cell height rather than the character height: the
		// renderers size a font by its cell, and asking for a character height gives a
		// noticeably bigger one.
		lf.lfHeight = static_cast<LONG>(std::lround(piece.style.size * UPSCALE));
		lf.lfWeight = piece.style.bold ? FW_BOLD : FW_NORMAL;
		lf.lfItalic = piece.style.italic ? TRUE : FALSE;
		// Not asked for here: GDI would run one unbroken line under the whole stretch, spacing
		// included, where the renderers lay a bar under each letter in the width of its own
		// advance. The bars are put on further down.
		lf.lfUnderline = FALSE;
		lf.lfStrikeOut = FALSE;
		lf.lfCharSet = DEFAULT_CHARSET;
		// The path can only be read back from an outline font, so ask for one.
		lf.lfOutPrecision = OUT_TT_PRECIS;
		wcsncpy_s(lf.lfFaceName, wxString::FromUTF8(piece.style.face).wc_str(),
			LF_FACESIZE - 1);

		ready.font = CreateFontIndirectW(&lf);
		if (!ready.font) { ok = false; break; }

		HFONT previous = (HFONT)SelectObject(dc, ready.font);

		// What GDI really found. It reports back the weight that was asked for rather than the
		// weight it found, so the font file is asked instead - and asked twice, because the answer
		// to "is this bold invented" is not in one face's table but in the difference between two.
		unsigned short weight = 0, selection = 0;
		bool read = ReadFontStyle(dc, weight, selection);
		int asked_weight = FaceWeight(weight, (selection & 0x20) != 0);
		bool postscript = FontIsPostScript(dc);

		bool drop_bold = false;
		if (piece.style.bold && read) {
			// The same request without the bold. If it lands on a face that says the same about
			// itself, then the family has no bolder member and what GDI is showing is its own
			// invention - either on a face that is already bold enough (a family whose own name is
			// the bold one), or on one that is not. Both are wrong here: GDI's invented bold walks
			// the letters apart, which no renderer does.
			LOGFONTW upright = lf;
			upright.lfWeight = FW_NORMAL;
			if (HFONT probe = CreateFontIndirectW(&upright)) {
				SelectObject(dc, probe);
				unsigned short probe_weight = 0, probe_selection = 0;
				int found = ReadFontStyle(dc, probe_weight, probe_selection) ?
					FaceWeight(probe_weight, (probe_selection & 0x20) != 0) : asked_weight;
				SelectObject(dc, ready.font);
				DeleteObject(probe);
				drop_bold = found == asked_weight;
			}
		}

		// An italic is made up by everyone, renderers included, and by slanting the outline rather
		// than by touching the advances. libass says in its own source that its slant matches GDI's
		// - but the matrix it says that about is the TrueType one. A PostScript face is slanted by
		// tan(10 degrees) instead, where GDI uses tan(18.77), so there the slant is taken off GDI
		// and put back by hand; otherwise GDI's is exactly right and is kept.
		bool drop_italic = piece.style.italic && read && !(selection & 0x01) && postscript;

		if (drop_bold || drop_italic) {
			LOGFONTW plain = lf;
			if (drop_bold) plain.lfWeight = FW_NORMAL;
			if (drop_italic) plain.lfItalic = FALSE;
			ready.plain = CreateFontIndirectW(&plain);
			if (ready.plain && drop_italic) ready.slant = 0.17632698070846498;
		}

		// Everything measured below comes from the face as it really is.
		if (ready.plain) SelectObject(dc, ready.plain);
		ready.dx = piece.style.scale_x / 100.0 / UPSCALE;
		ready.dy = piece.style.scale_y / 100.0 / UPSCALE;

		TEXTMETRICW metrics{};
		GetTextMetricsW(dc, &metrics);
		// FreeType slants about the origin, which is the baseline; the outline here is measured from
		// the top of the cell, so the baseline is the ascent down from it.
		ready.baseline = metrics.tmAscent;
		piece.ascent = metrics.tmAscent * ready.dy;
		piece.descent = metrics.tmDescent * ready.dy;

		// Where the font puts a bar, and how thick. The renderers centre it on the place the font
		// names and only draw it where the font names one at all, so the same is asked here. The
		// outline is measured from the top of the cell, so the baseline is the ascent down.
		if (piece.style.underline || piece.style.strikeout) {
			UINT wanted = GetOutlineTextMetricsW(dc, 0, nullptr);
			std::vector<char> room(wanted ? wanted : sizeof(OUTLINETEXTMETRICW));
			auto *outline = reinterpret_cast<OUTLINETEXTMETRICW *>(room.data());
			outline->otmSize = (UINT)room.size();
			if (wanted && GetOutlineTextMetricsW(dc, (UINT)room.size(), outline)) {
				auto place = [&](double from_baseline, double thickness) {
					Decoration out;
					if (thickness <= 0) return out;
					double middle = metrics.tmAscent - from_baseline;
					out.top = middle - thickness / 2;
					out.bottom = out.top + thickness;
					out.wanted = true;
					return out;
				};
				if (piece.style.underline && outline->otmsUnderscorePosition <= 0)
					ready.underline = place(outline->otmsUnderscorePosition,
						outline->otmsUnderscoreSize);
				if (piece.style.strikeout)
					ready.strikeout = place(outline->otmsStrikeoutPosition,
						outline->otmsStrikeoutSize);
			}
		}

		// One advance per character, the way the renderers add them up. Asking GDI for the
		// extent of a whole run instead lets it adjust the run, and that difference grows
		// along the row.
		LONG spacing = static_cast<LONG>(std::lround(piece.style.spacing * UPSCALE));
		LONG width = 0;
		for (size_t i = 0; i < piece.text.size(); ++i) {
			SIZE extent{};
			GetTextExtentPoint32W(dc, &piece.text[i], 1, &extent);
			ready.advances.push_back(extent.cx + spacing);
			ready.bars.push_back(extent.cx);
			width += extent.cx + spacing;
		}
		piece.width = width * ready.dx;

		SelectObject(dc, previous);
	}

	auto clean_up = [&]() {
		for (auto& ready : prepared) {
			if (ready.font) DeleteObject(ready.font);
			if (ready.plain) DeleteObject(ready.plain);
		}
		DeleteDC(dc);
	};

	if (!ok) { clean_up(); return false; }

	// ------------------------------------------------------------------- the layout
	size_t rows = pieces.back().row + 1;
	std::vector<double> row_width(rows, 0), row_ascent(rows, 0), row_descent(rows, 0);
	std::vector<bool> row_blank(rows, true), row_drawn(rows, false);
	for (auto const& piece : pieces) {
		row_width[piece.row] += piece.width;
		if (piece.had_char) row_blank[piece.row] = false;
		if (!piece.text.empty()) row_drawn[piece.row] = true;
	}

	// How tall a row is comes from what is drawn on it. A stretch with nothing left in it is
	// not on the row at all as far as the renderers are concerned, so a size written just
	// before a break, or at the very end of a line, must not stretch the row it stands on -
	// that used to push every row below it down. A row with nothing drawn on it has only what
	// its own stretches say, so there it is those that count.
	for (auto const& piece : pieces) {
		if (row_drawn[piece.row] && piece.text.empty()) continue;
		row_ascent[piece.row] = std::max(row_ascent[piece.row], piece.ascent);
		row_descent[piece.row] = std::max(row_descent[piece.row], piece.descent);
	}

	std::vector<double> row_top(rows, 0);
	double cursor = 0;
	for (size_t row = 0; row < rows; ++row) {
		row_top[row] = cursor;
		// A row with nothing on it at all is worth half a line, not a whole one - that is what
		// the renderers do, and counting it whole opens twice the gap it should. A row of
		// spaces is not that row: nothing shows, but it is as tall as its font.
		cursor += (row_blank[row] ? .5 : 1.) * (row_ascent[row] + row_descent[row]);
	}
	box_height = cursor;
	box_width = 0;
	for (double width : row_width) box_width = std::max(box_width, width);

	int horizontal = (alignment - 1) % 3;
	int vertical = (alignment - 1) / 3;
	double tx = horizontal == 0 ? 0. : horizontal == 1 ? .5 : 1.;
	// ASS counts the rows from the bottom: 7-9 are the top ones.
	double ty = vertical == 2 ? 0. : vertical == 1 ? .5 : 1.;
	double shift_x = -box_width * tx;
	double shift_y = -box_height * ty;

	std::vector<double> row_left(rows, 0), pen(rows, 0);
	for (size_t row = 0; row < rows; ++row) {
		row_left[row] = (box_width - row_width[row]) * tx;
		pen[row] = row_left[row];
	}
	for (auto& piece : pieces) {
		piece.x = pen[piece.row];
		pen[piece.row] += piece.width;
		// The rows of a line share a baseline, so a piece in a smaller font sits lower.
		piece.top = row_top[piece.row] + row_ascent[piece.row] - piece.ascent;
	}

	// Measured and laid out, which is all a caller that only wants to know where the pieces
	// sit needs. The outlines are the expensive half.
	if (want == Outlines::None) { clean_up(); return true; }

	// ----------------------------------------------------------------- the outlines
	bool drew_anything = false;
	for (size_t at = 0; at < pieces.size(); ++at) {
		auto& piece = pieces[at];
		auto& ready = prepared[at];
		if (piece.text.empty()) continue;

		// Drawn with the face as it really is, where GDI was making a bold up. Its made-up bold both
		// walks the letters apart and thickens them a great deal; what the renderers do is push the
		// outline out by a sixty-fourth of the em - about a pixel and a half at a size of a hundred
		// and twenty, and nothing at all beside GDI's. So the real face is far the closer of the
		// two, and it is the one whose letters sit where they should.
		HFONT previous = (HFONT)SelectObject(dc, ready.plain ? ready.plain : ready.font);
		BeginPath(dc);
		ExtTextOutW(dc, 0, 0, 0, nullptr, piece.text.c_str(), (UINT)piece.text.size(),
			ready.advances.data());
		EndPath(dc);

		int count = GetPath(dc, nullptr, nullptr, 0);
		std::vector<POINT> points;
		std::vector<BYTE> types;
		if (count > 0) {
			points.resize(count);
			types.resize(count);
			if (GetPath(dc, points.data(), types.data(), count) != count) count = 0;
		}
		SelectObject(dc, previous);
		if (count <= 0) continue;

		// Everything the renderer would have done to this piece after the scale. The two
		// shears are measured from the piece's own top left corner - its own ascent line and
		// its own first letter - not from the corner of the row it sits on: the renderer
		// leans each stretch about its own ascent line and starts the sideways lean afresh
		// wherever anything about the text changes. Which is why a word in another size slides
		// out of a leaning row in the first place, and why it has to be put back that way.
		AssTransform transform = BuildTransform(piece.style,
			-shift_y - piece.top, -shift_x - piece.x);

		std::string drawing;
		char command = 0;
		auto append = [&](char wanted, double x, double y) {
			if (command != wanted) {
				if (!drawing.empty()) drawing += ' ';
				drawing += wanted;
				command = wanted;
			}
			Vector2D point = transform.Apply(x + piece.x + shift_x, y + piece.top + shift_y);
			drawing += ' ';
			drawing += FormatNumber(point.X());
			drawing += ' ';
			drawing += FormatNumber(point.Y());
		};

		// Which way round the letters are wound, so that a bar can be laid the same way and add
		// to them rather than cut a gap where it crosses one.
		double turned = 0;
		for (int i = 1; i < count; ++i) {
			if ((types[i] & ~PT_CLOSEFIGURE) == PT_MOVETO) continue;
			turned += (double)points[i - 1].x * points[i].y -
			          (double)points[i].x * points[i - 1].y;
		}

		double sense = turned >= 0 ? 1. : -1.;

		if (want == Outlines::Loose) {
			// The piece's own corner is the origin here, and the units are the ones the line's
			// \fscx and \fscy still act on: what is written is what the renderer would have been
			// given, so the piece's own tags put it back exactly - lean, turn and all. A drawing
			// is leaned about its own y of nought and turned about \org, and the piece's corner is
			// its own ascent line and its own first letter, which is what the renderer leans the
			// letters about. So the two frames are the same one.
			double units = 1.0 / UPSCALE;
			auto rings = FlattenPath(points, types, units, .75);

			// The slant GDI was not allowed to make up, put back about the baseline.
			double baseline = ready.baseline * units;
			auto lean = [&](Vector2D point) {
				if (ready.slant == 0) return point;
				return Vector2D((float)(point.X() + ready.slant * (baseline - point.Y())),
				                point.Y());
			};
			if (ready.slant != 0)
				for (auto& ring : rings)
					for (auto& point : ring) point = lean(point);

			// The bars under and through the letters belong to the letters, so they are widened
			// with them.
			if (ready.underline.wanted || ready.strikeout.wanted) {
				LONG walked = 0;
				for (size_t i = 0; i < ready.bars.size() && i < ready.advances.size(); ++i) {
					LONG bar = ready.bars[i];
					for (auto const& deco : {ready.underline, ready.strikeout}) {
						if (!deco.wanted || bar <= 0) continue;
						float left = (float)(walked * units), right = (float)((walked + bar) * units);
						float top = (float)(deco.top * units), bottom = (float)(deco.bottom * units);
						Contour rect{lean(Vector2D(left, top)), lean(Vector2D(right, top)),
						             lean(Vector2D(right, bottom)), lean(Vector2D(left, bottom))};
						WindRing(rect, sense);
						rings.push_back(std::move(rect));
					}
					walked += ready.advances[i];
				}
			}

			piece.loose = DrawContours(rings);

			// The pen the renderer would have stroked with, in this frame: it divides the border by
			// the scale before stroking, which is why a border comes out the width that was asked
			// for however much the letters were scaled.
			double pen_x = piece.style.border.X() /
				std::max(std::abs(piece.style.scale_x) / 100., 1e-6);
			double pen_y = piece.style.border.Y() /
				std::max(std::abs(piece.style.scale_y) / 100., 1e-6);
			auto wide = Widen(rings, pen_x, pen_y, sense);
			// The letters are part of their own border: what the pen adds is a band around them,
			// and the middle has to stay solid or the seam would show along every edge.
			for (auto const& letter : rings) wide.push_back(letter);
			piece.wide = DrawContours(wide);

			if (!piece.loose.empty()) drew_anything = true;
			continue;
		}

		// The slant GDI was not allowed to make up, put back about the baseline - in the font's own
		// units, before the scale, which is where the renderers apply it too.
		auto lean = [&](double px, double py) {
			return ready.slant == 0 ? px : px + ready.slant * (ready.baseline - py);
		};

		for (int i = 0; i < count; ++i) {
			double x = lean(points[i].x, points[i].y) * ready.dx, y = points[i].y * ready.dy;
			switch (types[i] & ~PT_CLOSEFIGURE) {
			case PT_MOVETO: append('m', x, y); break;
			case PT_LINETO: append('l', x, y); break;
			case PT_BEZIERTO: append('b', x, y); break;
			default: break;
			}
		}

		// And a bar under, or through, each letter in the width of its own advance - the spacing
		// left out, which is what leaves the gaps the renderers show.
		if (ready.underline.wanted || ready.strikeout.wanted) {
			LONG pen = 0;
			for (size_t i = 0; i < ready.bars.size() && i < ready.advances.size(); ++i) {
				LONG bar = ready.bars[i];
				for (auto const& deco : {ready.underline, ready.strikeout}) {
					if (!deco.wanted || bar <= 0) continue;
					// Each corner leans by how far it is from the baseline, so a bar under leaning
					// letters leans with them rather than standing square under them.
					double left_top = lean(pen, deco.top) * ready.dx;
					double right_top = lean(pen + bar, deco.top) * ready.dx;
					double left_low = lean(pen, deco.bottom) * ready.dx;
					double right_low = lean(pen + bar, deco.bottom) * ready.dx;
					double top = deco.top * ready.dy, bottom = deco.bottom * ready.dy;
					if (turned >= 0) {
						append('m', left_top, top);
						append('l', right_top, top);
						append('l', right_low, bottom);
						append('l', left_low, bottom);
					}
					else {
						append('m', left_top, top);
						append('l', left_low, bottom);
						append('l', right_low, bottom);
						append('l', right_top, top);
					}
				}
				pen += ready.advances[i];
			}
		}

		if (!drawing.empty()) {
			piece.drawing = std::move(drawing);
			drew_anything = true;
		}
	}

	clean_up();
	return drew_anything;
}

#else

bool BuildPieces(std::vector<Piece>&, int, double&, double&, Outlines = Outlines::Baked) {
	return false;
}

#endif

} // namespace

bool Available() {
#ifdef __WXMSW__
	return true;
#else
	return false;
#endif
}

std::string WhyNot(const agi::Context *c, AssDialogue *line) {
	if (!Available())
		return from_wx(_("Converting text to shapes is only implemented on Windows so far."));

	auto blocks = line->ParseTags();
	for (auto& block : blocks)
		if (block->GetType() == AssBlockType::DRAWING)
			return from_wx(_("The line already mixes text with a drawing."));

	if (FindTag(blocks, "\\k") || FindTag(blocks, "\\K") || FindTag(blocks, "\\kf") ||
		FindTag(blocks, "\\ko"))
		return from_wx(_("The line is karaoke timed, and the syllables would be lost."));

	return {};
}

Placement WherePlaced(const agi::Context *c, AssDialogue *line) {
	Placement out;
	auto blocks = line->ParseTags();

	auto point = [](param_vec tag, size_t at) {
		return Vector2D((*tag)[at].Get<float>(0.f), (*tag)[at + 1].Get<float>(0.f));
	};

	if (auto tag = FindTag(blocks, "\\pos")) {
		if (tag->size() >= 2 && !(*tag)[0].omitted && !(*tag)[1].omitted) {
			out.at = out.first = out.second = point(tag, 0);
			out.told = true;
			return out;
		}
	}

	auto tag = FindTag(blocks, "\\move");
	if (!tag || tag->size() < 4) return out;
	for (size_t at = 0; at < 4; ++at)
		if ((*tag)[at].omitted) return out;

	out.first = point(tag, 0);
	out.second = point(tag, 2);
	out.moving = out.told = true;
	// Without times of its own the run takes the whole line, which is what the renderers do.
	out.to = line->End - line->Start;
	if (tag->size() >= 6 && !(*tag)[4].omitted && !(*tag)[5].omitted) {
		out.from = (*tag)[4].Get<int>(0);
		out.to = (*tag)[5].Get<int>(out.to);
		out.timed = true;
	}

	// How far along that run the frame on screen is. Without a video there is no frame to ask
	// about, so it is read where the run begins.
	int now = 0;
	if (c->project->Timecodes().IsLoaded())
		now = c->videoController->TimeAtFrame(c->videoController->GetFrameN()) - line->Start;
	double part = out.to > out.from ?
		std::min(std::max((double)(now - out.from) / (out.to - out.from), 0.), 1.) :
		(now >= out.to ? 1. : 0.);
	out.at = out.first + (out.second - out.first) * (float)part;
	return out;
}

std::pair<std::string, std::string> PlacementOverride(Placement const& placed,
                                                      Vector2D first, Vector2D second) {
	if (!placed.moving)
		return {"\\pos", agi::format("(%s,%s)",
			FormatNumber(first.X()), FormatNumber(first.Y()))};

	std::string value = agi::format("(%s,%s,%s,%s",
		FormatNumber(first.X()), FormatNumber(first.Y()),
		FormatNumber(second.X()), FormatNumber(second.Y()));
	// The times only if the line named them: without them the run takes the whole line, and saying
	// them would be saying something the line never said.
	if (placed.timed) value += agi::format(",%d,%d", placed.from, placed.to);
	value += ")";
	return {"\\move", value};
}

std::string PlacementTag(Placement const& placed, Vector2D first, Vector2D second) {
	auto [name, value] = PlacementOverride(placed, first, second);
	return name + value;
}

DecorPaint ReadDecorations(const agi::Context *c, AssDialogue *line) {
	DecorPaint out;
	auto blocks = line->ParseTags();

	AssStyle const fallback;
	AssStyle const *behind = c->ass->GetStyle(line->Style);
	if (!behind) behind = &fallback;

	auto colour_of = [&](const char *name, agi::Color unless) {
		if (auto found = FindTag(blocks, name)) return (*found)[0].Get<agi::Color>(unless);
		return unless;
	};
	auto alpha_of = [&](const char *name, unsigned char unless) {
		for (auto candidate : {name, "\\alpha"}) {
			if (auto found = FindTag(blocks, candidate)) {
				std::string said = (*found)[0].Get<std::string>(std::string());
				if (!said.empty()) return said;
			}
		}
		return agi::format("&H%02X&", (int)unless);
	};
	// The same thing as a number, for comparing rather than for writing: an alpha off a tag is
	// written however the line felt like writing it, and two spellings of one byte are one byte.
	auto alpha_value = [&](const char *name, unsigned char unless) {
		std::string said = alpha_of(name, unless);
		unsigned value = 0;
		size_t at = said.find_first_of("0123456789abcdefABCDEF");
		if (at == std::string::npos) return unless;
		for (; at < said.size(); ++at) {
			int digit = said[at];
			if (digit >= '0' && digit <= '9') value = value * 16 + (digit - '0');
			else if (digit >= 'a' && digit <= 'f') value = value * 16 + (digit - 'a' + 10);
			else if (digit >= 'A' && digit <= 'F') value = value * 16 + (digit - 'A' + 10);
			else break;
		}
		return (unsigned char)(value & 0xFF);
	};

	auto style = GatherStyle(c, line);
	out.pen = style.border;
	out.offset = style.shadow;

	agi::Color border_colour = colour_of("\\3c", behind->outline);
	out.border = "\\1c" + border_colour.GetAssOverrideFormatted() +
		"\\1a" + alpha_of("\\3a", behind->outline.a);
	out.shadow = "\\1c" + colour_of("\\4c", behind->shadow).GetAssOverrideFormatted() +
		"\\1a" + alpha_of("\\4a", behind->shadow.a);

	// A widened shape holds the letters inside it - that is what keeps its middle solid - so when it
	// is painted the colour the letters are painted, the letters have nothing left to add.
	//
	// Two things make that comparison less obvious than it looks. The fill has two spellings and
	// most lines use the shorter one, so both are asked for: \1c is what \c means. And either colour
	// can come from the style rather than from the line - in which case it arrives carrying the
	// style's own alpha byte, where a colour read off a tag carries none. So only the three colour
	// channels are compared, and how see-through each of them is is asked separately.
	agi::Color fill = colour_of("\\1c", colour_of("\\c", behind->primary));
	out.covers = fill.r == border_colour.r && fill.g == border_colour.g &&
		fill.b == border_colour.b &&
		alpha_value("\\1a", behind->primary.a) == alpha_value("\\3a", behind->outline.a);
	return out;
}

std::vector<std::vector<Vector2D>> WidenRings(std::vector<std::vector<Vector2D>> const& rings,
                                             double rx, double ry) {
	// Which way round the shape is wound, taken from the whole of it: the outer rings decide it, and
	// they enclose more than the holes do. It is one sign for all of them, holes included, which is
	// what leaves the ink on the same side of every ring.
	double area = 0;
	for (auto const& ring : rings)
		for (size_t i = 0; i < ring.size(); ++i) {
			auto const& p = ring[i];
			auto const& q = ring[(i + 1) % ring.size()];
			area += (double)p.X() * q.Y() - (double)q.X() * p.Y();
		}
	return Widen(rings, rx, ry, area >= 0 ? 1. : -1.);
}

std::vector<Converted> ConvertLines(const agi::Context *c,
		std::vector<AssDialogue *> const& lines, std::vector<std::string>& refusals) {
	std::vector<Converted> converted;

	for (auto line : lines) {
		auto blocks = line->ParseTags();

		bool has_text = false;
		for (auto& block : blocks)
			if (block->GetType() == AssBlockType::PLAIN &&
				block->GetText().find_first_not_of(" \t") != std::string::npos)
				has_text = true;
		if (!has_text) continue;

		std::string why = WhyNot(c, line);
		if (!why.empty()) {
			if (std::find(refusals.begin(), refusals.end(), why) == refusals.end())
				refusals.push_back(why);
			continue;
		}

		auto style = GatherStyle(c, line);
		// Where the line is drawn: the rotations turn about \org, and \org is given
		// relative to nothing - so the two have to be compared in the same frame. Without
		// a \pos the margins and the alignment say where it is, the same way they do for
		// the visual tools.
		// A line on a run is read where the run has got to on the frame on screen, and the shape it
		// becomes stands there: the turn goes into its numbers, so being carried along the run would
		// turn it as well - which is not what was on screen at any frame of it.
		auto placed = WherePlaced(c, line);
		style.position = placed.told ? placed.at : LayoutPosition(c, line, style.alignment);

		// One piece per stretch of text with its own tags. They are measured together, so
		// they share the rows and the baselines and end up in one frame.
		auto pieces = SplitPieces(c, line, style.alignment);
		// Where the line sits is a property of the line, not of a piece, and the rotations
		// need it to know how far \org is from it.
		for (auto& piece : pieces) piece.style.position = style.position;

		double box_width = 0, box_height = 0;
		if (!BuildPieces(pieces, style.alignment, box_width, box_height)) {
			std::string message = from_wx(_("The outline of the text could not be read "
				"from the font."));
			if (std::find(refusals.begin(), refusals.end(), message) == refusals.end())
				refusals.push_back(message);
			continue;
		}

		// Wide text is broken into rows by the renderer, and where it breaks depends on
		// its own measurements rather than these - so a row that would not have fitted is
		// refused instead of being turned into a shape that runs off the screen.
		double room = LayoutWidth(c, line);
		if (room > 0 && box_width > room + 1) {
			std::string message = from_wx(_("The text is wider than the margins allow, so "
				"the renderer would break it into rows this cannot work out yet."));
			if (std::find(refusals.begin(), refusals.end(), message) == refusals.end())
				refusals.push_back(message);
			continue;
		}

		Vector2D position = style.position;

		// Every piece is written with the line's own \pos, because they were all measured
		// from the point the line's alignment anchored - so \an7 puts each of them back
		// exactly where its text was.
		//
		// \clip and \iclip are deliberately left on: they are given in script coordinates
		// and mean the same thing to a drawing as to text. What the outline swallowed is
		// gone instead, the scale included - it is multiplied into the coordinates, so the
		// tags have to be neutralised or it would apply twice. What still applies to a
		// drawing the way it applied to the text - colours, borders, fades, transforms - is
		// carried over untouched.
		// The border and the shadow are all that is left of the transform once the letters have
		// none of it, so what it did to them is written out - each after the tags the line
		// carried, so it is these that have the last word.
		std::string decorated;
		{
			auto worked = TransformDecorations(style, style.shadow, style.border);
			if ((worked.shadow - style.shadow).Len() > .01f)
				decorated += agi::format("\\xshad%s\\yshad%s",
					FormatNumber(worked.shadow.X()), FormatNumber(worked.shadow.Y()));
			if ((worked.border - style.border).Len() > .01f)
				decorated += agi::format("\\xbord%s\\ybord%s",
					FormatNumber(worked.border.X()), FormatNumber(worked.border.Y()));
		}

		for (auto const& piece : pieces) {
			if (piece.drawing.empty()) continue;
			converted.push_back({line, agi::format(
				"{%s%s\\an7\\pos(%s,%s)\\fscx100\\fscy100\\frz0\\p1}%s{\\p0}",
				piece.tags, decorated, FormatNumber(position.X()), FormatNumber(position.Y()),
				piece.drawing)});
		}
	}

	return converted;
}

std::vector<Converted> ConvertSelection(const agi::Context *c,
		std::vector<std::string>& refusals) {
	return ConvertLines(c, c->selectionController->GetSortedSelection(), refusals);
}

std::vector<Decoration> BakeDecorations(const agi::Context *c, AssDialogue *line) {
	auto blocks = line->ParseTags();

	// A drawing has no border to speak of that this could stand in for, and a line whose border is
	// animated would have to be worked out afresh for every frame it is on screen. A line that
	// moves is no trouble: the shapes are what the letters are, and they travel with them.
	for (auto& block : blocks)
		if (block->GetType() == AssBlockType::DRAWING) return {};
	if (FindTag(blocks, "\\t")) return {};

	bool has_text = false;
	for (auto& block : blocks)
		if (block->GetType() == AssBlockType::PLAIN &&
			block->GetText().find_first_not_of(" \t") != std::string::npos)
			has_text = true;
	if (!has_text) return {};

	auto style = GatherStyle(c, line);
	bool wants_border = style.border.Len() > .01f;
	bool wants_shadow = style.shadow.Len() > .01f;
	if (!wants_border && !wants_shadow) return {};

	auto placed = WherePlaced(c, line);
	if (!placed.told)
		placed.at = placed.first = placed.second = LayoutPosition(c, line, style.alignment);
	Vector2D position = placed.at;
	style.position = position;

	auto pieces = SplitPieces(c, line, style.alignment);
	for (auto& piece : pieces) piece.style.position = position;

	double box_width = 0, box_height = 0;
	if (!BuildPieces(pieces, style.alignment, box_width, box_height, Outlines::Loose)) return {};

	// Wide text is broken into rows by the renderer, and where it breaks depends on its own
	// measurements rather than these - so the rows worked out here would not be the rows on
	// screen, and every shape would stand somewhere the letters are not.
	double room = LayoutWidth(c, line);
	if (room > 0 && box_width > room + 1) return {};

	// What the two of them are painted with, and whether the letters are already inside the border.
	auto paint = ReadDecorations(c, line);
	std::string border_paint = paint.border, shadow_paint = paint.shadow;
	bool covers = wants_border && paint.covers;

	int horizontal = (style.alignment - 1) % 3;
	int vertical = (style.alignment - 1) / 3;
	double tx = horizontal == 0 ? 0. : horizontal == 1 ? .5 : 1.;
	// ASS counts the rows from the bottom: 7-9 are the top ones.
	double ty = vertical == 2 ? 0. : vertical == 1 ? .5 : 1.;
	double shift_x = -box_width * tx;
	double shift_y = -box_height * ty;

	std::vector<Decoration> out;
	for (auto const& piece : pieces) {
		if (piece.loose.empty()) continue;
		Decoration made;
		// Where the stretch begins, which is the one point of it that its own lean leaves alone:
		// the renderer leans a stretch about its own ascent line and begins the sideways lean
		// afresh at its own first letter. The same point the split hangs a piece from.
		made.lean = Vector2D((float)(piece.x + shift_x), (float)(piece.top + shift_y));
		made.letters = piece.loose;
		made.bordered = piece.wide;
		made.border_paint = border_paint;
		made.shadow_paint = shadow_paint;
		made.shadow = style.shadow;
		made.has_border = wants_border;
		made.has_shadow = wants_shadow;
		made.covers_letters = covers;
		out.push_back(std::move(made));
	}
	return out;
}

DrawingPlace PlaceDrawing(const agi::Context *c, AssDialogue *line,
                          Vector2D low, Vector2D high) {
	DrawingPlace out;

	// A line that animates any of what would be taken out of the numbers would go on applying it
	// afterwards, so it is better read as it was written than read nearly right.
	std::string const& raw = line->Text.get();
	if (raw.find("\\t(") != std::string::npos) return out;

	auto blocks = line->ParseTags();
	auto style = GatherStyle(c, line);

	// A line on a run is read where the run has got to on the frame on screen. What is worked on is
	// what can be seen, and the shape that comes of it stands at that point: its numbers hold the
	// turn, so being carried on along the run would turn it as well.
	auto placed = WherePlaced(c, line);
	Vector2D position = placed.told ? placed.at :
		LayoutPosition(c, line, style.alignment);
	style.position = position;

	double scale_x = style.scale_x / 100.0, scale_y = style.scale_y / 100.0;
	// The box the alignment hangs the drawing by: its own coordinates, scaled. Where that box
	// sits does not come into it - the renderers measure the box but still count from the
	// coordinate origin, which is why an \an7 drawing lands exactly on its \pos whatever its
	// numbers are.
	double box_width = (high.X() - low.X()) * scale_x;
	double box_height = (high.Y() - low.Y()) * scale_y;

	int horizontal = (style.alignment - 1) % 3;
	int vertical = (style.alignment - 1) / 3;
	double tx = horizontal == 0 ? 0. : horizontal == 1 ? .5 : 1.;
	// ASS counts the rows from the bottom: 7-9 are the top ones.
	double ty = vertical == 2 ? 0. : vertical == 1 ? .5 : 1.;
	// How far the point the line is hung from sits to the right of the left edge of that box
	// and below the top of it, which is also what the lean and the turn are measured from.
	double left = box_width * tx;
	double ascent = box_height * ty;

	AssTransform transform = BuildTransform(style, ascent, left);

	out.ok = true;
	out.baked = transform.needed ||
		std::abs(scale_x - 1) > 1e-9 || std::abs(scale_y - 1) > 1e-9;
	out.map = [=](Vector2D point) {
		Vector2D at = transform.Apply(point.X() * scale_x - left,
		                              point.Y() * scale_y - ascent);
		return position + at;
	};
	return out;
}

#ifdef __WXMSW__

namespace {

/// Whether two pieces lean about the same point and by the same amount, which is what
/// decides whether they can share a line.
bool SameLean(Piece const& first, Piece const& second) {
	// Not called `near`: windows.h has taken that word.
	auto alike = [](double a, double b) { return std::abs(a - b) < 1e-6; };
	return alike(first.ascent, second.ascent) &&
		alike(first.style.scale_x, second.style.scale_x) &&
		alike(first.style.scale_y, second.style.scale_y) &&
		alike(first.style.angle, second.style.angle) &&
		alike(first.style.shear_x, second.style.shear_x) &&
		alike(first.style.shear_y, second.style.shear_y) &&
		// Out of the plane the placement is a projection, and two stretches projected
		// differently cannot be put back on one line either.
		alike(first.style.rot_x, second.style.rot_x) &&
		alike(first.style.rot_y, second.style.rot_y);
}

/// The text of a piece as it has to be written on a line of its own.
///
/// Both ends of it are now the end of a row, and the renderer drops the ordinary spaces
/// there - which would slide the piece against the one beside it, because the space was
/// measured. Those, and the ones that were hard to begin with, are written hard.
std::string PieceText(std::wstring const& text) {
	size_t first = text.find_first_not_of(L' ');
	size_t last = text.find_last_not_of(L' ');
	wxString out;
	for (size_t at = 0; at < text.size(); ++at) {
		wchar_t letter = text[at];
		bool hard = letter == L'\u00A0' ||
			(letter == L' ' && (first == std::wstring::npos || at < first || at > last));
		if (hard) out += "\\h";
		else out += wxUniChar(letter);
	}
	return from_wx(out);
}

} // namespace

std::vector<SplitLine> SplitForShear(const agi::Context *c, AssDialogue *line) {
	auto blocks = line->ParseTags();

	// A drawing has neither rows nor a font, so there is nothing in it to break up.
	for (auto& block : blocks)
		if (block->GetType() == AssBlockType::DRAWING) return {};
	// A reset partway through would have to be unpicked into the tags it stands for before a piece
	// could carry it. A line that moves is no trouble: its pieces move with it, because both ends
	// of its run are worked out the same way its one position used to be.

	bool first_block = true;
	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		if (!first_block)
			for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags)
				if (tag.Name == "\\r") return {};
		first_block = false;
	}

	// Measuring a line costs a font and a walk along every letter of it, and most lines have
	// nothing in them to break up at all. Only two things can put one stretch of a line on a
	// different footing from the next: a row of its own, or a tag partway through that changes
	// what a stretch is measured or leaned by. Neither of those is expensive to look for, and
	// without one of them the answer is already no.
	{
		static const char *matters[] = {
			"\\fs", "\\fn", "\\b", "\\i", "\\fscx", "\\fscy", "\\frz", "\\fr", "\\fax",
			"\\fay", "\\frx", "\\fry"
		};
		bool worth_it = false;
		bool first = true;
		for (auto& block : blocks) {
			if (block->GetType() == AssBlockType::PLAIN) {
				std::string const& raw = block->GetText();
				for (size_t at = 0; at + 1 < raw.size() && !worth_it; ++at)
					if (raw[at] == '\\' && (raw[at + 1] == 'N' || raw[at + 1] == 'n'))
						worth_it = true;
				continue;
			}
			if (block->GetType() != AssBlockType::OVERRIDE) continue;
			// What the line says at its start applies to all of it, so it cannot make one
			// stretch differ from another.
			if (first) { first = false; continue; }
			for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags)
				for (auto candidate : matters)
					if (tag.Name == candidate) worth_it = true;
		}
		if (!worth_it) return {};
	}

	auto style = GatherStyle(c, line);

	auto placed = WherePlaced(c, line);
	if (!placed.told)
		placed.at = placed.first = placed.second = LayoutPosition(c, line, style.alignment);
	Vector2D position = placed.at;

	auto pieces = SplitPieces(c, line, style.alignment);
	for (auto& piece : pieces) piece.style.position = position;

	double box_width = 0, box_height = 0;
	if (!BuildPieces(pieces, style.alignment, box_width, box_height, Outlines::None)) return {};

	// Wide text is broken into rows by the renderer, and where it breaks depends on its own
	// measurements rather than these - so the rows worked out here would not be the rows on
	// screen, and every piece would be put in the wrong place. Left whole instead.
	double room = LayoutWidth(c, line);
	if (room > 0 && box_width > room + 1) return {};

	// Is there anything to gain? More than one row always slides, and within a row it is the
	// stretches that lean differently from their neighbours.
	size_t rows = pieces.back().row + 1;
	bool needed = rows > 1;
	Piece const *previous = nullptr;
	for (auto const& piece : pieces) {
		if (piece.source_text.empty()) continue;
		if (previous && previous->row == piece.row && !SameLean(*previous, piece)) needed = true;
		previous = &piece;
	}
	if (!needed) return {};

	int horizontal = (style.alignment - 1) % 3;
	int vertical = (style.alignment - 1) / 3;
	double tx = horizontal == 0 ? 0. : horizontal == 1 ? .5 : 1.;
	// ASS counts the rows from the bottom: 7-9 are the top ones.
	double ty = vertical == 2 ? 0. : vertical == 1 ? .5 : 1.;
	double shift_x = -box_width * tx;
	double shift_y = -box_height * ty;

	auto base = StyleOnly(c, line->Style);

	std::vector<SplitLine> out;
	std::string pending;
	size_t at = 0;
	while (at < pieces.size()) {
		size_t start = at;
		size_t row = pieces[at].row;
		Piece const *reference = nullptr;
		std::string body;

		while (at < pieces.size()) {
			auto const& piece = pieces[at];
			if (piece.row != row) break;
			bool empty = piece.source_text.empty();
			if (!empty && reference && !SameLean(piece, *reference)) break;
			// The first piece of the line says everything in force where it begins, in the
			// tags below; the ones after it say only what changed.
			if (at != start && !piece.added_tags.empty())
				body += "{" + piece.added_tags + "}";
			body += PieceText(piece.source_text);
			if (!empty && !reference) reference = &piece;
			++at;
		}

		// A row with nothing on it shows nothing, but the tags written on it are still in
		// force further down the line.
		if (!reference) { pending += body; continue; }

		auto const& shown = reference->style;
		// Measured from the point the line is hung from, which is what \pos names. And that is
		// already where the piece begins, lean and all: the renderer leans a stretch about its
		// own ascent line and starts the sideways lean again at its own first letter, so the
		// corner of the piece is the one point of it the lean leaves alone. Which is exactly
		// what makes a stretch in another size slide out of a leaning row, and what putting it
		// on a line of its own puts right.
		double lean_x = pieces[start].x + shift_x;
		double lean_y = reference->top + shift_y;

		// Whether the piece is turned out of the plane, where what happens to it is a
		// projection and not a shift, and so cannot be folded into where it sits.
		bool tilted = std::abs(shown.rot_x) > 1e-9 || std::abs(shown.rot_y) > 1e-9;

		// And then the turn. A piece on a line of its own turns about its own \pos, so the
		// \pos it is given has to be where the piece really begins on screen - turn included.
		// Naming a shared \org instead and leaving \pos in the flat frame renders the same at
		// rest, but then \pos is not where the line begins, and everything that moves a line
		// by moving its \pos - the free transform included - moves the wrong point.
		const double pi = 3.14159265358979;
		double radians = shown.angle * pi / 180.0;
		double sz = -std::sin(radians), cz = std::cos(radians);
		double turned_x = lean_x * cz - lean_y * sz;
		double turned_y = lean_x * sz + lean_y * cz;

		// \org turns \pos about itself, so with one the line does not begin at \pos either.
		// Out of the plane none of that can be folded in: the piece has to keep turning about
		// the same point as the line it came from, or it would be projected from somewhere
		// else and land at a different depth. So it is given that point as its own \org - and
		// then \pos is read before the turn, where the offset of the piece is the plain shift
		// worked out above. Which point \org names does not change what the piece looks like
		// around its anchor, only where the anchor is measured from.
		Vector2D centre = shown.has_origin ? shown.origin : position;

		// Where the piece begins, for a line hung from a given point. Asked once for where the
		// line is now, and once for each end of its run if it moves - the answer is a straight
		// function of the point, so a run stays a run.
		auto begins = [&](Vector2D from) {
			if (tilted)
				return Vector2D((float)(from.X() + lean_x), (float)(from.Y() + lean_y));
			Vector2D anchor = from;
			if (shown.has_origin) {
				double from_x = from.X() - shown.origin.X();
				double from_y = from.Y() - shown.origin.Y();
				anchor = Vector2D((float)(shown.origin.X() + from_x * cz - from_y * sz),
				                  (float)(shown.origin.Y() + from_x * sz + from_y * cz));
			}
			return Vector2D((float)(anchor.X() + turned_x), (float)(anchor.Y() + turned_y));
		};
		Vector2D q = begins(position);

		std::string tags;
		if (shown.face != base.face) tags += "\\fn" + shown.face;
		if (std::abs(shown.size - base.size) > 1e-6)
			tags += "\\fs" + FormatNumber(shown.size);
		if (shown.bold != base.bold) tags += shown.bold ? "\\b1" : "\\b0";
		if (shown.italic != base.italic) tags += shown.italic ? "\\i1" : "\\i0";
		if (shown.underline != base.underline) tags += shown.underline ? "\\u1" : "\\u0";
		if (shown.strikeout != base.strikeout) tags += shown.strikeout ? "\\s1" : "\\s0";
		if (std::abs(shown.spacing - base.spacing) > 1e-6)
			tags += "\\fsp" + FormatNumber(shown.spacing);
		if (std::abs(shown.scale_x - base.scale_x) > 1e-6)
			tags += "\\fscx" + FormatNumber(shown.scale_x);
		if (std::abs(shown.scale_y - base.scale_y) > 1e-6)
			tags += "\\fscy" + FormatNumber(shown.scale_y);
		if (std::abs(shown.angle - base.angle) > 1e-9)
			tags += "\\frz" + FormatNumber(shown.angle);
		if (std::abs(shown.shear_x) > 1e-9) tags += "\\fax" + FormatNumber(shown.shear_x);
		if (std::abs(shown.shear_y) > 1e-9) tags += "\\fay" + FormatNumber(shown.shear_y);
		if (tilted) {
			if (std::abs(shown.rot_x) > 1e-9) tags += "\\frx" + FormatNumber(shown.rot_x);
			if (std::abs(shown.rot_y) > 1e-9) tags += "\\fry" + FormatNumber(shown.rot_y);
		}
		// Flat, no \org: each piece turns about its own \pos, and \pos was worked out above
		// with the turn already in it. What the glyphs do around their anchor is the same
		// either way, and this way the anchor is a point that means something.
		tags += "\\an7" + PlacementTag(placed, begins(placed.first), begins(placed.second));
		if (tilted)
			tags += agi::format("\\org(%s,%s)",
				FormatNumber(centre.X()), FormatNumber(centre.Y()));
		tags += pieces[start].tags;

		out.push_back({"{" + tags + "}" + pending + body});
		pending.clear();
	}

	return out;
}

#else

std::vector<SplitLine> SplitForShear(const agi::Context *, AssDialogue *) { return {}; }

#endif

} // namespace text_to_shape
