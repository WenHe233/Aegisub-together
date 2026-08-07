// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include "visual_feature.h"
#include "visual_tool.h"
#include "visual_color_segment.h"

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
	MASK_BRUSH,
	MASK_FREEHAND,
	MASK_COLOR,
	MASK_LAST
};

enum class VisualToolMaskAction {
	None,
	Undo,
	Redo,
	Create,
	Clear,
	RemoveText,
	GenerateText,
	AutoFill,
	SelectionMode,
	AISelect
};

/// Draws a line-only polygon on the video and creates an ASS drawing mask.
class VisualToolMask final : public VisualTool<VisualDraggableFeature> {
	wxToolBar *toolBar = nullptr;
	std::unique_ptr<OpenGLText> gl_text;
	VisualToolMaskMode mode = MASK_POINTS;
	std::vector<Vector2D> points;
	std::vector<std::vector<Vector2D>> mask_regions;
	Vector2D shape_start;
	bool drawing = false;
	bool mouse_inside = false;
	int hovered_point = -1;
	int dragged_point = -1;
	float mask_brush_radius = 22.f;
	bool mask_brush_drawing = false;
	bool mask_brush_slider_dragging = false;
	Vector2D mask_brush_last;
	VisualToolMaskAction hovered_action = VisualToolMaskAction::None;
	enum class ColorStage { Range, Sample, Ready };
	ColorStage color_stage = ColorStage::Range;
	Vector2D color_range_start;
	Vector2D color_range_end;
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
	bool brush_slider_dragging = false;
	bool color_auto_fill = false;
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
		bool contours_only = false;
		std::vector<std::vector<Vector2D>> contours;
	};
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
	std::pair<Vector2D, Vector2D> ToleranceBounds() const;
	std::pair<Vector2D, Vector2D> OffsetBounds() const;
	std::pair<Vector2D, Vector2D> BrushBounds() const;
	std::pair<Vector2D, Vector2D> MaskBrushBounds() const;
	VisualToolMaskAction ActionAt(Vector2D point);
	int PointAt(Vector2D point) const;
	void ClearPreview();
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
	void UpdateColorBrushSize(Vector2D point);
	bool PrepareColorSelection(Vector2D sample_point);
	bool PrepareAISelection();
	bool EnsureColorSegmenter();
	void ShowColorModeMenu();
	void ShowAISelectionMenu();
	void UpdateColorCursor();
	void RefreshColorContours();
	void PaintColorBrush(Vector2D from, Vector2D to);
	void SyncColorSegmenterFromContours();
	void ResetAIRefinement();
	bool AcceptAIRefinement();
	std::unique_ptr<wxImage> RunAIImageEdit(wxImage const& scene, wxImage const& mask,
		std::string const& prompt, wxString const& progress_message, bool use_cloudinary);
	void DrawTopBar();
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
