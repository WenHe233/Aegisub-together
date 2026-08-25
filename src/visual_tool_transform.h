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

/// @file visual_tool_transform.h
/// @brief Reshaping the selected drawings by dragging on the video

#pragma once

#include "text_to_shape.h"
#include "typesetting_textbox.h"
#include "typesetting_transform.h"
#include "visual_feature.h"
#include "visual_tool.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class OpenGLText;

/// A linear map of the plane: what a scale, a turn and a lean come to when they are put
/// together. Only the two by two part - where something sits is kept separately.
struct TransformMatrix2 {
	double a = 1, b = 0;
	double c = 0, d = 1;
};

/// Which shape of transform the handles describe.
enum class VisualToolTransformMode {
	Free,     ///< scale, rotate and move, written as tags rather than baked into a shape
	Arch,     ///< two handles above and two below, bending the top and bottom edges
	Distort,  ///< four corner handles, straight lines staying straight
	Warp      ///< Photoshop's warp: a 3x3 mesh over a bicubic patch
};

/// The buttons of the tool's own bar across the top of the video.
enum class VisualToolTransformAction {
	None,
	Undo,
	Redo,
	AutoPerspectiveReset,
	Apply,
	Cancel,
	/// Whether the border, the shadow and the blur are scaled along with the text.
	RecalcBord,
	RecalcShad,
	/// Whether the border and the shadow are kept exactly, by being drawn as shapes. An upright
	/// pen cannot be leaned or squashed, so this is the only way they can follow one - and it
	/// answers the same question as the two above, which is why it rules them out.
	MaintainDecor,
	RecalcBlur,
	RecalcClip,
	UniformSize,
	/// The chain beside the free transform's scale: held, one slider drives both axes; broken,
	/// each axis has a slider of its own.
	ScaleLink,
	ScaleX,
	ScaleY,
	/// The free transform's turn and the two leans, as numbers rather than as handles.
	Rotation,
	ShearX,
	ShearY,
	/// The two axes a distortion turns out of the plane, so a solved perspective can be
	/// nudged. The third is what dragging the four corners already says.
	DistortAngleX,
	DistortAngleY,
	AutoPerspectiveKeepOriginalSize
};

/// Bend or distort the selected drawings by dragging handles over the video.
///
/// Nothing is written while the reshaping is being worked out. The video is given copies
/// of the lines carrying the reshaped drawing and renders those instead of the originals,
/// so what is on screen is the real result - while the file, the edit box and the undo
/// history stay untouched until Apply. Cancel has nothing to put back, because nothing was
/// ever taken away.
///
/// Every drag maps the *original* drawings rather than the last result, so dragging back
/// and forth cannot make the shape creep.
class VisualToolTransform final : public VisualTool<VisualDraggableFeature> {
	VisualToolTransformMode mode;
	/// A guided distort in which the target quadrilateral is drawn as four directed points.
	bool auto_perspective = false;
	/// Uniform percentage applied to the whole selection by the Scale slider.
	double uniform_size = 100.0;
	/// The same per axis, for when the chain beside it has been broken. Kept in step with
	/// `uniform_size` while it is held, so breaking the chain changes nothing on its own.
	double size_x = 100.0;
	double size_y = 100.0;
	/// Whether the two axes of the scale move together.
	bool scale_linked = true;
	/// What the two axis sliders have added to the perspective a distortion solves, x then y.
	/// Held as an offset so dragging a corner afterwards keeps the nudge.
	double distort_angle_offset[2] = {0, 0};
	/// How much bigger the selection has been made, along each of the box's own axes, counting
	/// only what was asked of the scale. A turn and a lean leave it alone, which is what lets
	/// the border, the shadow and the blur follow the size without following anything else.
	Vector2D scale_growth{1.f, 1.f};
	/// Keep the whole selection's authored dimensions, spacing and relative placement while
	/// applying the target plane's perspective. This deliberately rules out the percentage
	/// slider: the two controls answer the same question.
	bool auto_perspective_keep_original_size = false;
	/// The active row's visible source quadrilateral when it has one; otherwise the complete
	/// selection frame used by Distort. It can be adjusted before the target is drawn.
	Vector2D source_corners[4];
	bool source_moved = false;
	/// True only when the active row is itself a quadrilateral drawing. Otherwise Auto
	/// perspective uses the complete selection's source frame exactly as Distort does.
	bool auto_perspective_active_reference = false;
	static constexpr size_t no_feature = static_cast<size_t>(-1);
	/// Where the four source handles begin in `features`, or no_feature while they are hidden.
	size_t source_feature_first = no_feature;
	/// Where the target-point handles begin. During construction there are as many as have
	/// already been placed; after placement all four remain live.
	size_t target_feature_first = no_feature;
	/// The separate handle which moves the completed target as a whole.
	size_t target_move_feature = no_feature;
	/// The unfinished target path. It is cleared as soon as four valid points are applied,
	/// while the same four positions continue in `corners` as editable target handles.
	std::vector<Vector2D> auto_perspective_points;

