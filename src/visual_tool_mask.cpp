// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "visual_tool_mask.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "ai_client.h"
#include "compat.h"
#include "dialog_ai_connection.h"
#include "dialog_progress.h"
#include "format.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_frame.h"

#include <libaegisub/color.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <set>
#include <wx/base64.h>
#include <wx/button.h>
#include <wx/cursor.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/mstream.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/toolbar.h>

namespace {
	constexpr int button_id_base = 1600;
	constexpr double image_colour_tolerance = 25.0;

	ai::CloudinaryCredentials configured_cloudinary() {
		return {OPT_GET("AI/Cloudinary/Cloud Name")->GetString(),
			OPT_GET("AI/Cloudinary/API Key")->GetString(), ai::GetCloudinarySecret()};
	}

	bool point_in_polygon(Vector2D point, std::vector<Vector2D> const& polygon) {
		bool inside = false;
		for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
			Vector2D a = polygon[i];
			Vector2D b = polygon[j];
			bool crosses = (a.Y() > point.Y()) != (b.Y() > point.Y());
			if (crosses && point.X() < (b.X() - a.X()) * (point.Y() - a.Y()) / (b.Y() - a.Y()) + a.X())
				inside = !inside;
		}
		return inside;
	}

	double polygon_area(std::vector<Vector2D> const& polygon) {
		double area = 0.0;
		for (size_t i = 0; i < polygon.size(); ++i) {
			Vector2D a = polygon[i];
			Vector2D b = polygon[(i + 1) % polygon.size()];
			area += static_cast<double>(a.X()) * b.Y() - static_cast<double>(b.X()) * a.Y();
		}
		return area * .5;
	}

	std::vector<std::pair<int, int>> polygon_spans(std::vector<Vector2D> const& polygon,
		float y, float x_scale, int width) {
		std::vector<float> intersections;
		intersections.reserve(polygon.size());
		for (size_t i = 0; i < polygon.size(); ++i) {
			Vector2D a = polygon[i];
			Vector2D b = polygon[(i + 1) % polygon.size()];
			if ((a.Y() > y) == (b.Y() > y)) continue;
			intersections.push_back(a.X() + (y - a.Y()) * (b.X() - a.X()) / (b.Y() - a.Y()));
		}
		std::sort(intersections.begin(), intersections.end());

		std::vector<std::pair<int, int>> spans;
		spans.reserve(intersections.size() / 2);
		for (size_t i = 1; i < intersections.size(); i += 2) {
			int start = std::clamp(static_cast<int>(std::ceil(intersections[i - 1] * x_scale - .5f)), 0, width);
			int end = std::clamp(static_cast<int>(std::ceil(intersections[i] * x_scale - .5f)), 0, width);
			if (end > start) spans.emplace_back(start, end);
		}
		return spans;
	}

	std::vector<unsigned char> rasterize_polygon(std::vector<Vector2D> const& polygon,
		int width, int height) {
		std::vector<unsigned char> inside(static_cast<size_t>(width) * height);
		for (int y = 0; y < height; ++y)
			for (auto [start, end] : polygon_spans(polygon, y + .5f, 1.f, width))
				std::fill(inside.begin() + static_cast<size_t>(y) * width + start,
					inside.begin() + static_cast<size_t>(y) * width + end, 1);
		return inside;
	}

	std::string drawing_style_tags(AssStyle const *style) {
		std::string tags;
		if (!style || style->alignment != 7) tags += "\\an7";
		if (!style || style->outline_w != 0.0) tags += "\\bord0";
		if (!style || style->shadow_w != 0.0) tags += "\\shad0";
		if (!style || style->scalex != 100.0) tags += "\\fscx100";
		if (!style || style->scaley != 100.0) tags += "\\fscy100";
		return tags;
	}

	double colour_distance(double r1, double g1, double b1, double r2, double g2, double b2) {
		double dr = r1 - r2;
		double dg = g1 - g2;
		double db = b1 - b2;
		return std::sqrt(dr * dr + dg * dg + db * db);
	}

}

VisualToolMask::VisualToolMask(VideoDisplay *parent, agi::Context *context)
: VisualTool<VisualDraggableFeature>(parent, context)
, gl_text(std::make_unique<OpenGLText>())
{
}

VisualToolMask::~VisualToolMask() {
	UpdateActionTooltip(VisualToolMaskAction::None);
	parent->SetCursor(wxNullCursor);
	if (toolBar)
		toolBar->Unbind(wxEVT_TOOL, &VisualToolMask::OnSubTool, this);
}

void VisualToolMask::UpdateCursor() {
	// Keep the platform cursor untouched. The active mask mode is rendered as
	// a separate overlay next to it in Draw().
	parent->SetCursor(wxCursor(wxCURSOR_ARROW));
}

void VisualToolMask::AddTool(std::string const& command_name, VisualToolMaskMode tool_mode) {
	auto command = cmd::get(command_name);
	int icon_size = OPT_GET("App/Toolbar Icon Size")->GetInt();
	toolBar->AddTool(button_id_base + tool_mode, command->StrDisplay(c), command->Icon(icon_size),
		command->GetTooltip("Video"), wxITEM_CHECK);
}

void VisualToolMask::SetToolbar(wxToolBar *toolbar) {
	toolBar = toolbar;
	toolBar->AddSeparator();
	AddTool("video/tool/mask/rectangle", MASK_RECTANGLE);
	AddTool("video/tool/mask/points", MASK_POINTS);
	AddTool("video/tool/mask/brush", MASK_BRUSH);
	AddTool("video/tool/mask/freehand", MASK_FREEHAND);
	AddTool("video/tool/mask/color", MASK_COLOR);
	toolBar->ToggleTool(button_id_base + mode, true);
	toolBar->Realize();
	toolBar->Show(true);
	toolBar->Bind(wxEVT_TOOL, &VisualToolMask::OnSubTool, this);
	UpdateCursor();
}

void VisualToolMask::OnSubTool(wxCommandEvent& event) {
	SetSubTool(event.GetId() - button_id_base);
}

void VisualToolMask::SetSubTool(int subtool) {
	if (subtool < MASK_RECTANGLE || subtool >= MASK_LAST)
		return;
	auto next_mode = static_cast<VisualToolMaskMode>(subtool);
	if (next_mode == mode) return;
	if (next_mode == MASK_COLOR) {
		ClearPreview();
	}
	else {
		points.clear();
		mask_regions.clear();
		drawing = false;
		mask_brush_drawing = false;
		if (mode == MASK_COLOR) ResetColorSelection();
	}
	mode = next_mode;
	drawing = false;
	mask_undo_history.clear();
	mask_redo_history.clear();
	if (mode == MASK_COLOR)
		ResetColorSelection();
	if (parent->HasCapture())
		parent->ReleaseMouse();

	if (toolBar) {
		for (int i = MASK_RECTANGLE; i < MASK_LAST; ++i)
			toolBar->ToggleTool(button_id_base + i, i == mode);
	}
	UpdateCursor();
	parent->Render();
}

int VisualToolMask::GetSubTool() {
	return mode;
}

void VisualToolMask::OnCoordinateSystemsChanged() {
	drawing = false;
	dragged_point = -1;
	if (mode == MASK_COLOR && !color_contours_dirty)
		RefreshColorContours();
}

void VisualToolMask::UpdateRectangle(Vector2D end) {
	points = {
		shape_start,
		Vector2D(end.X(), shape_start.Y()),
		end,
		Vector2D(shape_start.X(), end.Y())
	};
}

void VisualToolMask::UpdateFreehand(Vector2D point) {
	if (points.empty() || (FromScriptCoords(points.back()) - FromScriptCoords(point)).Len() >= 2.f)
		points.push_back(point);
}

void VisualToolMask::CommitCurrentRegion() {
	if (points.size() >= 3 && std::abs(polygon_area(points)) > .5)
		mask_regions.push_back(std::move(points));
	points.clear();
}

void VisualToolMask::PaintMaskBrush(Vector2D from, Vector2D to) {
	float distance = (to - from).Len();
	float step = std::max(1.f, mask_brush_radius * .3f);
	int steps = distance > 0.f ? std::max(1, static_cast<int>(std::ceil(distance / step))) : 0;
	float script_radius = mask_brush_radius * .5f *
		(script_res.X() / std::max(1.f, video_size.X()) +
		 script_res.Y() / std::max(1.f, video_size.Y()));
	constexpr int circle_points = 48;
	for (int i = 0; i <= steps; ++i) {
		float progress = steps ? static_cast<float>(i) / steps : 0.f;
		Vector2D centre = ToScriptCoords(from + (to - from) * progress);
		std::vector<Vector2D> circle;
		circle.reserve(circle_points);
		for (int point = 0; point < circle_points; ++point) {
			float angle = static_cast<float>(point * 2.0 * M_PI / circle_points);
			circle.emplace_back(centre.X() + std::cos(angle) * script_radius,
				centre.Y() + std::sin(angle) * script_radius);
		}
		mask_regions.push_back(std::move(circle));
	}
}

void VisualToolMask::UpdateMaskBrushSize(Vector2D point) {
	auto [top_left, bottom_right] = MaskBrushBounds();
	float left = top_left.X() + 70.f;
	float right = bottom_right.X() - 12.f;
	double ratio = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f);
	mask_brush_radius = static_cast<float>(std::lround(2.0 + ratio * 198.0));
}

void VisualToolMask::UpdateActionTooltip(VisualToolMaskAction action) {
	hovered_action = action;
	auto inside = [&](std::pair<Vector2D, Vector2D> const& bounds) {
		return mouse_pos.X() >= bounds.first.X() && mouse_pos.X() <= bounds.second.X() &&
			mouse_pos.Y() >= bounds.first.Y() && mouse_pos.Y() <= bounds.second.Y();
	};
	if (mask_brush_slider_dragging || (mode == MASK_BRUSH && inside(MaskBrushBounds()))) {
		parent->SetToolTip(agi::wxformat(_("Brush: %d px"), static_cast<int>(std::lround(mask_brush_radius))));
		return;
	}
	if (tolerance_dragging || (mode == MASK_COLOR && has_color_sample && inside(ToleranceBounds()))) {
		parent->SetToolTip(agi::wxformat(_("Tolerance: %.1f"), color_tolerance));
		return;
	}
	if (offset_dragging || (CanOffsetSelection() && inside(OffsetBounds()))) {
		parent->SetToolTip(agi::wxformat(_("Offset: %d px"), color_offset));
		return;
	}
	bool color_brush_mode = mode == MASK_COLOR &&
		(color_selection_mode == VisualSelectionMode::BrushAdd ||
		 color_selection_mode == VisualSelectionMode::BrushSubtract);
	if (brush_slider_dragging || (color_brush_mode && inside(BrushBounds()))) {
		parent->SetToolTip(agi::wxformat(_("Brush: %d px"), static_cast<int>(std::lround(color_brush_radius))));
		return;
	}
	switch (action) {
		case VisualToolMaskAction::Create:
			parent->SetToolTip(ai_refining ? _("Accept") : _("Create mask"));
			break;
		case VisualToolMaskAction::RemoveText: parent->SetToolTip(_("AI text removal")); break;
		case VisualToolMaskAction::GenerateText: parent->SetToolTip(_("AI custom generation")); break;
		default:
		parent->UnsetToolTip();
	}
}

bool VisualToolMask::CanCreateMask() const {
	if (ai_refining) return ai_working_image && ai_working_image->IsOk() &&
		std::any_of(ai_selection_mask.begin(), ai_selection_mask.end(), [](unsigned char selected) { return selected != 0; });
	if (!c->selectionController->GetActiveLine()) return false;
	if (mode == MASK_COLOR) return !color_contours.empty();
	return !mask_regions.empty() || (points.size() >= 3 && std::abs(polygon_area(points)) > .5);
}

bool VisualToolMask::CanCancel() const {
	if (ai_refining) return true;
	if (drawing || mask_brush_drawing || dragged_point >= 0 || !points.empty() || !mask_regions.empty()) return true;
	return mode == MASK_COLOR && (color_stage != ColorStage::Range ||
		color_range_start != color_range_end || has_color_sample || !color_contours.empty());
}

bool VisualToolMask::CanOffsetSelection() const {
	return mode == MASK_COLOR && color_stage == ColorStage::Ready &&
		!color_segmenter.Empty();
}

