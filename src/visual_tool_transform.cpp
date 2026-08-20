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

/// @file visual_tool_transform.cpp
/// @brief Reshaping the selected drawings by dragging on the video

#include "visual_tool_transform.h"

#include "text_to_shape.h"
#include "typesetting_perspective.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "command/command.h"
#include "compat.h"
#include "frame_main.h"
#include "gl_text.h"
#include "subtitle_line_combiner.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"

#include <libaegisub/color.h>
#include <libaegisub/format.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>

#include <wx/colour.h>
#include <wx/frame.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>

namespace {
	/// The warp's last handle, the one below the shape that moves the whole of it. The corners
	/// and their direction handles come first, so it sits after the twelve of them.
	const int warp_move_handle = 12;

	/// The lines the tool draws, over a dimmed video: red, because on a busy frame black
	/// dashes disappear into the picture.
	const wxColour mesh_colour(255, 62, 62);

	const double pi = 3.14159265358979;

	/// A number as a tag wants it: no more precision than is useful, no trailing zeroes.
	std::string Number(double value) {
		std::string out = agi::format("%.3f", value);
		if (out.find('.') != std::string::npos) {
			while (!out.empty() && out.back() == '0') out.pop_back();
			if (!out.empty() && out.back() == '.') out.pop_back();
		}
		return out.empty() || out == "-0" ? "0" : out;
	}

	/// Turn a point about another, the way \frz turns text: anticlockwise on screen, which
	/// with y pointing down is this pair of formulas.
	Vector2D RotateAbout(Vector2D point, Vector2D pivot, double degrees) {
		if (std::abs(degrees) < 1e-9) return point;
		double radians = degrees * pi / 180.0;
		double sine = std::sin(radians), cosine = std::cos(radians);
		Vector2D offset = point - pivot;
		return pivot + Vector2D((float)(offset.X() * cosine + offset.Y() * sine),
		                        (float)(-offset.X() * sine + offset.Y() * cosine));
	}

	using Matrix2 = TransformMatrix2;

	Matrix2 Multiply(Matrix2 const& left, Matrix2 const& right) {
		return {left.a * right.a + left.b * right.c, left.a * right.b + left.b * right.d,
		        left.c * right.a + left.d * right.c, left.c * right.b + left.d * right.d};
	}

	Vector2D ApplyMatrix(Matrix2 const& map, Vector2D point) {
		return Vector2D((float)(map.a * point.X() + map.b * point.Y()),
		                (float)(map.c * point.X() + map.d * point.Y()));
	}

	/// The map that undoes this one. False for one that has collapsed, where nothing undoes it.
	bool Invert(Matrix2 const& map, Matrix2& out) {
		double det = map.a * map.d - map.b * map.c;
		if (std::abs(det) < 1e-12) return false;
		out = {map.d / det, -map.b / det, -map.c / det, map.a / det};
		return true;
	}

	/// A turn the way \frz turns: anticlockwise on screen, where y points down.
	Matrix2 Turn(double degrees) {
		double radians = degrees * pi / 180.0;
		double sine = std::sin(radians), cosine = std::cos(radians);
		return {cosine, sine, -sine, cosine};
	}

	/// What a line's own tags do to it.
	///
	/// The lean comes after the scale, not before: libass multiplies \fax by
	/// scale_x/scale_y before using it, which is the same as leaning the already-scaled
	/// glyph. Getting that order the wrong way round makes the lean come out short by
	/// exactly fscy/fscx.
	Matrix2 LineMatrix(Vector2D scale, Vector2D shear, double angle) {
		Matrix2 scaled{scale.X() / 100.0, 0, 0, scale.Y() / 100.0};
		Matrix2 sheared{1, shear.X(), shear.Y(), 1};
		return Multiply(Turn(angle), Multiply(scaled, sheared));
	}

	/// Split a map back into the numbers a line can carry, keeping the lean it already has
	/// along y.
	///
	/// \fay is left exactly as the line said it: a turn, two scales and \fax can describe
	/// any map on their own, so there is never a need to touch it - and rewriting it would
	/// change every other number for nothing. Holding it fixed leaves four unknowns for four
	/// equations, and for a line nobody has dragged the answer is what it already said.
	///
	/// Returns false for a map that has collapsed, where there would be nothing to say.
	bool SplitMatrix(Matrix2 const& map, double shear_y, double& angle, double& shear_x,
	                 Vector2D& scale) {
		// Turned back, the bottom row of the map has to read (sy * fay, sy) - and that one
		// ratio is what fixes the turn.
		double first = map.a - shear_y * map.b;
		double second = map.c - shear_y * map.d;
		if (std::hypot(first, second) < 1e-12) return false;

		double radians = std::atan2(-second, first);
		double cosine = std::cos(radians), sine = std::sin(radians);

		double along = cosine * map.a - sine * map.c;
		double across = sine * map.b + cosine * map.d;
		if (along < 0) {
			// The same map read the other way up. A line can carry a negative scale, but a
			// negative width reads as a mistake rather than as a mirror.
			radians += pi;
			cosine = -cosine;
			sine = -sine;
			along = -along;
			across = -across;
		}
		if (std::abs(along) < 1e-9) return false;

		angle = radians * 180.0 / pi;
		scale = Vector2D((float)(along * 100), (float)(across * 100));
		shear_x = (cosine * map.b - sine * map.d) / along;
		return true;
	}

	/// What a line says for a tag that carries one number, or the fallback if it says
	/// nothing. The base has readers for the tags it needs; these two it does not.
	double TagNumber(AssDialogue *line, const char *name, double fallback) {
		for (auto& block : line->ParseTags()) {
			if (block->GetType() != AssBlockType::OVERRIDE) continue;
			for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags)
				if (tag.Name == name && !tag.Params.empty())
					return tag.Params[0].Get<double>(fallback);
		}
		return fallback;
	}

	/// Whether text contains two words separated by whitespace or an ASS hard line break.
	/// UTF-8 bytes are deliberately treated as ordinary word bytes; whitespace and \N/\n are
	/// ASCII in ASS, so this neither splits nor damages non-Latin text.
	bool HasAtLeastTwoWords(std::string const& text) {
		int words = 0;
		bool in_word = false;
		for (size_t i = 0; i < text.size(); ++i) {
			unsigned char ch = static_cast<unsigned char>(text[i]);
			bool separator = std::isspace(ch) ||
				(text[i] == '\\' && i + 1 < text.size() &&
				 (text[i + 1] == 'N' || text[i + 1] == 'n'));
			if (separator) {
				in_word = false;
				if (text[i] == '\\') ++i;
				continue;
			}
			if (!in_word) {
				in_word = true;
				if (++words >= 2) return true;
			}
		}
		return false;
	}

	/// The same, but only from the first tag block.
	///
	/// This is what is in force where the line begins. Looking further in would pick up a value
	/// meant for one word - a \fscx on the emphasised one, say - and treat it as the line's own.
	double FirstBlockNumber(std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks,
	                        const char *name, double fallback) {
		for (auto& block : blocks) {
			if (block->GetType() != AssBlockType::OVERRIDE) continue;
			for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags)
				if (tag.Name == name && !tag.Params.empty())
					return tag.Params[0].Get<double>(fallback);
			// Only the first block: what comes after belongs to part of the line.
			return fallback;
		}
		return fallback;
	}

	/// How far along its box a line's anchor sits, from its alignment.
	/// The convex hull of a set of points, anticlockwise in script coordinates.
	std::vector<Vector2D> ConvexHull(std::vector<Vector2D> const& points) {
		if (points.size() < 3) return {};

		std::vector<Vector2D> sorted = points;
		std::sort(sorted.begin(), sorted.end(), [](Vector2D a, Vector2D b) {
			return a.X() != b.X() ? a.X() < b.X() : a.Y() < b.Y();
		});
		auto turn = [](Vector2D o, Vector2D a, Vector2D b) {
			return (double)(a.X() - o.X()) * (b.Y() - o.Y()) -
				(double)(a.Y() - o.Y()) * (b.X() - o.X());
		};

		std::vector<Vector2D> hull(sorted.size() * 2);
		size_t at = 0;
		for (auto const& point : sorted) {
			while (at >= 2 && turn(hull[at - 2], hull[at - 1], point) <= 0) --at;
			hull[at++] = point;
		}
		size_t lower = at + 1;
		for (size_t i = sorted.size() - 1; i-- > 0;) {
			while (at >= lower && turn(hull[at - 2], hull[at - 1], sorted[i]) <= 0) --at;
			hull[at++] = sorted[i];
		}
		hull.resize(at ? at - 1 : 0);
		if (hull.size() < 3) return {};
		return hull;
	}

	/// The hull with the vertices that hardly bend it taken out, so a shape drawn as four
	/// corners is four corners again even when a click or a curve fit left a little wobble
	/// along an edge.
	std::vector<Vector2D> SimplifyHull(std::vector<Vector2D> hull, double tolerance) {
		bool dropped = true;
		while (dropped && hull.size() > 4) {
			dropped = false;
			double worst = tolerance;
			size_t at = hull.size();
			for (size_t i = 0; i < hull.size(); ++i) {
				Vector2D before = hull[(i + hull.size() - 1) % hull.size()];
				Vector2D after = hull[(i + 1) % hull.size()];
				Vector2D edge = after - before;
				double length = edge.Len();
				if (length < 1e-6) continue;
				double away = std::abs((double)(hull[i].X() - before.X()) * edge.Y() -
					(double)(hull[i].Y() - before.Y()) * edge.X()) / length;
				if (away >= worst) continue;
				worst = away;
				at = i;
			}
			if (at < hull.size()) {
				hull.erase(hull.begin() + at);
				dropped = true;
			}
		}
		return hull;
	}

	/// The smallest rectangle, at any angle, that holds a convex hull.
	///
	/// The smallest one always has a side along a side of the hull, so every hull edge is
	/// tried and the best kept. Returns false when the hull has no area.
	bool MinimumAreaBox(std::vector<Vector2D> const& hull, typesetting::OrientedBox& out) {
		if (hull.size() < 3) return false;

		double best_area = 0;
		bool found = false;
		for (size_t i = 0; i < hull.size(); ++i) {
			Vector2D edge = hull[(i + 1) % hull.size()] - hull[i];
			if (edge.Len() < 1e-6f) continue;

			typesetting::OrientedBox frame;
			frame.angle = (float)(std::atan2(edge.Y(), edge.X()) * 180.0 / pi);
			frame.centre = Vector2D(0.f, 0.f);

			Vector2D low, high;
			for (size_t j = 0; j < hull.size(); ++j) {
				Vector2D local = frame.ToLocal(hull[j]);
				if (!j) { low = high = local; }
				else { low = low.Min(local); high = high.Max(local); }
			}

			Vector2D size = high - low;
			double area = (double)size.X() * size.Y();
			if (found && !(area < best_area)) continue;

			found = true;
			best_area = area;
			out.angle = frame.angle;
			out.centre = frame.ToScript((low + high) / 2);
			out.half = size / 2;
		}
		return found && best_area > 0;
	}

	Vector2D AnchorFractions(int align) {
		int horizontal = (align - 1) % 3;
		int vertical = (align - 1) / 3;
		return Vector2D(horizontal == 0 ? 0.f : horizontal == 1 ? .5f : 1.f,
		                vertical == 2 ? 0.f : vertical == 1 ? .5f : 1.f);
	}

	/// A perspective target has to walk around one convex, non-degenerate quadrilateral.
	/// The sign is deliberately not normalized: clockwise and anticlockwise point orders are
	/// different directed mappings and Auto perspective preserves the one the user drew.
	bool DirectedQuad(std::vector<Vector2D> const& points) {
		if (points.size() != 4) return false;
		double direction = 0;
		for (int i = 0; i < 4; ++i) {
			Vector2D first = points[(i + 1) % 4] - points[i];
			Vector2D second = points[(i + 2) % 4] - points[(i + 1) % 4];
			double cross = first.X() * second.Y() - first.Y() * second.X();
			if (std::abs(cross) < 1e-3) return false;
			if (!direction) direction = cross;
			else if ((cross > 0) != (direction > 0)) return false;
		}
		return true;
	}
}

VisualToolTransform::VisualToolTransform(VideoDisplay *parent, agi::Context *context,
                                         VisualToolTransformMode mode,
                                         std::string return_tool,
                                         bool auto_perspective)
: VisualTool<VisualDraggableFeature>(parent, context)
, mode(mode)
, auto_perspective(auto_perspective)
, gl_text(std::make_unique<OpenGLText>())
, return_tool(std::move(return_tool))
{
	// A projective distortion must leave blur exactly as authored. Recomputing it from a
	// perspective solution is especially unstable because that solution has no single scale.
	if (mode == VisualToolTransformMode::Distort)
		recalc_blur = false;

	// Auto perspective exposes no decoration recalculation choices. The line geometry follows
	// the directed quadrilateral, existing decoration values stay as authored, and only an
	// existing clip is carried through the same map automatically.
	if (auto_perspective) {
		recalc_bord = false;
		recalc_shad = false;
		maintain_decor = false;
		recalc_blur = false;
		recalc_clip = true;
	}
	selection_connection = context->selectionController->AddSelectionListener(
		[this] { ExitTool(); });
	connections.push_back(context->ass->AddCommitListener(
		&VisualToolTransform::OnFileReplaced, this));
	if (context->parent)
		context->parent->Bind(wxEVT_CHAR_HOOK, &VisualToolTransform::OnCharHook, this);
	preview_interface.AttachHost(parent->GetPreviewBar(), [this](int id) {
		// Focus first: Apply and Cancel can synchronously replace and destroy this tool.
		this->parent->SetFocus();
		Perform(static_cast<VisualToolTransformAction>(id));
	}, [this](int id, double value, bool) {
		if (static_cast<VisualToolTransformAction>(id) ==
			VisualToolTransformAction::AutoPerspectiveSize)
			UpdateAutoPerspectiveSize(value);
	});
	Collect();
}

VisualToolTransform::~VisualToolTransform() {
	// Switching to another tool destroys this one without going through ExitTool, and the
	// video would carry on showing a preview of something the file never said.
	//
	// Unless it is the window that is going. Then handing the lines back only makes the video
	// render one more frame on its way out, which is the flash seen when the program is closed in
	// the middle of a session - and there is nothing to put right anyway, since a preview never
	// lived anywhere but in the video's own copy of the file.
	if (!WindowGoing()) ClearPreview();
	if (c->parent)
		c->parent->Unbind(wxEVT_CHAR_HOOK, &VisualToolTransform::OnCharHook, this);
	// However this ends - including the window closing - the editor must not be left dead.
	LockEditing(false);
}

bool VisualToolTransform::WindowGoing() const {
	// The window has begun to come apart, so anything drawn or handed to the video from here would
	// only be one more frame nobody asked to see.
	return (c->frame && c->frame->IsClosing()) ||
		(c->parent && c->parent->IsBeingDeleted());
}

bool VisualToolTransform::TagsMode() const {
	return mode == VisualToolTransformMode::Free || mode == VisualToolTransformMode::Distort;
}

bool VisualToolTransform::CollectTextBox() {
	textbox_document.reset();
	textbox_lines.clear();
	AssDialogue *line = c->selectionController->GetActiveLine();
	if (!line || !c->imageMask || !c->imageMask->IsTextBoxGroup(line)) return false;
	textbox_lines = c->imageMask->GetGroupLines(line);
	if (textbox_lines.empty()) return false;
	auto loaded = typesetting::textbox::Load(*c->ass, *textbox_lines.front());
	if (!loaded) {
		textbox_lines.clear();
		return false;
	}
	textbox_document = std::move(*loaded);
	typesetting::textbox::Corners(*textbox_document, textbox_original_corners);
	return true;
}

typesetting::textbox::Document VisualToolTransform::TransformedTextBox() const {
	auto transformed = *textbox_document;
	Vector2D target[4];
	if (mode == VisualToolTransformMode::Distort)
		std::copy(corners, corners + 4, target);
	else
		for (int i = 0; i < 4; ++i) target[i] = MapPoint(textbox_original_corners[i]);
	typesetting::textbox::SetCorners(transformed, target);
	return transformed;
}

bool VisualToolTransform::Active() const {
	return TagsMode() ? !tag_lines.empty() : editor.has_value();
}

bool VisualToolTransform::LinesAlive() const {
	if (tag_lines.empty() && !editor) return false;

	std::set<AssDialogue const *> live;
	for (auto const& line : c->ass->Events) live.insert(&line);

	for (auto const& found : tag_lines)
		if (!live.count(found.line)) return false;
	for (auto line : textbox_lines)
		if (!live.count(line)) return false;
	if (editor)
		for (auto line : editor->lines())
			if (!live.count(line)) return false;
	return true;
}

void VisualToolTransform::OnFileReplaced(int type) {
	if (type != AssFile::COMMIT_NEW) return;

	// Everything this session was holding belonged to the file that has just gone. Dropped
	// before leaving, so that leaving does not try to hand those lines back to the video.
	tag_lines.clear();
	split_lines.clear();
	shear_split = false;
	split_built = false;
	editor.reset();
	textbox_document.reset();
	textbox_lines.clear();
	features.clear();
	sel_features.clear();
	ExitTool();
}