	std::unique_ptr<OpenGLText> gl_text;

	/// The drawings being reshaped. Holds the original shapes, the result of the last
	/// mapping, and the outline that is drawn while it is only a preview.
	std::optional<typesetting::ShapeEditor> editor;
	/// A textbox stays live under the tag-based transforms. Its generated rows are only a
	/// rendering of this document, so transform the document boundary instead of each row.
	std::optional<typesetting::textbox::Document> textbox_document;
	std::vector<AssDialogue *> textbox_lines;
	Vector2D textbox_original_corners[4];

	/// The box the drawings lie in, at whatever angle they lie at.
	///
	/// For the free transform this is the rectangle the selection would fill if it neither
	/// leaned nor stood out of the plane: the space every gesture is measured in, where a
	/// scale along x is \fscx and a lean is one number. What is drawn, and what the handles
	/// sit on, is this rectangle put back into the shape the selection really has.
	typesetting::OrientedBox box;
	/// That shape: the map from the box onto the quadrilateral the selection really fills.
	/// Empty for a selection that is a rectangle already, which is most of them.
	typesetting::PointMap frame_shape;
	/// The way back: that quadrilateral onto the box. What the distort, the arch and the warp
	/// are expressed from, so that what their handles say is the change from the shape the
	/// selection rests in rather than from a rectangle it never filled.
	typesetting::PointMap rest_to_box;
	/// And the box's own four corners through it, which is the frame that is drawn.
	Vector2D frame_quad[4];

	/// The net as the selection rests in it, which is what a bend is measured against.
	typesetting::WarpNet rest_net;
	/// The four corners the distort drags. The arch and the warp use the control net.
	Vector2D corners[4];
	/// What those corners come to as a map. Kept rather than built where it is needed, since
	/// it is asked for once per corner of every line on every repaint.
	typesetting::PointMap distort_map;
	typesetting::WarpNet net;

	/// A drag of the whole shape rather than of one handle: the four corners as they were when
	/// it began, and where it was taken hold of. Kept so that every mouse move is worked out
	/// from the start of the gesture and rounding cannot walk the shape away.
	Vector2D hold_corners[4];
	Vector2D hold_start;

	/// A drag of the mesh itself rather than of a handle: where on the patch it started,
	/// what the net was at the time, and where that point of the surface was. Kept so
	/// every mouse move works out from the start of the gesture and the mesh cannot creep.
	double hold_u = 0;
	double hold_v = 0;
	typesetting::WarpNet hold_net;
	Vector2D hold_origin;

	/// One selected line as the free transform found it. Everything it needs is something
	/// ASS can say, so nothing here is baked into a shape.
	struct TagLine {
		AssDialogue *line = nullptr;

		/// A piece of another line, made for the lean and not in the file, so it has to be
		/// kept alive here; `source` is the line in the file it was cut from.
		std::shared_ptr<AssDialogue> owned;
		AssDialogue *source = nullptr;
		/// Whether this line has been cut into pieces, so what is drawn is the pieces and
		/// this line itself has to stop being drawn.
		bool replaced = false;

