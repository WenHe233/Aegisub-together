// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include "visual_feature.h"
#include "visual_tool.h"
#include "visual_color_segment.h"
#include "spline.h"
#include "command/command.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

class OpenGLText;
class wxToolBar;
class wxCommandEvent;
class wxPopupTransientWindow;

/// Button IDs
enum VisualToolVectorClipMode {
	VCLIP_DRAG = 0, // Assumed to be at the start
	VCLIP_LINE,
	VCLIP_BICUBIC,
	VCLIP_BRUSH,
	VCLIP_CONVERT,
	VCLIP_APPEND,
	VCLIP_INSERT,
	VCLIP_REMOVE,
	VCLIP_FREEHAND,
	VCLIP_FREEHAND_SMOOTH,
	VCLIP_COLOR,
	VCLIP_LAST // Leave this at the end and don't use it
};

enum VisualToolVectorClipUpdate {
	VCLIP_BRUSH_ACTION_ADD = 1,
	VCLIP_BRUSH_ACTION_DELETE
};

/// @class VisualToolVectorClipDraggableFeature
/// @brief VisualDraggableFeature with information about a feature's location
///        in the spline
struct VisualToolVectorClipDraggableFeature final : public VisualDraggableFeature {
	/// Which curve in the spline this feature is a point on
	size_t idx = 0;
	/// First curve of the contour containing this feature
	size_t path_start = 0;
	/// 0-3; indicates which part of the curve this point is
	int point = 0;
};