void VisualToolTransform::SendPreview() {
	if (!LinesAlive() || WindowGoing()) return;
	if (TagsMode()) {
		if (TextBoxMode()) {
			auto transformed = TransformedTextBox();
			auto generated = typesetting::textbox::Generate(c, *textbox_lines.front(), transformed);
			std::vector<AssDialogue> hidden;
			hidden.reserve(textbox_lines.size());
			for (auto line : textbox_lines) {
				AssDialogue copy(*line);
				copy.Comment = true;
				hidden.push_back(std::move(copy));
			}
			std::vector<AssDialogue const *> changed, added;
			for (auto const& line : hidden) changed.push_back(&line);
			for (auto const& line : generated) added.push_back(&line);
			c->videoController->PreviewSubtitles(changed, added);
			return;
		}
		if (maintain_decor) EnsureDecor();
		auto const& lines = Lines();

		// The copies only have to live until the call returns: the video takes its own. All of
		// them are made before any pointer into the list is taken, or growing it would leave
		// those pointers behind.
		std::vector<AssDialogue> copies;
		/// Whether a copy is a line the file does not have, which has to be added rather than
		/// stood in for.
		std::vector<bool> extra;
		copies.reserve(lines.size() * 3 + tag_lines.size());
		for (auto const& found : lines) {
			// The shapes that stand for the border and the shadow, and then the letters over them.
			//
			// A preview adds its lines to the end of the file, so the letters have to go there with
			// the shapes or the shapes would be drawn over them - which is why the line they belong
			// to is silenced where it stands and put back at the end, in the order it all has to be
			// drawn in. Accepting does the same thing in the file, except that there it can insert.
			if (maintain_decor && !found.decor.empty()) {
				if (!found.owned) {
					AssDialogue silenced(*found.line);
					silenced.Comment = true;
					copies.push_back(std::move(silenced));
					extra.push_back(false);
				}
				for (auto const& decor : found.decor) {
					for (bool shadow : {true, false}) {
						if (shadow ? !decor.has_shadow : !decor.has_border) continue;
						AssDialogue copy(*found.line);
						copy.Text = DecorLineText(found, decor, shadow);
						copy.Comment = false;
						copies.push_back(std::move(copy));
						extra.push_back(true);
					}
				}
				// The shape preserves the border, not the glyph fill. Always keep the original
				// letters on top: omitting them can expose another language layer underneath.
				AssDialogue copy(*found.line);
				copy.Text = TagLineText(found);
				copy.Comment = false;
				copies.push_back(std::move(copy));
				extra.push_back(true);
				continue;
			}

			AssDialogue copy(*found.line);
			copy.Text = TagLineText(found);
			copies.push_back(std::move(copy));
			extra.push_back(found.owned != nullptr);
		}
		// A line that has been cut into pieces has to stop being drawn, or it would be drawn
		// underneath them - and a comment is how a line stops being drawn without being taken
		// out of the file.
		for (auto const& found : tag_lines) {
			if (!found.replaced) continue;
			AssDialogue silenced(*found.line);
			silenced.Comment = true;
			copies.push_back(std::move(silenced));
			extra.push_back(false);
		}

		// The pieces are not in the file, so they go in as lines of their own rather than as
		// changes to lines that are there.
		std::vector<AssDialogue const *> changed, added;
		for (size_t at = 0; at < copies.size(); ++at)
			(extra[at] ? added : changed).push_back(&copies[at]);
		c->videoController->PreviewSubtitles(changed, added);
		return;
	}

	if (!editor) return;
	// The copies only have to live until the call returns: the video takes its own.
	auto preview = editor->PreviewLines();
	std::vector<AssDialogue const *> silenced, drawings;
	silenced.reserve(preview.silenced.size());
	for (auto const& line : preview.silenced) silenced.push_back(line.get());
	drawings.reserve(preview.drawings.size());
	for (auto const& line : preview.drawings) drawings.push_back(line.get());
	c->videoController->PreviewSubtitles(silenced, drawings);
}

void VisualToolTransform::ClearPreview() {
	if (!LinesAlive()) return;
	if (TextBoxMode()) {
		std::vector<AssDialogue const *> pointers(textbox_lines.begin(), textbox_lines.end());
		if (!pointers.empty()) c->videoController->PreviewSubtitles(pointers);
		return;
	}
	if (TagsMode()) {
		std::vector<AssDialogue const *> pointers;
		for (auto const& found : tag_lines) pointers.push_back(found.line);
		if (!pointers.empty()) c->videoController->PreviewSubtitles(pointers);
		return;
	}

	if (!editor) return;
	// The real lines never changed, so handing them over again is what puts the video
	// back to showing them.
	std::vector<AssDialogue const *> pointers;
	for (auto line : editor->lines()) pointers.push_back(line);
	if (!pointers.empty()) c->videoController->PreviewSubtitles(pointers);
}

bool VisualToolTransform::CollectTags() {
	tag_lines.clear();
	split_lines.clear();
	shear_split = false;
	split_built = false;
	gesture_scale = Vector2D(1.f, 1.f);
	gesture_angle = 0;
	gesture_move = Vector2D(0.f, 0.f);
	gesture_anchor = Vector2D(0.f, 0.f);
	// Left behind once, and then the tool opened with the last lean still in force and the
	// lines moved the moment it appeared.
	gesture_shear = Vector2D(0.f, 0.f);
	frame_linear = TransformMatrix2();
	frame_offset = Vector2D(0.f, 0.f);

	auto lines = TextBoxMode() ? textbox_lines : c->selectionController->GetSortedSelection();
	for (auto line : lines) {
		if (!IsDisplayed(line)) continue;
		tag_lines.push_back(ReadLine(line));
	}
	if (tag_lines.empty()) return false;

	BuildBox();
	return true;
}

VisualToolTransform::TagLine VisualToolTransform::ReadLine(AssDialogue *line) {
	TagLine found;
	found.line = line;
	// \pos, \org, \an and \q belong to the line as a whole, so where they are said does
	// not matter; everything else does, and is read from the start of the line only.
	found.pos = GetLinePosition(line);
	// Which is where the run begins for a line that moves, rather than where the line is now. What
	// every one of these tools works on is the frame on screen.
	found.placed = text_to_shape::WherePlaced(c, line);
	if (found.placed.told) found.pos = found.placed.at;
	else found.placed.at = found.placed.first = found.placed.second = found.pos;
	found.org = GetLineOrigin(line);

	AssStyle const default_style;
	AssStyle const *style = c->ass->GetStyle(line->Style);
	if (!style) style = &default_style;
	auto blocks = line->ParseTags();

	found.scale = Vector2D(
		(float)FirstBlockNumber(blocks, "\\fscx", style->scalex),
		(float)FirstBlockNumber(blocks, "\\fscy", style->scaley));
	found.shear = Vector2D(
		(float)FirstBlockNumber(blocks, "\\fax", 0),
		(float)FirstBlockNumber(blocks, "\\fay", 0));
	found.bord = Vector2D(
		(float)FirstBlockNumber(blocks, "\\xbord",
			FirstBlockNumber(blocks, "\\bord", style->outline_w)),
		(float)FirstBlockNumber(blocks, "\\ybord",
			FirstBlockNumber(blocks, "\\bord", style->outline_w)));
	found.shad = Vector2D(
		(float)FirstBlockNumber(blocks, "\\xshad",
			FirstBlockNumber(blocks, "\\shad", style->shadow_w)),
		(float)FirstBlockNumber(blocks, "\\yshad",
			FirstBlockNumber(blocks, "\\shad", style->shadow_w)));
	found.blur = FirstBlockNumber(blocks, "\\blur", 0);
	found.be = FirstBlockNumber(blocks, "\\be", 0);
	found.clip = ReadClip(line);
	double wrap_style = TagNumber(line, "\\q", -1);
	found.has_wrap_style = wrap_style >= 0;
	bool has_q2 = false;
	for (auto& block : line->ParseTags()) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		for (auto const& tag : static_cast<AssDialogueBlockOverride *>(block.get())->Tags)
			if (tag.Name == "\\q" && !tag.Params.empty() &&
				std::abs(tag.Params[0].Get<double>(-1) - 2) <= 1e-6)
				has_q2 = true;
	}
	for (auto& block : line->ParseTags())
		if (block->GetType() == AssBlockType::DRAWING) found.drawing = true;
	std::string stripped_text = line->GetStrippedText();
	found.ensure_q2 = TagsMode() && !stripped_text.empty() &&
		!has_q2 && HasAtLeastTwoWords(stripped_text);
	found.angle = (float)FirstBlockNumber(blocks, "\\frz",
		FirstBlockNumber(blocks, "\\fr", style->angle));
	found.angle_x = FirstBlockNumber(blocks, "\\frx", 0);
	found.angle_y = FirstBlockNumber(blocks, "\\fry", 0);
	// Guided perspective always starts from a flat row. The authored perspective and
	// shear tags are removed from the preview copy and solved again from the four points.
	if (auto_perspective) {
		found.shear = Vector2D();
		found.angle_x = 0;
		found.angle_y = 0;
		found.org = Vector2D();
	}
	found.align = GetLineAlignment(line);

	// For text the extents start at zero, but a drawing's own coordinates can start
	// anywhere - and the renderer draws it there, so where its ink begins is part of
	// where its box is. Leaving that out is what blew the box up on a line that had
	// already become a shape.
	auto extents = GetLineBaseExtents(line);
	found.box_first = extents.first;
	found.box_second = extents.second;
	Vector2D size = extents.second - extents.first;
	found.size = Vector2D(size.X() * found.scale.X() / 100.f,
	                      size.Y() * found.scale.Y() / 100.f);
	found.ink = Vector2D(extents.first.X() * found.scale.X() / 100.f,
	                     extents.first.Y() * found.scale.Y() / 100.f);

	// Whether the margins are breaking this line as it stands. Only measured, not changed:
	// what it decides is whether the wrapping can safely be switched off.
	{
		int script_w = 0, script_h = 0;
		c->ass->GetResolution(script_w, script_h);
		auto margin = line->Margin;
		if (AssStyle *style = c->ass->GetStyle(line->Style))
			for (int i = 0; i < 3; ++i)
				if (margin[i] == 0) margin[i] = style->Margin[i];
		double room = std::max(0, script_w - margin[0] - margin[1]);
		found.wrapped = room > 0 && found.size.X() > room + 1;
	}

	found.start_pos = found.pos;
	found.start_org = found.org;
	found.start_scale = found.scale;
	found.start_shear = found.shear;
	found.start_bord = found.bord;
	found.start_shad = found.shad;
	found.start_blur = found.blur;
	found.start_be = found.be;
	found.start_angle = found.angle;
	return found;
}

std::vector<VisualToolTransform::TagLine> const& VisualToolTransform::Lines() const {
	return shear_split ? split_lines : tag_lines;
}

void VisualToolTransform::EnsureShearSplit() {
	if (!TagsMode() || shear_split) return;

	if (!split_built) {
		split_built = true;
		bool any = false;
		for (auto& found : tag_lines) {
			auto pieces = text_to_shape::SplitForShear(c, found.line);
			if (pieces.empty()) {
				// Nothing to gain, or nothing that could be put back exactly. Either way the
				// line goes on as itself.
				split_lines.push_back(found);
				continue;
			}
			found.replaced = true;
			any = true;
			for (auto const& piece : pieces) {
				auto owned = std::make_shared<AssDialogue>(*found.line);
				owned->Text = piece.text;
				TagLine read = ReadLine(owned.get());
				read.owned = std::move(owned);
				read.source = found.line;
				split_lines.push_back(std::move(read));
			}
		}
		// A selection where every line stands on its own already is left alone: breaking it
		// up would add lines to the file for nothing.
		if (!any) split_lines.clear();
	}

	if (!split_lines.empty()) shear_split = true;
}

void VisualToolTransform::EnsureDecor() {
	if (!TagsMode()) return;
	for (auto *set : {&tag_lines, &split_lines})
		for (auto& found : *set) {
			if (found.decor_built) continue;
			found.decor_built = true;
			found.decor = text_to_shape::BakeDecorations(c, found.line);
		}
}

std::pair<Vector2D, Vector2D> VisualToolTransform::MovedEnds(TagLine const& original,
	Applied const& applied) const {
	if (!original.placed.moving) return {applied.pos, applied.pos};
	if (auto_perspective)
		return {MapPoint(original.placed.first), MapPoint(original.placed.second)};

	// The gesture's own map, applied to each end from where the line stood. On the frame on screen
	// the two ends mix back to exactly the point the handle was let go at, whatever the map was,
	// because that is the point they were measured from.
	Matrix2 whole = TotalMatrix();
	if (!original.org)
		return {applied.pos + ApplyMatrix(whole, original.placed.first - original.start_pos),
		        applied.pos + ApplyMatrix(whole, original.placed.second - original.start_pos)};

	// With an \org the turn carries the anchor as well, so where the line runs on screen is its
	// \pos run turned about \org - and it is the run on screen that the gesture acts on. So each
	// end is taken there, moved, and turned back into what \move has to say.
	auto about = [](Vector2D point, Vector2D centre, double degrees) {
		return centre + ApplyMatrix(Turn(degrees), point - centre);
	};
	Vector2D was_centre = original.start_org ? original.start_org : original.start_pos;
	Vector2D now_centre = applied.org ? applied.org : applied.pos;
	Vector2D was_shown = about(original.start_pos, was_centre, original.start_angle);
	Vector2D now_shown = about(applied.pos, now_centre, applied.angle);

	auto taken = [&](Vector2D end) {
		Vector2D shown = about(end, was_centre, original.start_angle);
		Vector2D landed = now_shown + ApplyMatrix(whole, shown - was_shown);
		return about(landed, now_centre, -applied.angle);
	};
	return {taken(original.placed.first), taken(original.placed.second)};
}

bool VisualToolTransform::DecorHint() const {
	// Nothing to advise where the pair are already being kept, and nothing to advise in the bending
	// modes either: there the shapes are made whatever anyone thinks about it.
	if (maintain_decor || !TagsMode()) return false;
	for (auto const& found : tag_lines)
		if (found.bord.Len() > .01f || found.shad.Len() > .01f) return true;
	return false;
}

bool VisualToolTransform::HasDecor() const {
	for (auto const& found : Lines())
		for (auto const& decor : found.decor)
			if (decor.has_border || decor.has_shadow) return true;
	return false;
}

std::string VisualToolTransform::DecorLineText(TagLine const& original,
	text_to_shape::Decoration const& decor, bool shadow) const {
	auto *tool = const_cast<VisualToolTransform *>(this);

	// The same tags the letters get, so that what comes out is scaled, leaned and turned exactly
	// as they are. What differs is where it hangs from, what paints it, and whether it is a shape.
	AssDialogue copy(*original.line);
	copy.Text = TagLineText(original);

	// A shadow with no border is the letters again, moved - so it stays letters. Sharper than any
	// polygon of them, a great deal smaller, and exactly right. With a border it has to be the
	// widened shape instead, or the shadow would be thinner than what it stands behind.
	bool as_shape = decor.has_border;

	Applied applied = ApplyGesture(original);
	// The corner of the stretch, which is what the shape hangs from. It was measured with the
	// line's own scale in it, so it follows that scale changing.
	auto followed = [](double now, double was) {
		return std::abs(was) > 1e-6 ? (float)(now / was) : 1.f;
	};
	Vector2D lean(decor.lean.X() * followed(applied.scale.X(), original.start_scale.X()),
	              decor.lean.Y() * followed(applied.scale.Y(), original.start_scale.Y()));

	// Read before the turn, the way the renderer reads a glyph's own place in the line - which
	// only works if the shape turns about the same point as the letters. A line with no \org turns
	// about its own \pos, and the shape's \pos is not the line's, so the centre is written out.
	// The corner is where a shape hangs from; letters hang from their own alignment, which the
	// line already says, so they are only moved.
	Vector2D shift = as_shape ? lean : Vector2D(0.f, 0.f);
	// The shadow is the same shape moved the way the renderer moves it: it adds the offset to the
	// glyph's own shift, which rides in the translation column of the transform - so the turn
	// reaches it, and neither the scale nor the lean does. Which is why it is added here, before
	// the turn, and kept exactly as the line asked for it.
	if (shadow) shift = shift + original.shad;

	// Both ends of the run, if the line is on one, each carrying the same offset.
	auto [first_end, second_end] = MovedEnds(original, applied);

	// Moving \pos moves what a line with no \org turns about, and turning about another point is
	// not the same turn - so the centre the letters use is written out either way.
	tool->SetOverride(&copy, "\\org", (original.org ? applied.org : applied.pos).PStr());
	if (as_shape) tool->SetOverride(&copy, "\\an", "7");
	{
		auto [name, value] = text_to_shape::PlacementOverride(original.placed,
			first_end + shift, second_end + shift);
		tool->SetOverride(&copy, name, value);
	}
	tool->SetOverride(&copy, "\\bord", "0");
	tool->SetOverride(&copy, "\\shad", "0");

	std::string paint = shadow ? decor.shadow_paint : decor.border_paint;
	std::string whole = copy.Text.get();
	size_t close = whole.find('}');
	if (close == std::string::npos) whole = "{}" + whole, close = 1;

	// The letters again, in the other colour: everything the line said stays, and what paints them
	// goes at the end of the first block so that nothing said there can undo it.
	if (!as_shape) {
		whole.insert(close, paint);
		return whole;
	}

	// Or the shape, and then only the first block is kept - which is where everything in force at
	// the stretch is said - with the shape in place of the words.
	std::string head = whole.substr(0, close + 1);
	head.insert(head.size() - 1, paint + "\\p1");
	std::string const& body = decor.bordered.empty() ? decor.letters : decor.bordered;
	return head + body + "{\\p0}";
}

void VisualToolTransform::LineCorners(TagLine const& original, Vector2D corners[4]) const {
	// A line turned out of the plane sits on screen as a quadrilateral, so that is what has
	// to be gone round - its unturned box is somewhere else entirely.
	if (Perspective(original)) {
		LineQuad(original, corners);
		return;
	}

	Vector2D fractions = AnchorFractions(original.align);
	Vector2D top_left = original.pos + original.ink -
		Vector2D(original.size.X() * fractions.X(), original.size.Y() * fractions.Y());
	Vector2D pivot = original.org ? original.org : original.pos;
	for (int corner = 0; corner < 4; ++corner) {
		Vector2D at = top_left +
			Vector2D(corner == 1 || corner == 2 ? original.size.X() : 0.f,
			         corner >= 2 ? original.size.Y() : 0.f);
		corners[corner] = RotateAbout(at, pivot, original.angle);
	}
}

void VisualToolTransform::BuildBox() {
	if (tag_lines.empty()) return;

	// The box lies the way the text lies, which only means something if the lines agree
	// about it - and if they do, a scale along the box is exactly \fscx and \fscy.
	float angle = tag_lines.front().angle;
	for (auto const& found : tag_lines)
		if (std::abs(found.angle - angle) > .01f) { angle = 0; break; }

	typesetting::OrientedBox frame;
	frame.angle = angle;
	frame.centre = Vector2D(0.f, 0.f);

	Vector2D low, high;
	bool first = true;
	for (auto const& found : tag_lines) {
		Vector2D corners[4];
		LineCorners(found, corners);
		for (auto const& corner : corners) {
			Vector2D local = frame.ToLocal(corner);
			if (first) { low = high = local; first = false; }
			else { low = low.Min(local); high = high.Max(local); }
		}
	}

	box.angle = angle;
	shear_frame_angle = angle;
	box.centre = frame.ToScript((low + high) / 2);
	box.half = (high - low) / 2;
	// A box with no size has no handles worth dragging.
	box.half = box.half.Max(Vector2D(4.f, 4.f));

	// A frame larger than the video cannot be grabbed - its handles end up off screen, and only
	// zooming right out brings them back. It is a control frame and nothing more: the lines
	// follow the map it describes whatever size it is, so it is kept inside the script.
	//
	// Auto perspective has no such handles: the target is the quadrilateral that was drawn, and
	// this box is only the rectangle that is mapped onto it. Trimming it there would trim the
	// source of the map instead of a control, which squeezes and shifts the result - and a mask
	// large enough to reach the edge of the frame is exactly the case where it happened.
	int script_w = 0, script_h = 0;
	c->ass->GetResolution(script_w, script_h);
	if (!auto_perspective && script_w > 0 && script_h > 0) {
		// A little inside the edge, so the handles on the far side are never flush against it.
		const float inset = 20.f;
		box.half = box.half.Min(Vector2D(script_w * .5f - inset, script_h * .5f - inset));
		box.half = box.half.Max(Vector2D(4.f, 4.f));
		box.centre = box.centre.Max(box.half + Vector2D(inset, inset)).Min(
			Vector2D(script_w - inset, script_h - inset) - box.half);
	}

	BuildAutoPerspectiveBox();
}