		/// Where the line stands now. Each drag folds what came before it into these, so
		/// that a gesture is always measured from a standing start.
		Vector2D pos;
		/// Where the line is on the frame on screen, and the run it is on if it moves. `pos` is
		/// that same point: everything is worked out from where the line is now, and both ends of
		/// the run are taken through the gesture when it is written back.
		text_to_shape::Placement placed;
		Vector2D org;        ///< bad when the line has none, and then \frz turns about pos
		Vector2D scale{100.f, 100.f};
		Vector2D shear;      ///< \fax and \fay
		Vector2D bord;       ///< the border, per axis
		Vector2D shad;       ///< the shadow offset, per axis
		double blur = 0;     ///< \blur
		double be = 0;       ///< \be

		/// The line's border and shadow as shapes, in the frame its own tags act on, worked out
		/// once from the line as the tool found it. Only the tags on them change as the gesture
		/// goes on: the shapes are what the letters were, so a lean leans them with the letters.
		std::vector<text_to_shape::Decoration> decor;
		bool decor_built = false;

		/// The line's clip, if it has one. Kept as it was read, so that the gesture is
		/// always applied to the original rather than to its own last result.
		struct Clip {
			bool present = false;
			bool inverse = false;    ///< \iclip rather than \clip
			bool rectangle = false;  ///< the two-corner form
			Vector2D first;
			Vector2D second;
			int scale = 1;           ///< the \clip(<n>, ...) the drawing was written with
			std::string drawing;
		};
		Clip clip;
		float angle = 0;
		/// \frx and \fry. A line with either of these is not a rectangle on screen, so it is
		/// carried as the four corners it really has instead of as a scale and a turn.
		double angle_x = 0;
		double angle_y = 0;
		/// The line's own extents before its scale, which is what the corners are worked out
		/// from - and worked back to.
		Vector2D box_first;
		Vector2D box_second;
		/// The box the letters really fill, in that same frame. Not the same box: a font's cell
		/// keeps room above the capitals for accents and below the baseline for descenders, and
		/// a word with neither reaches into neither. Only the frame is measured from this -
		/// everything written back is worked out from the cell, which is what the renderer
		/// places. False when the letters could not be measured that way.
		Vector2D ink_first;
		Vector2D ink_second;
		bool has_ink = false;
		Vector2D size;       ///< how big it is on screen, with its scale applied
		Vector2D ink;        ///< the top left of its ink, relative to where it is anchored
		int align = 2;
		bool drawing = false;  ///< a shape rather than words, which leans about its own point
		/// Whether the line already says how it wants to be wrapped, and whether it is being
		/// wrapped as it stands. A line that is not wrapped can have wrapping turned off
		/// without moving anything; one that is would collapse into a single row.
		bool has_wrap_style = false;
		bool wrapped = false;
		/// Tag-based transforms must keep multi-word text on the authored rows. Without \q2
		/// libass may reflow it after the transform changes its apparent width.
		bool ensure_q2 = false;

		/// What the line said when the tool opened, so that only what really changed is
		/// written back.
		Vector2D start_pos;
		Vector2D start_org;
		Vector2D start_scale{100.f, 100.f};
		Vector2D start_shear;
		Vector2D start_bord;
		Vector2D start_shad;
		double start_blur = 0;
		double start_be = 0;
		float start_angle = 0;
	};
	std::vector<TagLine> tag_lines;

	/// The same selection with every row, and every stretch that leans about its own point,
	/// on a line of its own. Built the first time a lean is dragged, because that is the only
	/// gesture that needs it, and kept for the rest of the session.
	std::vector<TagLine> split_lines;
	/// Whether the pieces are what the gesture is being applied to.
	bool shear_split = false;
	/// Whether the pieces have been worked out yet. They are only worth building once, and a
	/// selection where no line needs breaking up leaves this true and the pieces empty.
	bool split_built = false;

	/// What the gesture has come to so far, all measured from where it started.
	Vector2D gesture_scale{1.f, 1.f};
	/// How far the box leans, as \fax and \fay do, in the frame the box was read in.
	Vector2D gesture_shear{0.f, 0.f};
	float shear_frame_angle = 0;

