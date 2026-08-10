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

/// @file text_to_shape.h
/// @brief Turning the text of a line into an ASS drawing
///
/// Only a drawing can be reshaped, so anything that bends or distorts text has to
/// come through here first. The conversion is one way: the font ends up baked into
/// the outline and the text stops being text.

#pragma once

#include "vector2d.h"

#include <functional>
#include <string>
#include <vector>

namespace agi { struct Context; }
class AssDialogue;

namespace text_to_shape {

/// Whether this build can convert text at all. There is one backend so far, the
/// Windows one, so elsewhere the answer is no rather than a wrong shape.
bool Available();

/// Why this line cannot be converted, or an empty string if it can.
///
/// The conversion is deliberately narrow to begin with, and says which case it is
/// rather than producing a shape that only looks nearly right.
std::string WhyNot(const agi::Context *c, AssDialogue *line);

/// What a line would say once its text became a drawing.
struct Converted {
	AssDialogue *line = nullptr;
	std::string text;
};

/// What every selected line that still holds text would say as a drawing.
///
/// Writes nothing and commits nothing: the caller decides whether the conversion ever
/// reaches the file, which is what lets it be shown as a preview first. One message per
/// distinct reason lands in `refusals` for the lines that could not be converted.
std::vector<Converted> ConvertSelection(const agi::Context *c,
                                        std::vector<std::string>& refusals);

/// The border and the shadow of a line, drawn as shapes.
///
/// A pen cannot be leaned or squashed: \bord is one number, an upright ellipse, and it cannot even
/// be negative - so a line that has been sheared or narrowed can be given no border that follows
/// its letters. Drawn as a shape the border follows them exactly, because it is then part of the
/// same geometry; the shadow is that shape again, moved the way the renderer moves it.
///
/// What a line paints its border and its shadow with, and how wide the pen is.
///
/// Asked wherever one of the two has to be stood in for by a shape, which is everywhere a lean or a
/// bend is involved: an upright pen cannot follow either, and \bord cannot go negative, so there is
/// no value that would be right.
struct DecorPaint {
	/// The \1c and \1a that paint a shape the border's colour, and the shadow's, ready to write.
	std::string border;
	std::string shadow;
	Vector2D pen;      ///< \xbord and \ybord, or what the style says
	Vector2D offset;   ///< \xshad and \yshad
	/// Whether the letters are painted the border's own colour, in which case a widened shape holds
	/// them already and the line of letters has nothing left to add.
	bool covers = false;
};

/// Read that off a line, taking each of them from the line if the line says and from the style
/// behind it if not - which is what the renderer does.
DecorPaint ReadDecorations(const agi::Context *c, AssDialogue *line);

/// The rings that widen a shape by an upright elliptical pen: a band along every edge and the pen's
/// own shape at every corner that turns away from the ink. They are meant to be added to the shape's
/// own rings and read by the non-zero rule, which is what leaves a hole a hole and the middle solid.
///
/// The rings given have to be wound against each other the way a fill needs them - which is how they
/// come out of a font, and out of any drawing that fills correctly.
std::vector<std::vector<Vector2D>> WidenRings(std::vector<std::vector<Vector2D>> const& rings,
                                             double rx, double ry);

/// One per stretch of the line, because a stretch can be painted differently from its neighbour.
/// The shapes are in the frame the line's own tags act on, so that giving them those tags puts
/// them exactly where the letters are - which is what makes them follow a lean.
struct Decoration {
	/// How far the stretch's own corner - the point the renderer leans and turns it about - is
	/// from where the line is anchored, in script units as the line reads now.
	Vector2D lean;
	std::string letters;       ///< the stretch's letters
	std::string bordered;      ///< the same, widened by the border the line asked for
	std::string border_paint;  ///< the \1c and \1a that paint a shape the border's colour
	std::string shadow_paint;
	Vector2D shadow;           ///< the offset the line asked for, before any turn
	bool has_border = false;
	bool has_shadow = false;
	/// Whether the widened shape is painted the colour the letters are painted, in which case it
	/// holds them inside it already and the line of letters has nothing left to add.
	bool covers_letters = false;
};

/// Empty when the line has neither a border nor a shadow, and empty as well when it says something
/// a shape cannot stand in for: it moves, or it animates. Nothing is written either way.
std::vector<Decoration> BakeDecorations(const agi::Context *c, AssDialogue *line);

/// Where a line is on the frame the video is showing, and how it gets there.
///
/// \move is a straight run between two points over two times, so where a line is depends on the
/// frame being looked at. That position is what the geometry has to be worked out from; and because
/// the run is a straight mixture of its two ends, and everything these tools do to a point is
/// affine, taking both ends through the same map puts the whole run right rather than only the one
/// frame that was seen.
struct Placement {
	Vector2D at;           ///< where the line is on the frame on screen
	Vector2D first;        ///< the first end of the run, or where it stands if it does not move
	Vector2D second;
	int from = 0;          ///< the two times, in milliseconds from the start of the line
	int to = 0;
	bool timed = false;    ///< whether the line named the times itself
	bool moving = false;   ///< whether the line said \move at all
	bool told = false;     ///< whether it said where it is at all; without it the alignment does
};

/// Read that off a line. `told` is false for a line that says neither \pos nor \move, and then it
/// is for the caller to fill in where the alignment and the margins put it.
Placement WherePlaced(const agi::Context *c, AssDialogue *line);

/// The tag that puts a line's run back, given where its two ends have got to: \move for a line that
/// moves, and \pos for one that does not. The name and the value apart, for a caller that writes
/// its tags one at a time.
std::pair<std::string, std::string> PlacementOverride(Placement const& placed,
                                                      Vector2D first, Vector2D second);
/// The same, written out.
std::string PlacementTag(Placement const& placed, Vector2D first, Vector2D second);

/// One stretch of a line, ready to stand on a line of its own where it stood.
struct SplitLine {
	std::string text;
};

/// Break a line up so that every row, and every stretch that carries its own font metrics,
/// scale or turn, becomes a line of its own standing exactly where it stood.
///
/// A lean is applied from the top left of the row a stretch sits on, not from the stretch
/// itself, so a word in a larger size inside a row leans about a point that is not its own
/// and slides sideways; the rows have the same trouble between them. Once each stretch is a
/// line with its own \pos it leans about itself, and the handles mean what they say.
///
/// Empty when the line has nothing worth breaking up, and empty as well when a piece could
/// not be put back exactly - a \move, a reset partway through, or a turn out of the plane.
/// Nothing is written either way: the caller decides what becomes of the pieces.
std::vector<SplitLine> SplitForShear(const agi::Context *c, AssDialogue *line);

/// Where the points of a drawing really are on screen.
///
/// The numbers in a drawing are not where it is. The renderers hang it from the box its own
/// coordinates make, by the line's alignment - so on anything but \an7 it is already offset
/// by part of its own size - and then scale it, lean it and turn it about the point the line
/// is positioned at. A shape read straight from its numbers is therefore somewhere else than
/// the shape on screen, and bending it there bends the wrong part of the picture.
struct DrawingPlace {
	/// False when the line says something this cannot take out of the numbers - it moves, or
	/// it animates - and the drawing is better left as it was read.
	bool ok = false;
	/// Whether anything was taken out of the numbers, so that the tags saying it have to be
	/// neutralised on the line the result is written to or it would be applied twice.
	bool baked = false;
	/// A point of the drawing, as written, in script coordinates.
	std::function<Vector2D (Vector2D)> map;
};

/// Work that out for one line, given the box its drawing's own coordinates occupy - which is
/// what the alignment hangs it by, so it has to be measured before this can be asked.
DrawingPlace PlaceDrawing(const agi::Context *c, AssDialogue *line,
                          Vector2D low, Vector2D high);

} // namespace text_to_shape