VisualToolTransform::TagLine const *VisualToolTransform::ActiveTagLine() const {
	AssDialogue *active = c->selectionController->GetActiveLine();
	auto found = std::find_if(tag_lines.begin(), tag_lines.end(),
		[&](TagLine const& line) { return line.line == active; });
	return found == tag_lines.end() ? nullptr : &*found;
}

bool VisualToolTransform::AutoPerspectiveFitsShape() const {
	TagLine const *found = ActiveTagLine();
	return found && found->drawing;
}

void VisualToolTransform::BuildAutoPerspectiveBox() {
	if (!auto_perspective) return;

	// The four points are drawn where the active line is to end up, so a shape is the source of
	// the map - not the box round everything that happens to be selected. It lands on the points
	// exactly, and the rest of the selection follows it through the same map, keeping its size
	// and its place relative to it.
	//
	// Measured the other way the fit depended on how far the other lines reached, which is not
	// always where they are drawn: a row turned about its anchor sweeps a much larger box than
	// the glyphs occupy, and letter spacing is not in the measurement at all. Both quietly made
	// the block too big and left the active line short of the points drawn to be filled.
	//
	// Only a shape has an outline to be measured though. Text is measured rather than drawn, and
	// one row of a selection is a worse answer than the box round all of it - so for text the
	// box stays as it was built, round everything.
	TagLine const *found = ActiveTagLine();
	if (!found || !found->drawing) {
		if (!source_moved) box.Corners(source_corners);
		return;
	}

	// A drawing is not an upright rectangle. The mask on a sign leans a degree or two, and its
	// upright bounding box then has a triangle of air at every corner - fitted to the four
	// points, that air is fitted with it and the shape lands short of them. The smallest
	// rectangle that holds the outline is the one the shape really lies in, so that is what
	// the box becomes.
	std::vector<Vector2D> hull = SimplifyHull(ConvexHull(ShapeOutline(*found)), 1.0);
	typesetting::OrientedBox tight;
	if (MinimumAreaBox(hull, tight)) {
		// A shape with no direction of its own - a circle, a blob - has a smallest rectangle at
		// almost any angle, and picking one of them would tilt the frame for no reason. The
		// tilt is only taken when it is worth something.
		Vector2D upright[4];
		LineCorners(*found, upright);
		Vector2D low = upright[0], high = upright[0];
		for (int i = 1; i < 4; ++i) { low = low.Min(upright[i]); high = high.Max(upright[i]); }
		double straight = (double)(high.X() - low.X()) * (high.Y() - low.Y());
		double tilted = 4.0 * tight.half.X() * tight.half.Y();
		if (!(straight > 0) || tilted < straight * .98) {
			box = tight;
			box.half = box.half.Max(Vector2D(4.f, 4.f));
			shear_frame_angle = box.angle;
			SeedAutoPerspectiveSource(*found);
			return;
		}
	}

	typesetting::OrientedBox frame;
	frame.angle = found->angle;
	frame.centre = Vector2D(0.f, 0.f);

	Vector2D line_corners[4];
	LineCorners(*found, line_corners);
	Vector2D low, high;
	for (int i = 0; i < 4; ++i) {
		Vector2D local = frame.ToLocal(line_corners[i]);
		if (!i) { low = high = local; }
		else { low = low.Min(local); high = high.Max(local); }
	}

	box.angle = frame.angle;
	shear_frame_angle = frame.angle;
	box.centre = frame.ToScript((low + high) / 2);
	box.half = ((high - low) / 2).Max(Vector2D(4.f, 4.f));
	SeedAutoPerspectiveSource(*found);
}

VisualDraggableFeature *VisualToolTransform::FeatureAt(size_t index) {
	size_t at = 0;
	for (auto& feature : features)
		if (at++ == index) return &feature;
	return nullptr;
}

std::vector<Vector2D> VisualToolTransform::ShapeOutline(TagLine const& found) {
	// The drawing's own points, taken to where the renderer puts them: scaled, shifted by what
	// the alignment does to its box, and turned by \frz about whatever it turns about.
	std::vector<Vector2D> outline = GetLineDrawingPoints(found.line);
	Vector2D fractions = AnchorFractions(found.align);
	Vector2D shift(found.size.X() * fractions.X(), found.size.Y() * fractions.Y());
	Vector2D pivot = found.org ? found.org : found.pos;
	for (auto& point : outline)
		point = RotateAbout(found.pos - shift +
			Vector2D(point.X() * found.scale.X() / 100.f,
			         point.Y() * found.scale.Y() / 100.f),
			pivot, found.angle);
	return outline;
}

void VisualToolTransform::SeedAutoPerspectiveSource(TagLine const& found) {
	if (source_moved) return;
	box.Corners(source_corners);

	// A rectangle is only ever an approximation of a shape. When the shape is itself a
	// quadrilateral - which a hand-drawn sign mask nearly always is - its own four corners are
	// the exact answer, and nothing is left over at them to be stretched with the rest.
	std::vector<Vector2D> quad = SimplifyHull(ConvexHull(ShapeOutline(found)), 2.0);
	if (quad.size() != 4) return;

	// In the order the box uses, so a corner of the source and a corner of the target mean the
	// same corner of the picture.
	bool taken[4] = {false, false, false, false};
	for (int i = 0; i < 4; ++i) {
		double best = 0;
		int at = -1;
		for (int j = 0; j < 4; ++j) {
			if (taken[j]) continue;
			double away = (quad[j] - source_corners[i]).Len();
			if (at >= 0 && !(away < best)) continue;
			best = away;
			at = j;
		}
		if (at < 0) return;
		taken[at] = true;
		source_corners[i] = quad[at];
	}
}

typesetting::PointMap VisualToolTransform::AutoPerspectiveMap() const {
	auto forward = typesetting::QuadMap(box, corners);
	// Until the four points are drawn there is no target, and the source must not act on its
	// own: it would distort the lines before anything had been asked of it.
	if (!touched) return forward;

	Vector2D upright[4];
	box.Corners(upright);
	bool untouched = true;
	for (int i = 0; i < 4; ++i)
		untouched = untouched && (source_corners[i] - upright[i]).Len() < 1e-3f;
	if (untouched) return forward;

	auto inverse = typesetting::QuadInverseMap(box, source_corners);
	return [inverse, forward](Vector2D point) { return forward(inverse(point)); };
}

namespace {
	/// A map done in the frame of a box lying at some angle: into that frame, the map, and
	/// back out again.
	Matrix2 InFrame(float box_angle, Matrix2 const& map) {
		double radians = box_angle * pi / 180.0;
		double sine = std::sin(radians), cosine = std::cos(radians);
		Matrix2 into{cosine, sine, -sine, cosine};
		Matrix2 out_of{cosine, -sine, sine, cosine};
		return Multiply(out_of, Multiply(map, into));
	}

	/// The gesture as a linear map: the scale along the box's axes, then the turn, then the
	/// lean - in the order they are applied to what is on screen.
	Matrix2 GestureMatrix(float box_angle, Vector2D scale, float turn,
	                      float shear_angle, Vector2D shear) {
		Matrix2 scaled{scale.X(), 0, 0, scale.Y()};
		Matrix2 leaning{1, shear.X(), shear.Y(), 1};
		return Multiply(InFrame(shear_angle, leaning),
			Multiply(Turn(turn), InFrame(box_angle, scaled)));
	}
}

Vector2D VisualToolTransform::GesturePivot() const {
	// The middle of the box as it was when this gesture began. Deliberately not the middle
	// of the *scaled* box: that would depend on the scale, the scale is measured against a
	// point this pivot places, and the two would chase each other.
	return box.centre + gesture_move;
}

namespace {
	/// How far the renderer shifts a line sideways for its lean.
	///
	/// It leans text about the top of its box rather than about the point it is anchored at,
	/// and the amount goes with \fscx - libass multiplies \fax by scale_x/scale_y and then
	/// applies it to coordinates that have been scaled by scale_y. A drawing leans about the
	/// point it sits at, so for one of those this is nothing.
	Vector2D LeanOffset(double ascent, double shear_x, double scale_x, double angle) {
		double reach = ascent * shear_x * scale_x / 100.0;
		if (std::abs(reach) < 1e-9) return Vector2D(0.f, 0.f);
		double radians = angle * pi / 180.0;
		return Vector2D((float)(reach * std::cos(radians)), (float)(-reach * std::sin(radians)));
	}
}

TransformMatrix2 VisualToolTransform::TotalMatrix() const {
	return Multiply(frame_linear, GestureMatrix(box.angle, gesture_scale, gesture_angle,
		shear_frame_angle, gesture_shear));
}

bool VisualToolTransform::Perspective(TagLine const& original) {
	return std::abs(original.angle_x) > 1e-9 || std::abs(original.angle_y) > 1e-9;
}

void VisualToolTransform::LineQuad(TagLine const& original, Vector2D corners[4]) const {
	typesetting::PerspectiveTags tags;
	tags.pos = original.pos;
	tags.org = original.org ? original.org : original.pos;
	tags.scale = original.scale;
	tags.shear_x = original.shear.X();
	tags.shear_y = original.shear.Y();
	tags.angle_z = original.angle;
	tags.angle_x = original.angle_x;
	tags.angle_y = original.angle_y;
	typesetting::PerspectiveQuad(tags, original.align, original.box_first, original.box_second,
		script_res / layout_res, corners);
}

bool VisualToolTransform::PerspectiveMovePlane(Vector2D const held_corners[4],
	typesetting::OrientedBox& source, Vector2D projected[4]) const {
	// The distortion is applied after the renderer's authored perspective. Compose the current
	// distortion onto a representative line plane, so moving through it works both before and
	// after a corner of the distortion frame has been changed.
	auto held_distortion = typesetting::QuadMap(box, held_corners);
	double best_area = 0;
	for (auto const& found : tag_lines) {
		if (!Perspective(found)) continue;

		Vector2D line_corners[4], moved_corners[4];
		LineQuad(found, line_corners);
		bool finite = true;
		for (int i = 0; i < 4; ++i) {
			moved_corners[i] = held_distortion(line_corners[i]);
			finite = finite && std::isfinite(moved_corners[i].X()) &&
				std::isfinite(moved_corners[i].Y());
		}
		if (!finite) continue;

		double twice_area = 0;
		for (int i = 0; i < 4; ++i)
			twice_area += moved_corners[i].X() * moved_corners[(i + 1) % 4].Y() -
				moved_corners[(i + 1) % 4].X() * moved_corners[i].Y();
		double area = std::abs(twice_area) * .5;
		if (!(area > best_area) || area < 1e-3) continue;

		best_area = area;
		source.angle = 0;
		source.centre = (found.box_first + found.box_second) / 2;
		source.half = ((found.box_second - found.box_first) / 2).Max(
			Vector2D(.5f, .5f));
		std::copy(moved_corners, moved_corners + 4, projected);
	}
	return best_area > 0;
}

VisualToolTransform::Applied VisualToolTransform::ApplyGesture(
	TagLine const& original) const {
	Applied out;

	// A line turned out of the plane is a quadrilateral on screen, and only its corners can
	// be followed: a map applied after the projection is not one that can be applied before
	// it. So the corners are moved and the tags are solved again from where they land. The
	// distort takes the same road whatever the line says, because a projective map turns even
	// a plain rectangle into a quadrilateral.
	if (mode == VisualToolTransformMode::Distort || Perspective(original)) {
		Vector2D corners[4];
		LineQuad(original, corners);
		for (auto& corner : corners) {
			// Auto perspective sizes the selection as one object in the source plane. Scaling
			// every line around its own anchor would change the glyphs but leave the distances
			// between rows behind; doing it around the common box centre keeps the whole layout
			// proportional, and mapping afterwards makes it follow the target perspective.
			if (auto_perspective && !auto_perspective_keep_original_size) {
				float factor = static_cast<float>(auto_perspective_size / 100.0);
				corner = box.centre + (corner - box.centre) * factor;
			}
			corner = MapPoint(corner);
		}

		Vector2D pivot = original.org ? original.org : original.pos;
		// A distortion is most stable when expressed around the quad itself. Keeping a remote
		// authored origin is mathematically equivalent, but produces extreme pos/scale values
		// and loses precision. Free transform keeps its origin because there the compact affine
		// form is normally retained.
		Vector2D preferred_org = mode == VisualToolTransformMode::Distort ?
			Vector2D() : MapPoint(pivot);
		auto solved = typesetting::SolvePerspective(corners, original.align,
			original.box_first, original.box_second, script_res / layout_res,
			preferred_org);
		// A preferred origin can still leave the arithmetic with nothing to solve. The middle
		// of the quadrilateral is the stable fallback.
		if (!solved.ok)
			solved = typesetting::SolvePerspective(corners, original.align,
				original.box_first, original.box_second, script_res / layout_res, Vector2D());
		if (solved.ok && auto_perspective_keep_original_size) {
			// The four target points still supply the plane and its perspective, but the
			// authored scales supply its size. Shifting pos and org together is an exact
			// screen translation, so the smaller/larger result stays centred on the target.
			solved.scale = original.scale;
			Vector2D kept_corners[4];
			typesetting::PerspectiveQuad(solved, original.align, original.box_first,
				original.box_second, script_res / layout_res, kept_corners);
			Vector2D wanted_centre, kept_centre;
			for (int i = 0; i < 4; ++i) {
				wanted_centre = wanted_centre + corners[i];
				kept_centre = kept_centre + kept_corners[i];
			}
			Vector2D shift = (wanted_centre - kept_centre) / 4.f;
			solved.pos = solved.pos + shift;
			solved.org = solved.org + shift;
		}
		if (solved.ok) {
			out.perspective = true;
			out.pos = solved.pos;
			out.org = solved.org;
			out.scale = solved.scale;
			out.shear_x = solved.shear_x;
			out.shear_y = solved.shear_y;
			out.angle = solved.angle_z;
			out.angle_x = solved.angle_x;
			out.angle_y = solved.angle_y;
			return out;
		}
		// Nothing came of it - a quadrilateral folded over itself has no plane behind it - so
		// the line is left to the ordinary path rather than written out as nonsense.
	}

	// The frame and the gesture, then whatever the line already had.
	Matrix2 combined = Multiply(TotalMatrix(),
		LineMatrix(original.scale, original.shear, original.angle));

	out.angle = original.angle;
	out.shear_x = original.shear.X();
	out.scale = original.scale;
	// The line keeps its own \fay, and everything else is solved around it.
	SplitMatrix(combined, original.shear.Y(), out.angle, out.shear_x, out.scale);

	// How far above the point it is anchored at the renderer leans it, in unscaled units.
	double ascent = 0;
	if (!original.drawing && std::abs(original.scale.Y()) > 1e-6)
		ascent = original.size.Y() / (original.scale.Y() / 100.0) *
			AnchorFractions(original.align).Y();

	// Where the line really is now, where it has to end up, and what to write so that it
	// does - the lean shift has to be taken off the one and put back on the other.
	Vector2D standing = original.pos +
		LeanOffset(ascent, original.shear.X(), original.scale.X(), original.angle);
	out.pos = MapPoint(standing) - LeanOffset(ascent, out.shear_x, out.scale.X(), out.angle);
	out.org = original.org ? MapPoint(original.org) : original.org;
	return out;
}

Vector2D VisualToolTransform::FramePoint(Vector2D point) const {
	Vector2D from = point - box.centre;
	return box.centre + frame_offset +
		Vector2D((float)(frame_linear.a * from.X() + frame_linear.b * from.Y()),
		         (float)(frame_linear.c * from.X() + frame_linear.d * from.Y()));
}

Vector2D VisualToolTransform::FrameInverse(Vector2D point) const {
	double determinant = frame_linear.a * frame_linear.d - frame_linear.b * frame_linear.c;
	if (std::abs(determinant) < 1e-12) return point;
	Vector2D from = point - box.centre - frame_offset;
	return box.centre + Vector2D(
		(float)((frame_linear.d * from.X() - frame_linear.b * from.Y()) / determinant),
		(float)((frame_linear.a * from.Y() - frame_linear.c * from.X()) / determinant));
}

Vector2D VisualToolTransform::FrameGrowth() const {
	if (mode == VisualToolTransformMode::Distort) {
		// A projective map has no one scale: it stretches the far end more than the near one.
		// The average of each pair of opposite sides against the box it started as is what a
		// border can actually follow.
		Vector2D outline[4];
		box.Corners(outline);
		Vector2D moved[4];
		for (int i = 0; i < 4; ++i) moved[i] = MapPoint(outline[i]);
		double across = ((moved[1] - moved[0]).Len() + (moved[2] - moved[3]).Len()) / 2;
		double down = ((moved[3] - moved[0]).Len() + (moved[2] - moved[1]).Len()) / 2;
		double was_across = std::max(box.half.X() * 2.f, 1e-6f);
		double was_down = std::max(box.half.Y() * 2.f, 1e-6f);
		return Vector2D((float)(across / was_across), (float)(down / was_down));
	}

	double angle = 0, lean = 0;
	Vector2D grown(100.f, 100.f);
	SplitMatrix(TotalMatrix(), 0, angle, lean, grown);
	return Vector2D(std::abs(grown.X()) / 100.f, std::abs(grown.Y()) / 100.f);
}

bool VisualToolTransform::SquareOn() const {
	// A distort is never square-on: four corners that could be dragged into a rectangle again
	// would still have been dragged.
	if (mode == VisualToolTransformMode::Distort) return false;
	Matrix2 whole = TotalMatrix();
	return std::abs(whole.b) < 1e-6 && std::abs(whole.c) < 1e-6 &&
		whole.a > 0 && whole.d > 0;
}

Vector2D VisualToolTransform::MapPoint(Vector2D point) const {
	// The distort is a quadrilateral rather than a scale and a turn, so where its handles have
	// been put is the whole of what it says.
	if (mode == VisualToolTransformMode::Distort)
		return distort_map ? distort_map(point) : point;
	return FramePoint(GesturePoint(point));
}

Vector2D VisualToolTransform::GesturePoint(Vector2D point) const {
	Vector2D local = box.ToLocal(point);
	local = gesture_anchor + (local - gesture_anchor) * gesture_scale;
	Vector2D turned = RotateAbout(box.ToScript(local), box.centre, gesture_angle);

	// The lean comes last, about the middle of the box, so both edges give a little rather
	// than one edge doing all the moving.
	if (std::abs(gesture_shear.X()) > 1e-9 || std::abs(gesture_shear.Y()) > 1e-9) {
		typesetting::OrientedBox frame;
		frame.angle = shear_frame_angle;
		frame.centre = box.centre;
		Vector2D in_frame = frame.ToLocal(turned);
		turned = frame.ToScript(Vector2D(
			in_frame.X() + (float)gesture_shear.X() * in_frame.Y(),
			in_frame.Y() + (float)gesture_shear.Y() * in_frame.X()));
	}
	return turned + gesture_move;
}