	/// Everything done to the box before the gesture in progress, as a map about its middle.
	/// The box itself is never re-measured, so it never jumps: this is what carries the shape
	/// it has taken, including a lean, which a rectangle could not hold.
	TransformMatrix2 frame_linear;
	Vector2D frame_offset{0.f, 0.f};
	float gesture_angle = 0;
	Vector2D gesture_move{0.f, 0.f};
	/// The corner of the box that stays put while the other one is dragged, in the box's
	/// own frame.
	Vector2D gesture_anchor{0.f, 0.f};
	/// Where the mouse was when a move or a rotation began.
	Vector2D gesture_start;
	float gesture_start_angle = 0;
	enum class FreeHoldMode { None, Move, Rotate };
	FreeHoldMode free_hold_mode = FreeHoldMode::None;

	/// Dragging empty space around the arch/warp handles selects every point inside the box.
	bool box_selecting = false;
	bool box_select_add = false;
	Vector2D box_select_start;

	/// Whether anything has been dragged yet, which is what makes Apply worth pressing.
	bool touched = false;

	/// Whether the lines that could not be converted have already been complained about,
	/// so it is said once per session rather than on every refresh.
	bool reported = false;

	/// Set once the tool has asked to be replaced. Several things can end a session at
	/// almost the same moment - a button, a key, the selection changing - and only the
	/// first of them should get to schedule the switch.
	bool leaving = false;

	/// The line the session belongs to. When another one is picked it is a new session:
	/// new shapes, and no history carried over from the old one.
	AssDialogue *session_line = nullptr;

	/// One step of the tool's own history. The handles say everything, because the shape
	/// is always mapped from the untouched original.
	struct HistoryState {
		Vector2D corners[4];
		typesetting::WarpNet net;
		Vector2D scale{1.f, 1.f};
		float angle = 0;
		Vector2D move{0.f, 0.f};
		Vector2D anchor{0.f, 0.f};
		Vector2D shear{0.f, 0.f};
		double uniform_size = 100.0;
		double size_x = 100.0;
		double size_y = 100.0;
		double distort_angle_offset[2] = {0, 0};
		Vector2D scale_growth{1.f, 1.f};
		/// What had been done to the box before this step.
		TransformMatrix2 frame_linear;
		Vector2D frame_offset{0.f, 0.f};
		/// Whether the lines had been broken up for the lean yet, so a step back can put
		/// them together again.
		bool split = false;
		/// Auto perspective: the four points the fit is measured from, and whether a target
		/// had been drawn at all - so a step back can take one away again.
		Vector2D source_corners[4];
		bool source_moved = false;
		std::vector<Vector2D> auto_perspective_points;
		bool touched = false;
	};
	std::vector<HistoryState> undo_history;
	std::vector<HistoryState> redo_history;

	/// Whether the things measured in pixels rather than in ems follow the scale. On by
	/// default: a border that stays put while the text grows reads as a different design.
	bool recalc_bord = true;
	bool recalc_shad = true;
	/// Off by default, because it turns one line into three. The distort has it on from the start:
	/// there is no distort without a lean, and a lean is exactly what a pen cannot follow.
	bool maintain_decor = false;
	bool recalc_blur = true;
	bool recalc_clip = true;

	VisualToolTransformAction hovered_action = VisualToolTransformAction::None;

	/// Whether any line has a border or a shadow worth drawing as a shape, which is what decides
	/// whether accepting adds lines to the file or only rewrites them.
	bool HasDecor() const;

	/// Work out the shapes that stand for every line's border and shadow, for the lines that have
	/// not had theirs worked out yet. Measuring a line costs a font and a walk along every letter
	/// of it, so it is done once and kept - and only when it is wanted at all.
	void EnsureDecor();

	/// One line's border, or its shadow, as the line it has to become: the same tags the letters
	/// get, so that it leans and turns with them, and the shape in place of the words.
	std::string DecorLineText(TagLine const& original,
	                          text_to_shape::Decoration const& decor, bool shadow) const;

	/// Whether the window itself is on its way out, in which case there is no picture left worth
	/// putting right: handing the lines back would only cost one more rendered frame, which is the
	/// flash seen on closing the program in the middle of a session.
	bool WindowGoing() const;