class VisualToolVectorClip : public VisualTool<VisualToolVectorClipDraggableFeature> {
	std::unique_ptr<OpenGLText> gl_text;
	bool drawing_mode = false;
	Spline spline; /// The current spline
	wxToolBar *toolBar = nullptr; /// The subtoolbar
	int mode = VCLIP_DRAG; /// 0-8
	bool inverse = false; /// is iclip?
	int featureSize = 0;
	static constexpr size_t no_path = static_cast<size_t>(-1);
	size_t active_path_start = no_path;
	size_t held_curve_index = no_path;
	std::vector<Feature *> held_curve_features;
	bool drag_commit_pending = false;
	enum class PointSelectionShape { Rectangle, Freehand };
	PointSelectionShape drag_selection_shape = PointSelectionShape::Rectangle;
	PointSelectionShape remove_selection_shape = PointSelectionShape::Rectangle;
	std::vector<Vector2D> selection_lasso;
	enum class ColorStage { Range, Sample, Ready };
	enum class ColorRangeShape { Rectangle, Freehand };
	enum class ColorAction { None, RangeShape, Undo, Redo, SelectionMode, Templates, AISelect, AutoFill, SmoothEdges, EdgeSnap, Accept, Cancel };
	ColorStage color_stage = ColorStage::Range;
	ColorAction hovered_color_action = ColorAction::None;
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
	double color_tolerance = 2.0;
	int color_offset = 0;
	bool has_color_sample = false;
	bool color_auto_fill = false;
	bool color_smooth_edges = false;
	double color_smooth_tolerance = 10.0;
	double color_smooth_angle = 35.0;
	bool color_edge_snap = false;
	double color_edge_snap_radius = 6.0;
	int color_frame_width = 0;
	int color_frame_height = 0;
	bool color_drawing = false;
	bool tolerance_dragging = false;
	bool offset_dragging = false;
	bool smooth_tolerance_dragging = false;
	bool smooth_angle_dragging = false;
	bool edge_snap_radius_dragging = false;
	bool brush_slider_dragging = false;
	VisualSelectionMode color_selection_mode = VisualSelectionMode::PipetteAdd;
	using ColorSampleOperation = VisualColorSampleOperation;
	std::vector<ColorSampleOperation> color_sample_operations;
	bool color_ai_base = false;
	bool brush_add_mode = true;
	float color_brush_radius = 20.f;
	bool color_brush_drawing = false;
	bool color_brush_moved = false;
	Vector2D color_brush_last;
	std::vector<Vector2D> color_brush_stroke;
	std::vector<SplineCurve> color_brush_base_spline;
	bool color_brush_preview_changed = false;
	struct BrushGeometryCache;
	std::unique_ptr<BrushGeometryCache> color_brush_geometry;
	long long last_alt_press_ms = 0;
	int color_return_mode = VCLIP_DRAG;
	wxPopupTransientWindow *brush_popup = nullptr;
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
		std::vector<ColorSampleOperation> sample_operations;
		bool ai_base = false;
	};
	using ColorTemplate = VisualColorTemplate;
	mutable std::vector<std::vector<Vector2D>> color_display_contours;
	mutable bool color_display_dirty = true;
	mutable size_t color_display_source_points = 0;
	mutable std::map<std::string, float> text_width_cache;
	std::vector<ColorHistoryState> color_undo_history;
	std::vector<ColorHistoryState> color_redo_history;

	std::set<Feature *> box_added;

	Vector2D drawing_pos;
	Vector2D drawing_org;
	Vector2D drawing_scale = Vector2D(100.f, 100.f);
	Vector2D drawing_alignment_shift;
	float drawing_rotation = 0.f;
	int drawing_scale_level = 1;

	void Save(int precision_override = -1);
	void Commit(wxString message="") override;
	void OnFrameChanged() override;
	void OnCoordinateSystemsChanged() override;
	void OnLineChanged() override;
	Vector2D DrawingToScreen(Vector2D point) const;
	Vector2D ScreenToAlignedDrawing(Vector2D point) const;
	void TransformSplineToScreen();
	std::string EncodeDrawing();

	void AddTool(std::string command_name, VisualToolVectorClipMode mode);
	void OnToolbar(wxCommandEvent& event);
	void ShowBrushSettings();
	void UpdateBrushToolbar();
	void ShowPointSelectionMenu(int subtool);
	PointSelectionShape PointSelectionShapeForMode() const;
	bool PointInsideCurrentSelection(Vector2D point) const;
	void OnSubtitleCommit(int type);
	size_t PathEnd(size_t path_start) const;
	std::vector<float> PathPoints(size_t path_start) const;
	void NormalizeActivePath();
	bool SelectPathAt(Vector2D point);
	void ResetColorSelection();
	ColorHistoryState CaptureColorHistory() const;
	ColorHistoryState CaptureColorBrushHistory() const;
	void RestoreColorHistory(ColorHistoryState state);
	void PushColorHistory();
	void PushColorBrushHistory();
	bool UndoColorHistory();
	bool RedoColorHistory();
	bool InitializeBrushSelection();
	bool PrepareColorSelection(Vector2D sample_point);
	bool PrepareAISelection();
	bool EnsureColorSegmenter();
	bool CanOffsetSelection() const;
	void ShowColorModeMenu();
	void ShowRangeShapeMenu();
	std::vector<Vector2D> ColorRangeBoundary() const;
	void ShowColorTemplatesMenu();
	void ShowAISelectionMenu();
	void UpdateColorTooltip();
	bool CanCaptureColorTemplate() const;
	ColorTemplate CaptureColorTemplate(std::string name) const;
	bool LoadColorTemplate(ColorTemplate const& color_template);
	static std::vector<ColorTemplate>& ColorTemplates();
	void UpdateColorCursor();
	void RefreshColorContours();
	std::vector<std::vector<Vector2D>> SnapColorContoursToEdges(
		std::vector<std::vector<Vector2D>> raw_contours) const;
	std::vector<std::vector<Vector2D>> const& ColorDisplayContours() const;
	std::vector<std::vector<SplineCurve>> BuildSmoothedColorSplines() const;
	void PaintColorBrush(Vector2D from, Vector2D to);
	void PaintSelectionBrush(Vector2D from, Vector2D to);
	void SyncColorSegmenterFromContours();
	bool ApplyBrushStroke(std::vector<Vector2D> const& stroke);
	void UpdateColorTolerance(Vector2D point);
	void UpdateColorOffset(Vector2D point);
	void UpdateSmoothTolerance(Vector2D point);
	void UpdateSmoothAngle(Vector2D point);
	void UpdateEdgeSnapRadius(Vector2D point);
	void UpdateColorBrushSize(Vector2D point);
	float SliderLabelWidth(wxString const& label) const;
	float MeasuredTextWidth(wxString const& label, bool bold) const;
	std::pair<Vector2D, Vector2D> ColorToleranceBounds();
	std::pair<Vector2D, Vector2D> ColorOffsetBounds() const;
	std::pair<Vector2D, Vector2D> SmoothToleranceBounds();
	std::pair<Vector2D, Vector2D> SmoothAngleBounds();
	std::pair<Vector2D, Vector2D> EdgeSnapBounds();
	std::pair<Vector2D, Vector2D> EdgeSnapRadiusBounds();
	std::pair<Vector2D, Vector2D> ColorBrushBounds();
	std::pair<Vector2D, Vector2D> ColorActionBounds(ColorAction action);
	float ColorTopBarHeight();
	ColorAction ColorActionAt(Vector2D point);
	void DrawColorMode();
	void AcceptColorContours();
	void CommitBrushContours();
	void CloseColorMode();
	bool DeleteActivePath();

	void MakeFeature(size_t idx, size_t path_start);
	void MakeFeatures();
	void SyncCurveFeatures(size_t idx);

	bool InitializeHold() override;
	void OnDoubleClick() override;
	void UpdateHold() override;
	void EndHold() override;

	void UpdateDrag(Feature *feature) override;
	bool InitializeDrag(Feature *feature) override;
	void EndDrag(Feature *feature) override;

	void DoRefresh() override;
	void Draw() override;

public:
	VisualToolVectorClip(VideoDisplay *parent, agi::Context *context, bool edit_drawing = false);
	~VisualToolVectorClip();
	void OnMouseEvent(wxMouseEvent& event) override;
	bool OnMouseWheel(wxMouseEvent& event) override;
	bool OnKeyEvent(wxKeyEvent& event) override;
	void SetToolbar(wxToolBar *tb) override;

	void SetSubTool(int subtool) override;
	void UpdateTool(int subtool) override;
	int GetSubTool() override;
};

/// Vector-clip style editor for the active line's ASS drawing.
class VisualToolMaskEdit final : public VisualToolVectorClip {
public:
	VisualToolMaskEdit(VideoDisplay *parent, agi::Context *context)
	: VisualToolVectorClip(parent, context, true) { }
};