void VisualToolTransform::RebaseGesture() {
	if (mode != VisualToolTransformMode::Free) return;

	// The gesture, as a map about the middle of the box, composed onto the frame. A
	// multiplication and one translation: nothing is measured again, so the box keeps the
	// shape it has taken instead of springing back to a rectangle.
	Matrix2 gesture = GestureMatrix(box.angle, gesture_scale, gesture_angle,
		shear_frame_angle, gesture_shear);
	Vector2D shift = GesturePoint(box.centre) - box.centre;

	frame_offset = frame_offset +
		Vector2D((float)(frame_linear.a * shift.X() + frame_linear.b * shift.Y()),
		         (float)(frame_linear.c * shift.X() + frame_linear.d * shift.Y()));
	frame_linear = Multiply(frame_linear, gesture);

	gesture_scale = Vector2D(1.f, 1.f);
	gesture_angle = 0;
	gesture_move = Vector2D(0.f, 0.f);
	gesture_anchor = Vector2D(0.f, 0.f);
	gesture_shear = Vector2D(0.f, 0.f);
}

std::string VisualToolTransform::TagLineText(TagLine const& original) const {
	// Opening Auto perspective is not itself an edit. Until four points have produced a target,
	// the renderer must receive the authored line byte-for-byte; tag neutralisation belongs only
	// to the internal calculation state.
	if (auto_perspective && !touched)
		return original.line->Text.get();

	AssDialogue copy(*original.line);
	auto *tool = const_cast<VisualToolTransform *>(this);
	if (auto_perspective)
		for (auto const *tag : {"\\frx", "\\fry", "\\fax", "\\fay", "\\org"})
			tool->RemoveOverride(&copy, tag);
	if (original.ensure_q2)
		tool->SetOverride(&copy, "\\q", "2");

	// One map for the whole selection; what each line has to say to follow it depends on the
	// map it already had. Compared with what the line said when the tool opened, not with the
	// standing start a gesture is measured from, so that only what really changed is written.
	Applied applied = ApplyGesture(original);
	Vector2D moved = applied.pos;
	Vector2D scale = applied.scale;
	double angle = applied.angle;
	double shear_x = applied.shear_x;

	// Anything the line sets again partway through would override what is written just below,
	// so it is dealt with first - while the start of the line still holds the values those
	// later blocks were read against.
	{
		Vector2D ratio(
			std::abs(original.start_scale.X()) > 1e-6 ?
				scale.X() / original.start_scale.X() : 1.f,
			std::abs(original.start_scale.Y()) > 1e-6 ?
				scale.Y() / original.start_scale.Y() : 1.f);
		// The border and the shadow keep their size below, so a later block's are left as they
		// are; only the blur, which is a spread in pixels of the finished picture, follows how
		// much bigger the letters became.
		float by_x = 1.f, by_y = 1.f;
		AdjustLaterTags(&copy, original, ratio, angle - original.start_angle, by_x, by_y,
			std::sqrt(std::max<double>(std::abs(ratio.X() * ratio.Y()), 1e-9)));
	}

	auto [first_end, second_end] = MovedEnds(original, applied);
	if (original.placed.moving) {
		// A line that moves goes on moving: both ends of its run are taken through the gesture, so
		// the whole run follows and not only the frame that was looked at.
		auto [name, value] = text_to_shape::PlacementOverride(original.placed,
			first_end, second_end);
		tool->SetOverride(&copy, name, value);
	}
	else if ((moved - original.start_pos).Len() > .005f)
		tool->SetOverride(&copy, "\\pos", moved.PStr());
	// Out of the plane the origin is part of the answer rather than something carried along,
	// so it is always written - the corners only come out right with the one it was solved for.
	if (applied.perspective) {
		tool->SetOverride(&copy, "\\org", applied.org.PStr());
		tool->SetOverride(&copy, "\\frx", Number(applied.angle_x));
		tool->SetOverride(&copy, "\\fry", Number(applied.angle_y));
		if (std::abs(applied.shear_y - original.start_shear.Y()) > 1e-4)
			tool->SetOverride(&copy, "\\fay", Number(applied.shear_y));
	}
	else if (original.org)
		tool->SetOverride(&copy, "\\org", applied.org.PStr());
	if (std::abs(scale.X() - original.start_scale.X()) > 1e-3)
		tool->SetOverride(&copy, "\\fscx", Number(scale.X()));
	if (std::abs(scale.Y() - original.start_scale.Y()) > 1e-3)
		tool->SetOverride(&copy, "\\fscy", Number(scale.Y()));
	if (std::abs(angle - original.start_angle) > 1e-3)
		tool->SetOverride(&copy, "\\frz", Number(angle));
	if (std::abs(shear_x - original.start_shear.X()) > 1e-4)
		tool->SetOverride(&copy, "\\fax", Number(shear_x));

	// The border keeps the width it was asked for, which means it is left exactly as the line said
	// it - the renderer divides it by the scale before stroking, so a scale never moved it anyway.
	//
	// Compensating an upright pen for a lean was tried and is a trap: what it takes to give back a
	// round pen through a lean of one is a pen half again as wide, and through a lean of three,
	// three times - so the border stretched instead of holding still, and which way it stretched
	// depended on which way the lean went. The renderer leans the pen along with the letters, which
	// is what it does to their own strokes too, and that is the honest answer.
	Vector2D kept_bord = original.bord;

	// The offset takes the direction the gesture took it, and a length that keeps the gap looking
	// the same: a shadow is read against how wide the letters are, so it follows how much narrower
	// or wider they became. Keeping the pixels instead leaves a gap that reads twice as wide on
	// letters half as wide, and following the gesture faithfully would make it longer still.
	//
	// Only the turn acts on the offset, so only the turn has to be undone at the end.
	Vector2D kept_shad = original.shad;
	Vector2D shown = ApplyMatrix(Turn(original.start_angle), original.shad);
	Vector2D taken = ApplyMatrix(TotalMatrix(), shown);
	Matrix2 unturn;
	if (shown.Len() > 1e-6 && taken.Len() > 1e-6 && Invert(Turn(angle), unturn)) {
		double narrowed = std::abs(original.start_scale.X()) > 1e-6 ?
			std::abs(scale.X() / original.start_scale.X()) : 1.0;
		double wanted = shown.Len() * narrowed;
		kept_shad = ApplyMatrix(unturn, taken * (float)(wanted / taken.Len()));
	}

	auto write_pair = [&](Vector2D value, Vector2D was, const char *both,
	                      const char *along, const char *across) {
		if ((value - was).Len() < 1e-3) return;
		// One tag while both axes agree, which is how a line is usually written; two only
		// when they have to differ.
		if (std::abs(value.X() - value.Y()) < 1e-4)
			tool->SetOverride(&copy, both, Number(value.X()));
		else {
			tool->SetOverride(&copy, along, Number(value.X()));
			tool->SetOverride(&copy, across, Number(value.Y()));
		}
	};

	if (recalc_bord)
		write_pair(kept_bord, original.start_bord, "\\bord", "\\xbord", "\\ybord");
	if (recalc_shad)
		write_pair(kept_shad, original.start_shad, "\\shad", "\\xshad", "\\yshad");
	// With the two of them standing on their own lines as shapes, the letters must not draw them
	// again - and the shapes are exact, where a pen could only have been approximate.
	if (maintain_decor && !original.decor.empty()) {
		tool->SetOverride(&copy, "\\bord", "0");
		tool->SetOverride(&copy, "\\shad", "0");
	}
	if (recalc_clip && original.clip.present) {
		// A rectangle that has been turned is a quadrilateral now, so the tag changes form -
		// and SetOverride takes the other form of the pair out for us.
		std::string mapped = MapClip(original.clip);
		if (!mapped.empty())
			tool->SetOverride(&copy, original.clip.inverse ? "\\iclip" : "\\clip",
				"(" + mapped + ")");
	}
	if (recalc_blur) {
		// A blur is a spread in pixels of the finished picture, so unlike the border and the
		// shadow it does follow how much bigger the letters became - and having no direction, it
		// follows the two scales together.
		double grew_x = std::abs(original.start_scale.X()) > 1e-6 ?
			scale.X() / original.start_scale.X() : 1.0;
		double grew_y = std::abs(original.start_scale.Y()) > 1e-6 ?
			scale.Y() / original.start_scale.Y() : 1.0;
		double grow = std::sqrt(std::max(std::abs(grew_x * grew_y), 1e-9));
		double blur = original.blur * grow;
		double be = original.be * grow;
		if (std::abs(blur - original.start_blur) > 1e-3)
			tool->SetOverride(&copy, "\\blur", Number(blur));
		if (std::abs(be - original.start_be) > .5)
			tool->SetOverride(&copy, "\\be", Number(std::round(be)));
	}

	return copy.Text.get();
}

namespace {
	/// A convex shape cut down to where some measure of a point is nought or more: Sutherland
	/// and Hodgman's method. The measure has to be affine - how far past a straight edge a
	/// point is, or how far it is from a vanishing line - so that where an edge crosses can be
	/// found by interpolating along it. Fewer than three points come back when nothing is left.
	template<typename Measure>
	std::vector<Vector2D> CutToHalfPlane(std::vector<Vector2D> shape, Measure measure) {
		if (shape.size() < 3) return {};
		std::vector<Vector2D> kept;
		kept.reserve(shape.size() + 2);
		for (size_t at = 0; at < shape.size(); ++at) {
			Vector2D current = shape[at];
			Vector2D next = shape[(at + 1) % shape.size()];
			double now = measure(current), then = measure(next);
			if (now >= 0) kept.push_back(current);
			if ((now >= 0) != (then >= 0)) {
				double along = std::abs(now - then) < 1e-12 ? 0. : now / (now - then);
				kept.push_back(current + (next - current) * (float)along);
			}
		}
		return kept.size() >= 3 ? kept : std::vector<Vector2D>();
	}

	/// The same, cut down to a box - which is four of those, in the box's own frame.
	std::vector<Vector2D> CutToBox(std::vector<Vector2D> shape,
	                               typesetting::OrientedBox const& box, Vector2D margin) {
		Vector2D limit = box.half + margin;
		shape = CutToHalfPlane(std::move(shape),
			[&](Vector2D at) { return box.ToLocal(at).X() + limit.X(); });
		shape = CutToHalfPlane(std::move(shape),
			[&](Vector2D at) { return limit.X() - box.ToLocal(at).X(); });
		shape = CutToHalfPlane(std::move(shape),
			[&](Vector2D at) { return box.ToLocal(at).Y() + limit.Y(); });
		shape = CutToHalfPlane(std::move(shape),
			[&](Vector2D at) { return limit.Y() - box.ToLocal(at).Y(); });
		return shape;
	}
}

std::string VisualToolTransform::MapClip(TagLine::Clip const& clip) const {
	if (!clip.present) return {};

	// Square-on means no turn, no lean and no mirroring - and that has to be asked of the map
	// as a whole, not of the gesture being made. Asking only the gesture meant that on a logo
	// turned earlier, a plain scale still counted as square-on, and every band of a gradient
	// was written as the bounding box of its turned self.
	bool square_on = SquareOn();

	if (clip.rectangle) {
		Vector2D low = clip.first.Min(clip.second);
		Vector2D high = clip.first.Max(clip.second);

		if (square_on) {
			// Nothing to pad: a rectangular clip snaps to whole pixels, so two bands that share
			// an edge already tile without help.
			Vector2D first = MapPoint(low), second = MapPoint(high);
			Vector2D at = first.Min(second), to = first.Max(second);
			return agi::format("%s,%s,%s,%s", Number(at.X()), Number(at.Y()),
				Number(to.X()), Number(to.Y()));
		}

		// A turned rectangle is a quadrilateral, and only a drawing can say that - but a
		// drawing clip is drawn with soft edges, while a rectangular one snaps to whole pixels.
		// Two bands of a gradient that shared an edge would each cover it half way and leave a
		// dark seam, so the band is grown by half a unit on screen: which is half a unit
		// divided by however much the frame has grown, and never more than half the band
		// itself, or one band would swallow the next.
		Vector2D growth = FrameGrowth();
		Vector2D pad(
			std::min((high.X() - low.X()) * .5f, .5f / std::max(growth.X(), 1e-6f)),
			std::min((high.Y() - low.Y()) * .5f, .5f / std::max(growth.Y(), 1e-6f)));
		low = low - pad;
		high = high + pad;

		std::vector<Vector2D> shape = {
			low, Vector2D(high.X(), low.Y()), high, Vector2D(low.X(), high.Y())
		};

		// The part of the band that is nowhere near the box cuts nothing, and under a distort
		// it is also the part that reaches across the line the map sends to infinity. Cut it
		// away first and every coordinate stays where it belongs. The margin leaves room for
		// what spreads beyond the text itself - a border, a glow.
		if (mode == VisualToolTransformMode::Distort) {
			shape = CutToBox(std::move(shape), box, box.half * .25f + Vector2D(24.f, 24.f));
			// And to the side of the plane the map still says anything about. When the shape is
			// nearly folded over, the line it sends to infinity comes close enough to the box
			// that a band can still straddle it - and a band that does comes back inside out,
			// covering everything drawn before it.
			//
			// Where to stop is measured against the box itself: half of the least it has to put
			// up with anywhere on its own corners. Past that a point is magnified more than
			// twice as hard as anything the text goes through, which is far enough out to be of
			// no interest to a clip.
			Vector2D outline[4];
			box.Corners(outline);
			double least = 1e9;
			for (auto const& corner : outline)
				least = std::min(least, typesetting::QuadDepth(box, corners, corner));
			double floor_depth = std::max(least * .5, 1e-3);
			shape = CutToHalfPlane(std::move(shape), [&](Vector2D at) {
				return typesetting::QuadDepth(box, corners, at) - floor_depth;
			});
		}
		if (shape.size() < 3) return {};

		for (auto& point : shape) point = MapPoint(point);
		// And if something still came back meaningless, the clip is better left as it was than
		// written out wrong.
		for (auto const& point : shape)
			if (!std::isfinite(point.X()) || !std::isfinite(point.Y()) ||
				std::abs(point.X()) > 1e5f || std::abs(point.Y()) > 1e5f) return {};

		std::string out = "m";
		for (size_t at = 0; at < shape.size(); ++at)
			out += " " + Number(shape[at].X()) + " " + Number(shape[at].Y()) +
				(at ? "" : " l");
		return out;
	}

	// A drawing: every pair of numbers is a point, whatever command it belongs to, so the
	// commands can be copied across untouched.
	double divisor = static_cast<double>(1 << std::max(0, clip.scale - 1));
	std::string out;
	double pending = 0;
	bool have_pending = false;
	size_t at = 0;
	while (at < clip.drawing.size()) {
		while (at < clip.drawing.size() &&
			std::isspace(static_cast<unsigned char>(clip.drawing[at]))) ++at;
		if (at >= clip.drawing.size()) break;

		size_t end = at;
		if (std::isalpha(static_cast<unsigned char>(clip.drawing[at]))) {
			while (end < clip.drawing.size() &&
				std::isalpha(static_cast<unsigned char>(clip.drawing[end]))) ++end;
			if (!out.empty()) out += ' ';
			out += clip.drawing.substr(at, end - at);
			at = end;
			continue;
		}

		while (end < clip.drawing.size() && !std::isspace(static_cast<unsigned char>(
			clip.drawing[end])) && !std::isalpha(static_cast<unsigned char>(
			clip.drawing[end]))) ++end;
		double value = 0;
		try { value = std::stod(clip.drawing.substr(at, end - at)); }
		catch (...) { at = end; continue; }
		at = end;

		if (!have_pending) { pending = value; have_pending = true; continue; }
		have_pending = false;
		Vector2D mapped = MapPoint(Vector2D((float)(pending / divisor),
		                                    (float)(value / divisor)));
		// A drawn clip cannot be cut down to the box the way a band can - it may be any shape at
		// all - so a point that has landed out past infinity is taken as a sign to leave the
		// whole clip alone rather than write a shape nobody meant.
		if (!std::isfinite(mapped.X()) || !std::isfinite(mapped.Y()) ||
			std::abs(mapped.X()) > 1e5f || std::abs(mapped.Y()) > 1e5f) return {};
		out += ' ' + Number(mapped.X() * divisor) + ' ' + Number(mapped.Y() * divisor);
	}

	if (out.empty()) return {};
	return clip.scale != 1 ? agi::format("%d,%s", clip.scale, out) : out;
}

void VisualToolTransform::HandleRole(int index, Vector2D& grabbed, Vector2D& anchor,
                                    int& role) const {
	// Four corners to size it both ways, then the middles of the four sides to size it one
	// way, then a leaning handle beyond each side. Turning is done outside the box.
	// What a handle drags is one point of the box; what stays is the point across from it.
	static const Vector2D corners[4] = {
		Vector2D(-1.f, -1.f), Vector2D(1.f, -1.f), Vector2D(1.f, 1.f), Vector2D(-1.f, 1.f)
	};
	static const Vector2D sides[4] = {
		Vector2D(0.f, -1.f), Vector2D(1.f, 0.f), Vector2D(0.f, 1.f), Vector2D(-1.f, 0.f)
	};

	role = index < 8 ? 0 : index < 12 ? 2 : 3;
	Vector2D unit = index < 4 ? corners[index] :
		index < 8 ? sides[index - 4] :
		index < 12 ? sides[index - 8] : sides[index - 12];
	grabbed = Vector2D(unit.X() * box.half.X(), unit.Y() * box.half.Y());
	anchor = Vector2D(-unit.X() * box.half.X(), -unit.Y() * box.half.Y());
}