	/// Whether this mode says what it does in tags rather than in an outline. The free
	/// transform and the distort do; the arch and the warp bend curves no tag can describe.
	bool TagsMode() const;
	bool TextBoxMode() const { return textbox_document.has_value(); }
	bool CollectTextBox();
	typesetting::textbox::Document TransformedTextBox() const;

	/// Whether there is anything to work on. The shape modes hold an editor; the modes that
	/// speak in tags hold the lines themselves.
	bool Active() const;

	/// Whether the lines this session is holding are still in the file. Opening another file
	/// destroys all of them, and anything done through those pointers afterwards is a crash.
	bool LinesAlive() const;
	/// The file was replaced wholesale, so there is nothing left to hold on to.
	void OnFileReplaced(int type);
	/// The settings that follow from which transform this is, rather than from anything the user
	/// has said. Applied when the tool opens and again whenever the mode changes.
	void SettleForMode();

	void Collect();
	/// Hand the video the reshaped lines, or the originals again to undo that.
	void SendPreview();
	void ClearPreview();
	/// Lock the line editor while the preview owns what the lines say, because editing them
	/// from under it would be editing something the user cannot see.
	void LockEditing(bool locked);
	/// And the menus that would run a script over the lines while the preview is what is on
	/// screen: the scripts would read the file rather than the picture, and committing would
	/// throw the reshaping away.
	void LockMenus(bool locked);

	/// The controls LockEditing switched off, so that only those are switched back on -
	/// some of them were already unusable for reasons of their own.
	void PlaceFeatures();
	/// Move the existing handles to where the state now says, without building the list
	/// again - which would pull the ground out from under a drag in progress.
	void SyncFeatures();
	/// Work out the preview from where the handles are. Writes nothing.
	void Rebuild();

	/// Read the selected lines as they are, for a transform written in tags.
	bool CollectTags();
	/// Everything one line says that the transform has to know, measured as it stands.
	/// `measure_ink` asks for the exact glyph outline bounds. An active quadrilateral gives Auto
	/// perspective its source directly, so measuring every other selected row's glyph outlines
	/// there is expensive work whose result the projective map never consumes.
	TagLine ReadLine(AssDialogue *line, bool measure_ink = true);
	/// The lines the gesture is applied to: the selection, or its pieces once a lean has
	/// asked for them.
	std::vector<TagLine> const& Lines() const;
	/// Break the selection up so that a lean can be written for each row and each stretch
	/// that leans about its own point. Does nothing when no line needs it.
	void EnsureShearSplit();
	/// Measure the box the handles sit on, from the lines as they now stand.
	/// A known active outline was already validated while choosing Auto perspective's fast
	/// reference path; reusing it avoids decoding and placing that drawing again.
	void BuildBox(std::vector<Vector2D> const *known_active_outline = nullptr);
	/// Seed the yellow source from an active quadrilateral or Distort's selection frame.
	/// The optional outline is the already placed active drawing gathered while measuring the
	/// box, so entering the tool does not parse and project the same drawing a second time.
	void BuildAutoPerspectiveBox(std::vector<Vector2D> const *active_outline = nullptr);
	/// The nth handle. The features are an intrusive list, and there are never more than a
	/// handful of them, so walking to one costs nothing worth a lookup table.
	VisualDraggableFeature *FeatureAt(size_t index);
	/// The selected line the tool works from, or nothing when it is not among the lines
	/// this tool collected.
	TagLine const *ActiveTagLine() const;
	/// A line's drawing, taken to where the renderer puts it on screen.
	std::vector<Vector2D> ShapeOutline(TagLine const& found, bool unskewed = false);
	/// The map from the source quadrilateral onto the drawn one.
	typesetting::PointMap AutoPerspectiveMap() const;
	/// The projective mapping shared verbatim by Distort and Auto perspective.
	typesetting::PointMap DistortionMap(Vector2D const target[4],
		typesetting::PointMap inverse) const;
	/// Build the quadrilateral occupied by the selection at its authored size, on the
	/// perspective plane described by the four target points and centred inside them.
	bool AutoPerspectiveOriginalSizeTarget(Vector2D target[4]) const;

