// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include "visual_feature.h"
#include "visual_tool.h"
#include "visual_color_segment.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class AssStyle;
class AssDialogue;
class wxImage;
class wxCommandEvent;
class wxKeyEvent;
class wxToolBar;
class OpenGLText;
struct VideoFrame;
namespace agi { struct Color; }

enum VisualToolMaskMode {
	MASK_RECTANGLE = 0,
	MASK_POINTS,
	/// The same points as MASK_POINTS, joined by curves instead of straight edges. The two
	/// share their point list, which is why switching between them keeps the work.
	MASK_BEZIER,
	MASK_BRUSH,
	MASK_FREEHAND,
	MASK_COLOR,
	MASK_LAST
};

enum class VisualToolMaskAction {
	None,
	RangeShape,
	Undo,
	Redo,
	Create,
	Clear,
	RemoveText,
	GenerateText,
	AutoFill,
	SelectionMode,
	AISelect,
	Templates,
	SmoothEdges,
	EdgeSnap,
	MaskBrushSize,
	Tolerance,
	Offset,
	SmoothTolerance,
	SmoothAngle,
	EdgeSnapRadius,
	ColorBrushSize
};

/// Draws a line-only polygon on the video and creates an ASS drawing mask.
class VisualToolMask final : public VisualTool<VisualDraggableFeature> {
	wxToolBar *toolBar = nullptr;
	std::unique_ptr<OpenGLText> gl_text;
	VisualToolMaskMode mode = MASK_POINTS;
	/// Line and curve differ only in how the points they share are joined, so nearly every
	/// test in the tool is about the pair rather than about one of them.
	bool IsPointMode() const { return mode == MASK_POINTS || mode == MASK_BEZIER; }
	/// When Alt was last pressed, so a second press soon after can be told from the first
	/// and used to flip between the two tools of a pair.
	long long last_alt_press_ms = 0;
	std::vector<Vector2D> points;
	/// Whether the edge leaving points[i] is a curve. Kept per edge rather than taken from
	/// the current tool, so switching between line and curve decides what the next edge
	/// will be without rewriting the ones already placed.
	std::vector<char> point_curved;
	/// The two handles shaping a curved edge, one per end. Only meaningful where
	/// point_curved says so; they are seeded to the smooth default when an edge becomes a
	/// curve, and are the user's to move from then on.
	std::vector<Vector2D> edge_control_out;
	std::vector<Vector2D> edge_control_in;
	/// Handle being hovered or dragged, as edge * 2 plus 0 for the outgoing handle and 1
	/// for the incoming one. -1 when none.
	/// Handle size, read from the same setting the vector clip uses so the two tools draw
	/// points that are the same size and shape.
	int featureSize = 0;
	int hovered_control = -1;
	int dragged_control = -1;
	std::vector<std::vector<Vector2D>> mask_regions;
	struct MaskHistoryState {
		std::vector<Vector2D> points;
		std::vector<char> point_curved;
		std::vector<Vector2D> edge_control_out;
		std::vector<Vector2D> edge_control_in;
		std::vector<std::vector<Vector2D>> regions;
	};
	std::vector<MaskHistoryState> mask_undo_history;
	std::vector<MaskHistoryState> mask_redo_history;
	Vector2D shape_start;
	bool drawing = false;
	bool mouse_inside = false;
	int hovered_point = -1;
	int dragged_point = -1;
	float mask_brush_radius = 22.f;
	bool mask_brush_drawing = false;
	bool mask_brush_slider_dragging = false;
	Vector2D mask_brush_last;
	/// Centre of the circle the brush stamped last, so a stroke made of many small motions
	/// does not stamp the same one again at the start of every one of them.
	Vector2D mask_brush_last_centre;
	/// The brush's stamped circles merged into one region, held apart from mask_regions so
	/// the merge is carried forward rather than redone from the contours on every stamp.
	struct BrushShape;
	std::unique_ptr<BrushShape> mask_brush_shape;
	VisualToolMaskAction hovered_action = VisualToolMaskAction::None;
	VisualToolMaskAction external_slider_action = VisualToolMaskAction::None;
	enum class ColorStage { Range, Sample, Ready };
	enum class ColorRangeShape { Rectangle, Freehand };
	ColorStage color_stage = ColorStage::Range;
	Vector2D color_range_start;
	Vector2D color_range_end;
	ColorRangeShape color_range_shape = ColorRangeShape::Rectangle;
	/// The freehand range in script coordinates; empty while the range is a
	/// rectangle, in which case color_range_start/end describe it on their own.
	std::vector<Vector2D> color_range_path;
	std::vector<std::vector<Vector2D>> color_contours;
	VisualColorSegmenter color_segmenter;
	bool color_contours_dirty = false;
	VisualColorSample color_sample;
	bool has_color_sample = false;
	int color_frame_width = 0;
	int color_frame_height = 0;
	double color_tolerance = 2.0;
	int color_offset = 0;
	bool tolerance_dragging = false;
	bool offset_dragging = false;
	bool smooth_tolerance_dragging = false;
	bool smooth_angle_dragging = false;
	bool edge_snap_radius_dragging = false;
	bool brush_slider_dragging = false;
	bool color_auto_fill = false;
	bool color_smooth_edges = false;
	double color_smooth_tolerance = 10.0;
	double color_smooth_angle = 35.0;
	bool color_edge_snap = false;
	double color_edge_snap_radius = 6.0;
	std::vector<VisualColorSampleOperation> color_sample_operations;
	bool color_ai_base = false;
	VisualSelectionMode color_selection_mode = VisualSelectionMode::PipetteAdd;
	float color_brush_radius = 20.f;
	bool color_brush_drawing = false;
	bool color_brush_moved = false;
	Vector2D color_brush_last;
	std::vector<Vector2D> color_brush_stroke;
	struct ColorHistoryState {
		VisualColorSegmenter segmenter;
		VisualColorSample sample;
		ColorStage stage = ColorStage::Sample;
		double tolerance = 2.0;
		int offset = 0;
		bool has_sample = false;
		bool auto_fill = false;
		bool smooth_edges = false;
		double smooth_tolerance = 10.0;
		double smooth_angle = 35.0;
		bool edge_snap = false;
		double edge_snap_radius = 6.0;
		bool contours_only = false;
		std::vector<std::vector<Vector2D>> contours;
		std::vector<VisualColorSampleOperation> sample_operations;
		bool ai_base = false;
	};
	mutable std::vector<std::vector<Vector2D>> color_display_contours;
	mutable bool color_display_dirty = true;
	mutable size_t color_display_source_points = 0;
	mutable std::map<std::string, float> text_width_cache;
	std::vector<ColorHistoryState> color_undo_history;
	std::vector<ColorHistoryState> color_redo_history;
	bool ai_refining = false;
	std::unique_ptr<wxImage> ai_working_image;
	std::vector<unsigned char> ai_selection_mask;
	std::vector<std::string> ai_prompt_history;
	AssDialogue *ai_session_line = nullptr;
	int ai_crop_x = 0;
	int ai_crop_y = 0;
	int ai_crop_width = 0;
	int ai_crop_height = 0;