void VisualToolTransform::AdjustLaterTags(AssDialogue *line, TagLine const& original,
                                         Vector2D scale_ratio, double turn, double grow_x,
                                         double grow_y, double grow) const {
	auto blocks = line->ParseTags();
	Matrix2 whole = TotalMatrix();

	// What is in force as the line is read. A block partway through changes it from there on,
	// which is exactly why its values have to be worked out rather than left alone.
	Vector2D state_scale = original.start_scale;
	Vector2D state_shear = original.start_shear;
	double state_angle = original.start_angle;

	bool first_block = true;
	bool changed = false;

	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		auto& tags = static_cast<AssDialogueBlockOverride*>(block.get())->Tags;

		// The state as this block leaves it, read before anything is written over it.
		for (auto const& tag : tags) {
			if (tag.Params.empty() || tag.Params[0].omitted) continue;
			std::string const& name = tag.Name;
			double value = tag.Params[0].Get<double>(0);
			if (name == "\\fscx") state_scale = Vector2D((float)value, state_scale.Y());
			else if (name == "\\fscy") state_scale = Vector2D(state_scale.X(), (float)value);
			else if (name == "\\frz" || name == "\\fr") state_angle = value;
			else if (name == "\\fax") state_shear = Vector2D((float)value, state_shear.Y());
			else if (name == "\\fay") state_shear = Vector2D(state_shear.X(), (float)value);
		}

		if (first_block) {
			// The first block is where the transform writes its own values; the ones after it
			// are what would otherwise undo them.
			first_block = false;
			continue;
		}

		// What this stretch of the line has to say to follow the same map. A scale or a turn
		// leaves the lean alone - the renderer applies it after the scale - but a lean does not,
		// so it is solved for rather than scaled.
		double solved_angle = state_angle;
		double solved_shear = state_shear.X();
		Vector2D solved_scale = state_scale;
		bool solved = SplitMatrix(
			Multiply(whole, LineMatrix(state_scale, state_shear, state_angle)),
			state_shear.Y(), solved_angle, solved_shear, solved_scale);

		for (auto& tag : tags) {
			if (tag.Params.empty() || tag.Params[0].omitted) continue;
			std::string const& name = tag.Name;
			double value = tag.Params[0].Get<double>(0);

			auto write = [&](double result) {
				tag.Params[0].Set(result);
				changed = true;
			};

			if (name == "\\fscx") write(value * scale_ratio.X());
			else if (name == "\\fscy") write(value * scale_ratio.Y());
			else if (name == "\\frz" || name == "\\fr") write(value + turn);
			else if (name == "\\fax") { if (solved) write(solved_shear); }
			// Unticked means untouched, wherever in the line the tag sits.
			else if (name == "\\bord" || name == "\\xbord") {
				if (recalc_bord) write(value * grow_x);
			}
			else if (name == "\\ybord") { if (recalc_bord) write(value * grow_y); }
			else if (name == "\\shad" || name == "\\xshad") {
				if (recalc_shad) write(value * grow_x);
			}
			else if (name == "\\yshad") { if (recalc_shad) write(value * grow_y); }
			else if (name == "\\blur" || name == "\\be") {
				if (recalc_blur) write(value * grow);
			}
		}
	}

	if (changed) line->UpdateText(blocks);
}

VisualToolTransform::TagLine::Clip VisualToolTransform::ReadClip(AssDialogue *line) {
	TagLine::Clip out;
	for (auto& block : line->ParseTags()) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags) {
			if (tag.Name != "\\clip" && tag.Name != "\\iclip") continue;
			auto const& params = tag.Params;
			out.present = true;
			out.inverse = tag.Name == "\\iclip";

			if (params.size() >= 4) {
				out.rectangle = true;
				out.first = Vector2D(params[0].Get<float>(0), params[1].Get<float>(0));
				out.second = Vector2D(params[2].Get<float>(0), params[3].Get<float>(0));
			}
			else if (params.size() == 2) {
				out.scale = params[0].Get<int>(1);
				out.drawing = params[1].Get<std::string>("");
			}
			else if (params.size() == 1) {
				out.drawing = params[0].Get<std::string>("");
			}
			else out.present = false;
		}
	}
	return out;
}

/// Read the selection's drawings, converting any text in memory as it goes.
void VisualToolTransform::Collect() {
	// What was being worked out, in case the shapes turn out to be the same ones: a
	// refresh is not a reason to throw a reshaping away.
	bool had_editor = editor.has_value();
	typesetting::OrientedBox old_box = box;
	typesetting::WarpNet old_net = net;
	Vector2D old_corners[4];
	for (int i = 0; i < 4; ++i) old_corners[i] = corners[i];
	bool was_touched = touched;

	features.clear();
	sel_features.clear();
	editor.reset();
	touched = false;

	AssDialogue *line = c->selectionController->GetActiveLine();
	if (line != session_line) {
		// Another line is another job, and the history belonged to the old one.
		session_line = line;
		source_moved = false;
		reported = false;
		undo_history.clear();
		redo_history.clear();
	}

	if (!typesetting::CanTransform(c)) return;

	// A line that is not on screen has nothing to preview, so the tool does not start on
	// one - it hands the video straight back instead of sitting there with no handles.
	if (!active_line) {
		ExitTool();
		return;
	}

	// A textbox document expresses its rectangle through its generated rows. Treating it as
	// one document here would resize that rectangle; Auto perspective deliberately places
	// the individual rows without changing their dimensions.
	if (!auto_perspective) CollectTextBox();
	else {
		textbox_document.reset();
		textbox_lines.clear();
	}

	// These modes say everything in tags, so they need no drawings and convert nothing: they
	// read the lines as they are.
	if (TagsMode()) {
		bool had_lines = !tag_lines.empty();

		tag_lines.clear();
		if (!CollectTags()) return;
		if (TextBoxMode()) {
			Vector2D edge = textbox_original_corners[1] - textbox_original_corners[0];
			box.angle = static_cast<float>(std::atan2(edge.Y(), edge.X()) * 180.0 / pi);
			box.centre = (textbox_original_corners[0] + textbox_original_corners[1] +
				textbox_original_corners[2] + textbox_original_corners[3]) / 4;
			box.half = Vector2D(
				(textbox_original_corners[1] - textbox_original_corners[0]).Len() * .25f +
					(textbox_original_corners[2] - textbox_original_corners[3]).Len() * .25f,
				(textbox_original_corners[3] - textbox_original_corners[0]).Len() * .25f +
					(textbox_original_corners[2] - textbox_original_corners[1]).Len() * .25f);
			box.half = box.half.Max(Vector2D(4.f, 4.f));
		}

		// The corners start on the untouched box, so nothing moves until one is dragged -
		// unless the same box came back, which means the same lines did, and then they go back
		// where the user left them. This is what keeps a distort alive through a zoom or a pan.
		if (TextBoxMode()) std::copy(textbox_original_corners, textbox_original_corners + 4, corners);
		else box.Corners(corners);
		if (mode == VisualToolTransformMode::Distort && had_lines && was_touched &&
			(old_box.centre - box.centre).Len() < .01 &&
			(old_box.half - box.half).Len() < .01 &&
			std::abs(old_box.angle - box.angle) < .01) {
			for (int i = 0; i < 4; ++i) corners[i] = old_corners[i];
			touched = true;
		}

		// The distort is a projective map, and what it comes to in tags nearly always includes
		// a lean - which the renderer applies to each row from that row's own corner, not from
		// the top of the line. So the rows would slide against one another whatever is dragged,
		// and there is nothing to wait for: the lines are broken up as the tool opens.
		if (mode == VisualToolTransformMode::Distort && !TextBoxMode()) {
			EnsureShearSplit();
			// A distort is a lean, and a lean is exactly what an upright pen cannot follow - so
			// the pair are kept as shapes from the start here, and the two switches that answer
			// the same question the other way are off to match.
			if (!auto_perspective) {
				maintain_decor = true;
				recalc_bord = false;
				recalc_shad = false;
				EnsureDecor();
			}
		}

		LockEditing(true);
		PlaceFeatures();
		Rebuild();
		return;
	}

	typesetting::ShapeEditor found = TextBoxMode() ?
		typesetting::ShapeEditor(c, textbox_lines) : typesetting::ShapeEditor(c);
	if (!found.ok()) {
		// Nothing usable at all is worth saying out loud, once.
		if (!reported && !found.refusals().empty()) {
			reported = true;
			wxString message = _("The text could not be converted:");
			for (auto const& why : found.refusals())
				message += "\n\n" + to_wx(why);
			wxMessageBox(message, _("Convert text to shapes"), wxOK | wxICON_WARNING,
				c->parent);
		}
		return;
	}

	if (!reported && !found.refusals().empty()) {
		reported = true;
		wxString message = _("Some lines were left as text:");
		for (auto const& why : found.refusals())
			message += "\n\n" + to_wx(why);
		wxMessageBox(message, _("Convert text to shapes"), wxOK | wxICON_INFORMATION,
			c->parent);
	}

	box = found.Box();
	editor.emplace(std::move(found));

	// The handles start on the untouched box, so nothing moves until something is dragged.
	box.Corners(corners);
	typesetting::WarpReset(box, net);

	// Unless the same box came back, which means the same drawings did: then the handles go
	// back where the user left them. This is what keeps a reshaping alive through a zoom, a
	// pan or any other refresh.
	if (had_editor && was_touched &&
		(old_box.centre - box.centre).Len() < .01 &&
		(old_box.half - box.half).Len() < .01 &&
		std::abs(old_box.angle - box.angle) < .01) {
		net = old_net;
		for (int i = 0; i < 4; ++i) corners[i] = old_corners[i];
		touched = true;
	}

	LockEditing(true);
	PlaceFeatures();
	Rebuild();
}

void VisualToolTransform::LockMenus(bool locked) {
	// The menus that run something over the lines themselves. While the session is open the video
	// is being shown copies the file does not have yet, so a script that reads or rewrites the
	// selection would be working on something other than what is on screen - and would throw the
	// reshaping away as soon as it committed.
	//
	// Found by their own titles, translated the same way the menu bar translated them, because a
	// menu can come and go - the Muteki one appears only when its macros load - and counting from
	// the left would then grey out the wrong one.
	auto frame = dynamic_cast<wxFrame *>(c->parent);
	if (!frame) return;
	wxMenuBar *bar = frame->GetMenuBar();
	if (!bar) return;

	for (const char *title : {"A&I", "A&utomation", "Muteki Fansub"}) {
		int at = bar->FindMenu(wxGetTranslation(title));
		if (at != wxNOT_FOUND) bar->EnableTop(at, !locked);
	}
}

void VisualToolTransform::LockEditing(bool locked) {
	// Held by the sessions together, not by whichever of them built it. Switching modes from the
	// menu builds the new tool before the old one is destroyed, so the new one arrives to find
	// everything already disabled - nothing left to record - and the old one would then put it all
	// back on its way out, with a session still running.
	static int held = 0;
	static std::vector<wxWindow *> held_controls;

	if (locked) {
		if (held++ > 0) return;
	}
	else {
		if (held > 0) --held;
		if (held > 0) return;
	}

	LockMenus(locked);
	if (!c->editBox) return;

	if (!locked) {
		for (auto control : held_controls)
			control->Enable(true);
		held_controls.clear();
		c->editBox->Enable(true);
		return;
	}

	if (!held_controls.empty()) return;

	// Every control, not just the panel: a disabled panel still leaves its children looking
	// and behaving as usual on Windows, the text control included.
	std::function<void (wxWindow *)> walk = [&](wxWindow *window) {
		for (auto child : window->GetChildren()) {
			if (child->IsEnabled()) {
				child->Enable(false);
				held_controls.push_back(child);
			}
			walk(child);
		}
	};
	walk(c->editBox);
	c->editBox->Enable(false);

	// Disabling the panel hands the focus to whatever comes next, and the tool needs the
	// video to have it for Enter and Escape to reach here.
	parent->SetFocus();
}

int VisualToolTransform::HandleCount() const {
	switch (mode) {
		case VisualToolTransformMode::Free: return 16;
		// Two per side, so every edge can be bent. The corners stay where they are, which is
		// what keeps an arch an arch: the shape bends without going anywhere.
		case VisualToolTransformMode::Arch: return 8;
		case VisualToolTransformMode::Distort: return 4;
		// Four corners, two direction handles each, and the one under the shape that moves it.
		default: return warp_move_handle + 1;
	}
}

Vector2D VisualToolTransform::HandlePosition(int index) const {
	switch (mode) {
		case VisualToolTransformMode::Free: {
			Vector2D grabbed, anchor;
			int role = 0;
			HandleRole(index, grabbed, anchor, role);
			return MapPoint(box.ToScript(grabbed));
		}
		case VisualToolTransformMode::Arch: return net.tangent[index];
		case VisualToolTransformMode::Distort: return corners[index];
		default: {
			if (index == warp_move_handle) {
				// The middle of the bottom edge of the patch. SyncFeatures pushes it a little
				// further out from there, so it sits under the shape rather than on it.
				Vector2D control[16];
				typesetting::WarpControls(net, control);
				return typesetting::WarpPoint(control, .5, 1.);
			}
			return index < 4 ? net.corner[index] : net.tangent[index - 4];
		}
	}
}

void VisualToolTransform::MoveHandle(int index, Vector2D to) {
	if (index < 0 || index >= HandleCount()) return;
	switch (mode) {
		case VisualToolTransformMode::Free: {
			Vector2D grabbed, anchor;
			int role = 0;
			HandleRole(index, grabbed, anchor, role);

			if (role == 2) {
				// A leaning handle: how far the side has slid sideways, against how far it
				// sits from the middle, is the lean itself.
				typesetting::OrientedBox frame;
				frame.angle = shear_frame_angle;
				frame.centre = box.centre;
				Vector2D wanted = frame.ToLocal(to - gesture_move);
				Vector2D from = frame.ToLocal(
					RotateAbout(box.ToScript(grabbed), box.centre, gesture_angle));
				// `from` is where the side would be without any lean, so the difference
				// against the mouse is the whole lean rather than a step of one - adding the
				// lean so far would count it twice.
				if (std::abs(grabbed.X()) < 1e-6 && std::abs(from.Y()) > 1e-6)
					gesture_shear = Vector2D((float)((wanted.X() - from.X()) / from.Y()),
						gesture_shear.Y());
				else if (std::abs(from.X()) > 1e-6)
					gesture_shear = Vector2D(gesture_shear.X(),
						(float)((wanted.Y() - from.Y()) / from.X()));
				break;
			}

			if (role == 3) {
				// Move the entire selection on the screen axis named by the handle's arrow:
				// top/bottom are Y, left/right are X.
				Vector2D moved = to - gesture_start;
				bool vertical = index == 12 || index == 14;
				Vector2D wanted = vertical ? Vector2D(0.f, moved.Y()) :
					Vector2D(moved.X(), 0.f);
				TransformMatrix2 inverse;
				gesture_move = Invert(frame_linear, inverse) ? ApplyMatrix(inverse, wanted) : wanted;
				break;
			}

			// A corner or a side keeps the point across from it still, so the box scales
			// about that point - and the scale is what \fscx and \fscy will say. It is
			// measured along the box's own axes as they now lie, so a scale after a turn
			// still scales the box rather than the screen.
			gesture_anchor = anchor;
			// In the space the gesture works in, not on screen: the mouse has already been
			// brought back through the frame, and measuring the two against each other in
			// different spaces collapsed the box the moment a turned frame was scaled.
			Vector2D fixed_point = GesturePoint(box.ToScript(anchor));
			double radians = (box.angle - gesture_angle) * pi / 180.0;
			Vector2D along((float)std::cos(radians), (float)std::sin(radians));
			Vector2D across(-along.Y(), along.X());
			Vector2D reach = to - fixed_point;

			auto ratio = [](double got, double wanted, float fallback) {
				if (std::abs(wanted) < 1e-6) return fallback;
				double value = got / wanted;
				// A scale of nothing would collapse the line and leave no way back.
				if (std::abs(value) < .02) value = value < 0 ? -.02 : .02;
				return (float)value;
			};
			bool drives_x = std::abs(grabbed.X() - anchor.X()) > 1e-6;
			bool drives_y = std::abs(grabbed.Y() - anchor.Y()) > 1e-6;
			Vector2D wanted(
				ratio(reach.X() * along.X() + reach.Y() * along.Y(),
					grabbed.X() - anchor.X(), gesture_scale.X()),
				ratio(reach.X() * across.X() + reach.Y() * across.Y(),
					grabbed.Y() - anchor.Y(), gesture_scale.Y()));

			// Holding alt keeps the proportions: the axis that moved the most decides, and
			// the other follows it - so a side handle scales both ways.
			if (alt_down) {
				float same = drives_x && drives_y ?
					std::max(std::abs(wanted.X()), std::abs(wanted.Y())) :
					drives_x ? std::abs(wanted.X()) : std::abs(wanted.Y());
				wanted = Vector2D(wanted.X() < 0 ? -same : same,
				                  wanted.Y() < 0 ? -same : same);
			}
			gesture_scale = wanted;
			break;
		}
		case VisualToolTransformMode::Arch:
			net.tangent[index] = to;
			break;
		case VisualToolTransformMode::Distort:
			corners[index] = to;
			break;
		default:
			// The handle under the shape moves the whole of it. Always measured from where the
			// gesture began, never from the last mouse move: one small step after another would
			// let rounding walk the mesh away.
			if (index == warp_move_handle) {
				Vector2D moved = to - hold_start;
				net = hold_net;
				// The corners and their handles are the whole of the boundary; what dragging the
				// mesh added to the middle is a difference from those, so it comes along on its
				// own.
				for (auto& point : net.corner) point = point + moved;
				for (auto& point : net.tangent) point = point + moved;
				break;
			}
			if (index < 4) {
				// A corner takes its two direction handles with it, so the shape swings
				// about the corner instead of the boundary snapping straight.
				typesetting::WarpMoveCorner(net, index, to - net.corner[index]);
			}
			else net.tangent[index - 4] = to;
			break;
	}
}

void VisualToolTransform::PlaceFeatures() {
	features.clear();
	sel_features.clear();
	if (!Active()) return;
	if (auto_perspective) {
		distort_map = AutoPerspectiveMap();
		source_feature_first = no_feature;
		if (!auto_perspective_points.empty()) return;
		if (touched) {
			auto feature = std::make_unique<VisualDraggableFeature>();
			feature->type = DRAG_BIG_SQUARE;
			feature->layer = 2;
			features.push_back(*feature.release());
		}
		// The four points the proportion is measured from, so it can be said exactly rather
		// than guessed at from a rectangle round the shape.
		source_feature_first = features.size();
		for (int i = 0; i < 4; ++i) {
			auto feature = std::make_unique<VisualDraggableFeature>();
			feature->type = DRAG_SMALL_SQUARE;
			feature->layer = 1;
			features.push_back(*feature.release());
		}
		SyncFeatures();
		return;
	}

	int count = HandleCount();
	for (int i = 0; i < count; ++i) {
		auto feature = std::make_unique<VisualDraggableFeature>();
		// In the warp the four corners are squares and the eight direction handles small
		// circles, the way Photoshop draws them; the corners sit on the higher layer so a
		// handle resting on one cannot steal the click.
		bool is_corner = mode == VisualToolTransformMode::Warp && i < 4;
		if (mode == VisualToolTransformMode::Free) {
			// Squares to size it and smaller squares beyond the sides to lean it. Rotation
			// owns the otherwise empty area outside the box, rather than four extra handles.
			feature->type = i < 8 ? DRAG_BIG_SQUARE : DRAG_SMALL_SQUARE;
			feature->layer = i < 4 || i >= 12 ? 1 : 0;
		}
		else if (mode == VisualToolTransformMode::Distort) {
			// Corners, so they look and catch the mouse the way the free transform's corners do.
			feature->type = DRAG_BIG_SQUARE;
			feature->layer = 1;
		}
		else if (mode == VisualToolTransformMode::Warp && i == warp_move_handle) {
			// A box with a crosshair through it, the way the drag tool draws the point a line
			// is positioned by - and on the higher layer, since it is the one handle that must
			// never be stolen by something resting on it.
			feature->type = DRAG_BIG_SQUARE;
			feature->layer = 2;
		}
		else {
			feature->type = mode == VisualToolTransformMode::Warp ?
				(is_corner ? DRAG_BIG_SQUARE : DRAG_SMALL_CIRCLE) : DRAG_BIG_CIRCLE;
			feature->layer = mode == VisualToolTransformMode::Warp && !is_corner ? 0 : 1;
		}
		features.push_back(*feature.release());
	}
	SyncFeatures();
}