	/// What one line becomes under the gesture in progress: the numbers, before anything is
	/// decided about which of them are worth writing. The one place that works this out, so
	/// that writing the tags and folding the gesture into the starting point cannot drift
	/// apart.
	struct Applied {
		Vector2D pos;
		Vector2D org;
		Vector2D scale{100.f, 100.f};
		double shear_x = 0;
		double angle = 0;
		/// Only for a line turned out of the plane, where these are solved from the corners
		/// rather than left alone.
		bool perspective = false;
		double shear_y = 0;
		double angle_x = 0;
		double angle_y = 0;
	};
	Applied ApplyGesture(TagLine const& original) const;

	/// Where the two ends of a line's run have got to under the gesture - the same point twice for
	/// a line that does not move. A run is a straight mixture of its ends and the gesture is affine,
	/// so taking the ends through it is exact on every frame of the run.
	std::pair<Vector2D, Vector2D> MovedEnds(TagLine const& original, Applied const& applied) const;
	/// Whether this line is turned out of the plane. Only that: a plane is what a distortion
	/// solves, and a lean has none.
	static bool Perspective(TagLine const& original);
	/// Whether this line stands on screen as anything other than a rectangle, and so has to be
	/// carried by its four corners rather than by a size and a turn.
	///
	/// A turn out of the plane does that. So does a lean: \fax slides the top of the box away
	/// from the bottom and leaves a parallelogram, and a rectangle the width of the box is then
	/// short by the whole of the slide - on a title leaning by one and two thirds, a third of
	/// its own length.
	static bool Skewed(TagLine const& original);
	/// The extents the frame is measured from: the letters where they can be measured, moved
	/// so that hanging them by the line's own alignment still lands on the letters.
	void FrameExtents(TagLine const& original, Vector2D& first, Vector2D& second) const;
	/// The rectangle a line would fill if it neither leaned nor had been turned out of the
	/// plane - its scale and its \frz kept, nothing else. This is what the box is measured
	/// from, so that the box stays the one space a scale and a lean mean something in.
	bool UnskewedCorners(TagLine const& original, Vector2D corners[4]) const;
	/// Work out `frame_shape` and `frame_quad` from the line the tool works from. One shape for
	/// the whole selection: lines of a group are leaned and turned together, so it is exact for
	/// them, and where they disagree there is no one shape to take and the frame stays flat.
	/// Given a set of lines, the shape is worked out from those instead of from the selection's
	/// own - which is what lets the modes that convert text to drawings read the placement out
	/// of the tags they still have.
	/// The optional hull is the already calculated hull of the selection. Building the box and
	/// finding its authored quadrilateral need the same geometry; sharing it avoids another full
	/// walk and sort of every drawing point when the tool opens.
	void BuildFrameShape(std::vector<TagLine> const *given = nullptr,
		std::vector<Vector2D> const *selection_hull = nullptr);
	/// The same, for the modes that work on converted drawings and have no tags to read: the
	/// tightest parallelogram round the letters themselves.
	void BuildEditorFrameShape();
	/// Take four points as the frame, put in the order the box's own corners are in. False for
	/// anything that would not make a frame: the wrong number of points, or a quadrilateral
	/// folded over itself.
	bool SetFrameQuad(std::vector<Vector2D> const& quad);
	/// The same, for four points that are already in that order.
	bool SetFrameQuadInOrder(Vector2D const quad[4]);
	/// A point of the box, in the shape the selection really has. The identity where there is
	/// no shape to apply, so every caller can use it without asking.
	Vector2D ShapePoint(Vector2D box_local) const;
	/// The middle of that shape, which is what a turn turns about and what the frame's own
	/// translation is measured from.
	Vector2D FrameOrigin() const;