	void AddTool(std::string const& command_name, VisualToolMaskMode tool_mode);
	void OnSubTool(wxCommandEvent& event);
	void UpdateCursor();
	void UpdateRectangle(Vector2D end);
	void UpdateFreehand(Vector2D point);
	void CommitCurrentRegion();
	void PaintMaskBrush(Vector2D from, Vector2D to);
	void UpdateMaskBrushSize(Vector2D point);
	void UpdateActionTooltip(VisualToolMaskAction action);
	bool CanCreateMask() const;
	bool CanCancel() const;
	bool CanOffsetSelection() const;
	std::pair<Vector2D, Vector2D> ActionBounds(VisualToolMaskAction action) const;
	float TopBarHeight() const;
	float SliderLabelWidth(wxString const& label) const;
	float MeasuredTextWidth(wxString const& label, bool bold) const;
	std::pair<Vector2D, Vector2D> ToleranceBounds() const;
	std::pair<Vector2D, Vector2D> OffsetBounds() const;
	std::pair<Vector2D, Vector2D> SmoothToleranceBounds() const;
	std::pair<Vector2D, Vector2D> SmoothAngleBounds() const;
	std::pair<Vector2D, Vector2D> EdgeSnapBounds() const;
	std::pair<Vector2D, Vector2D> EdgeSnapRadiusBounds() const;
	std::pair<Vector2D, Vector2D> BrushBounds() const;
	std::pair<Vector2D, Vector2D> MaskBrushBounds() const;
	VisualToolMaskAction ActionAt(Vector2D point);
	int PointAt(Vector2D point) const;
	/// Hit test the curve handles, which sit above the points they belong to.
	int ControlAt(Vector2D point) const;
	/// Give every edge the handles a smooth curve through the points would have, for those
	/// that have none yet. Called whenever the points move, since a handle only makes sense
	/// relative to the ends of its own edge.
	void SeedEdgeControls();
	void ClearPreview();
	MaskHistoryState CaptureMaskHistory() const;
	void RestoreMaskHistory(MaskHistoryState state);
	void PushMaskHistory();
	bool UndoMaskHistory();
	bool RedoMaskHistory();
	void ResetColorSelection();
	ColorHistoryState CaptureColorHistory() const;
	ColorHistoryState CaptureColorBrushHistory() const;
	void RestoreColorHistory(ColorHistoryState state);
	void PushColorHistory();
	void PushColorBrushHistory();
	bool UndoColorHistory();
	bool RedoColorHistory();
	void UpdateColorTolerance(Vector2D point);
	void UpdateColorOffset(Vector2D point);
	void UpdateSmoothTolerance(Vector2D point);
	void UpdateSmoothAngle(Vector2D point);
	void UpdateEdgeSnapRadius(Vector2D point);
	void UpdateColorBrushSize(Vector2D point);
	bool PrepareColorSelection(Vector2D sample_point);
	bool PrepareAISelection();
	bool EnsureColorSegmenter();
	void ShowColorModeMenu();
	void ShowRangeShapeMenu();
	std::vector<Vector2D> ColorRangeBoundary() const;
	void ShowColorTemplatesMenu();
	void ShowAISelectionMenu();
	bool CanCaptureColorTemplate() const;
	VisualColorTemplate CaptureColorTemplate(std::string name) const;
	bool LoadColorTemplate(VisualColorTemplate const& color_template);
	void UpdateColorCursor();
	void RefreshColorContours();
	std::vector<std::vector<Vector2D>> SnapColorContoursToEdges(
		std::vector<std::vector<Vector2D>> raw_contours) const;
	std::vector<std::vector<Vector2D>> const& ColorDisplayContours() const;
	std::vector<std::vector<SplineCurve>> BuildSmoothedColorSplines() const;
	void PaintColorBrush(Vector2D from, Vector2D to);
	void SyncColorSegmenterFromContours();
	void ResetAIRefinement();
	bool AcceptAIRefinement();
	std::unique_ptr<wxImage> RunAIImageEdit(wxImage const& scene, wxImage const& mask,
		std::string const& prompt, wxString const& progress_message, bool use_cloudinary);
	void DrawTopBar();
	void UpdatePreviewInterface();
	void PerformPreviewAction(VisualToolMaskAction action);
	void UpdateExternalSlider(VisualToolMaskAction action, double value, bool final);
	Vector2D ColourSamplePoint() const;
	agi::Color SampleColour(AssStyle const *style) const;
	std::string EncodeDrawing() const;
	void CreateMask();
	void CreateAIMask(VisualToolMaskAction action);
	std::vector<unsigned char> EncodePng(wxImage const& image) const;
	std::vector<AssDialogue *> ConvertImageToAss(wxImage image, AssDialogue *source,
		int crop_x, int crop_y, int crop_width, int crop_height) const;

	void OnCoordinateSystemsChanged() override;
	void Draw() override;

public:
	VisualToolMask(VideoDisplay *parent, agi::Context *context);
	~VisualToolMask();

	void OnMouseEvent(wxMouseEvent& event) override;
	bool OnMouseWheel(wxMouseEvent& event) override;
	bool OnKeyEvent(wxKeyEvent& event) override;
	void SetToolbar(wxToolBar *toolbar) override;
	void SetSubTool(int subtool) override;
	int GetSubTool() override;
};