void VisualToolTransform::SyncFeatures() {
	// A feature is hit-tested against the mouse in pixels, so where it sits is a canvas
	// position. The handles themselves are kept in script coordinates, because that is
	// what the shape is measured in and what has to survive a zoom.
	if (auto_perspective) {
		distort_map = AutoPerspectiveMap();
		if (source_feature_first != no_feature)
			for (int i = 0; i < 4; ++i)
				if (auto *handle = FeatureAt(source_feature_first + i))
					handle->pos = FromScriptCoords(source_corners[i]);
		if (source_feature_first != no_feature && source_feature_first > 0) {
			Vector2D screen[4];
			for (int i = 0; i < 4; ++i) screen[i] = FromScriptCoords(corners[i]);
			Vector2D middle = (screen[0] + screen[1] + screen[2] + screen[3]) / 4;
			Vector2D bottom = (screen[2] + screen[3]) / 2;
			Vector2D away = bottom - middle;
			features.front().pos = bottom +
				(away.Len() > 1e-3 ? away.Unit() * 30.f : Vector2D(0.f, 30.f));
		}
		return;
	}

	int index = 0;
	for (auto& feature : features) {
		Vector2D at = FromScriptCoords(HandlePosition(index));
		// The turning and leaning handles sit a little way beyond their corner or side,
		// measured on screen so they stay the same distance out however far the video is
		// zoomed. The leaning ones sit further out, to keep them off the sizing handles.
		if (mode == VisualToolTransformMode::Free && index >= 8) {
			Vector2D centre = FromScriptCoords(MapPoint(box.centre));
			Vector2D away = at - centre;
			if (away.Len() > 1e-3) at = at + away.Unit() * 28.f;
			if (index >= 12 && away.Len() > 1e-3) {
				Vector2D tangent(-away.Y(), away.X());
				if (index == 13 || index == 14) tangent = tangent * -1.f;
				at = at + tangent.Unit() * 16.f;
			}
		}
		// The warp's move handle sits clear of the bottom edge, outwards from the middle of the
		// patch - so it stays off the shape whichever way round the shape lies.
		if (mode == VisualToolTransformMode::Warp && index == warp_move_handle) {
			Vector2D control[16];
			typesetting::WarpControls(net, control);
			Vector2D middle = FromScriptCoords(typesetting::WarpPoint(control, .5, .5));
			Vector2D away = at - middle;
			if (away.Len() > 1e-3) at = at + away.Unit() * 30.f;
		}
		feature.pos = at;
		++index;
	}

	// Everything that moves a corner comes through here, so this is the one place the map has
	// to be worked out again.
	if (mode == VisualToolTransformMode::Distort)
		distort_map = typesetting::QuadMap(box, corners);
}

void VisualToolTransform::Rebuild() {
	if (TagsMode()) {
		SendPreview();
		parent->Render();
		return;
	}
	if (!editor) return;
	// The arch and the warp are the same patch with different numbers of handles free.
	// The border and the shadow always become shapes of their own here: a pen stays upright whatever
	// happens to the shape, so under a bend there is nothing else they could be - and nothing to
	// decide either, which is why there is no switch for it in these modes.
	auto map = typesetting::WarpMap(box, net);
	// The visual transform only consumes the generated preview rows. Contour and layer
	// extraction is for gradient geometry and used to duplicate the entire glyph walk here.
	editor->Build(map, true, recalc_clip, true, false);
	SendPreview();
	parent->Render();
}

// ------------------------------------------------------------------ the tool's history

VisualToolTransform::HistoryState VisualToolTransform::Capture() const {
	HistoryState state;
	for (int i = 0; i < 4; ++i) state.corners[i] = corners[i];
	state.net = net;
	state.scale = gesture_scale;
	state.angle = gesture_angle;
	state.move = gesture_move;
	state.anchor = gesture_anchor;
	state.shear = gesture_shear;
	state.frame_linear = frame_linear;
	state.frame_offset = frame_offset;
	state.split = shear_split;
	for (int i = 0; i < 4; ++i) state.source_corners[i] = source_corners[i];
	state.source_moved = source_moved;
	state.touched = touched;
	return state;
}

void VisualToolTransform::RestoreState(HistoryState const& state) {
	for (int i = 0; i < 4; ++i) corners[i] = state.corners[i];
	net = state.net;
	gesture_scale = state.scale;
	gesture_angle = state.angle;
	gesture_move = state.move;
	gesture_anchor = state.anchor;
	gesture_shear = state.shear;
	if (mode == VisualToolTransformMode::Free) {
		frame_linear = state.frame_linear;
		frame_offset = state.frame_offset;
		shear_split = state.split && !split_lines.empty();
	}
	if (auto_perspective) {
		for (int i = 0; i < 4; ++i) source_corners[i] = state.source_corners[i];
		source_moved = state.source_moved;
		// Whether there is a target at all is part of the step: undoing the four points has to
		// take the handle that moves them away with them.
		touched = state.touched;
		PlaceFeatures();
		Rebuild();
		return;
	}
	SyncFeatures();
	Rebuild();
}

void VisualToolTransform::PushHistory() {
	undo_history.push_back(Capture());
	redo_history.clear();
}

bool VisualToolTransform::UndoHistory() {
	if (!Active() || undo_history.empty()) return false;
	redo_history.push_back(Capture());
	auto state = undo_history.back();
	undo_history.pop_back();
	RestoreState(state);
	return true;
}

bool VisualToolTransform::RedoHistory() {
	if (!Active() || redo_history.empty()) return false;
	undo_history.push_back(Capture());
	auto state = redo_history.back();
	redo_history.pop_back();
	RestoreState(state);
	return true;
}

// --------------------------------------------------------------- accepting, rejecting

void VisualToolTransform::Accept() {
	if (!LinesAlive()) {
		ExitTool();
		return;
	}
	if (TagsMode()) {
		if (TextBoxMode()) {
			auto transformed = TransformedTextBox();
			AssDialogue prototype(*textbox_lines.front());
			auto originals = textbox_lines;
			// Apply changes both the file and the selection. Neither listener may rebuild this
			// tool halfway through replacing the generated rows.
			selection_connection.Block();
			file_changed_connection.Block();
			typesetting::textbox::Apply(c, prototype, std::move(originals), transformed);
			file_changed_connection.Unblock();
			selection_connection.Unblock();
			ExitTool();
			return;
		}
		wxString what = auto_perspective ? _("auto perspective") :
			mode == VisualToolTransformMode::Distort ? _("distort") : _("free transform");
		if (maintain_decor) EnsureDecor();
		bool adding = shear_split || (maintain_decor && HasDecor());
		if (!adding) {
			// Only tags change, so no lines are added and nothing is turned into a comment.
			for (auto const& found : tag_lines)
				found.line->Text = TagLineText(found);
			commit_id = -1;
			VisualToolBase::Commit(what);
			ExitTool();
			return;
		}

		// The pieces go into the file now. The first of them takes over the line it was cut
		// from - so nothing has to be deleted and the line keeps its place - and the rest
		// follow it in order.
		auto& events = c->ass->Events;
		std::vector<AssDialogue *> written;

		auto put_after = [&](AssDialogue *previous, AssDialogue const& model,
		                     std::string const& text) {
			auto fresh = new AssDialogue(model);
			fresh->Text = text;
			fresh->Comment = false;
			auto at = events.iterator_to(*previous);
			++at;
			events.insert(at, *fresh);
			written.push_back(fresh);
			return fresh;
		};
		// Everything one stretch becomes, in the order it has to be drawn in: the shadow, the
		// border over it, and the letters over both.
		auto texts_for = [&](TagLine const& source) {
			std::vector<std::string> out;
			if (maintain_decor) {
				for (auto const& decor : source.decor) {
					if (decor.has_shadow) out.push_back(DecorLineText(source, decor, true));
					if (decor.has_border) out.push_back(DecorLineText(source, decor, false));
				}
			}
			// The letters always go last. Decoration shapes preserve the outline/shadow; they
			// must never replace the glyph fill itself.
			out.push_back(TagLineText(source));
			return out;
		};

		for (auto const& origin : tag_lines) {
			std::vector<std::string> texts;
			if (!origin.replaced) texts = texts_for(origin);
			else
				for (auto const& piece : split_lines) {
					if (piece.source != origin.line) continue;
					auto more = texts_for(piece);
					texts.insert(texts.end(), more.begin(), more.end());
				}
			if (texts.empty()) continue;

			// The first of them takes over the line it came from - so nothing has to be deleted
			// and the line keeps its place - and the rest follow it in order.
			origin.line->Text = texts.front();
			written.push_back(origin.line);
			AssDialogue *previous = origin.line;
			for (size_t at = 1; at < texts.size(); ++at)
				previous = put_after(previous, *origin.line, texts[at]);
		}

		commit_id = -1;
		// Lines were added, so this is not the text-only commit a visual tool normally makes.
		// Blocked around it for the same reason the base blocks it: the listener would send us
		// back through Collect in the middle of finishing up.
		file_changed_connection.Block();
		c->ass->Commit(what, AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);
		file_changed_connection.Unblock();

		if (!written.empty()) {
			Selection selection(written.begin(), written.end());
			c->selectionController->SetSelectionAndActive(std::move(selection), written.front());
		}
		ExitTool();
		return;
	}

	if (!editor) return;

	// The first and only time the lines are touched. A fresh commit id keeps it a step of
	// its own in the undo history, and the commit is what makes the video re-read the file
	// rather than the preview copies.
	editor->Apply();

	// Lines were added, so this is not the text-only commit a visual tool normally makes -
	// and the drawings are what the user will want to carry on with, so they end up
	// selected.
	auto added = editor->applied();
	bool removing_textbox = TextBoxMode();
	std::vector<std::unique_ptr<AssDialogue>> removed_textbox_lines;
	if (removing_textbox) {
		// Arch and Warp bake the words into drawings. They no longer have a rectangular
		// text-flow model, so do not leave an Effect marker or textbox source metadata behind.
		for (auto line : added) {
			line->Effect = "";
			c->ass->DeleteExtradataValue(*line, typesetting::textbox::data_key);
		}
		removed_textbox_lines.reserve(textbox_lines.size());
		for (auto line : textbox_lines) {
			c->ass->Events.erase(c->ass->Events.iterator_to(*line));
			removed_textbox_lines.emplace_back(line);
		}
		c->ass->CleanExtradata();
	}
	wxString message = mode == VisualToolTransformMode::Arch ? _("arch") :
		mode == VisualToolTransformMode::Distort ? _("distort") : _("warp");
	// Blocked around the commit for the same reason the base blocks it: the listener would
	// send us back through Collect in the middle of finishing up.
	file_changed_connection.Block();
	if (removing_textbox) selection_connection.Block();
	// The active line must never point at one of the detached textbox sources while commit
	// listeners inspect the file. This was the list.hpp:1310 assertion captured in the dump.
	if (!added.empty()) {
		Selection selection(added.begin(), added.end());
		c->selectionController->SetSelectionAndActive(std::move(selection), added.front());
	}
	c->ass->Commit(message, AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL |
		(removing_textbox ? AssFile::COMMIT_EXTRADATA : 0));
	file_changed_connection.Unblock();
	if (removing_textbox) selection_connection.Unblock();

	ExitTool();
}

void VisualToolTransform::Reject() {
	// Nothing in the file to put back. Leaving is what tells the video to show the lines
	// again instead of the preview copies.
	ExitTool();
}

void VisualToolTransform::ExitTool() {
	if (leaving) return;
	leaving = true;
	preview_interface.Clear();
	LockEditing(false);
	if (WindowGoing()) return;

	// However the session ends, the video has to stop showing the preview copies. After
	// Apply that means handing over lines that already carry the result, which is what
	// they should be showing anyway.
	ClearPreview();

	editor.reset();
	textbox_document.reset();
	textbox_lines.clear();
	features.clear();
	sel_features.clear();
	undo_history.clear();
	redo_history.clear();
	touched = false;

	// Switching tools destroys this object, so it cannot happen while one of its own
	// event handlers is still running.
	std::string command = return_tool;
	agi::Context *context = c;
	parent->CallAfter([command, context] { cmd::call(command, context); });
}

// ------------------------------------------------------------------------- the top bar

wxString VisualToolTransform::LabelFor(VisualToolTransformAction action) const {
	switch (action) {
		case VisualToolTransformAction::Apply: return _("Accept (ENTER)");
		case VisualToolTransformAction::Cancel: return _("Cancel (ESC)");
		case VisualToolTransformAction::RecalcBord: return _("recalculate bord");
		case VisualToolTransformAction::RecalcShad: return _("recalculate shad");
		case VisualToolTransformAction::MaintainDecor: return _("maintain bord & shad");
		case VisualToolTransformAction::RecalcClip: return _("recalculate clip");
		case VisualToolTransformAction::AutoPerspectiveSize: return _("Size");
		case VisualToolTransformAction::AutoPerspectiveKeepOriginalSize:
			return _("keep original size");
		default: return wxString();
	}
}

void VisualToolTransform::UpdatePreviewInterface() const {
	using Interface = VisualToolPreviewInterface;
	Interface::Page page;

	auto add = [&](VisualToolTransformAction action, Interface::ControlKind kind,
		Interface::ControlStyle style = Interface::ControlStyle::Neutral,
		bool selected = false) -> Interface::Control& {
		Interface::Control control;
		control.id = static_cast<int>(action);
		control.kind = kind;
		control.label = LabelFor(action);
		control.style = style;
		control.enabled = ActionEnabled(action);
		control.selected = selected;
		page.controls.push_back(std::move(control));
		return page.controls.back();
	};

	if (auto_perspective) {
		add(VisualToolTransformAction::Undo, Interface::ControlKind::Undo);
		add(VisualToolTransformAction::Redo, Interface::ControlKind::Redo);
		add(VisualToolTransformAction::Apply, Interface::ControlKind::Button,
			Interface::ControlStyle::Accept);
		add(VisualToolTransformAction::Cancel, Interface::ControlKind::Button,
			Interface::ControlStyle::Cancel);
		auto& size = add(VisualToolTransformAction::AutoPerspectiveSize,
			Interface::ControlKind::Slider);
		size.enabled = touched && !auto_perspective_keep_original_size;
		size.value = auto_perspective_size;
		size.minimum = 25;
		size.maximum = 200;
		size.step = 1;
		size.value_text = to_wx(agi::format("%.0f%%", auto_perspective_size));
		size.width = 170;
		add(VisualToolTransformAction::AutoPerspectiveKeepOriginalSize,
			Interface::ControlKind::Toggle, Interface::ControlStyle::Neutral,
			auto_perspective_keep_original_size);
		wxString message = touched && auto_perspective_points.empty() ?
			_("Redraw the 4 points.") : _("Draw 4 points.");
		// Only worth saying when one line really is the measure of the others.
		if (tag_lines.size() > 1 && AutoPerspectiveFitsShape())
			if (AssDialogue *active = c->selectionController->GetActiveLine()) {
				std::string pattern =
					from_wx(_("Line %d is fitted to the points, the rest follow it."));
				message += " " + to_wx(agi::format(pattern.c_str(), active->Row + 1));
			}
		message += " " + _("The yellow dashed rectangle is the reference.");
		page.message = message;
		preview_interface.SetPage(std::move(page));
		return;
	}

	add(VisualToolTransformAction::Undo, Interface::ControlKind::Undo);
	add(VisualToolTransformAction::Redo, Interface::ControlKind::Redo);
	add(VisualToolTransformAction::Apply, Interface::ControlKind::Button,
		Interface::ControlStyle::Accept);
	add(VisualToolTransformAction::Cancel, Interface::ControlKind::Button,
		Interface::ControlStyle::Cancel);
	page.controls.push_back({0, Interface::ControlKind::Spacer});

	if (TagsMode()) {
		add(VisualToolTransformAction::RecalcBord, Interface::ControlKind::Toggle,
			Interface::ControlStyle::Neutral, recalc_bord);
		add(VisualToolTransformAction::RecalcShad, Interface::ControlKind::Toggle,
			Interface::ControlStyle::Neutral, recalc_shad);
		add(VisualToolTransformAction::MaintainDecor, Interface::ControlKind::Toggle,
			Interface::ControlStyle::Neutral, maintain_decor);
	}
	add(VisualToolTransformAction::RecalcClip, Interface::ControlKind::Toggle,
		Interface::ControlStyle::Neutral, recalc_clip);

	if (TagsMode() && DecorHint())
		page.message = _("The bord and the shad can be scatchy, using the maintain option is "
			"advised. It will transform into a shape.");
	preview_interface.SetPage(std::move(page));
}

std::pair<Vector2D, Vector2D> VisualToolTransform::ActionBounds(
	VisualToolTransformAction action) const {
	UpdatePreviewInterface();
	return preview_interface.BoundsFor(static_cast<int>(action), *gl_text, canvas_size);
}

float VisualToolTransform::TopBarHeight() const {
	UpdatePreviewInterface();
	if (preview_interface.HasExternalHost()) return 0.f;
	return preview_interface.Height(*gl_text, canvas_size);
}

bool VisualToolTransform::ActionEnabled(VisualToolTransformAction action) const {
	if (auto_perspective)
		return action == VisualToolTransformAction::Undo ? !undo_history.empty() :
			action == VisualToolTransformAction::Redo ? !redo_history.empty() :
			action == VisualToolTransformAction::Apply ? touched :
			action == VisualToolTransformAction::Cancel ? true :
			action == VisualToolTransformAction::AutoPerspectiveSize ?
				touched && !auto_perspective_keep_original_size :
			action == VisualToolTransformAction::AutoPerspectiveKeepOriginalSize ? true : false;

	switch (action) {
		case VisualToolTransformAction::Undo: return !undo_history.empty();
		case VisualToolTransformAction::Redo: return !redo_history.empty();
		// Anything that has been dragged is worth writing, even if it has since been
		// undone back to where it started: the text may still have to become a drawing.
		case VisualToolTransformAction::Apply: return touched;
		case VisualToolTransformAction::Cancel: return true;
		case VisualToolTransformAction::RecalcBord:
		case VisualToolTransformAction::RecalcShad:
		// Only where there is something to choose between. The arch and the warp always keep the
		// pair as shapes, because under a bend there is no other answer.
		case VisualToolTransformAction::MaintainDecor:
		case VisualToolTransformAction::RecalcBlur:
			return TagsMode();
		// A clip is script coordinates whether the text stayed text or became an outline, so
		// it can follow the reshaping in every mode - and has to be allowed not to, since a
		// clip that was drawn around something else should stay where it was drawn.
		case VisualToolTransformAction::RecalcClip:
			return true;
		default: return false;
	}
}