	/// The four corners of a line's box on screen, as it was read.
	void LineQuad(TagLine const& original, Vector2D corners[4]) const;
	/// The same for any box of the line's own extents, which is what lets the letters be gone
	/// round instead of the cell they sit in.
	void LineQuad(TagLine const& original, Vector2D first, Vector2D second,
		Vector2D corners[4]) const;
	/// The four corners of the letters themselves, wherever they stand - turned, leaning or
	/// out of the plane. False for a line whose letters could not be measured, and then the
	/// cell is all there is to go round.
	bool InkCorners(TagLine const& original, Vector2D corners[4]) const;
	/// Where a line stands on screen: its quadrilateral if it is turned out of the plane,
	/// and its turned box if it is not.
	void LineCorners(TagLine const& original, Vector2D corners[4]) const;
	/// The largest authored perspective plane in the selection, after the distortion already
	/// present at the start of a move. Used so even the first move follows the line's own plane.
	bool PerspectiveMovePlane(Vector2D const held_corners[4],
		typesetting::OrientedBox& source, Vector2D projected[4]) const;
	/// What one line would say with the gesture applied.
	std::string TagLineText(TagLine const& original) const;
	/// The same gesture applied to a clip: its coordinates are script coordinates like any
	/// others. Returns an empty string when there is nothing to write.
	std::string MapClip(TagLine::Clip const& clip) const;
	/// What clip a line carries, if any.
	static TagLine::Clip ReadClip(AssDialogue *line);
	/// Carry the transform into the tags set partway through a line. A value written there
	/// overrides the one at the start, so without this the tail of a line would ignore the
	/// whole gesture.
	///
	/// Called before the start of the line is written, so that the state in force at each of
	/// the later blocks can still be read from the line itself.
	void AdjustLaterTags(AssDialogue *line, TagLine const& original, Vector2D scale_ratio,
	                     double turn, double grow_x, double grow_y, double grow) const;
	/// Where a point of the box ends up under the gesture in progress.
	Vector2D MapPoint(Vector2D point) const;
	/// The frame and the gesture in progress as one map, which is everything that has been
	/// done to the selection since the tool opened.
	TransformMatrix2 TotalMatrix() const;
	/// How much bigger the selection has become along each axis, which is what the things
	/// measured in pixels - the border, the shadow, the blur - have to be told.
	Vector2D FrameGrowth() const;
	/// Whether what has been done leaves a rectangle a rectangle. Only then can a rectangular
	/// clip stay one.
	bool SquareOn() const;

	/// A point of the box under the gesture alone, before the frame is applied. This is the
	/// space a gesture is measured in, and the space a handle's own maths works in.
	Vector2D GesturePoint(Vector2D point) const;
	/// The frame, and its inverse - which is how a mouse position on screen is brought back
	/// into the space the gesture works in.
	Vector2D FramePoint(Vector2D point) const;
	Vector2D FrameInverse(Vector2D point) const;
	/// What the gesture turns about, on screen.
	Vector2D GesturePivot() const;
	/// Compose the gesture onto the frame and start a fresh one. Called at the beginning of
	/// every drag, so that no gesture is ever measured against a state another gesture has
	/// already moved - and because it is a multiplication, nothing is re-measured and nothing
	/// jumps.
	void RebaseGesture();
	/// The same, but leaving the lean live.
	///
	/// The lean is the one part of a gesture with a number of its own on the bar, and a slider
	/// reading nought while the box went on leaning would be lying about it. Kept outside the
	/// frame it also goes on meaning the same lean however the box is scaled afterwards, where
	/// folding it in would leave the same shape reading as a different number.
	void RebaseKeepingShear();
	/// Feature index -> which corner or side of the box it drags, which point stays, and
	/// what it does: 0 sizes, 1 turns, 2 leans.
	void HandleRole(int index, Vector2D& grabbed, Vector2D& anchor, int& role) const;

	/// How many handles this mode has, and which point of the net each one is.
	int HandleCount() const;
	Vector2D HandlePosition(int index) const;
	void MoveHandle(int index, Vector2D to);

	HistoryState Capture() const;
	void RestoreState(HistoryState const& state);
	void PushHistory();
	bool UndoHistory();
	bool RedoHistory();

	/// Write the reshaping into the lines and leave the tool.
	void Accept();
	/// Leave without writing anything.
	void Reject();
	/// Hand the video back to the tool that was in use before this one.
	void ExitTool();