std::pair<Vector2D, Vector2D> VisualToolMask::ToleranceBounds() const {
	return {Vector2D(96.f, 10.f), Vector2D(236.f, 44.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::OffsetBounds() const {
	float left = has_color_sample ? 244.f : 96.f;
	return {Vector2D(left, 10.f), Vector2D(left + 132.f, 44.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::BrushBounds() const {
	float left = CanOffsetSelection() ? OffsetBounds().second.X() + 8.f : 96.f;
	return {Vector2D(left, 10.f), Vector2D(left + 140.f, 44.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::MaskBrushBounds() const {
	return {Vector2D(96.f, 10.f), Vector2D(236.f, 44.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::ActionBounds(VisualToolMaskAction action) const {
	if (action == VisualToolMaskAction::Undo)
		return {Vector2D(12.f, 10.f), Vector2D(46.f, 44.f)};
	if (action == VisualToolMaskAction::Redo)
		return {Vector2D(54.f, 10.f), Vector2D(88.f, 44.f)};
	float top = 10.f;
	constexpr float height = 34.f;
	constexpr float gap = 8.f;
	bool brush_mode = color_selection_mode == VisualSelectionMode::BrushAdd ||
		color_selection_mode == VisualSelectionMode::BrushSubtract;
	float left = mode == MASK_BRUSH ? MaskBrushBounds().second.X() + gap :
		mode == MASK_COLOR && brush_mode ? BrushBounds().second.X() + gap :
		mode == MASK_COLOR && CanOffsetSelection() ? OffsetBounds().second.X() + gap :
		96.f;
	auto mode_label = [&]() -> wxString {
		switch (color_selection_mode) {
			case VisualSelectionMode::PipetteAdd: return _("Pipette add");
			case VisualSelectionMode::PipetteSubtract: return _("Pipette subtract");
			case VisualSelectionMode::BrushAdd: return _("Brush add");
			case VisualSelectionMode::BrushSubtract: return _("Brush subtract");
		}
		return wxString();
	};
		auto label_for = [&](VisualToolMaskAction item) -> wxString {
		switch (item) {
			case VisualToolMaskAction::Create: return ai_refining ? _("Accept (ENTER)") : _("Create mask (ENTER)");
			case VisualToolMaskAction::Clear: return _("Cancel (ESC)");
			case VisualToolMaskAction::RemoveText:
				return ai_refining ? _("Erase again") : _("AI text removal");
			case VisualToolMaskAction::GenerateText: return _("AI custom generation");
			case VisualToolMaskAction::AutoFill: return _("Auto fill");
			case VisualToolMaskAction::SelectionMode: return mode_label();
			case VisualToolMaskAction::AISelect: return _("AI recognition");
			default: return wxString();
		}
	};
	auto width_for = [&](VisualToolMaskAction item) {
		gl_text->SetFont("Verdana", 9, true, false);
		int text_width, text_height;
		gl_text->GetExtent(from_wx(label_for(item)), text_width, text_height);
		return static_cast<float>(text_width +
			(item == VisualToolMaskAction::SelectionMode ? 32 : 24));
	};
	std::vector<VisualToolMaskAction> actions;
	if (ai_refining)
		actions = {VisualToolMaskAction::RemoveText, VisualToolMaskAction::GenerateText,
			VisualToolMaskAction::Create, VisualToolMaskAction::Clear};
	else if (mode == MASK_COLOR) {
		actions = {VisualToolMaskAction::SelectionMode};
		actions.push_back(VisualToolMaskAction::AISelect);
		if (has_color_sample)
			actions.push_back(VisualToolMaskAction::AutoFill);
		actions.push_back(VisualToolMaskAction::Create);
		actions.push_back(VisualToolMaskAction::Clear);
	}
	else
		actions = {VisualToolMaskAction::Create, VisualToolMaskAction::Clear,
			VisualToolMaskAction::RemoveText, VisualToolMaskAction::GenerateText};
	for (auto item : actions) {
		float width = width_for(item);
		if (left + width > canvas_size.X() - 8.f && left > 12.f) {
			left = 12.f;
			top += height + gap;
		}
		if (item == action) return {Vector2D(left, top), Vector2D(left + width, top + height)};
		left += width + gap;
	}
	return {Vector2D(left, top), Vector2D(left, top + height)};
}

float VisualToolMask::TopBarHeight() const {
	if (ai_refining) return ActionBounds(VisualToolMaskAction::Clear).second.Y() + 10.f;
	auto last = mode == MASK_COLOR ? VisualToolMaskAction::Clear :
		VisualToolMaskAction::GenerateText;
	return ActionBounds(last).second.Y() + 10.f;
}

VisualToolMaskAction VisualToolMask::ActionAt(Vector2D point) {
	for (auto action : {VisualToolMaskAction::Undo, VisualToolMaskAction::Redo}) {
		bool enabled;
		if (mode == MASK_COLOR && !ai_refining)
			enabled = action == VisualToolMaskAction::Undo ? !color_undo_history.empty() :
				!color_redo_history.empty();
		else
			enabled = action == VisualToolMaskAction::Undo ? !mask_undo_history.empty() :
				!mask_redo_history.empty();
		if (!enabled) continue;
		auto [top_left, bottom_right] = ActionBounds(action);
		if (point.X() >= top_left.X() && point.X() <= bottom_right.X() &&
			point.Y() >= top_left.Y() && point.Y() <= bottom_right.Y()) return action;
	}
	if (ai_refining) {
		for (auto action : {VisualToolMaskAction::RemoveText, VisualToolMaskAction::GenerateText,
			VisualToolMaskAction::Create, VisualToolMaskAction::Clear}) {
			bool pending = !mask_regions.empty() ||
				(points.size() >= 3 && std::abs(polygon_area(points)) > .5);
			if ((action == VisualToolMaskAction::RemoveText ||
				action == VisualToolMaskAction::GenerateText) && !pending) continue;
			if (action == VisualToolMaskAction::Create && !CanCreateMask()) continue;
			if (action == VisualToolMaskAction::Clear && !CanCancel()) continue;
			if (action == VisualToolMaskAction::GenerateText && ai::GetApiKey().empty()) continue;
			auto [top_left, bottom_right] = ActionBounds(action);
			if (point.X() >= top_left.X() && point.X() <= bottom_right.X() &&
				point.Y() >= top_left.Y() && point.Y() <= bottom_right.Y()) return action;
		}
		return VisualToolMaskAction::None;
	}
	std::vector<VisualToolMaskAction> actions;
	if (ai_refining)
		actions = {VisualToolMaskAction::RemoveText, VisualToolMaskAction::GenerateText,
			VisualToolMaskAction::Create, VisualToolMaskAction::Clear};
	else if (mode == MASK_COLOR) {
		actions = {VisualToolMaskAction::SelectionMode};
		actions.push_back(VisualToolMaskAction::AISelect);
		if (has_color_sample)
			actions.push_back(VisualToolMaskAction::AutoFill);
		actions.push_back(VisualToolMaskAction::Create);
		actions.push_back(VisualToolMaskAction::Clear);
	}
	else actions = {VisualToolMaskAction::Create,
			VisualToolMaskAction::Clear, VisualToolMaskAction::RemoveText,
			VisualToolMaskAction::GenerateText};
	for (auto action : actions) {
		if (action == VisualToolMaskAction::Clear && !CanCancel()) continue;
		if (action == VisualToolMaskAction::AutoFill && !has_color_sample) continue;
		if (action == VisualToolMaskAction::SelectionMode && color_stage == ColorStage::Range) continue;
		if (action == VisualToolMaskAction::AISelect && color_stage == ColorStage::Range) continue;
		if (action == VisualToolMaskAction::GenerateText && ai::GetApiKey().empty()) continue;
		if ((action == VisualToolMaskAction::Create || action == VisualToolMaskAction::RemoveText ||
			action == VisualToolMaskAction::GenerateText) && !CanCreateMask()) continue;
		auto [top_left, bottom_right] = ActionBounds(action);
		if (point.X() >= top_left.X() && point.X() <= bottom_right.X() &&
			point.Y() >= top_left.Y() && point.Y() <= bottom_right.Y()) return action;
	}
	return VisualToolMaskAction::None;
}

void VisualToolMask::ResetColorSelection() {
	color_stage = ColorStage::Range;
	color_selection_mode = VisualSelectionMode::PipetteAdd;
	color_range_start = color_range_end = Vector2D();
	color_contours.clear();
	color_segmenter.Clear();
	color_contours_dirty = false;
	has_color_sample = false;
	color_frame_width = color_frame_height = 0;
	color_offset = 0;
	color_auto_fill = false;
	drawing = false;
	tolerance_dragging = false;
	offset_dragging = false;
	brush_slider_dragging = false;
	color_brush_drawing = false;
	color_brush_moved = false;
	color_brush_stroke.clear();
	color_undo_history.clear();
	color_redo_history.clear();
	color_undo_history.reserve(16);
	color_redo_history.reserve(16);
	parent->SetCursor(wxCursor(wxCURSOR_ARROW));
}

VisualToolMask::ColorHistoryState VisualToolMask::CaptureColorHistory() const {
	return {color_segmenter, color_sample, color_stage, color_tolerance, color_offset,
		has_color_sample, color_auto_fill};
}

VisualToolMask::ColorHistoryState VisualToolMask::CaptureColorBrushHistory() const {
	ColorHistoryState state;
	state.sample = color_sample;
	state.stage = color_stage;
	state.tolerance = color_tolerance;
	state.offset = color_offset;
	state.has_sample = has_color_sample;
	state.auto_fill = color_auto_fill;
	state.contours_only = true;
	state.contours = color_contours;
	return state;
}

void VisualToolMask::RestoreColorHistory(ColorHistoryState state) {
	color_sample = state.sample;
	color_stage = state.stage;
	color_tolerance = state.tolerance;
	color_offset = state.offset;
	has_color_sample = state.has_sample;
	color_auto_fill = state.auto_fill;
	if (state.contours_only) {
		color_contours = std::move(state.contours);
		color_contours_dirty = true;
	}
	else {
		color_segmenter = std::move(state.segmenter);
		color_contours_dirty = false;
		RefreshColorContours();
	}
	UpdateColorCursor();
	parent->Render();
}

void VisualToolMask::PushColorHistory() {
	if (color_contours_dirty) SyncColorSegmenterFromContours();
	constexpr size_t maximum_history = 16;
	if (color_undo_history.size() == maximum_history)
		color_undo_history.erase(color_undo_history.begin());
	color_undo_history.push_back(CaptureColorHistory());
	color_redo_history.clear();
}

void VisualToolMask::PushColorBrushHistory() {
	constexpr size_t maximum_history = 16;
	if (color_undo_history.size() == maximum_history)
		color_undo_history.erase(color_undo_history.begin());
	color_undo_history.push_back(CaptureColorBrushHistory());
	color_redo_history.clear();
}

bool VisualToolMask::UndoColorHistory() {
	if (color_undo_history.empty()) return false;
	color_redo_history.push_back(color_undo_history.back().contours_only ?
		CaptureColorBrushHistory() : CaptureColorHistory());
	auto state = std::move(color_undo_history.back());
	color_undo_history.pop_back();
	RestoreColorHistory(std::move(state));
	return true;
}

bool VisualToolMask::RedoColorHistory() {
	if (color_redo_history.empty()) return false;
	color_undo_history.push_back(color_redo_history.back().contours_only ?
		CaptureColorBrushHistory() : CaptureColorHistory());
	auto state = std::move(color_redo_history.back());
	color_redo_history.pop_back();
	RestoreColorHistory(std::move(state));
	return true;
}

void VisualToolMask::ClearPreview() {
	ResetAIRefinement();
	points.clear();
	mask_regions.clear();
	mask_undo_history.clear();
	mask_redo_history.clear();
	mask_brush_drawing = false;
	mask_brush_slider_dragging = false;
	ResetColorSelection();
	dragged_point = hovered_point = -1;
	tolerance_dragging = false;
	if (parent->HasCapture()) parent->ReleaseMouse();
	UpdateActionTooltip(VisualToolMaskAction::None);
	parent->Render();
}

VisualToolMask::MaskHistoryState VisualToolMask::CaptureMaskHistory() const {
	return {points, mask_regions};
}

void VisualToolMask::RestoreMaskHistory(MaskHistoryState state) {
	points = std::move(state.points);
	mask_regions = std::move(state.regions);
	drawing = false;
	mask_brush_drawing = false;
	dragged_point = hovered_point = -1;
	if (parent->HasCapture()) parent->ReleaseMouse();
	parent->SetFocus();
	parent->Render();
}

void VisualToolMask::PushMaskHistory() {
	constexpr size_t maximum_history = 32;
	if (mask_undo_history.size() == maximum_history)
		mask_undo_history.erase(mask_undo_history.begin());
	mask_undo_history.push_back(CaptureMaskHistory());
	mask_redo_history.clear();
}

bool VisualToolMask::UndoMaskHistory() {
	if (mask_undo_history.empty()) return false;
	mask_redo_history.push_back(CaptureMaskHistory());
	auto state = std::move(mask_undo_history.back());
	mask_undo_history.pop_back();
	RestoreMaskHistory(std::move(state));
	return true;
}

bool VisualToolMask::RedoMaskHistory() {
	if (mask_redo_history.empty()) return false;
	mask_undo_history.push_back(CaptureMaskHistory());
	auto state = std::move(mask_redo_history.back());
	mask_redo_history.pop_back();
	RestoreMaskHistory(std::move(state));
	return true;
}

void VisualToolMask::ResetAIRefinement() {
	ai_refining = false;
	ai_working_image.reset();
	ai_selection_mask.clear();
	ai_prompt_history.clear();
	ai_session_line = nullptr;
	ai_crop_x = ai_crop_y = ai_crop_width = ai_crop_height = 0;
	if (parent->HasCapture()) parent->ReleaseMouse();
	parent->SetCursor(wxCursor(wxCURSOR_ARROW));
}

void VisualToolMask::UpdateColorTolerance(Vector2D point) {
	if (color_contours_dirty) SyncColorSegmenterFromContours();
	auto [top_left, bottom_right] = ToleranceBounds();
	constexpr float slider_left_padding = 75.f;
	float left = top_left.X() + slider_left_padding;
	float right = bottom_right.X() - 12.f;
	double value = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f) * 20.0;
	color_tolerance = std::round(value * 10.0) / 10.0;
	RefreshColorContours();
}

void VisualToolMask::UpdateColorOffset(Vector2D point) {
	if (color_contours_dirty) SyncColorSegmenterFromContours();
	auto [top_left, bottom_right] = OffsetBounds();
	float left = top_left.X() + 58.f;
	float right = bottom_right.X() - 12.f;
	double ratio = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f);
	color_offset = static_cast<int>(std::lround(ratio * 50.0 - 25.0));
	RefreshColorContours();
}

void VisualToolMask::UpdateColorBrushSize(Vector2D point) {
	auto [top_left, bottom_right] = BrushBounds();
	float left = top_left.X() + 70.f;
	float right = bottom_right.X() - 12.f;
	double ratio = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f);
	color_brush_radius = static_cast<float>(std::lround(2.0 + ratio * 198.0));
}

void VisualToolMask::UpdateColorCursor() {
	if (color_stage == ColorStage::Range || mouse_pos.Y() < TopBarHeight()) {
		parent->SetCursor(wxCursor(wxCURSOR_ARROW));
		return;
	}
	if (color_selection_mode == VisualSelectionMode::PipetteAdd ||
		color_selection_mode == VisualSelectionMode::PipetteSubtract)
		parent->SetCursor(MakeVisualColorPickerCursor());
	else if (color_selection_mode == VisualSelectionMode::BrushAdd ||
		color_selection_mode == VisualSelectionMode::BrushSubtract)
		parent->SetCursor(wxCursor(wxCURSOR_BLANK));
	else
		parent->SetCursor(wxCursor(wxCURSOR_ARROW));
}

bool VisualToolMask::EnsureColorSegmenter() {
	if (!color_segmenter.Empty()) return true;
	try {
		auto frame = c->videoController->GetFrame(frame_number, true);
		if (!frame || frame->width <= 0 || frame->height <= 0) return false;
		auto to_frame = [&](Vector2D script) {
			return Vector2D(script.X() * frame->width / script_res.X(),
				script.Y() * frame->height / script_res.Y());
		};
		Vector2D a = to_frame(color_range_start), b = to_frame(color_range_end);
		if (!color_segmenter.PrepareEmpty(*frame, static_cast<int>(std::floor(a.X())),
			static_cast<int>(std::floor(a.Y())), static_cast<int>(std::ceil(b.X())),
			static_cast<int>(std::ceil(b.Y())))) return false;
		color_frame_width = frame->width;
		color_frame_height = frame->height;
		return true;
	}
	catch (...) { return false; }
}

void VisualToolMask::ShowColorModeMenu() {
	if (color_stage == ColorStage::Range) return;
	constexpr int pipette_add_id = 17311;
	constexpr int pipette_subtract_id = 17312;
	constexpr int brush_add_id = 17313;
	constexpr int brush_subtract_id = 17314;
	wxMenu menu;
	auto add_mode_item = [&](int id, wxString const& label) {
		menu.Append(id, label);
	};
	add_mode_item(pipette_add_id, _("Pipette add"));
	add_mode_item(pipette_subtract_id, _("Pipette subtract"));
	add_mode_item(brush_add_id, _("Brush add"));
	add_mode_item(brush_subtract_id, _("Brush subtract"));
	auto [top_left, bottom_right] = ActionBounds(VisualToolMaskAction::SelectionMode);
	int selected = parent->GetPopupMenuSelectionFromUser(menu,
		wxPoint(static_cast<int>(top_left.X()), static_cast<int>(bottom_right.Y())));
	if (selected == pipette_add_id) color_selection_mode = VisualSelectionMode::PipetteAdd;
	else if (selected == pipette_subtract_id) color_selection_mode = VisualSelectionMode::PipetteSubtract;
	else if (selected == brush_add_id) color_selection_mode = VisualSelectionMode::BrushAdd;
	else if (selected == brush_subtract_id) color_selection_mode = VisualSelectionMode::BrushSubtract;
	UpdateColorCursor();
	parent->Render();
}

void VisualToolMask::ShowAISelectionMenu() {
	if (color_stage == ColorStage::Range) return;
	if (!configured_cloudinary().Complete()) {
		wxMessageBox(_("Cloudinary is not configured correctly. Open AI connection settings and enter the cloud name, API key and API secret."),
			_("AI recognition failed"), wxOK | wxICON_ERROR, c->parent);
		return;
	}
	PrepareAISelection();
	UpdateColorCursor();
	parent->Render();
}

bool VisualToolMask::PrepareAISelection() {
	if (color_stage == ColorStage::Range || !configured_cloudinary().Complete()) return false;
	try {
		auto frame = c->videoController->GetFrame(frame_number, true);
		if (!frame || frame->width <= 0 || frame->height <= 0) return false;
		auto to_frame = [&](Vector2D script) {
			return Vector2D(script.X() * frame->width / script_res.X(),
				script.Y() * frame->height / script_res.Y());
		};
		Vector2D raw_a = to_frame(color_range_start);
		Vector2D raw_b = to_frame(color_range_end);
		int left = std::clamp(static_cast<int>(std::floor(std::min(raw_a.X(), raw_b.X()))), 0, frame->width - 1);
		int top = std::clamp(static_cast<int>(std::floor(std::min(raw_a.Y(), raw_b.Y()))), 0, frame->height - 1);
		int right = std::clamp(static_cast<int>(std::ceil(std::max(raw_a.X(), raw_b.X()))), left + 1, frame->width);
		int bottom = std::clamp(static_cast<int>(std::ceil(std::max(raw_a.Y(), raw_b.Y()))), top + 1, frame->height);
		wxImage crop = GetImage(*frame).GetSubImage(wxRect(left, top, right - left, bottom - top));
		auto contours = GenerateVisualAISelection(c->parent, crop);
		for (auto& contour : contours) {
			for (auto& point : contour)
				point = point + Vector2D(static_cast<float>(left), static_cast<float>(top));
		}
		PushColorHistory();
		color_segmenter.PrepareEmpty(*frame, left, top, right, bottom);
		color_segmenter.SetContours(contours, true);
		has_color_sample = false;
		color_auto_fill = false;
		color_offset = 0;
		color_frame_width = frame->width;
		color_frame_height = frame->height;
		color_stage = ColorStage::Ready;
		RefreshColorContours();
		UpdateColorCursor();
		return true;
	}
	catch (std::exception const& error) {
		wxMessageBox(to_wx(error.what()), _("AI recognition failed"), wxOK | wxICON_ERROR, c->parent);
		return false;
	}
}

bool VisualToolMask::PrepareColorSelection(Vector2D sample_point) {
	try {
		auto frame = c->videoController->GetFrame(frame_number, true);
		if (!frame || frame->width <= 0 || frame->height <= 0) return false;
		auto to_frame = [&](Vector2D script) {
			return Vector2D(script.X() * frame->width / script_res.X(),
				script.Y() * frame->height / script_res.Y());
		};
		Vector2D raw_sample = to_frame(sample_point);
		color_sample = VisualColorSegmenter::Sample(*frame,
			static_cast<int>(std::lround(raw_sample.X())), static_cast<int>(std::lround(raw_sample.Y())));
		if (!EnsureColorSegmenter()) return false;
		if (color_contours_dirty) SyncColorSegmenterFromContours();
		PushColorHistory();
		bool prepared = color_segmenter.AddSample(*frame, color_sample,
			color_selection_mode == VisualSelectionMode::PipetteAdd);
		if (!prepared) return false;
		has_color_sample = true;
		color_frame_width = frame->width;
		color_frame_height = frame->height;
		color_stage = ColorStage::Ready;
		UpdateColorCursor();
		RefreshColorContours();
		return true;
	}
	catch (...) {
		return false;
	}
}

void VisualToolMask::PaintColorBrush(Vector2D from, Vector2D to) {
	if (color_frame_width <= 0 ||
		color_frame_height <= 0 || video_size.X() <= 0.f || video_size.Y() <= 0.f) return;
	std::vector<std::vector<Vector2D>> screen_contours = color_contours;
	for (auto& contour : screen_contours)
		for (auto& point : contour) point = FromScriptCoords(point);
	bool add = color_selection_mode == VisualSelectionMode::BrushAdd;
	screen_contours = ApplyVectorBrushStroke(std::move(screen_contours),
		{from, to}, color_brush_radius, add);
	color_contours = screen_contours;
	for (auto& contour : color_contours)
		for (auto& point : contour) point = ToScriptCoords(point);
	color_offset = 0;
	color_auto_fill = false;
	color_contours_dirty = true;
}

void VisualToolMask::SyncColorSegmenterFromContours() {
	if (color_segmenter.Empty() || color_frame_width <= 0 || color_frame_height <= 0) return;
	std::vector<std::vector<Vector2D>> frame_contours = color_contours;
	for (auto& contour : frame_contours) {
		for (auto& script : contour)
			script = Vector2D(script.X() * color_frame_width / script_res.X(),
				script.Y() * color_frame_height / script_res.Y());
	}
	color_segmenter.SetContours(frame_contours);
	color_contours_dirty = false;
}

void VisualToolMask::RefreshColorContours() {
	color_contours.clear();
	if (color_segmenter.Empty()) return;
	auto raw_contours = color_segmenter.Extract(color_tolerance, color_auto_fill, color_offset);
	if (color_frame_width <= 0 || color_frame_height <= 0) return;
	for (auto& contour : raw_contours) {
		for (auto& point : contour)
			point = Vector2D(point.X() * script_res.X() / color_frame_width,
				point.Y() * script_res.Y() / color_frame_height);
		color_contours.push_back(std::move(contour));
	}
	color_contours_dirty = false;
}

int VisualToolMask::PointAt(Vector2D point) const {
	for (int i = static_cast<int>(points.size()) - 1; i >= 0; --i) {
		if ((point - FromScriptCoords(points[i])).Len() <= 6.f)
			return i;
	}
	return -1;
}

void VisualToolMask::OnMouseEvent(wxMouseEvent& event) {
	shift_down = event.ShiftDown();
	ctrl_down = event.CmdDown();
	alt_down = event.AltDown();
	mouse_pos = event.GetPosition();
	mouse_inside = !event.Leaving();
	if (ai_refining && c->selectionController->GetActiveLine() != ai_session_line) {
		ClearPreview();
		return;
	}
	if (mode == MASK_COLOR) UpdateColorCursor();
	if (mask_brush_slider_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateMaskBrushSize(mouse_pos);
		if (event.LeftUp()) {
			mask_brush_slider_dragging = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
			parent->SetFocus();
		}
		UpdateActionTooltip(VisualToolMaskAction::None);
		parent->Render();
		return;
	}
	if (mask_brush_drawing && (event.Dragging() || event.LeftUp())) {
		PaintMaskBrush(mask_brush_last, mouse_pos);
		mask_brush_last = mouse_pos;
		if (event.LeftUp()) {
			mask_brush_drawing = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
			parent->SetFocus();
		}
		parent->Render();
		return;
	}
	bool brush_mode = mode == MASK_COLOR &&
		(color_selection_mode == VisualSelectionMode::BrushAdd ||
		 color_selection_mode == VisualSelectionMode::BrushSubtract);
	if (color_brush_drawing && (event.Dragging() || event.LeftUp())) {
		if ((mouse_pos - color_brush_last).Len() >= 1.f) {
			PaintColorBrush(color_brush_last, mouse_pos);
			color_brush_last = mouse_pos;
			color_brush_moved = true;
		}
		if (event.LeftUp()) {
			color_brush_drawing = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
		}
		parent->Render();
		return;
	}
	if (brush_slider_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateColorBrushSize(mouse_pos);
		if (event.LeftUp()) {
			brush_slider_dragging = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
			parent->SetFocus();
		}
		UpdateActionTooltip(VisualToolMaskAction::None);
		parent->Render();
		return;
	}

	if (event.Leaving() && !drawing && !color_brush_drawing && dragged_point < 0 &&
		!tolerance_dragging && !offset_dragging && !brush_slider_dragging) {
		hovered_point = -1;
		UpdateActionTooltip(VisualToolMaskAction::None);
		parent->Render();
		return;
	}

	auto action = ActionAt(mouse_pos);
	hovered_point = mode == MASK_COLOR ? -1 : PointAt(mouse_pos);
	UpdateActionTooltip(action);

	if (tolerance_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateColorTolerance(mouse_pos);
		if (event.LeftUp()) {
			tolerance_dragging = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
			parent->SetFocus();
		}
		UpdateActionTooltip(VisualToolMaskAction::None);
		parent->Render();
		return;
	}
	if (offset_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateColorOffset(mouse_pos);
		if (event.LeftUp()) {
			offset_dragging = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
			parent->SetFocus();
		}
		UpdateActionTooltip(VisualToolMaskAction::None);
		parent->Render();
		return;
	}

	if (event.LeftDown()) {
		if (mode == MASK_BRUSH) {
			auto [brush_top_left, brush_bottom_right] = MaskBrushBounds();
			if (mouse_pos.X() >= brush_top_left.X() && mouse_pos.X() <= brush_bottom_right.X() &&
				mouse_pos.Y() >= brush_top_left.Y() && mouse_pos.Y() <= brush_bottom_right.Y()) {
				mask_brush_slider_dragging = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
				UpdateMaskBrushSize(mouse_pos);
				UpdateActionTooltip(VisualToolMaskAction::None);
				return;
			}
		}
		if (mode == MASK_COLOR && has_color_sample) {
			auto [tolerance_top_left, tolerance_bottom_right] = ToleranceBounds();
			if (mouse_pos.X() >= tolerance_top_left.X() && mouse_pos.X() <= tolerance_bottom_right.X() &&
				mouse_pos.Y() >= tolerance_top_left.Y() && mouse_pos.Y() <= tolerance_bottom_right.Y()) {
				PushColorHistory();
				tolerance_dragging = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
				UpdateColorTolerance(mouse_pos);
				UpdateActionTooltip(VisualToolMaskAction::None);
				return;
			}
		}
		if (CanOffsetSelection()) {
			auto [offset_top_left, offset_bottom_right] = OffsetBounds();
			if (mouse_pos.X() >= offset_top_left.X() && mouse_pos.X() <= offset_bottom_right.X() &&
				mouse_pos.Y() >= offset_top_left.Y() && mouse_pos.Y() <= offset_bottom_right.Y()) {
				PushColorHistory();
				offset_dragging = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
				UpdateColorOffset(mouse_pos);
				UpdateActionTooltip(VisualToolMaskAction::None);
				return;
			}
		}
		if (brush_mode) {
			auto [brush_top_left, brush_bottom_right] = BrushBounds();
			if (mouse_pos.X() >= brush_top_left.X() && mouse_pos.X() <= brush_bottom_right.X() &&
				mouse_pos.Y() >= brush_top_left.Y() && mouse_pos.Y() <= brush_bottom_right.Y()) {
				brush_slider_dragging = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
				UpdateColorBrushSize(mouse_pos);
				UpdateActionTooltip(VisualToolMaskAction::None);
				return;
			}
		}
		if (action != VisualToolMaskAction::None) {
			if (action == VisualToolMaskAction::Undo) {
				if (mode == MASK_COLOR && !ai_refining) UndoColorHistory();
				else UndoMaskHistory();
			}
			else if (action == VisualToolMaskAction::Redo) {
				if (mode == MASK_COLOR && !ai_refining) RedoColorHistory();
				else RedoMaskHistory();
			}
			else if (action == VisualToolMaskAction::Create) CreateMask();
			else if (action == VisualToolMaskAction::Clear) ClearPreview();
			else if (action == VisualToolMaskAction::AutoFill) {
				if (color_contours_dirty) SyncColorSegmenterFromContours();
				PushColorHistory();
				color_auto_fill = !color_auto_fill;
				RefreshColorContours();
			}
			else if (action == VisualToolMaskAction::SelectionMode) ShowColorModeMenu();
			else if (action == VisualToolMaskAction::AISelect) ShowAISelectionMenu();
			else CreateAIMask(action);
			parent->Render();
			return;
		}
		if (mouse_pos.Y() < TopBarHeight()) return;
		if (mode == MASK_BRUSH) {
			PushMaskHistory();
			CommitCurrentRegion();
			mask_brush_drawing = true;
			mask_brush_last = mouse_pos;
			PaintMaskBrush(mouse_pos, mouse_pos);
			if (!parent->HasCapture()) parent->CaptureMouse();
			parent->SetFocus();
			parent->Render();
			return;
		}

		if (mode == MASK_COLOR) {
			Vector2D script_point = ToScriptCoords(mouse_pos);
			if (color_stage == ColorStage::Range) {
				script_point = Vector2D(std::clamp(script_point.X(), 0.f, script_res.X()),
					std::clamp(script_point.Y(), 0.f, script_res.Y()));
				color_range_start = color_range_end = script_point;
				drawing = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
			}
			else if (color_selection_mode == VisualSelectionMode::PipetteAdd ||
				color_selection_mode == VisualSelectionMode::PipetteSubtract)
				PrepareColorSelection(script_point);
			else if (brush_mode) {
				PushColorBrushHistory();
				color_stage = ColorStage::Ready;
				color_brush_drawing = true;
				color_brush_moved = true;
				color_brush_last = mouse_pos;
				color_brush_stroke.clear();
				color_brush_stroke.push_back(mouse_pos);
				PaintColorBrush(mouse_pos, mouse_pos);
				if (!parent->HasCapture()) parent->CaptureMouse();
			}
			parent->SetFocus();
			parent->Render();
			return;
		}

		if (hovered_point >= 0) {
			PushMaskHistory();
			dragged_point = hovered_point;
			if (!parent->HasCapture())
				parent->CaptureMouse();
			parent->SetFocus();
		}
		else {
			Vector2D script_point = ToScriptCoords(mouse_pos);
			PushMaskHistory();
			if (mode == MASK_POINTS) {
				points.push_back(script_point);
			}
			else {
				points.clear();
				mask_regions.clear();
				drawing = true;
				shape_start = script_point;
				points.clear();
				if (mode == MASK_RECTANGLE)
					UpdateRectangle(script_point);
				else
					UpdateFreehand(script_point);
				if (!parent->HasCapture())
					parent->CaptureMouse();
			}
		}
	}

	if (dragged_point >= 0 && (event.Dragging() || event.LeftUp())) {
		points[dragged_point] = ToScriptCoords(mouse_pos);
		if (event.LeftUp()) {
			dragged_point = -1;
			if (parent->HasCapture())
				parent->ReleaseMouse();
			parent->SetFocus();
			hovered_point = PointAt(mouse_pos);
		}
		parent->Render();
		return;
	}

	if (drawing && (event.Dragging() || event.LeftUp())) {
		Vector2D script_point = ToScriptCoords(mouse_pos);
		if (mode == MASK_COLOR) {
			script_point = Vector2D(std::clamp(script_point.X(), 0.f, script_res.X()),
				std::clamp(script_point.Y(), 0.f, script_res.Y()));
			color_range_end = script_point;
		}
		else if (mode == MASK_RECTANGLE)
			UpdateRectangle(script_point);
		else
			UpdateFreehand(script_point);
	}

	if (drawing && event.LeftUp()) {
		drawing = false;
		if (mode == MASK_COLOR &&
			(FromScriptCoords(color_range_end) - FromScriptCoords(color_range_start)).Len() >= 3.f)
		{
			color_stage = ColorStage::Sample;
			EnsureColorSegmenter();
			UpdateColorCursor();
		}
		if (parent->HasCapture())
			parent->ReleaseMouse();
		parent->SetFocus();
	}

	parent->Render();
}

bool VisualToolMask::OnMouseWheel(wxMouseEvent& event) {
	int wheel = event.GetWheelRotation();
	bool color_brush_mode = mode == MASK_COLOR &&
		(color_selection_mode == VisualSelectionMode::BrushAdd ||
		 color_selection_mode == VisualSelectionMode::BrushSubtract);
	if ((mode == MASK_BRUSH || color_brush_mode) && event.AltDown() && wheel) {
		int wheel_delta = std::max(1, event.GetWheelDelta());
		int steps = wheel / wheel_delta;
		if (!steps) steps = wheel > 0 ? 1 : -1;
		if (mode == MASK_BRUSH)
			mask_brush_radius = std::clamp(mask_brush_radius + steps * 2.f, 2.f, 200.f);
		else
			color_brush_radius = std::clamp(color_brush_radius + steps * 2.f, 2.f, 200.f);
		parent->Render();
		// The gesture belongs to the brush, not the video zoom/pan handler.
		return false;
	}
	return VisualTool<VisualDraggableFeature>::OnMouseWheel(event);
}

bool VisualToolMask::OnKeyEvent(wxKeyEvent& event) {
	int key = event.GetKeyCode();
	if (event.CmdDown() && (key == 'Z' || key == 'Y')) {
		bool redo = key == 'Y' || event.ShiftDown();
		if (mode == MASK_COLOR && !ai_refining)
			return redo ? RedoColorHistory() : UndoColorHistory();
		return redo ? RedoMaskHistory() : UndoMaskHistory();
	}
	if (ai_refining) {
		if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER)
			return AcceptAIRefinement();
		if (event.GetKeyCode() == WXK_ESCAPE) {
			ClearPreview();
			return true;
		}
		return false;
	}
	bool alt_key = key == WXK_ALT;
#ifdef WXK_RALT
	alt_key = alt_key || key == WXK_RALT;
#endif
	if (alt_key) {
		if (mode == MASK_COLOR || mode == MASK_BRUSH)
			return true;
		if (!event.IsAutoRepeat())
			SetSubTool(mode == MASK_COLOR ? MASK_RECTANGLE : (mode + 1) % MASK_COLOR);
		return true;
	}
	if ((event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER) &&
		CanCreateMask() && !drawing) {
		CreateMask();
		return true;
	}
	if (event.GetKeyCode() == WXK_ESCAPE && CanCancel()) {
		ClearPreview();
		return true;
	}
	return false;
}

void VisualToolMask::Draw() {
	if (ai_refining && c->selectionController->GetActiveLine() != ai_session_line) {
		ResetAIRefinement();
		points.clear();
		mask_regions.clear();
		drawing = false;
	}
	wxColour line_colour = to_wx(line_color_primary_opt->GetColor());
	wxColour point_colour = to_wx(highlight_color_primary_opt->GetColor());
	if (ai_refining && ai_working_image && ai_working_image->IsOk()) {
		gl.DrawImage(*ai_working_image, FromScriptCoords(Vector2D(ai_crop_x, ai_crop_y)),
			FromScriptCoords(Vector2D(ai_crop_x + ai_crop_width, ai_crop_y + ai_crop_height)));
	}
	// Display the active function beside the unchanged system pointer.
	auto draw_mode_indicator = [&] {
		if (!mouse_inside || !mouse_pos) return;
		Vector2D icon = mouse_pos + Vector2D(17.f, 17.f);
		gl.SetFillColour(*wxBLACK, .55f);
		gl.SetLineColour(*wxWHITE, .9f, 1);
		gl.DrawRectangle(icon - Vector2D(6.f, 5.f), icon + Vector2D(6.f, 5.f));
		gl.SetLineColour(point_colour, 1.f, 1);
		if (mode == MASK_RECTANGLE) {
			gl.DrawRectangle(icon - Vector2D(4.f, 3.f), icon + Vector2D(4.f, 3.f));
		}
		else if (mode == MASK_POINTS) {
			gl.DrawLine(icon + Vector2D(-4.f, 2.f), icon + Vector2D(0.f, -3.f));
			gl.DrawLine(icon + Vector2D(0.f, -3.f), icon + Vector2D(4.f, 2.f));
			gl.DrawCircle(icon + Vector2D(-4.f, 2.f), 1.f);
			gl.DrawCircle(icon + Vector2D(0.f, -3.f), 1.f);
			gl.DrawCircle(icon + Vector2D(4.f, 2.f), 1.f);
		}
		else if (mode == MASK_BRUSH) {
			gl.SetFillColour(wxColour(166, 80, 230), .60f);
			gl.SetLineColour(wxColour(190, 115, 245), 1.f, 1);
			gl.DrawCircle(icon, 4.f);
		}
		else if (mode == MASK_FREEHAND) {
			gl.DrawLine(icon + Vector2D(-4.f, 2.f), icon + Vector2D(-1.f, -2.f));
			gl.DrawLine(icon + Vector2D(-1.f, -2.f), icon + Vector2D(2.f, 2.f));
			gl.DrawLine(icon + Vector2D(2.f, 2.f), icon + Vector2D(4.f, -2.f));
		}
		else {
			gl.SetFillColour(*wxBLACK, 1.f);
			gl.DrawRectangle(icon - Vector2D(5.f, 4.f), icon + Vector2D(5.f, 4.f));
			gl.SetLineColour(wxColour(80, 220, 255), 1.f, 2);
			gl.DrawLine(icon + Vector2D(-3.f, 3.f), icon + Vector2D(3.f, -3.f));
			gl.DrawCircle(icon + Vector2D(3.f, -3.f), 1.5f);
		}
	};

	if (mode == MASK_COLOR) {
		std::vector<float> flat_points;
		std::vector<int> starts;
		std::vector<int> counts;
		for (auto const& contour : color_contours) {
			starts.push_back(static_cast<int>(flat_points.size() / 2));
			counts.push_back(static_cast<int>(contour.size()));
			for (auto point : contour) {
				auto screen = FromScriptCoords(point);
				flat_points.push_back(screen.X());
				flat_points.push_back(screen.Y());
			}
		}
		if (!flat_points.empty()) {
			gl.SetLineColour(line_colour, .9f, 2);
			gl.SetFillColour(line_colour, .18f);
			gl.DrawMultiPolygon(flat_points, starts, counts, video_pos, video_size, false);
		}
		if (color_stage != ColorStage::Range || drawing) {
			Vector2D a = FromScriptCoords(color_range_start);
			Vector2D b = FromScriptCoords(color_range_end);
			Vector2D top_left = a.Min(b), bottom_right = a.Max(b);
			gl.SetLineColour(point_colour, .95f, 2);
			gl.DrawDashedLine(top_left, Vector2D(bottom_right.X(), top_left.Y()), 6.f);
			gl.DrawDashedLine(Vector2D(bottom_right.X(), top_left.Y()), bottom_right, 6.f);
			gl.DrawDashedLine(bottom_right, Vector2D(top_left.X(), bottom_right.Y()), 6.f);
			gl.DrawDashedLine(Vector2D(top_left.X(), bottom_right.Y()), top_left, 6.f);
		}
		bool brush_mode = color_selection_mode == VisualSelectionMode::BrushAdd ||
			color_selection_mode == VisualSelectionMode::BrushSubtract;
		if (brush_mode && color_stage != ColorStage::Range && mouse_inside &&
			mouse_pos && mouse_pos.Y() >= TopBarHeight()) {
			wxColour brush_colour = color_selection_mode == VisualSelectionMode::BrushAdd ?
				wxColour(55, 230, 115) : wxColour(245, 80, 90);
			gl.SetFillColour(brush_colour, .12f);
			gl.SetLineColour(brush_colour, 1.f, 2);
			gl.DrawCircle(mouse_pos, color_brush_radius);
			gl.DrawCircle(mouse_pos, 2.f);
		}
	}
	else if (!mask_regions.empty() || !points.empty()) {
		std::vector<Vector2D> screen_points;
		std::vector<float> flat_points;
		std::vector<int> starts, counts;
		auto append_region = [&](std::vector<Vector2D> const& region) {
			if (region.empty()) return;
			starts.push_back(static_cast<int>(flat_points.size() / 2));
			counts.push_back(static_cast<int>(region.size()));
			for (Vector2D point : region) {
				Vector2D screen = FromScriptCoords(point);
				flat_points.push_back(screen.X());
				flat_points.push_back(screen.Y());
			}
		};
		for (auto const& region : mask_regions) append_region(region);
		append_region(points);
		for (Vector2D point : points) {
			Vector2D screen = FromScriptCoords(point);
			screen_points.push_back(screen);
		}
		wxColour region_colour = mode == MASK_BRUSH ? wxColour(166, 80, 230) : line_colour;
		gl.SetLineColour(region_colour, mode == MASK_BRUSH ? 0.f : .7f, 2);
		gl.SetFillColour(region_colour, mode == MASK_BRUSH ? .30f : .10f);
		if (!flat_points.empty())
			gl.DrawMultiPolygon(flat_points, starts, counts, video_pos, video_size, false);
		else if (screen_points.size() == 2) gl.DrawLine(screen_points[0], screen_points[1]);
		gl.SetFillColour(point_colour, .65f);
		for (size_t i = 0; i < screen_points.size(); ++i)
			gl.DrawCircle(screen_points[i], static_cast<int>(i) == hovered_point ||
				static_cast<int>(i) == dragged_point ? 3.f : 1.5f);
		if (mode == MASK_POINTS && mouse_inside && dragged_point < 0 &&
			mouse_pos.Y() >= TopBarHeight() && ActionAt(mouse_pos) == VisualToolMaskAction::None) {
			Vector2D preview = ToScriptCoords(mouse_pos);
			preview = Vector2D(std::clamp(preview.X(), 0.f, script_res.X()),
				std::clamp(preview.Y(), 0.f, script_res.Y()));
			Vector2D screen_preview = FromScriptCoords(preview);
			gl.SetLineColour(line_colour, .85f, 2);
			gl.DrawDashedLine(screen_preview, screen_points.front(), 6.f);
			if (screen_points.size() > 1)
				gl.DrawDashedLine(screen_preview, screen_points.back(), 6.f);
		}
	}
	if (mode == MASK_BRUSH && mouse_inside && mouse_pos && mouse_pos.Y() >= TopBarHeight()) {
		gl.SetFillColour(wxColour(166, 80, 230), .32f);
		gl.SetLineColour(wxColour(166, 80, 230), 0.f, 1);
		gl.DrawCircle(mouse_pos, mask_brush_radius);
	}

	if (mode != MASK_BRUSH && !(mode == MASK_COLOR && color_stage != ColorStage::Range &&
		(color_selection_mode == VisualSelectionMode::BrushAdd ||
		 color_selection_mode == VisualSelectionMode::BrushSubtract)))
	draw_mode_indicator();
	DrawTopBar();
}

void VisualToolMask::DrawTopBar() {
	gl.SetFillColour(*wxBLACK, .72f);
	gl.SetLineColour(*wxBLACK, 0.f, 1);
	gl.DrawRectangle(Vector2D(0.f, 0.f), Vector2D(canvas_size.X(), TopBarHeight()));

	auto rounded_rectangle = [&](Vector2D top_left, Vector2D bottom_right,
		float radius, wxColour colour) {
		float safe_radius = std::min({radius, (bottom_right.X() - top_left.X()) * .5f,
			(bottom_right.Y() - top_left.Y()) * .5f});
		gl.SetFillColour(colour, 1.f);
		gl.SetLineColour(colour, 0.f, 1);
		gl.DrawRectangle(top_left + Vector2D(safe_radius, 0.f), bottom_right - Vector2D(safe_radius, 0.f));
		gl.DrawRectangle(top_left + Vector2D(0.f, safe_radius), bottom_right - Vector2D(0.f, safe_radius));
		gl.DrawCircle(top_left + Vector2D(safe_radius, safe_radius), safe_radius);
		gl.DrawCircle(Vector2D(bottom_right.X() - safe_radius, top_left.Y() + safe_radius), safe_radius);
		gl.DrawCircle(Vector2D(top_left.X() + safe_radius, bottom_right.Y() - safe_radius), safe_radius);
		gl.DrawCircle(bottom_right - Vector2D(safe_radius, safe_radius), safe_radius);
	};
	if (mode == MASK_BRUSH) {
		auto [brush_top_left, brush_bottom_right] = MaskBrushBounds();
		rounded_rectangle(brush_top_left, brush_bottom_right, 7.f, wxColour(55, 59, 64));
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		std::string brush_label = from_wx(_("Brush size"));
		int text_width, text_height;
		gl_text->GetExtent(brush_label, text_width, text_height);
		gl_text->Print(brush_label, static_cast<int>(brush_top_left.X() + 10.f),
			static_cast<int>((brush_top_left.Y() + brush_bottom_right.Y() - text_height) * .5f));
		float slider_left = brush_top_left.X() + 70.f;
		float slider_right = brush_bottom_right.X() - 12.f;
		float slider_y = (brush_top_left.Y() + brush_bottom_right.Y()) * .5f;
		gl.SetLineColour(wxColour(130, 135, 140), 1.f, 3);
		gl.DrawLine(Vector2D(slider_left, slider_y), Vector2D(slider_right, slider_y));
		float knob_x = slider_left + (mask_brush_radius - 2.f) / 198.f * (slider_right - slider_left);
		gl.SetFillColour(wxColour(190, 115, 245), 1.f);
		gl.DrawCircle(Vector2D(knob_x, slider_y), 5.f);
	}
	{
		for (auto action : {VisualToolMaskAction::Undo, VisualToolMaskAction::Redo}) {
			auto [top_left, bottom_right] = ActionBounds(action);
			bool enabled;
			if (mode == MASK_COLOR && !ai_refining)
				enabled = action == VisualToolMaskAction::Undo ? !color_undo_history.empty() :
					!color_redo_history.empty();
			else
				enabled = action == VisualToolMaskAction::Undo ? !mask_undo_history.empty() :
					!mask_redo_history.empty();
			wxColour colour = enabled ? wxColour(55, 59, 64) : wxColour(66, 69, 73);
			if (enabled && hovered_action == action)
				colour = colour.ChangeLightness(118);
			rounded_rectangle(top_left, bottom_right, 7.f, colour);
			wxColour content = enabled ? *wxWHITE : wxColour(145, 148, 152);
			gl.SetLineColour(content, 1.f, 3);
			float direction = action == VisualToolMaskAction::Undo ? -1.f : 1.f;
			Vector2D centre((top_left.X() + bottom_right.X()) * .5f,
				(top_left.Y() + bottom_right.Y()) * .5f);
			Vector2D tip = centre + Vector2D(direction * 7.f, 0.f);
			gl.DrawLine(centre - Vector2D(direction * 7.f, 0.f), tip);
			gl.DrawLine(tip, tip - Vector2D(direction * 5.f, 5.f));
			gl.DrawLine(tip, tip - Vector2D(direction * 5.f, -5.f));
		}
	}

	if (mode == MASK_COLOR && has_color_sample) {
		auto [top_left, bottom_right] = ToleranceBounds();
		rounded_rectangle(top_left, bottom_right, 7.f, wxColour(55, 59, 64));
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		std::string label = from_wx(_("Tolerance"));
		int text_width, text_height;
		gl_text->GetExtent(label, text_width, text_height);
		gl_text->Print(label, static_cast<int>(top_left.X() + 10.f),
			static_cast<int>((top_left.Y() + bottom_right.Y() - text_height) * .5f));
		float slider_left = top_left.X() + 75.f;
		float slider_right = bottom_right.X() - 12.f;
		float slider_y = (top_left.Y() + bottom_right.Y()) * .5f;
		gl.SetLineColour(wxColour(130, 135, 140), 1.f, 3);
		gl.DrawLine(Vector2D(slider_left, slider_y), Vector2D(slider_right, slider_y));
		float knob_x = slider_left + static_cast<float>(color_tolerance / 20.0) * (slider_right - slider_left);
		gl.SetFillColour(wxColour(80, 220, 255), 1.f);
		gl.DrawCircle(Vector2D(knob_x, slider_y), 5.f);

	}
	if (CanOffsetSelection()) {
		int text_width, text_height;
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		auto [offset_top_left, offset_bottom_right] = OffsetBounds();
		rounded_rectangle(offset_top_left, offset_bottom_right, 7.f, wxColour(55, 59, 64));
		std::string offset_label = from_wx(_("Offset"));
		gl_text->GetExtent(offset_label, text_width, text_height);
		gl_text->Print(offset_label, static_cast<int>(offset_top_left.X() + 10.f),
			static_cast<int>((offset_top_left.Y() + offset_bottom_right.Y() - text_height) * .5f));
		float offset_slider_left = offset_top_left.X() + 58.f;
		float offset_slider_right = offset_bottom_right.X() - 12.f;
		float offset_slider_y = (offset_top_left.Y() + offset_bottom_right.Y()) * .5f;
		gl.SetLineColour(wxColour(130, 135, 140), 1.f, 3);
		gl.DrawLine(Vector2D(offset_slider_left, offset_slider_y),
			Vector2D(offset_slider_right, offset_slider_y));
		float offset_knob_x = offset_slider_left + (color_offset + 25.f) / 50.f *
			(offset_slider_right - offset_slider_left);
		gl.SetFillColour(wxColour(80, 220, 255), 1.f);
		gl.DrawCircle(Vector2D(offset_knob_x, offset_slider_y), 5.f);
	}
	bool brush_mode = mode == MASK_COLOR &&
		(color_selection_mode == VisualSelectionMode::BrushAdd ||
		 color_selection_mode == VisualSelectionMode::BrushSubtract);
	if (brush_mode) {
		auto [brush_top_left, brush_bottom_right] = BrushBounds();
		rounded_rectangle(brush_top_left, brush_bottom_right, 7.f, wxColour(55, 59, 64));
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		std::string brush_label = from_wx(_("Brush size"));
		int text_width, text_height;
		gl_text->GetExtent(brush_label, text_width, text_height);
		gl_text->Print(brush_label, static_cast<int>(brush_top_left.X() + 10.f),
			static_cast<int>((brush_top_left.Y() + brush_bottom_right.Y() - text_height) * .5f));
		float slider_left = brush_top_left.X() + 70.f;
		float slider_right = brush_bottom_right.X() - 12.f;
		float slider_y = (brush_top_left.Y() + brush_bottom_right.Y()) * .5f;
		gl.SetLineColour(wxColour(130, 135, 140), 1.f, 3);
		gl.DrawLine(Vector2D(slider_left, slider_y), Vector2D(slider_right, slider_y));
		float knob_x = slider_left + (color_brush_radius - 2.f) / 198.f *
			(slider_right - slider_left);
		gl.SetFillColour(wxColour(80, 220, 255), 1.f);
		gl.DrawCircle(Vector2D(knob_x, slider_y), 5.f);
	}

	auto label_for = [&](VisualToolMaskAction action) -> wxString {
		switch (action) {
			case VisualToolMaskAction::Create: return ai_refining ? _("Accept (ENTER)") : _("Create mask (ENTER)");
			case VisualToolMaskAction::Clear: return _("Cancel (ESC)");
			case VisualToolMaskAction::RemoveText:
				return ai_refining ? _("Erase again") : _("AI text removal");
			case VisualToolMaskAction::GenerateText: return _("AI custom generation");
			case VisualToolMaskAction::AutoFill: return _("Auto fill");
			case VisualToolMaskAction::SelectionMode:
				switch (color_selection_mode) {
					case VisualSelectionMode::PipetteAdd: return _("Pipette add");
					case VisualSelectionMode::PipetteSubtract: return _("Pipette subtract");
					case VisualSelectionMode::BrushAdd: return _("Brush add");
					case VisualSelectionMode::BrushSubtract: return _("Brush subtract");
				}
			case VisualToolMaskAction::AISelect: return _("AI recognition");
			default: return wxString();
		}
	};
	std::vector<VisualToolMaskAction> actions;
	if (ai_refining)
		actions = {VisualToolMaskAction::RemoveText, VisualToolMaskAction::GenerateText,
			VisualToolMaskAction::Create, VisualToolMaskAction::Clear};
	else if (mode == MASK_COLOR) {
		actions = {VisualToolMaskAction::SelectionMode};
		actions.push_back(VisualToolMaskAction::AISelect);
		if (has_color_sample)
			actions.push_back(VisualToolMaskAction::AutoFill);
		actions.push_back(VisualToolMaskAction::Create);
		actions.push_back(VisualToolMaskAction::Clear);
	}
	else actions = {VisualToolMaskAction::Create,
			VisualToolMaskAction::Clear, VisualToolMaskAction::RemoveText,
			VisualToolMaskAction::GenerateText};
	for (auto action : actions) {
		auto [top_left, bottom_right] = ActionBounds(action);
		bool pending = !mask_regions.empty() ||
			(points.size() >= 3 && std::abs(polygon_area(points)) > .5);
		bool enabled = (action == VisualToolMaskAction::Clear && CanCancel()) ||
			(action == VisualToolMaskAction::AutoFill && has_color_sample) ||
			action == VisualToolMaskAction::SelectionMode || CanCreateMask();
		if (action == VisualToolMaskAction::RemoveText)
			enabled = ai_refining ? pending : CanCreateMask();
		if (action == VisualToolMaskAction::GenerateText)
			enabled = !ai::GetApiKey().empty() && (ai_refining ? pending : CanCreateMask());
		if (action == VisualToolMaskAction::SelectionMode)
			enabled = enabled && color_stage != ColorStage::Range;
		if (action == VisualToolMaskAction::AISelect)
			enabled = color_stage != ColorStage::Range;
		wxColour colour(55, 59, 64);
		if (action == VisualToolMaskAction::Create) colour = wxColour(31, 153, 76);
		else if (action == VisualToolMaskAction::Clear) colour = wxColour(183, 54, 61);
		else if (action == VisualToolMaskAction::RemoveText) colour = wxColour(180, 105, 43);
		else if (action == VisualToolMaskAction::AISelect) colour = wxColour(180, 105, 43);
		else if (action == VisualToolMaskAction::GenerateText) colour = wxColour(35, 125, 153);
		else if (action == VisualToolMaskAction::AutoFill && color_auto_fill) colour = wxColour(35, 125, 153);
		if (!enabled) colour = wxColour(66, 69, 73);
		else if (hovered_action == action) colour = colour.ChangeLightness(118);
		rounded_rectangle(top_left, bottom_right, 7.f, colour);

		wxColour content = enabled ? *wxWHITE : wxColour(145, 148, 152);
		gl.SetLineColour(content, 1.f, 3);
		if (action == VisualToolMaskAction::SelectionMode) {
			float icon_y = (top_left.Y() + bottom_right.Y()) * .5f;
			gl.SetFillColour(content, 1.f);
			gl.DrawTriangle(Vector2D(bottom_right.X() - 14.f, icon_y - 2.f),
				Vector2D(bottom_right.X() - 6.f, icon_y - 2.f),
				Vector2D(bottom_right.X() - 10.f, icon_y + 3.f));
		}
		gl_text->SetFont("Verdana", 9, true, false);
		gl_text->SetColour(enabled ? agi::Color(255, 255, 255, 255) : agi::Color(145, 148, 152, 255));
		std::string text = from_wx(label_for(action));
		int text_width, text_height;
		gl_text->GetExtent(text, text_width, text_height);
		gl_text->Print(text, static_cast<int>(top_left.X() + 12.f),
			static_cast<int>((top_left.Y() + bottom_right.Y() - text_height) * .5f));
	}
	if (ai_refining) {
		auto clear_bounds = ActionBounds(VisualToolMaskAction::Clear);
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(220, 223, 226, 255));
		gl_text->Print(from_wx(_("You can refine the result again if you want.")),
			static_cast<int>(clear_bounds.second.X() + 12.f),
			static_cast<int>((clear_bounds.first.Y() + clear_bounds.second.Y() - 13.f) * .5f));
	}

}

Vector2D VisualToolMask::ColourSamplePoint() const {
	std::vector<Vector2D> const *sample_points = nullptr;
	if (mode == MASK_COLOR) {
		double largest_area = 0.0;
		for (auto const& contour : color_contours) {
			double area = std::abs(polygon_area(contour));
			if (contour.size() >= 2 && area > largest_area) {
				largest_area = area;
				sample_points = &contour;
			}
		}
	}
	else if (!points.empty())
		sample_points = &points;
	else if (!mask_regions.empty())
		sample_points = &mask_regions.front();
	if (!sample_points || sample_points->size() < 2)
		return script_res * .5f;
	auto const& polygon = *sample_points;
	double area = polygon_area(polygon);
	Vector2D edge = polygon[1] - polygon[0];
	Vector2D inward = edge.Perpendicular().Unit();
	if (area < 0.0)
		inward = -inward;
	Vector2D candidate = (polygon[0] + polygon[1]) * .5f + inward * 5.f;
	if (point_in_polygon(candidate, polygon))
		return candidate;

	Vector2D centre(0.f, 0.f);
	for (Vector2D point : polygon)
		centre = centre + point;
	centre = centre / static_cast<float>(polygon.size());
	if (point_in_polygon(centre, polygon))
		return centre;

	return (polygon[0] + polygon[1]) * .5f;
}

agi::Color VisualToolMask::SampleColour(AssStyle const *style) const {
	agi::Color fallback = style ? style->primary : agi::Color(255, 255, 255);
	if (mode == MASK_COLOR && has_color_sample)
		return agi::Color(color_sample.red, color_sample.green, color_sample.blue);
	try {
		auto frame = c->videoController->GetFrame(frame_number, true);
		if (!frame || frame->width <= 0 || frame->height <= 0 || frame->pitch <= 0)
			return fallback;

		Vector2D sample = ColourSamplePoint();
		int x = std::clamp(static_cast<int>(std::lround(sample.X() * frame->width / script_res.X())), 0, frame->width - 1);
		int y = std::clamp(static_cast<int>(std::lround(sample.Y() * frame->height / script_res.Y())), 0, frame->height - 1);
		int red = 0, green = 0, blue = 0, samples = 0;
		for (int dy = -1; dy <= 1; ++dy) {
			int source_y = std::clamp(y + dy, 0, frame->height - 1);
			if (frame->flipped)
				source_y = frame->height - 1 - source_y;
			for (int dx = -1; dx <= 1; ++dx) {
				int source_x = std::clamp(x + dx, 0, frame->width - 1);
				size_t offset = static_cast<size_t>(source_y) * frame->pitch + static_cast<size_t>(source_x) * 4;
				if (offset + 2 >= frame->data.size())
					continue;
				blue += frame->data[offset];
				green += frame->data[offset + 1];
				red += frame->data[offset + 2];
				++samples;
			}
		}
		if (samples)
			return agi::Color(red / samples, green / samples, blue / samples);
	}
	catch (...) {
	}
	return fallback;
}

std::string VisualToolMask::EncodeDrawing() const {
	if (mode == MASK_COLOR) {
		std::string encoded;
		for (auto const& contour : color_contours) {
			if (contour.size() < 3) continue;
			if (!encoded.empty()) encoded += " ";
			encoded += "m ";
			for (size_t i = 0; i < contour.size(); ++i) {
				if (i == 1) encoded += " l ";
				else if (i > 1) encoded += " ";
				encoded += contour[i].Str(' ', 1);
			}
		}
		return encoded;
	}
	std::string encoded;
	auto append_region = [&](std::vector<Vector2D> const& region) {
		if (region.size() < 3) return;
		if (!encoded.empty()) encoded += " ";
		encoded += "m ";
		for (size_t i = 0; i < region.size(); ++i) {
			if (i == 1) encoded += " l ";
			else if (i > 1) encoded += " ";
			if (mode == MASK_BRUSH)
				encoded += region[i].Str(' ', 1);
			else
				encoded += agi::format("%d %d", static_cast<int>(std::lround(region[i].X())),
					static_cast<int>(std::lround(region[i].Y())));
		}
	};
	for (auto const& region : mask_regions) append_region(region);
	append_region(points);
	return encoded;
}

void VisualToolMask::CreateMask() {
	if (ai_refining) {
		AcceptAIRefinement();
		return;
	}
	if (!CanCreateMask())
		return;

	AssDialogue *source = c->selectionController->GetActiveLine();
	AssStyle *style = c->ass->GetStyle(source->Style.get());
	agi::Color colour = SampleColour(style);

	auto mask = new AssDialogue;
	mask->Style = source->Style;
	mask->Layer = source->Layer;
	int frame_time = c->videoController->TimeAtFrame(frame_number, agi::vfr::EXACT);
	if (source->Start <= frame_time && frame_time < source->End) {
		mask->Start = source->Start;
		mask->End = source->End;
	}
	else {
		mask->Start = c->videoController->TimeAtFrame(frame_number, agi::vfr::START);
		mask->End = c->videoController->TimeAtFrame(frame_number, agi::vfr::END);
	}

	std::string tags = "{" + drawing_style_tags(style);
	tags += "\\blur1\\pos(0,0)\\1c" + colour.GetAssOverrideFormatted() + "\\p1}";
	mask->Text = tags + EncodeDrawing();

	c->ass->Events.insert(c->ass->iterator_to(*source), *mask);
	c->ass->Commit(_("Create mask"), AssFile::COMMIT_DIAG_ADDREM);
	c->selectionController->SetSelectionAndActive({mask}, mask);
	ClearPreview();
}

std::vector<unsigned char> VisualToolMask::EncodePng(wxImage const& image) const {
	wxMemoryOutputStream stream;
	if (!image.SaveFile(stream, wxBITMAP_TYPE_PNG))
		throw ai::Error("A PNG-kép kódolása sikertelen.");
	std::vector<unsigned char> data(stream.GetSize());
	if (!data.empty()) stream.CopyTo(data.data(), data.size());
	return data;
}

std::vector<AssDialogue *> VisualToolMask::ConvertImageToAss(wxImage image, AssDialogue *source,
	int crop_x, int crop_y, int crop_width, int crop_height) const {
	if (!image.IsOk()) return {};
	if (crop_width <= 0 || crop_height <= 0) return {};
	if (image.GetWidth() != crop_width || image.GetHeight() != crop_height)
		image = image.Scale(crop_width, crop_height, wxIMAGE_QUALITY_BOX_AVERAGE);
	if (!image.IsOk()) return {};
	int image_width = image.GetWidth();
	int image_height = image.GetHeight();
	if (image_width <= 0 || image_height <= 0)
		return {};

	// Image2ASS-compatible output: one bitmap row per ASS line, p1 only, and
	// integer coordinates throughout. Scaling the bitmap before conversion
	// avoids subpixel rectangles, which can render with visible seams.
	float image_x_scale = static_cast<float>(image_width) / crop_width;
	float image_y_scale = static_cast<float>(image_height) / crop_height;
	std::vector<Vector2D> image_points;
	image_points.reserve(points.size());
	for (auto point : points)
		image_points.emplace_back((point.X() - crop_x) * image_x_scale,
			(point.Y() - crop_y) * image_y_scale);

	AssStyle *style = c->ass->GetStyle(source->Style.get());
	int frame_time = c->videoController->TimeAtFrame(frame_number, agi::vfr::EXACT);
	agi::Time start = source->Start;
	agi::Time end = source->End;
	if (!(source->Start <= frame_time && frame_time < source->End)) {
		start = c->videoController->TimeAtFrame(frame_number, agi::vfr::START);
		end = c->videoController->TimeAtFrame(frame_number, agi::vfr::END);
	}

	auto pixels = image.GetData();
	std::vector<AssDialogue *> lines;
	for (int y = 0; y < image_height; ++y) {
		struct ImageRun {
			int start;
			int end;
			bool transparent;
			int red;
			int green;
			int blue;
		};

		std::vector<bool> opaque(static_cast<size_t>(image_width), false);
		if (image.HasAlpha()) {
			auto alpha = image.GetAlpha() + static_cast<size_t>(y) * image_width;
			for (int x = 0; x < image_width; ++x)
				opaque[x] = alpha[x] != 0;
		}
		else {
			for (auto [span_start, span_end] : polygon_spans(image_points, y + .5f, 1.f, image_width))
				std::fill(opaque.begin() + span_start, opaque.begin() + span_end, true);
		}

		std::vector<ImageRun> runs;
		double average_r = 0.0, average_g = 0.0, average_b = 0.0;
		double variance_r = 0.0, variance_g = 0.0, variance_b = 0.0;
		unsigned char last_r = 0, last_g = 0, last_b = 0;
		int run_width = 0;
		bool run_transparent = true;
		int run_start = 0;

		auto begin_run = [&](int x) {
			size_t offset = (static_cast<size_t>(y) * image_width + x) * 3;
			average_r = last_r = pixels[offset];
			average_g = last_g = pixels[offset + 1];
			average_b = last_b = pixels[offset + 2];
			variance_r = variance_g = variance_b = 0.0;
			run_width = 1;
			run_start = x;
			run_transparent = !opaque[x];
		};
		auto finish_run = [&] {
			ImageRun run{run_start, run_start + run_width, run_transparent,
				static_cast<int>(std::lround(average_r)),
				static_cast<int>(std::lround(average_g)),
				static_cast<int>(std::lround(average_b))};
			if (!runs.empty() && runs.back().transparent == run.transparent &&
				(run.transparent || (runs.back().red == run.red && runs.back().green == run.green &&
					runs.back().blue == run.blue)))
				runs.back().end = run.end;
			else
				runs.push_back(run);
		};

		begin_run(0);
		for (int x = 1; x < image_width; ++x) {
			size_t offset = (static_cast<size_t>(y) * image_width + x) * 3;
			unsigned char red = pixels[offset];
			unsigned char green = pixels[offset + 1];
			unsigned char blue = pixels[offset + 2];
			bool transparent = !opaque[x];
			double next_average_r = average_r + (red - average_r) / (run_width + 1);
			double next_average_g = average_g + (green - average_g) / (run_width + 1);
			double next_average_b = average_b + (blue - average_b) / (run_width + 1);
			double next_variance_r = variance_r + (red - average_r) * (red - next_average_r);
			double next_variance_g = variance_g + (green - average_g) * (green - next_average_g);
			double next_variance_b = variance_b + (blue - average_b) * (blue - next_average_b);
			bool merge = (run_transparent && transparent) ||
				(run_transparent == transparent &&
					colour_distance(last_r, last_g, last_b, red, green, blue) < image_colour_tolerance &&
					colour_distance(next_variance_r, next_variance_g, next_variance_b,
						0.0, 0.0, 0.0) < image_colour_tolerance);
			if (merge) {
				++run_width;
				average_r = next_average_r;
				average_g = next_average_g;
				average_b = next_average_b;
				variance_r = next_variance_r;
				variance_g = next_variance_g;
				variance_b = next_variance_b;
				last_r = red;
				last_g = green;
				last_b = blue;
			}
			else {
				finish_run();
				begin_run(x);
			}
		}
		finish_run();

		while (!runs.empty() && runs.front().transparent)
			runs.erase(runs.begin());
		while (!runs.empty() && runs.back().transparent)
			runs.pop_back();
		if (runs.empty()) continue;

		// Image2ASS places each bitmap row independently. Leading transparent
		// pixels are represented by shifting the line's whole-number position;
		// each subsequent run is a fresh m 0 0 rectangle and libass lays those
		// drawing blocks out consecutively. Using absolute coordinates inside
		// each colour block makes libass add the offsets repeatedly and produces
		// the characteristic patch growing to the right.
		int row_x = crop_x + runs.front().start;
		int row_y = crop_y + y;
		std::string drawing_text;
		bool current_transparent = false;
		std::string current_colour;
		for (auto const& run : runs) {
			int shape_width = run.end - run.start;
			if (shape_width <= 0) continue;
			std::string tags;
			if (run.transparent != current_transparent) {
				tags += run.transparent ? "\\1a&HFF&" : "\\1a&H00&";
				current_transparent = run.transparent;
			}
			if (!run.transparent) {
				auto colour = agi::Color(run.red, run.green, run.blue).GetAssOverrideFormatted();
				if (colour != current_colour) {
					tags += "\\1c" + colour;
					current_colour = std::move(colour);
				}
			}
			drawing_text += "{" + tags + "}";
			drawing_text += agi::format("m 0 0 l 0 1 %d 1 %d 0", shape_width, shape_width);
		}
		if (drawing_text.empty()) continue;

		auto line = new AssDialogue;
		line->Style = source->Style;
		line->Layer = source->Layer;
		line->Start = start;
		line->End = end;
		line->Text = "{" + drawing_style_tags(style) + agi::format("\\pos(%d,%d)", row_x, row_y) +
			"\\1a&H00&\\p1}" + drawing_text;
		lines.push_back(line);
	}
	return lines;
}

std::unique_ptr<wxImage> VisualToolMask::RunAIImageEdit(wxImage const& scene,
	wxImage const& mask, std::string const& prompt, wxString const& progress_message,
	bool use_cloudinary) {
	std::vector<unsigned char> image_png;
	std::vector<unsigned char> mask_png;
	try {
		image_png = EncodePng(scene);
		mask_png = EncodePng(mask);
	}
	catch (std::exception const& error) {
		wxMessageBox(to_wx(error.what()), _("AI masking failed"), wxOK | wxICON_ERROR, c->parent);
		return {};
	}

	std::string encoded_result;
	std::string error_message;
	{
		// Destroy the modal progress window before showing a request error. wxWidgets
		// can otherwise process the progress window's delayed close event after the
		// error message closes, dereferencing its already-finished progress sink.
		DialogProgress progress(c->parent, _("AI masking"), progress_message);
		try {
			progress.Run([&](agi::ProgressSink *sink) {
				sink->SetIndeterminate();
				try {
					if (use_cloudinary)
						encoded_result = ai::CloudinaryGenerativeRemove(configured_cloudinary(), image_png, "text",
							[sink] { return sink->IsCancelled(); });
					else
						encoded_result = ai::EditImage(ai::GetApiKey(),
							OPT_GET("AI/OpenAI/Image Model")->GetString(), image_png, mask_png,
							agi::format("%dx%d", scene.GetWidth(), scene.GetHeight()), prompt,
							[sink] { return sink->IsCancelled(); });
				}
				catch (std::exception const& error) {
					error_message = error.what();
				}
			});
		}
		catch (agi::UserCancelException const&) {
			return {};
		}
	}
	if (!error_message.empty()) {
		wxMessageBox(to_wx(error_message), _("AI masking failed"), wxOK | wxICON_ERROR, c->parent);
		return {};
	}

	wxMemoryBuffer decoded = wxBase64Decode(encoded_result.data(), encoded_result.size());
	if (!decoded.GetDataLen()) {
		wxMessageBox(_("The AI image response could not be decoded."), _("AI masking failed"),
			wxOK | wxICON_ERROR, c->parent);
		return {};
	}
	wxMemoryInputStream result_stream(decoded.GetData(), decoded.GetDataLen());
	auto result = std::make_unique<wxImage>(result_stream, wxBITMAP_TYPE_PNG);
	if (!result->IsOk()) {
		wxMessageBox(_("The AI image result could not be converted to ASS."), _("AI masking failed"),
			wxOK | wxICON_ERROR, c->parent);
		return {};
	}
	if (result->GetWidth() != scene.GetWidth() || result->GetHeight() != scene.GetHeight())
		*result = result->Scale(scene.GetWidth(), scene.GetHeight(), wxIMAGE_QUALITY_HIGH);
	return result;
}

bool VisualToolMask::AcceptAIRefinement() {
	if (!ai_refining || !ai_working_image || ai_selection_mask.empty()) return false;
	AssDialogue *source = c->selectionController->GetActiveLine();
	if (!source || source != ai_session_line) return false;
	int width = ai_working_image->GetWidth();
	int height = ai_working_image->GetHeight();
	int left = width, top = height, right = 0, bottom = 0;
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
			if (ai_selection_mask[static_cast<size_t>(y) * width + x]) {
				left = std::min(left, x); top = std::min(top, y);
				right = std::max(right, x + 1); bottom = std::max(bottom, y + 1);
			}
	if (right <= left || bottom <= top) return false;
	wxImage result = ai_working_image->GetSubImage(wxRect(left, top, right - left, bottom - top));
	result.InitAlpha();
	auto alpha = result.GetAlpha();
	for (int y = top; y < bottom; ++y)
		for (int x = left; x < right; ++x)
			alpha[static_cast<size_t>(y - top) * (right - left) + x - left] =
				ai_selection_mask[static_cast<size_t>(y) * width + x] ? 255 : 0;
	auto lines = ConvertImageToAss(std::move(result), source, left, top, right - left, bottom - top);
	if (lines.empty()) {
		wxMessageBox(_("The AI image did not contain any pixels inside the mask."),
			_("AI masking failed"), wxOK | wxICON_ERROR, c->parent);
		return false;
	}

	Selection selection;
	auto position = c->ass->iterator_to(*source);
	for (auto line : lines) {
		c->ass->Events.insert(position, *line);
		selection.insert(line);
	}
	c->ass->Commit(_("Create AI mask"), AssFile::COMMIT_DIAG_ADDREM);
	c->selectionController->SetSelectionAndActive(std::move(selection), lines.front());
	ResetAIRefinement();
	points.clear();
	mask_regions.clear();
	drawing = false;
	UpdateActionTooltip(VisualToolMaskAction::None);
	parent->Render();
	return true;
}

void VisualToolMask::CreateAIMask(VisualToolMaskAction action) {
	if (action == VisualToolMaskAction::RemoveText && !configured_cloudinary().Complete()) {
		wxMessageBox(_("Cloudinary is not configured correctly. Open AI connection settings and enter the cloud name, API key and API secret."),
			_("AI text removal failed"), wxOK | wxICON_ERROR, c->parent);
		return;
	}
	if (action == VisualToolMaskAction::GenerateText && ai::GetApiKey().empty()) return;
	CommitCurrentRegion();
	if (mask_regions.empty()) return;
	AssDialogue *source = c->selectionController->GetActiveLine();
	if (!source || (ai_refining && source != ai_session_line)) return;

	std::string user_prompt;
	if (action == VisualToolMaskAction::GenerateText) {
		wxDialog dialog(c->parent, wxID_ANY, _("AI custom generation"),
			wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
		auto sizer = new wxBoxSizer(wxVERTICAL);
		sizer->Add(new wxStaticText(&dialog, wxID_ANY, _("AI prompt:")),
			wxSizerFlags().Border(wxLEFT | wxRIGHT | wxTOP));
		auto prompt_input = new wxTextCtrl(&dialog, wxID_ANY, "", wxDefaultPosition,
			wxSize(520, 170), wxTE_MULTILINE);
		sizer->Add(prompt_input, wxSizerFlags(1).Expand().Border(wxALL));
		sizer->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL),
			wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		dialog.SetSizerAndFit(sizer);
		dialog.SetMinSize(wxSize(560, 260));
		dialog.CenterOnParent();
		prompt_input->SetFocus();
		if (dialog.ShowModal() != wxID_OK) return;
		user_prompt = from_wx(prompt_input->GetValue());
		if (user_prompt.empty()) {
			wxMessageBox(_("Enter an AI prompt."), _("AI custom generation"),
				wxOK | wxICON_WARNING, c->parent);
			return;
		}
	}

	int script_width = std::max(1, static_cast<int>(std::lround(script_res.X())));
	int script_height = std::max(1, static_cast<int>(std::lround(script_res.Y())));
	wxImage working;
	if (ai_refining && ai_working_image && ai_working_image->IsOk())
		working = ai_working_image->Copy();
	else {
		auto frame = c->videoController->GetFrame(frame_number, true);
		if (!frame) return;
		working = GetImage(*frame).Scale(script_width, script_height, wxIMAGE_QUALITY_HIGH);
	}
	if (working.HasAlpha()) working.ClearAlpha();

	int selected_left = script_width, selected_top = script_height;
	int selected_right = 0, selected_bottom = 0;
	for (auto const& region : mask_regions)
		for (auto point : region) {
			selected_left = std::min(selected_left, static_cast<int>(std::floor(point.X())));
			selected_top = std::min(selected_top, static_cast<int>(std::floor(point.Y())));
			selected_right = std::max(selected_right, static_cast<int>(std::ceil(point.X())));
			selected_bottom = std::max(selected_bottom, static_cast<int>(std::ceil(point.Y())));
		}
	selected_left = std::clamp(selected_left, 0, script_width - 1);
	selected_top = std::clamp(selected_top, 0, script_height - 1);
	selected_right = std::clamp(std::max(selected_left + 1, selected_right), selected_left + 1, script_width);
	selected_bottom = std::clamp(std::max(selected_top + 1, selected_bottom), selected_top + 1, script_height);
	int selected_width = selected_right - selected_left;
	int selected_height = selected_bottom - selected_top;
	// Cloudinary receives only the affected local scene. A modest context band is
	// enough for texture continuation without letting unrelated surroundings
	// influence the generation.
	int margin = std::clamp(static_cast<int>(std::lround(std::max(selected_width, selected_height) * .08)), 8, 32);
	int crop_x = std::max(0, selected_left - margin);
	int crop_y = std::max(0, selected_top - margin);
	int crop_right = std::min(script_width, selected_right + margin);
	int crop_bottom = std::min(script_height, selected_bottom + margin);
	int crop_width = crop_right - crop_x;
	int crop_height = crop_bottom - crop_y;

	std::vector<unsigned char> selected(static_cast<size_t>(crop_width) * crop_height);
	for (auto const& region : mask_regions) {
		std::vector<Vector2D> local;
		local.reserve(region.size());
		for (auto point : region)
			local.emplace_back(point.X() - crop_x, point.Y() - crop_y);
		auto raster = rasterize_polygon(local, crop_width, crop_height);
		for (size_t i = 0; i < selected.size(); ++i)
			selected[i] = static_cast<unsigned char>(selected[i] || raster[i]);
	}

	double crop_pixels = std::max(1.0, static_cast<double>(crop_width) * crop_height);
	constexpr double minimum_request_pixels = 655360.0;
	constexpr double maximum_request_pixels = 921600.0;
	constexpr double cloudinary_minimum_dimension = 100.0;
	double scale;
	if (action == VisualToolMaskAction::RemoveText) {
		// Cloudinary rejects very small generative requests. Upscale only those
		// requests, preserving their aspect ratio, and scale the response back
		// before it is composited into the full-resolution preview.
		scale = std::max({1.0, cloudinary_minimum_dimension / crop_width,
			cloudinary_minimum_dimension / crop_height});
	}
	else {
		scale = crop_pixels < minimum_request_pixels ? std::sqrt(minimum_request_pixels / crop_pixels) :
			crop_pixels > maximum_request_pixels ? std::sqrt(maximum_request_pixels / crop_pixels) : 1.0;
	}
	int target_width = action == VisualToolMaskAction::RemoveText ?
		static_cast<int>(std::ceil(crop_width * scale)) :
		std::max(16, static_cast<int>(std::ceil(crop_width * scale / 16.0)) * 16);
	int target_height = action == VisualToolMaskAction::RemoveText ?
		static_cast<int>(std::ceil(crop_height * scale)) :
		std::max(16, static_cast<int>(std::ceil(crop_height * scale / 16.0)) * 16);
	// During refinement, working is a copy of ai_working_image, which is the
	// image currently visible in the preview. Each new erase therefore sends
	// the selected crop from the latest result rather than from the video frame.
	wxImage scene = working.GetSubImage(wxRect(crop_x, crop_y, crop_width, crop_height));
	if (target_width != crop_width || target_height != crop_height)
		scene = scene.Scale(target_width, target_height, wxIMAGE_QUALITY_HIGH);

	wxImage mask(target_width, target_height, false);
	std::fill(mask.GetData(), mask.GetData() + static_cast<size_t>(target_width) * target_height * 3, 0);
	mask.InitAlpha();
	auto mask_alpha = mask.GetAlpha();
	std::fill(mask_alpha, mask_alpha + static_cast<size_t>(target_width) * target_height, 255);
	for (int y = 0; y < target_height; ++y) {
		int source_y = std::min(crop_height - 1, y * crop_height / target_height);
		for (int x = 0; x < target_width; ++x) {
			int source_x = std::min(crop_width - 1, x * crop_width / target_width);
			if (selected[static_cast<size_t>(source_y) * crop_width + source_x])
				mask_alpha[static_cast<size_t>(y) * target_width + x] = 0;
		}
	}

	std::string prompt;
	if (action == VisualToolMaskAction::RemoveText) {
		prompt = "Fast localized generative erase. Replace only the transparent mask with the natural continuation of "
			"the same surface visible immediately around it. Remove all text, ink, symbols, marks and unwanted objects "
			"inside the mask. Use the surrounding opaque context to match material, colour, texture, lighting, perspective, "
			"focus and grain exactly. The mask edge is invisible: create no border, line, seam, halo, blur or colour shift. "
			"Keep all opaque pixels and image registration unchanged.";
	}
	else {
		prompt = "Kiindulásnak használja a csatolt képet. " + user_prompt;
		if (!ai_prompt_history.empty()) {
			prompt += " Earlier requests in this same preview session were: ";
			for (size_t i = 0; i < ai_prompt_history.size(); ++i) {
				if (i) prompt += "; ";
				prompt += ai_prompt_history[i];
			}
			prompt += ". Keep the result visually consistent with those earlier edits.";
		}
	}

	auto result = RunAIImageEdit(scene, mask, prompt, _("Generating the edited image..."),
		action == VisualToolMaskAction::RemoveText);
	if (!result) return;
	if (result->GetWidth() != crop_width || result->GetHeight() != crop_height)
		*result = result->Scale(crop_width, crop_height, wxIMAGE_QUALITY_HIGH);
	if (result->HasAlpha()) result->ClearAlpha();
	auto working_data = working.GetData();
	auto result_data = result->GetData();
	for (int y = 0; y < crop_height; ++y)
		for (int x = 0; x < crop_width; ++x) {
			size_t local = static_cast<size_t>(y) * crop_width + x;
			if (!selected[local]) continue;
			size_t destination = (static_cast<size_t>(y + crop_y) * script_width + x + crop_x) * 3;
			std::copy_n(result_data + local * 3, 3, working_data + destination);
		}

	if (!ai_refining)
		ai_selection_mask.assign(static_cast<size_t>(script_width) * script_height, 0);
	for (int y = 0; y < crop_height; ++y)
		for (int x = 0; x < crop_width; ++x)
			if (selected[static_cast<size_t>(y) * crop_width + x])
				ai_selection_mask[static_cast<size_t>(y + crop_y) * script_width + x + crop_x] = 1;

	ai_refining = true;
	ai_session_line = source;
	ai_working_image = std::make_unique<wxImage>(std::move(working));
	ai_crop_x = ai_crop_y = 0;
	ai_crop_width = script_width;
	ai_crop_height = script_height;
	if (action == VisualToolMaskAction::GenerateText) {
		if (ai_prompt_history.size() == 6) ai_prompt_history.erase(ai_prompt_history.begin());
		ai_prompt_history.push_back(std::move(user_prompt));
	}
	auto video_name = c->project->VideoName();
	if (!video_name.string().starts_with("?dummy")) {
		auto debug_path = video_name.parent_path() /
			(video_name.stem().string() + agi::format("-%d-ai-generated.jpg", frame_number));
		wxImage debug_image = ai_working_image->Copy();
		debug_image.SetOption(wxIMAGE_OPTION_QUALITY, 95);
		if (!debug_image.SaveFile(to_wx(debug_path.string()), wxBITMAP_TYPE_JPEG))
			wxMessageBox(_("The raw AI image could not be saved to:") + "\n" +
				to_wx(debug_path.string()), _("AI masking failed"),
				wxOK | wxICON_WARNING, c->parent);
	}
	mask_regions.clear();
	points.clear();
	mask_undo_history.clear();
	mask_redo_history.clear();
	drawing = false;
	gl.InvalidateImageCache();
	parent->Render();
}