VisualToolTransformAction VisualToolTransform::ActionAt(Vector2D point) const {
	if (!Active()) return VisualToolTransformAction::None;
	UpdatePreviewInterface();
	if (preview_interface.HasExternalHost()) return VisualToolTransformAction::None;
	return static_cast<VisualToolTransformAction>(
		preview_interface.HitTest(point, *gl_text, canvas_size));
}

void VisualToolTransform::Perform(VisualToolTransformAction action) {
	if (!ActionEnabled(action)) return;
	switch (action) {
		case VisualToolTransformAction::Undo: UndoHistory(); break;
		case VisualToolTransformAction::Redo: RedoHistory(); break;
		case VisualToolTransformAction::Apply: Accept(); break;
		case VisualToolTransformAction::Cancel: Reject(); break;
		// Fine-tuning a value and keeping the pair exactly are answers to the same question, so
		// whichever is turned on turns the other off.
		case VisualToolTransformAction::RecalcBord:
			recalc_bord = !recalc_bord;
			if (recalc_bord) maintain_decor = false;
			Rebuild();
			break;
		case VisualToolTransformAction::RecalcShad:
			recalc_shad = !recalc_shad;
			if (recalc_shad) maintain_decor = false;
			Rebuild();
			break;
		case VisualToolTransformAction::MaintainDecor:
			maintain_decor = !maintain_decor;
			if (maintain_decor) {
				recalc_bord = false;
				recalc_shad = false;
				EnsureDecor();
			}
			Rebuild();
			break;
		case VisualToolTransformAction::RecalcBlur:
			recalc_blur = !recalc_blur;
			Rebuild();
			break;
		case VisualToolTransformAction::RecalcClip:
			recalc_clip = !recalc_clip;
			Rebuild();
			break;
		case VisualToolTransformAction::AutoPerspectiveKeepOriginalSize:
			auto_perspective_keep_original_size = !auto_perspective_keep_original_size;
			if (touched) Rebuild();
			else parent->Render();
			break;
		default: break;
	}
}

void VisualToolTransform::UpdateAutoPerspectiveSize(double value) {
	if (!auto_perspective) return;
	auto_perspective_size = std::clamp(value, 25.0, 200.0);
	if (touched) Rebuild();
	else parent->Render();
	UpdatePreviewInterface();
}

void VisualToolTransform::DrawTopBar() {
	UpdatePreviewInterface();
	if (preview_interface.HasExternalHost()) return;
	preview_interface.Draw(gl, *gl_text, canvas_size, static_cast<int>(hovered_action));
}

bool VisualToolTransform::AddAutoPerspectivePoint(Vector2D point) {
	if (auto_perspective_points.empty()) {
		features.clear();
		sel_features.clear();
		source_feature_first = no_feature;
	}
	auto_perspective_points.push_back(point);
	if (auto_perspective_points.size() < 4) {
		parent->Render();
		return false;
	}

	// Keep the first three when the closing point would fold or cross the plane. The next
	// click replaces only that last attempt, which makes correcting it feel like mask drawing.
	if (!DirectedQuad(auto_perspective_points)) {
		auto_perspective_points.pop_back();
		parent->Render();
		return false;
	}

	PushHistory();
	for (int i = 0; i < 4; ++i) corners[i] = auto_perspective_points[i];
	auto_perspective_points.clear();
	touched = true;
	PlaceFeatures();
	Rebuild();
	return true;
}

void VisualToolTransform::DrawAutoPerspectiveSource() {
	// What the four points are fitted from: the active line's own box. Shown so the proportion
	// the result is stretched by is something to be seen rather than worked out backwards from
	// the outcome - a tall box on a wide quadrilateral says at a glance why the text spreads.
	wxColour yellow(255, 255, 0);
	gl.SetLineColour(yellow, 1.f, 1);
	for (int i = 0; i < 4; ++i)
		gl.DrawDashedLine(FromScriptCoords(source_corners[i]),
			FromScriptCoords(source_corners[(i + 1) % 4]), 6.f);

	if (source_feature_first == no_feature) return;
	for (int i = 0; i < 4; ++i) {
		auto *handle = FeatureAt(source_feature_first + i);
		if (!handle) continue;
		// Solid and outlined more heavily under the pointer, so it is clear which one a drag
		// would take hold of before the drag starts.
		bool under_mouse = handle == active_feature;
		gl.SetLineColour(yellow, 1.f, under_mouse ? 2 : 1);
		gl.SetFillColour(yellow, under_mouse ? .9f : .35f);
		handle->Draw(gl);
	}
}

void VisualToolTransform::DrawAutoPerspectivePath() {
	if (auto_perspective_points.empty()) {
		// Only the handle that moves the whole target, which exists once there is one. The
		// source handles are drawn with the rectangle they belong to.
		if (!touched || source_feature_first == 0 || features.empty()) return;
		Vector2D bottom = (FromScriptCoords(corners[2]) + FromScriptCoords(corners[3])) / 2;
		wxColour outline = to_wx(line_color_secondary_opt->GetColor());
		wxColour fill = to_wx(highlight_color_primary_opt->GetColor());
		gl.SetLineColour(outline, 1.f, 1);
		gl.DrawLine(bottom, features.front().pos);
		gl.SetFillColour(fill, .3f);
		features.front().Draw(gl);
		return;
	}

	wxColour path = to_wx(line_color_secondary_opt->GetColor());
	gl.SetLineColour(path, 1.f, 2);
	for (size_t i = 1; i < auto_perspective_points.size(); ++i)
		gl.DrawLine(FromScriptCoords(auto_perspective_points[i - 1]),
			FromScriptCoords(auto_perspective_points[i]));

	// The moving edge and its closing edge preview the same connected contour used by the
	// mask tools. Only the fixed, clicked points are retained.
	Vector2D pointer = mouse_pos;
	// The preview follows the cursor into the letterbox too, since a click there places a
	// corner as well; stopping the dashed edges at the video rect pointed them at a spot
	// where nothing would appear. mouse_pos is tested for validity because leaving the
	// display resets it to a near-zero float rather than NaN, which would otherwise pass
	// the top bar test on its own.
	if (pointer && pointer.Y() >= TopBarHeight()) {
		Vector2D last = FromScriptCoords(auto_perspective_points.back());
		gl.DrawDashedLine(last, pointer, 5.f);
		if (auto_perspective_points.size() > 1)
			gl.DrawDashedLine(pointer, FromScriptCoords(auto_perspective_points.front()), 5.f);
	}

	for (auto const& point : auto_perspective_points)
		DrawCorner(FromScriptCoords(point), false);
}

void VisualToolTransform::DrawCorner(Vector2D at, bool current, bool selected) {
	wxColour outline = to_wx(line_color_secondary_opt->GetColor());
	wxColour active = to_wx(highlight_color_secondary_opt->GetColor());
	bool soft_hover = mode == VisualToolTransformMode::Warp;
	gl.SetLineColour(current || selected ? active : outline, 1.f,
		current && !soft_hover ? 2 : 1);
	if (selected) gl.SetFillColour(active, .46f);
	else if (current && soft_hover) gl.SetFillColour(active, .14f);
	else gl.SetFillColour(*wxBLACK, 0.f);
	gl.DrawRectangle(at - Vector2D(5.f, 5.f), at + Vector2D(5.f, 5.f));
}

void VisualToolTransform::DrawShapeHandles() {
	wxColour outline = to_wx(line_color_secondary_opt->GetColor());
	wxColour base_fill = to_wx(highlight_color_primary_opt->GetColor());
	wxColour active_fill = to_wx(highlight_color_secondary_opt->GetColor());

	int index = 0;
	for (auto& feature : features) {
		bool current = &feature == active_feature;
		bool selected = sel_features.count(&feature) != 0;
		if (mode == VisualToolTransformMode::Warp && index == warp_move_handle)
			selected = false;
		// The distort's four, and the warp's first four, are corners of the box. The warp's
		// others steer its curves and the arch's bend its edges: those are not corners and are
		// not drawn as any.
		bool corner = mode == VisualToolTransformMode::Distort ||
			(mode == VisualToolTransformMode::Warp && index < 4);
		// The move handle is left to the framework: a big square is drawn as a box with a
		// crosshair through it, which is what the drag tool's own \pos handle looks like - so
		// the one handle that moves the shape looks like the tool that moves things.
		if (corner) DrawCorner(feature.pos, current, selected);
		else {
			bool soft_hover = mode == VisualToolTransformMode::Arch ||
				mode == VisualToolTransformMode::Warp;
			gl.SetLineColour(current || selected ? active_fill : outline, 1.f,
				current && !soft_hover ? 2 : 1);
			gl.SetFillColour(current || selected ? active_fill : base_fill,
				selected ? .58f : current ? (soft_hover ? .42f : .9f) : .3f);
			feature.Draw(gl);
		}
		++index;
	}
}

void VisualToolTransform::DrawFreeHandles() {
	wxColour outline = to_wx(line_color_secondary_opt->GetColor());
	wxColour active = to_wx(highlight_color_secondary_opt->GetColor());

	int index = 0;
	for (auto& feature : features) {
		bool current = &feature == active_feature;
		gl.SetLineColour(current ? active : outline, 1.f, current ? 2 : 1);
		gl.SetFillColour(*wxBLACK, 0.f);

		if (index < 8) {
			DrawCorner(feature.pos, current);
		}
		else if (index < 12) {
			// Leaning: a parallelogram, dropped the way the lean it holds goes, so it looks
			// like what it does.
			bool horizontal = index == 8 || index == 10;
			float lean = horizontal ? gesture_shear.X() : gesture_shear.Y();
			// A lean of nothing would draw a square, which says nothing at all.
			float tilt = std::clamp(lean, -1.f, 1.f);
			if (std::abs(tilt) < .25f) tilt = tilt < 0 ? -.25f : .25f;
			// Negative, because a positive ax pushes what is *below* the pivot to the
			// right: the icon has to fall the way the box falls, not the other way.
			float slide = -tilt * 5.f;

			Vector2D at = feature.pos;
			if (horizontal) {
				gl.DrawLine(at + Vector2D(-5.f + slide, -5.f), at + Vector2D(5.f + slide, -5.f));
				gl.DrawLine(at + Vector2D(5.f + slide, -5.f), at + Vector2D(5.f - slide, 5.f));
				gl.DrawLine(at + Vector2D(5.f - slide, 5.f), at + Vector2D(-5.f - slide, 5.f));
				gl.DrawLine(at + Vector2D(-5.f - slide, 5.f), at + Vector2D(-5.f + slide, -5.f));
			}
			else {
				gl.DrawLine(at + Vector2D(-5.f, -5.f + slide), at + Vector2D(-5.f, 5.f + slide));
				gl.DrawLine(at + Vector2D(-5.f, 5.f + slide), at + Vector2D(5.f, 5.f - slide));
				gl.DrawLine(at + Vector2D(5.f, 5.f - slide), at + Vector2D(5.f, -5.f - slide));
				gl.DrawLine(at + Vector2D(5.f, -5.f - slide), at + Vector2D(-5.f, -5.f + slide));
			}
		}
		else {
			// Positioning: the square and crosshair follow the drag tool's position handle,
			// while the arrowheads say which single axis this instance moves on.
			bool vertical = index == 12 || index == 14;
			Vector2D axis = vertical ? Vector2D(0.f, 1.f) : Vector2D(1.f, 0.f);
			Vector2D across(-axis.Y(), axis.X());
			DrawCorner(feature.pos, current);
			gl.DrawLine(feature.pos - axis * 3.f, feature.pos + axis * 3.f);
			gl.SetFillColour(current ? active : outline, .85f);
			gl.DrawTriangle(feature.pos - axis * 4.f,
				feature.pos - axis + across * 2.f,
				feature.pos - axis - across * 2.f);
			gl.DrawTriangle(feature.pos + axis * 4.f,
				feature.pos + axis + across * 2.f,
				feature.pos + axis - across * 2.f);
		}
		++index;
	}
	gl.SetFillColour(*wxBLACK, 0.f);
}

bool VisualToolTransform::FreePointInside(Vector2D point) const {
	Vector2D original[4], quad[4];
	if (TextBoxMode()) std::copy(textbox_original_corners, textbox_original_corners + 4, original);
	else box.Corners(original);
	for (int i = 0; i < 4; ++i) quad[i] = MapPoint(original[i]);

	int positive = 0, negative = 0;
	for (int i = 0; i < 4; ++i) {
		Vector2D edge = quad[(i + 1) % 4] - quad[i];
		Vector2D reach = point - quad[i];
		double side = edge.X() * reach.Y() - edge.Y() * reach.X();
		if (side > 0) ++positive;
		else if (side < 0) ++negative;
	}
	return !(positive && negative);
}

void VisualToolTransform::DrawFreeRotationGuide() {
	if (!mouse_pos || mouse_pos.Y() < TopBarHeight() || active_feature) return;
	if (free_hold_mode != FreeHoldMode::Rotate && FreePointInside(ToScriptCoords(mouse_pos))) return;

	Vector2D pivot = FromScriptCoords(MapPoint(box.centre));
	Vector2D reach = mouse_pos - pivot;
	if (reach.Len() < 18.f) return;

	wxColour colour = to_wx(highlight_color_secondary_opt->GetColor());
	gl.SetLineColour(colour, .9f, 1);
	gl.DrawLine(pivot, mouse_pos);

	Vector2D direction = reach.Unit();
	Vector2D above(direction.Y(), -direction.X());
	// Keep the rotation glyph on the visually upper side of the guide. Its arc has a
	// six-pixel radius, so nine pixels leaves a small gap instead of crossing the line.
	if (above.Y() > 0.f || (std::abs(above.Y()) < .001f && above.X() > 0.f)) above = above * -1.f;
	Vector2D icon = mouse_pos - direction * 14.f + above * 9.f;
	const int steps = 12;
	const double sweep = 285.0;
	Vector2D previous;
	for (int step = 0; step <= steps; ++step) {
		double radians = (step * sweep / steps - 45.0) * pi / 180.0;
		Vector2D at = icon + Vector2D((float)(std::cos(radians) * 6.0),
			(float)(std::sin(radians) * 6.0));
		if (step) gl.DrawLine(previous, at);
		previous = at;
	}
	double radians = (sweep - 45.0) * pi / 180.0;
	Vector2D along((float)-std::sin(radians), (float)std::cos(radians));
	Vector2D outward((float)std::cos(radians), (float)std::sin(radians));
	gl.SetFillColour(colour, .9f);
	gl.DrawTriangle(previous + along * 4.f, previous + outward * 3.f - along * 2.f,
		previous - outward * 3.f - along * 2.f);
	gl.SetFillColour(*wxBLACK, 0.f);
}

// ------------------------------------------------------------------------ the dragging

void VisualToolTransform::Commit(wxString) {
	// Nothing. A drag has nothing to write yet, and committing would send the video back
	// to the file - which is where the preview was being lost: every mouse move pushed the
	// reshaped copies to the video, and the framework's commit right afterwards replaced
	// them with the untouched lines again. Only Apply commits, through the base directly.
}

bool VisualToolTransform::InitializeDrag(VisualDraggableFeature *feature) {
	if (!Active()) return false;
	if (auto_perspective) {
		PushHistory();
		for (int i = 0; i < 4; ++i) hold_corners[i] = corners[i];
		hold_start = ToScriptCoords(feature->pos);
		return true;
	}

	if (mode == VisualToolTransformMode::Free) {
		// A handle here does not stand for itself, it stands for the whole gesture - so only
		// one of them may be dragged at a time. The framework drags everything that is
		// selected, which is why the selection is cut down to the one being grabbed.
		SetSelection(feature, true);

		int index = 0;
		for (auto& other : features) {
			if (&other == feature) break;
			++index;
		}

		// Fold what has been done so far into the starting point. Without this, a scale
		// measured from one corner would still be in force when another corner is grabbed,
		// and the box would jump the moment it was touched.
		PushHistory();

		// The leaning handles, and only those: the renderer leans each row from its own
		// corner, so until the rows are lines of their own there is no lean that can be
		// written for them. Recorded first, so a step back puts the lines together again.
		if (index >= 8 && index < 12) EnsureShearSplit();

		RebaseGesture();
		if (index >= 12)
			gesture_start = ToScriptCoords(feature->pos);
	}
	// The warp's move handle stands for the whole mesh rather than for a point of it, so what
	// the mesh was and where the handle started are what every mouse move is measured against.
	if (mode == VisualToolTransformMode::Warp) {
		int index = 0;
		for (auto& other : features) {
			if (&other == feature) break;
			++index;
		}
		if (index != warp_move_handle)
			if (auto *move = FeatureAt(warp_move_handle)) sel_features.erase(move);
		if (index == warp_move_handle) {
			// This is an action handle, not a mesh control point. It may be dragged, but it
			// must not join a point selection or move together with selected warp points.
			SetSelection(feature, true);
			hold_net = net;
			hold_start = ToScriptCoords(feature->pos);
		}
	}

	// Recorded before the drag changes anything, so undo lands on the shape as it was when
	// the drag started. The free transform has already done this above, together with
	// folding the previous gesture in.
	if (mode != VisualToolTransformMode::Free) PushHistory();
	return true;
}