	wxString LabelFor(VisualToolTransformAction action) const;
	void UpdatePreviewInterface() const;
	std::pair<Vector2D, Vector2D> ActionBounds(VisualToolTransformAction action) const;
	float TopBarHeight() const;
	VisualToolTransformAction ActionAt(Vector2D point) const;
	bool ActionEnabled(VisualToolTransformAction action) const;
	void Perform(VisualToolTransformAction action);
	void UpdateUniformSize(double value);
	/// Grow the selection along the box's own axes, about where it is displayed. One place for
	/// the linked slider and the two separate ones, so they cannot drift apart.
	void ApplyScaleRatio(double ratio_x, double ratio_y);
	void UpdateScaleAxis(VisualToolTransformAction action, double value);
	/// Turn the whole selection to the angle asked for, measured as the active line's \frz.
	void UpdateRotation(double value);
	/// Lean the box, which is the same thing the two leaning handles do - so the number and
	/// the handle say and set exactly the same quantity.
	void UpdateShear(bool vertical, double value);
	/// Nudge one of the two axes a distortion turns out of the plane.
	void UpdateDistortAngle(int axis, double value);
	/// How much bigger the selection has been made by the scale alone, gesture included. This is
	/// what the border, the shadow and the blur follow: a turn or a lean changes neither of them.
	Vector2D DecorGrowth() const;
	/// What the sliders have to show: the line the numbers are read from, and the numbers
	/// themselves as they now stand.
	Applied ActiveApplied() const;
	void DrawTopBar();
	/// The rectangle the fit is measured from, in yellow dashes.
	void DrawAutoPerspectiveSource();
	void DrawAutoPerspectivePath();
	bool AddAutoPerspectivePoint(Vector2D point);
	/// The free transform's own handles: plain outlines rather than the crossed blocks the
	/// other tools use, because they sit close together around the box.
	void DrawFreeHandles();
	void DrawFreeRotationGuide();
	bool FreePointInside(Vector2D point) const;
	/// One corner: a small empty square, which is what a corner looks like in every mode.
	void DrawCorner(Vector2D at, bool current, bool selected = false);
	/// The handles of the modes that are not the free transform. The corners are drawn as
	/// corners; everything else is left to the framework, which already draws it well.
	void DrawShapeHandles();

	/// Swallowed. The framework commits after every mouse move of a drag, which is how
	/// the other tools push their changes out - and a commit makes the video re-read the
	/// file, throwing away the preview it was holding. Applying commits explicitly.
	void Commit(wxString message = wxString()) override;

	bool InitializeDrag(VisualDraggableFeature *feature) override;
	void UpdateDrag(VisualDraggableFeature *feature) override;

	bool InitializeHold() override;
	void UpdateHold() override;
	void EndHold() override;

	void DoRefresh() override;
	void OnLineChanged() override;
	/// Zooming or panning the video changes nothing about the shapes, so it must not send
	/// the tool back to the file - that used to throw the reshaping away.
	void OnCoordinateSystemsChanged() override;
	void Draw() override;

	/// Any change of what is selected ends the session: the shapes it is holding belong
	/// to the lines that were selected when it started.
	agi::signal::Connection selection_connection;

	/// The visual tool command to go back to when this one is done with.
	std::string return_tool;

public:
	VisualToolTransform(VideoDisplay *parent, agi::Context *context,
	                    VisualToolTransformMode mode, std::string return_tool,
	                    bool auto_perspective = false);
	~VisualToolTransform();

	/// Change which transform this is without closing and reopening. Everything under way is
	/// discarded, as it would have been either way; the tool - and its bar - stay put.
	void SetMode(VisualToolTransformMode next, bool perspective);

	void OnMouseEvent(wxMouseEvent &event) override;
	bool OnMouseWheel(wxMouseEvent &event) override;
	bool OnKeyEvent(wxKeyEvent &event) override;

private:
	/// Escape, Enter and the history keys, caught on the whole window rather than on the
	/// video: the video only sees them while it has the focus, and it does not always have
	/// it - which is why Escape sometimes did nothing.
	void OnCharHook(wxKeyEvent &event);
	bool HandleKey(int key, bool control, bool shift);
};