void VisualToolTransform::UpdateDrag(VisualDraggableFeature *feature) {
	if (!Active()) return;
	if (auto_perspective) {
		if (source_feature_first != no_feature) {
			size_t index = 0;
			for (auto& other : features) {
				if (&other == feature) break;
				++index;
			}
			if (index >= source_feature_first && index < source_feature_first + 4) {
				source_corners[index - source_feature_first] = ToScriptCoords(feature->pos);
				source_moved = true;
				SyncFeatures();
				Rebuild();
				return;
			}
		}

		auto inverse = typesetting::QuadInverseMap(box, hold_corners);
		auto plane = typesetting::QuadMap(box, hold_corners);
		Vector2D moved = inverse(ToScriptCoords(feature->pos)) - inverse(hold_start);
		Vector2D source[4], next[4];
		box.Corners(source);
		for (int i = 0; i < 4; ++i) {
			Vector2D shifted = source[i] + moved;
			if (typesetting::QuadDepth(box, hold_corners, shifted) <= 1e-4) return;
			next[i] = plane(shifted);
			if (!std::isfinite(next[i].X()) || !std::isfinite(next[i].Y()) ||
				std::abs(next[i].X()) > 1e5f || std::abs(next[i].Y()) > 1e5f) return;
		}
		std::copy(next, next + 4, corners);
		SyncFeatures();
		touched = true;
		Rebuild();
		return;
	}

	// Only the handle under the mouse says anything in the free transform, and it says it
	// about the whole box.
	if (mode == VisualToolTransformMode::Free && active_feature && feature != active_feature)
		return;
	if ((mode == VisualToolTransformMode::Arch || mode == VisualToolTransformMode::Warp) &&
		sel_features.size() > 1) {
		// The framework has already moved every selected feature to its drag target. Consume
		// all of those targets before SyncFeatures writes their displayed positions back.
		if (feature != *sel_features.begin()) return;
		std::vector<std::pair<int, Vector2D>> targets;
		int at = 0;
		for (auto& candidate : features) {
			if (sel_features.count(&candidate))
				targets.emplace_back(at, ToScriptCoords(candidate.pos));
			++at;
		}
		for (auto const& target : targets) MoveHandle(target.first, target.second);
		SyncFeatures();
		touched = true;
		Rebuild();
		return;
	}

	int index = 0;
	for (auto& other : features) {
		if (&other == feature) break;
		++index;
	}
	// Back through the frame first: a gesture is measured in the space the box was read in,
	// and the mouse is on screen.
	MoveHandle(index, mode == VisualToolTransformMode::Free && index < 12 ?
		FrameInverse(ToScriptCoords(feature->pos)) : ToScriptCoords(feature->pos));

	// All of them, not just the one being dragged: the others belong to a box that has just
	// changed shape, and they were being left behind.
	SyncFeatures();

	touched = true;
	Rebuild();
}

bool VisualToolTransform::InitializeHold() {
	if (auto_perspective) return false;
	if (mode == VisualToolTransformMode::Free) {
		// Inside the box moves it; the otherwise empty area outside turns it. Measuring the
		// turn as an angle naturally makes a given mouse movement finer farther from the box.
		if (tag_lines.empty()) return false;
		Vector2D at = ToScriptCoords(mouse_pos);

		PushHistory();
		RebaseGesture();
		if (FreePointInside(at)) {
			free_hold_mode = FreeHoldMode::Move;
			gesture_start = FrameInverse(at);
		}
		else {
			Vector2D from = FrameInverse(at) - GesturePivot();
			if (from.Len() < 1e-3) return false;
			free_hold_mode = FreeHoldMode::Rotate;
			gesture_start_angle = (float)(std::atan2(from.Y(), from.X()) * 180.0 / pi);
		}
		return true;
	}

	if (mode == VisualToolTransformMode::Distort) {
		// The same as the free transform: anywhere inside what is on screen takes hold of the
		// whole of it, which is the one gesture that needs no handle.
		if (tag_lines.empty()) return false;
		Vector2D at = ToScriptCoords(mouse_pos);

		// Inside means on the same side of all four edges. Which side that is depends on which
		// way round the corners have ended up after being dragged about, so either will do as
		// long as it is the same one every time.
		int one_way = 0, the_other = 0;
		for (int i = 0; i < 4; ++i) {
			Vector2D edge = corners[(i + 1) % 4] - corners[i];
			Vector2D reach = at - corners[i];
			double side = edge.X() * reach.Y() - edge.Y() * reach.X();
			if (side > 0) ++one_way;
			else if (side < 0) ++the_other;
		}
		if (one_way && the_other) return false;

		PushHistory();
		for (int i = 0; i < 4; ++i) hold_corners[i] = corners[i];
		hold_start = at;
		return true;
	}

	// Only the arch and the warp have a mesh to grab; the rest is handles and nothing else.
	if (!editor || mode == VisualToolTransformMode::Distort ||
		mode == VisualToolTransformMode::Free) return false;

	Vector2D control[16];
	typesetting::WarpControls(net, control);

	// A tolerance in script units for what counts as "on the mesh", worked out from a few
	// pixels on screen - so it stays the same size to the eye at any zoom.
	Vector2D at = ToScriptCoords(mouse_pos);
	Vector2D nearby = ToScriptCoords(mouse_pos + Vector2D(8.f, 0.f));
	double tolerance = std::max<double>((nearby - at).Len(), 1.0);

	if (!typesetting::WarpLocate(control, at, tolerance, hold_u, hold_v)) return false;

	PushHistory();
	hold_net = net;

	// The arch has no handle for the middle of the mesh, so taking hold of it there moves the
	// whole thing instead - the same gesture the free transform and the distort have.
	if (mode == VisualToolTransformMode::Arch) {
		hold_start = at;
		return true;
	}

	hold_origin = typesetting::WarpPoint(control, hold_u, hold_v);
	return true;
}

void VisualToolTransform::UpdateHold() {
	if (mode == VisualToolTransformMode::Free) {
		if (free_hold_mode == FreeHoldMode::Move)
			gesture_move = FrameInverse(ToScriptCoords(mouse_pos)) - gesture_start;
		else if (free_hold_mode == FreeHoldMode::Rotate) {
			Vector2D from = FrameInverse(ToScriptCoords(mouse_pos)) - GesturePivot();
			if (from.Len() < 1e-3) return;
			double now = std::atan2(from.Y(), from.X()) * 180.0 / pi;
			gesture_angle = (float)(gesture_start_angle - now);
		}
		SyncFeatures();
		touched = true;
		Rebuild();
		return;
	}

	if (mode == VisualToolTransformMode::Distort) {
		// Prefer the perspective already authored on the selected rows. If there is none, the
		// distortion's own quadrilateral is the plane, which becomes projective as soon as one
		// of its corners is moved.
		typesetting::OrientedBox move_source = box;
		Vector2D move_corners[4];
		if (!PerspectiveMovePlane(hold_corners, move_source, move_corners))
			std::copy(hold_corners, hold_corners + 4, move_corners);

		auto inverse = typesetting::QuadInverseMap(move_source, move_corners);
		auto plane = typesetting::QuadMap(move_source, move_corners);
		Vector2D moved = inverse(ToScriptCoords(mouse_pos)) - inverse(hold_start);
		Vector2D next[4];
		for (int i = 0; i < 4; ++i) {
			Vector2D shifted = inverse(hold_corners[i]) + moved;
			// Do not let a drag cross the plane's vanishing line, where coordinates turn inside
			// out and become unbounded. The last valid position remains on screen instead.
			if (typesetting::QuadDepth(move_source, move_corners, shifted) <= 1e-4) return;
			next[i] = plane(shifted);
			if (!std::isfinite(next[i].X()) || !std::isfinite(next[i].Y()) ||
				std::abs(next[i].X()) > 1e5f || std::abs(next[i].Y()) > 1e5f) return;
		}
		std::copy(next, next + 4, corners);
		SyncFeatures();
		touched = true;
		Rebuild();
		return;
	}

	if (!editor || mode == VisualToolTransformMode::Distort ||
		mode == VisualToolTransformMode::Free) return;

	// Always from where the gesture began, never from the last frame of it: applying one
	// small delta after another would let rounding walk the mesh away.
	net = hold_net;

	if (mode == VisualToolTransformMode::Arch) {
		Vector2D moved = ToScriptCoords(mouse_pos) - hold_start;
		// The corners and their handles are the whole of the shape here; what dragging the mesh
		// added to the middle is a difference from those, so it comes along on its own.
		for (auto& point : net.corner) point = point + moved;
		for (auto& point : net.tangent) point = point + moved;
		SyncFeatures();
		touched = true;
		Rebuild();
		return;
	}

	typesetting::WarpDragInside(net, hold_u, hold_v, ToScriptCoords(mouse_pos) - hold_origin);
	SyncFeatures();
	touched = true;
	Rebuild();
}

void VisualToolTransform::EndHold() {
	free_hold_mode = FreeHoldMode::None;
}

void VisualToolTransform::OnMouseEvent(wxMouseEvent& event) {
	if (box_selecting) {
		mouse_pos = event.GetPosition();
		if (event.LeftIsDown()) {
			parent->Render();
			return;
		}

		box_selecting = false;
		Vector2D low = box_select_start.Min(mouse_pos);
		Vector2D high = box_select_start.Max(mouse_pos);
		if (mode == VisualToolTransformMode::Warp)
			if (auto *move = FeatureAt(warp_move_handle)) sel_features.erase(move);
		if (!box_select_add) sel_features.clear();
		size_t index = 0;
		for (auto& feature : features) {
			if (mode == VisualToolTransformMode::Warp && index++ == warp_move_handle)
				continue;
			if (feature.pos.X() >= low.X() && feature.pos.X() <= high.X() &&
				feature.pos.Y() >= low.Y() && feature.pos.Y() <= high.Y())
				SetSelection(&feature, false);
		}
		if (parent->HasCapture()) parent->ReleaseMouse();
		parent->SetFocus();
		parent->Render();
		return;
	}

	if (Active() && !dragging && !holding) {
		Vector2D point(event.GetPosition());
		auto action = ActionAt(point);
		if (action != VisualToolTransformAction::None || point.Y() < TopBarHeight()) {
			// The bar swallows what happens over it, so a button does not also start a
			// drag on a handle that happens to lie underneath.
			if (hovered_action != action) {
				hovered_action = action;
				parent->Render();
			}
			if (event.LeftDown() && action != VisualToolTransformAction::None)
				Perform(action);
			return;
		}
	}
	if (hovered_action != VisualToolTransformAction::None) {
		hovered_action = VisualToolTransformAction::None;
		parent->Render();
	}
	if (!auto_perspective &&
		(mode == VisualToolTransformMode::Arch || mode == VisualToolTransformMode::Warp) &&
		event.LeftDown()) {
		Vector2D point(event.GetPosition());
		bool on_handle = std::any_of(features.begin(), features.end(),
			[&](VisualDraggableFeature const& feature) { return feature.IsMouseOver(point); });
		bool on_mesh = false;
		if (!on_handle) {
			Vector2D control[16];
			typesetting::WarpControls(net, control);
			Vector2D script = ToScriptCoords(point);
			Vector2D nearby = ToScriptCoords(point + Vector2D(8.f, 0.f));
			double u = 0, v = 0;
			on_mesh = typesetting::WarpLocate(control, script,
				std::max<double>((nearby - script).Len(), 1.0), u, v);
		}
		if (!on_handle && !on_mesh) {
			box_selecting = true;
			box_select_add = event.CmdDown();
			box_select_start = point;
			mouse_pos = point;
			parent->CaptureMouse();
			parent->Render();
			return;
		}
	}
	if (auto_perspective && event.LeftDown()) {
		Vector2D point(event.GetPosition());
		bool on_handle = auto_perspective_points.empty() &&
			std::any_of(features.begin(), features.end(), [&](VisualDraggableFeature& feature) {
				return (point - feature.pos).Len() <= 12.f;
			});
		if (on_handle) {
			VisualTool<VisualDraggableFeature>::OnMouseEvent(event);
			return;
		}
		// A perspective quad routinely has corners past the frame edge, so a click outside
		// the video is accepted; the top bar already returned above.
		AddAutoPerspectivePoint(ToScriptCoords(point));
		return;
	}
	VisualTool<VisualDraggableFeature>::OnMouseEvent(event);
	if (mode == VisualToolTransformMode::Warp && !event.LeftIsDown())
		if (auto *move = FeatureAt(warp_move_handle)) sel_features.erase(move);
}

bool VisualToolTransform::HandleKey(int key, bool control, bool shift) {
	if (!Active()) return false;
	if (control && (key == 'Z' || key == 'Y')) {
		bool redo = key == 'Y' || shift;
		if (redo ? RedoHistory() : UndoHistory()) return true;
		// With nothing left to undo the guided page still keeps the keys: behind an unapplied
		// preview they would otherwise reach the file's own history.
		if (auto_perspective) return true;
		// Nothing left to step back to. Undoing past the start of a session would undo
		// whatever the user did before it, with a preview still on screen to confuse them,
		// so it closes the preview instead.
		if (!redo) Reject();
		return true;
	}
	if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
		Perform(VisualToolTransformAction::Apply);
		return true;
	}
	if (key == WXK_ESCAPE) {
		Perform(VisualToolTransformAction::Cancel);
		return true;
	}
	if (mode == VisualToolTransformMode::Free && !control &&
		(key == WXK_LEFT || key == WXK_RIGHT || key == WXK_UP || key == WXK_DOWN)) {
		Vector2D step(
			key == WXK_LEFT ? -1.f : key == WXK_RIGHT ? 1.f : 0.f,
			key == WXK_UP ? -1.f : key == WXK_DOWN ? 1.f : 0.f);
		PushHistory();
		RebaseGesture();
		frame_offset = frame_offset + step;
		SyncFeatures();
		touched = true;
		Rebuild();
		return true;
	}
	return false;
}

void VisualToolTransform::OnCharHook(wxKeyEvent& event) {
	if (!HandleKey(event.GetKeyCode(), event.CmdDown(), event.ShiftDown()))
		event.Skip();
}

bool VisualToolTransform::OnKeyEvent(wxKeyEvent& event) {
	return HandleKey(event.GetKeyCode(), event.CmdDown(), event.ShiftDown());
}

void VisualToolTransform::OnLineChanged() {
	// Another active line is another job, and the shapes being held belong to this one.
	ExitTool();
}

void VisualToolTransform::OnCoordinateSystemsChanged() {
	// The handles are kept in script coordinates, so a new mapping to the screen means the
	// same handles at different pixels - not new handles, and certainly not a reason to go
	// back to the file and lose the reshaping.
	SyncFeatures();
	parent->Render();
}

void VisualToolTransform::DoRefresh() {
	// A refresh in the middle of a gesture would re-read the shape, and the box and the
	// handles belong to the drawings as they were.
	if (dragging || holding) return;
	Collect();
}

void VisualToolTransform::Draw() {
	if (!Active()) return;

	wxColour line_colour = to_wx(line_color_primary_opt->GetColor());

	auto screen = [&](Vector2D point) { return FromScriptCoords(point); };

	// The whole frame is dimmed while the reshaping is being worked out - fainter than the
	// vector clip dims what it cuts away, because here the picture underneath is the thing
	// being judged. Everything the tool draws comes afterwards, so none of it is dimmed.
	gl.SetLineColour(*wxBLACK, 0.f, 1);
	gl.SetFillColour(*wxBLACK,
		static_cast<float>(shaded_area_alpha_opt->GetDouble() * .55));
	gl.DrawRectangle(video_pos, video_pos + video_size);
	gl.SetFillColour(*wxBLACK, 0.f);

	if (auto_perspective) {
		DrawAutoPerspectiveSource();
		DrawAutoPerspectivePath();
		DrawTopBar();
		return;
	}

	// The mesh, in thin dashed red. Dashed by drawing every other piece of the curve
	// rather than by dashing each piece, which at this size would come out solid.
	auto patch_curve = [&](Vector2D const control[16], bool along_u, double at) {
		const int steps = 24;
		Vector2D previous = screen(along_u ? typesetting::WarpPoint(control, 0, at)
		                                   : typesetting::WarpPoint(control, at, 0));
		for (int step = 1; step <= steps; ++step) {
			double t = (double)step / steps;
			Vector2D next = screen(along_u ? typesetting::WarpPoint(control, t, at)
			                               : typesetting::WarpPoint(control, at, t));
			if (step % 2) gl.DrawLine(previous, next);
			previous = next;
		}
	};

	if (mode == VisualToolTransformMode::Free) {
		// The box as it now stands, and where it started - so a scale or a turn can be seen
		// against what it was.
		// Only where the box is now. Showing where it was as well left a stale rectangle
		// lying about on screen with nothing to say.
		Vector2D original[4], now[4];
		if (TextBoxMode()) std::copy(textbox_original_corners, textbox_original_corners + 4, original);
		else box.Corners(original);
		for (int i = 0; i < 4; ++i) now[i] = MapPoint(original[i]);

		gl.SetLineColour(mesh_colour, 1.f, 1);
		for (int i = 0; i < 4; ++i)
			gl.DrawDashedLine(screen(now[i]), screen(now[(i + 1) % 4]), 5.f);
	}
	else if (mode == VisualToolTransformMode::Distort) {
		// The box the drawings started in, and the quadrilateral the corners now describe.
		Vector2D outline[4];
		if (TextBoxMode()) std::copy(textbox_original_corners, textbox_original_corners + 4, outline);
		else box.Corners(outline);
		gl.SetLineColour(line_colour, .6f, 1);
		for (int i = 0; i < 4; ++i)
			gl.DrawDashedLine(screen(outline[i]), screen(outline[(i + 1) % 4]), 6.f);

		gl.SetLineColour(mesh_colour, 1.f, 1);
		for (int i = 0; i < 4; ++i)
			gl.DrawDashedLine(screen(corners[i]), screen(corners[(i + 1) % 4]), 5.f);
	}
	else {
		Vector2D control[16];
		typesetting::WarpControls(net, control);
		gl.SetLineColour(mesh_colour, 1.f, 1);

		if (mode == VisualToolTransformMode::Arch) {
			// Only the frame: the arch bends the four edges, and a grid inside it would say
			// nothing they do not.
			patch_curve(control, true, 0);
			patch_curve(control, true, 1);
			patch_curve(control, false, 0);
			patch_curve(control, false, 1);

			// And the thin arms from each corner to the handles that steer its curves, so it is
			// plain which handle belongs to which edge.
			for (int corner = 0; corner < 4; ++corner) {
				int first, second;
				typesetting::WarpCornerTangents(corner, first, second);
				gl.DrawLine(screen(net.corner[corner]), screen(net.tangent[first]));
				gl.DrawLine(screen(net.corner[corner]), screen(net.tangent[second]));
			}
		}
		else {
			// The four lines each way that divide the patch into nine cells.
			for (int line = 0; line < 4; ++line) {
				patch_curve(control, true, line / 3.0);
				patch_curve(control, false, line / 3.0);
			}

			// And the thin arms from each corner to the handles that steer its curves.
			for (int corner = 0; corner < 4; ++corner) {
				int first, second;
				typesetting::WarpCornerTangents(corner, first, second);
				gl.DrawLine(screen(net.corner[corner]), screen(net.tangent[first]));
				gl.DrawLine(screen(net.corner[corner]), screen(net.tangent[second]));
			}
		}
	}

	if (mode == VisualToolTransformMode::Free) {
		DrawFreeRotationGuide();
		DrawFreeHandles();
	}
	else DrawShapeHandles();
	if (box_selecting) {
		Vector2D low = box_select_start.Min(mouse_pos);
		Vector2D high = box_select_start.Max(mouse_pos);
		wxColour selection = to_wx(highlight_color_secondary_opt->GetColor());
		gl.SetLineColour(selection, 1.f, 1);
		gl.SetFillColour(selection, .12f);
		gl.DrawRectangle(low, high);
		gl.SetFillColour(*wxBLACK, 0.f);
	}
	DrawTopBar();
}
