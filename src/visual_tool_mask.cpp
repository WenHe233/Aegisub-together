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
#include "imagemask_codec.h"
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
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>

#include <chrono>
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
	constexpr int image_colour_tolerance = 25;

	namespace bg = boost::geometry;
	using MaskPoint = bg::model::d2::point_xy<double>;
	using MaskPolygon = bg::model::polygon<MaskPoint>;
	using MaskMultiPolygon = bg::model::multi_polygon<MaskPolygon>;

	/// Sub-pixel detail dropped from a merged brush outline. The circles are stamped a
	/// fraction of a radius apart, so their union carries far more vertices along a
	/// straight run of the stroke than the shape of it needs.
	constexpr double mask_brush_simplify = .1;

	/// Rebuild a merged region from the contours the mask stores, so a new stroke carries
	/// on from whatever is already there rather than starting from nothing.
	MaskMultiPolygon MaskRegionFrom(std::vector<std::vector<Vector2D>> const& contours);

	/// The outline the mask points describe, edge by edge. A curved edge leaves and enters
	/// its ends along the line joining the neighbours on either side, which makes the joins
	/// smooth without asking for handles to be placed; a straight edge stays straight even
	/// where its neighbours curve, so one outline can be part polygon and part curve.
	/// Where a curved edge's handles sit when nobody has moved them: the smooth choice, so
	/// a new curve arrives looking like a curve and is then the user's to shape.
	std::pair<Vector2D, Vector2D> DefaultEdgeControls(std::vector<Vector2D> const& points,
			size_t edge) {
		size_t count = points.size();
		Vector2D previous = points[(edge + count - 1) % count];
		Vector2D start = points[edge];
		Vector2D end = points[(edge + 1) % count];
		Vector2D next = points[(edge + 2) % count];
		// With only two points the neighbours are the ends themselves, and the smooth
		// formula collapses onto them. Fall back to thirds of the chord: a straight start
		// the handles can then be pulled away from, rather than handles with nowhere to be.
		if (count < 3) {
			Vector2D along = (end - start) / 3.f;
			return {start + along, end - along};
		}
		return {start + (end - previous) / 6.f, end - (next - start) / 6.f};
	}

	std::vector<SplineCurve> CurveThroughPoints(std::vector<Vector2D> const& points,
			std::vector<char> const& curved, std::vector<Vector2D> const& control_out,
			std::vector<Vector2D> const& control_in) {
		std::vector<SplineCurve> curves;
		size_t count = points.size();
		if (count < 2) return curves;
		for (size_t i = 0; i < count; ++i) {
			Vector2D start = points[i];
			Vector2D end = points[(i + 1) % count];
			if (i >= curved.size() || !curved[i]) {
				curves.emplace_back(start, end);
				continue;
			}
			auto [out, in] = i < control_out.size() && i < control_in.size() ?
				std::pair<Vector2D, Vector2D>{control_out[i], control_in[i]} :
				DefaultEdgeControls(points, i);
			curves.emplace_back(start, out, in, end);
		}
		return curves;
	}

	std::vector<Vector2D> FlattenCurves(std::vector<SplineCurve> const& curves) {
		std::vector<Vector2D> out;
		if (curves.empty()) return out;
		out.push_back(curves.front().p1);
		for (auto const& curve : curves) {
			if (curve.type != SplineCurve::BICUBIC) {
				out.push_back(curve.p2);
				continue;
			}
			float length = (curve.p2 - curve.p1).Len() + (curve.p3 - curve.p2).Len() +
				(curve.p4 - curve.p3).Len();
			int steps = std::clamp(static_cast<int>(std::ceil(length / 3.f)), 4, 48);
			for (int step = 1; step <= steps; ++step)
				out.push_back(curve.GetPoint(static_cast<float>(step) / steps));
		}
		if (out.size() > 3 && (out.front() - out.back()).SquareLen() < 1e-5f) out.pop_back();
		return out;
	}

	MaskPolygon MakeMaskCircle(Vector2D centre, double radius) {
		constexpr int circle_points = 48;
		MaskPolygon polygon;
		auto& ring = polygon.outer();
		ring.reserve(circle_points + 1);
		for (int i = 0; i < circle_points; ++i) {
			double angle = i * 2.0 * M_PI / circle_points;
			ring.emplace_back(centre.X() + std::cos(angle) * radius,
				centre.Y() + std::sin(angle) * radius);
		}
		ring.push_back(ring.front());
		bg::correct(polygon);
		return polygon;
	}

	MaskMultiPolygon MaskRegionFrom(std::vector<std::vector<Vector2D>> const& contours) {
		MaskMultiPolygon region;
		for (auto const& contour : contours) {
			if (contour.size() < 3) continue;
			MaskPolygon polygon;
			auto& ring = polygon.outer();
			ring.reserve(contour.size() + 1);
			double signed_area = 0;
			for (size_t i = 0, previous = contour.size() - 1; i < contour.size(); previous = i++)
				signed_area += static_cast<double>(contour[previous].X()) * contour[i].Y() -
					static_cast<double>(contour[i].X()) * contour[previous].Y();
			for (auto point : contour) ring.emplace_back(point.X(), point.Y());
			ring.push_back(ring.front());
			bg::correct(polygon);
			if (!bg::is_valid(polygon)) continue;
			MaskMultiPolygon combined;
			// Positive is solid and negative is a hole, which is the order the contours
			// were written in, so replaying them in that order reproduces the region.
			if (signed_area >= 0) bg::union_(region, polygon, combined);
			else bg::difference(region, polygon, combined);
			region = std::move(combined);
		}
		return region;
	}

	/// Turn a merged region back into the contour lists the mask works in. ASS reads them
	/// with the non-zero winding rule, where an outer ring has to wind one way and a hole
	/// the other, so the ring order Boost hands back is corrected rather than copied.
	std::vector<std::vector<Vector2D>> MaskContoursFrom(MaskMultiPolygon const& region) {
		std::vector<std::vector<Vector2D>> contours;
		auto append = [&](auto const& ring, bool solid) {
			if (ring.size() < 4) return;
			size_t count = ring.size() - 1; // Boost repeats the first point at the end.
			double signed_area = 0;
			for (size_t i = 0, previous = count - 1; i < count; previous = i++)
				signed_area += ring[previous].x() * ring[i].y() -
					ring[i].x() * ring[previous].y();
			bool reverse = solid ? signed_area < 0 : signed_area > 0;
			std::vector<Vector2D> contour;
			contour.reserve(count);
			for (size_t i = 0; i < count; ++i) {
				auto const& point = ring[reverse ? count - 1 - i : i];
				contour.emplace_back(static_cast<float>(point.x()),
					static_cast<float>(point.y()));
			}
			contours.push_back(std::move(contour));
		};
		for (auto const& polygon : region) {
			append(polygon.outer(), true);
			for (auto const& inner : polygon.inners()) append(inner, false);
		}
		return contours;
	}
	/// How far a fitted brush outline may sit from the polygon it replaces. The mask's
	/// regions are in script coordinates and it writes them with one decimal, so this is
	/// finer than anything the drawing can express anyway.
	constexpr double mask_brush_fit_tolerance = .3;

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

}

VisualToolMask::VisualToolMask(VideoDisplay *parent, agi::Context *context)
: VisualTool<VisualDraggableFeature>(parent, context)
, gl_text(std::make_unique<OpenGLText>())
, featureSize(OPT_GET("Tool/Visual/Shape Handle Size")->GetInt())
{
	preview_interface.AttachHost(parent->GetPreviewBar(), [this](int id) {
		this->parent->SetFocus();
		PerformPreviewAction(static_cast<VisualToolMaskAction>(id));
	}, [this](int id, double value, bool final) {
		UpdateExternalSlider(static_cast<VisualToolMaskAction>(id), value, final);
	});
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
	AddTool("video/tool/mask/bezier", MASK_BEZIER);
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
	// Curve and line work on the same points, and the two stamping tools on the same merged
	// region, so switching within a pair only changes how the work is drawn - throwing it
	// away would defeat the point of being able to switch at all.
	bool same_pair = IsPointMode() && (next_mode == MASK_POINTS || next_mode == MASK_BEZIER);
	if (next_mode == MASK_COLOR) {
		ClearPreview();
	}
	else if (!same_pair) {
		points.clear();
		point_curved.clear();
		edge_control_out.clear();
		edge_control_in.clear();
		mask_regions.clear();
		drawing = false;
		mask_brush_drawing = false;
		if (mode == MASK_COLOR) ResetColorSelection();
	}
	mode = next_mode;
	drawing = false;
	if (!same_pair) {
		mask_undo_history.clear();
		mask_redo_history.clear();
	}
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
	// The colour selection is held in script coordinates, so panning and zooming
	// change only where it is drawn; there is nothing to recompute here.
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
	// Curved edges are flattened on the way in. A finished region is a plain outline here -
	// everything downstream, from the boolean merge to the rasteriser, expects one - so the
	// curve is kept as the shape it describes rather than as its recipe. The area is
	// measured on the flattened outline too: two points bowed apart enclose something the
	// straight polygon between them does not.
	bool curved = std::find(point_curved.begin(), point_curved.end(), 1) != point_curved.end();
	if (curved && points.size() >= 2) {
		auto outline = FlattenCurves(CurveThroughPoints(points, point_curved,
			edge_control_out, edge_control_in));
		if (outline.size() >= 3 && std::abs(polygon_area(outline)) > .5)
			mask_regions.push_back(std::move(outline));
	}
	else if (points.size() >= 3 && std::abs(polygon_area(points)) > .5)
		mask_regions.push_back(std::move(points));
	points.clear();
	point_curved.clear();
	edge_control_out.clear();
	edge_control_in.clear();
}

struct VisualToolMask::BrushShape {
	MaskMultiPolygon region;
};

void VisualToolMask::PaintMaskBrush(Vector2D from, Vector2D to) {
	if (!mask_brush_shape) mask_brush_shape = std::make_unique<BrushShape>();
	float distance = (to - from).Len();
	float step = std::max(1.f, mask_brush_radius * .3f);
	int steps = distance > 0.f ? std::max(1, static_cast<int>(std::ceil(distance / step))) : 0;
	float script_radius = mask_brush_radius * .5f *
		(script_res.X() / std::max(1.f, video_size.X()) +
		 script_res.Y() / std::max(1.f, video_size.Y()));
	// The circles are merged as they are stamped rather than piled on top of one another.
	// Left separate they reach the file as one contour each - a stroke was hundreds of
	// them - and the preview has to carry a winding count that deep through a stencil
	// buffer which wraps around long before that, which is what broke it up into rings.
	// One merged outline is also what the vector clip's brush produces, for the same
	// reasons, and it is what makes the curve fitting worth anything: a fit can follow the
	// edge of a stroke, but it can do nothing with a heap of overlapping circles.
	MaskMultiPolygon& shape = mask_brush_shape->region;
	for (int i = 0; i <= steps; ++i) {
		float progress = steps ? static_cast<float>(i) / steps : 0.f;
		Vector2D centre = ToScriptCoords(from + (to - from) * progress);
		// Each call starts where the last one ended, so the circle at progress zero
		// repeats one already stamped. Merging makes that harmless, but not free.
		if ((centre - mask_brush_last_centre).Len() < 1e-3f) continue;
		mask_brush_last_centre = centre;
		auto circle = MakeMaskCircle(centre, script_radius);
		if (shape.empty()) {
			shape.push_back(std::move(circle));
			continue;
		}
		MaskMultiPolygon merged;
		bg::union_(shape, circle, merged);
		shape = std::move(merged);
	}
	MaskMultiPolygon simplified;
	bg::simplify(shape, simplified, mask_brush_simplify);
	bg::correct(simplified);
	if (!simplified.empty() && bg::is_valid(simplified)) shape = std::move(simplified);

	// In brush mode every region is brush output, so the merged shape is all of it.
	mask_regions = MaskContoursFrom(shape);
}

void VisualToolMask::UpdateMaskBrushSize(Vector2D point) {
	auto [top_left, bottom_right] = MaskBrushBounds();
	float left = top_left.X() + SliderLabelWidth(_("Size"));
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
	if (offset_dragging || (mode == MASK_COLOR && inside(OffsetBounds()))) {
		parent->SetToolTip(agi::wxformat(_("Offset: %d px"), color_offset));
		return;
	}
	bool smoothing = mode == MASK_COLOR && color_smooth_edges;
	if (smooth_tolerance_dragging || (smoothing && inside(SmoothToleranceBounds()))) {
		parent->SetToolTip(agi::wxformat(_("Smooth tolerance: %.2f"), color_smooth_tolerance));
		return;
	}
	if (smooth_angle_dragging || (smoothing && inside(SmoothAngleBounds()))) {
		parent->SetToolTip(agi::wxformat(_("Angle threshold: %.1f deg"), color_smooth_angle));
		return;
	}
	if (edge_snap_radius_dragging ||
		(smoothing && color_edge_snap && inside(EdgeSnapRadiusBounds()))) {
		parent->SetToolTip(agi::wxformat(_("Edge search: %d px"),
			static_cast<int>(std::lround(color_edge_snap_radius))));
		return;
	}
	if (smoothing && inside(EdgeSnapBounds())) {
		parent->SetToolTip(_("Pull the outline onto the picture edge it traces, keeping the offset even along its length"));
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
			parent->SetToolTip(_("Accept"));
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

float VisualToolMask::SliderLabelWidth(wxString const& label) const {
	// Measured rather than hardcoded: a translated label is a different width, and
	// a guessed padding lets it run into the track. Cached because the layout is
	// recomputed for every button on every repaint, and the font never changes.
	return 10.f + MeasuredTextWidth(label, false) + 10.f;
}

float VisualToolMask::MeasuredTextWidth(wxString const& label, bool bold) const {
	std::string key = (bold ? "b:" : "r:") + from_wx(label);
	auto found = text_width_cache.find(key);
	if (found != text_width_cache.end()) return found->second;
	gl_text->SetFont("Verdana", 9, bold, false);
	int text_width, text_height;
	gl_text->GetExtent(from_wx(label), text_width, text_height);
	float width = static_cast<float>(text_width);
	text_width_cache.emplace(key, width);
	return width;
}

// The colour mode's bar is laid out exactly like the vector clip's: offset on the
// left, then the selection mode button with its own slider right behind it, then
// the button run. Keeping the two identical is the point - it is the same tool.
std::pair<Vector2D, Vector2D> VisualToolMask::ToleranceBounds() const {
	auto mode_bounds = ActionBounds(VisualToolMaskAction::SelectionMode);
	float left = mode_bounds.second.X() + 8.f;
	float top = mode_bounds.first.Y();
	float width = SliderLabelWidth(_("Tolerance")) + 82.f;
	if (left + width > canvas_size.X() - 8.f) {
		left = 12.f;
		top = mode_bounds.second.Y() + 8.f;
	}
	return {Vector2D(left, top), Vector2D(left + width, top + 34.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::OffsetBounds() const {
	float left = mode == MASK_COLOR && color_stage == ColorStage::Range &&
		!ai_refining ? 146.f : 96.f;
	return {Vector2D(left, 10.f),
		Vector2D(left + SliderLabelWidth(_("Offset")) + 82.f, 44.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::BrushBounds() const {
	auto mode_bounds = ActionBounds(VisualToolMaskAction::SelectionMode);
	float left = mode_bounds.second.X() + 8.f;
	float top = mode_bounds.first.Y();
	float width = SliderLabelWidth(_("Size")) + 102.f;
	if (left + width > canvas_size.X() - 8.f) {
		left = 12.f;
		top = mode_bounds.second.Y() + 8.f;
	}
	return {Vector2D(left, top), Vector2D(left + width, top + 34.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::MaskBrushBounds() const {
	return {Vector2D(96.f, 10.f),
		Vector2D(96.f + SliderLabelWidth(_("Size")) + 102.f, 44.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::SmoothToleranceBounds() const {
	float top = ActionBounds(VisualToolMaskAction::Clear).second.Y() + 8.f;
	return {Vector2D(12.f, top),
		Vector2D(12.f + SliderLabelWidth(_("Smooth tolerance")) + 102.f, top + 34.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::SmoothAngleBounds() const {
	auto tolerance = SmoothToleranceBounds();
	float left = tolerance.second.X() + 8.f;
	float top = tolerance.first.Y();
	float width = SliderLabelWidth(_("Angle threshold")) + 102.f;
	if (left + width > canvas_size.X() - 8.f) {
		left = 12.f;
		top = tolerance.second.Y() + 8.f;
	}
	return {Vector2D(left, top), Vector2D(left + width, top + 34.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::EdgeSnapBounds() const {
	auto angle = SmoothAngleBounds();
	float left = angle.second.X() + 8.f;
	float top = angle.first.Y();
	float width = MeasuredTextWidth(_("Auto snap"), true) + 24.f;
	if (left + width > canvas_size.X() - 8.f) {
		left = 12.f;
		top = angle.second.Y() + 8.f;
	}
	return {Vector2D(left, top), Vector2D(left + width, top + 34.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::EdgeSnapRadiusBounds() const {
	auto snap = EdgeSnapBounds();
	float left = snap.second.X() + 8.f;
	float top = snap.first.Y();
	float width = SliderLabelWidth(_("Edge search")) + 102.f;
	if (left + width > canvas_size.X() - 8.f) {
		left = 12.f;
		top = snap.second.Y() + 8.f;
	}
	return {Vector2D(left, top), Vector2D(left + width, top + 34.f)};
}

std::pair<Vector2D, Vector2D> VisualToolMask::ActionBounds(VisualToolMaskAction action) const {
	// While the range is being marked out, how to mark it is the first thing on the
	// bar; the history buttons step aside for it.
	bool ranging = mode == MASK_COLOR && color_stage == ColorStage::Range && !ai_refining;
	float history_left = ranging ? 62.f : 12.f;
	if (action == VisualToolMaskAction::RangeShape)
		return {Vector2D(12.f, 10.f), Vector2D(54.f, 44.f)};
	if (action == VisualToolMaskAction::Undo)
		return {Vector2D(history_left, 10.f), Vector2D(history_left + 34.f, 44.f)};
	if (action == VisualToolMaskAction::Redo)
		return {Vector2D(history_left + 42.f, 10.f), Vector2D(history_left + 76.f, 44.f)};
	// Sits in the smoothing row rather than in the button run, so it is left out
	// of the layout walk below.
	if (action == VisualToolMaskAction::EdgeSnap) return EdgeSnapBounds();
	float top = 10.f;
	constexpr float height = 34.f;
	constexpr float gap = 8.f;
	bool pipette_mode = color_selection_mode == VisualSelectionMode::PipetteAdd ||
		color_selection_mode == VisualSelectionMode::PipetteSubtract;
	bool brush_mode = !pipette_mode;
	// The offset slider holds its place even while it cannot be used, so the button
	// run does not shift about as the selection progresses through its stages.
	float left = mode == MASK_BRUSH ? MaskBrushBounds().second.X() + gap :
		mode == MASK_COLOR ? OffsetBounds().second.X() + gap :
		ActionBounds(VisualToolMaskAction::Redo).second.X() + gap;
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
			// The same two words every other top bar uses, whatever the button goes on to
			// do underneath. Enter and Escape still work; naming them in the label made
			// these the only buttons in the application that did.
			case VisualToolMaskAction::Create: return _("Accept");
			case VisualToolMaskAction::Clear: return _("Cancel");
			case VisualToolMaskAction::RemoveText:
				return ai_refining ? _("Erase again") : _("AI text removal");
			case VisualToolMaskAction::GenerateText: return _("AI custom generation");
			case VisualToolMaskAction::AutoFill: return _("Auto fill");
			case VisualToolMaskAction::SelectionMode: return mode_label();
			case VisualToolMaskAction::AISelect: return _("AI recognition");
			case VisualToolMaskAction::Templates: return _("Templates");
			case VisualToolMaskAction::SmoothEdges: return _("Smooth edges");
			case VisualToolMaskAction::EdgeSnap: return _("Auto snap");
			default: return wxString();
		}
	};
	auto width_for = [&](VisualToolMaskAction item) {
		return MeasuredTextWidth(label_for(item), true) +
			(item == VisualToolMaskAction::SelectionMode ||
			 item == VisualToolMaskAction::Templates ? 32.f : 24.f);
	};
	std::vector<VisualToolMaskAction> actions;
	if (ai_refining)
		actions = {VisualToolMaskAction::RemoveText, VisualToolMaskAction::GenerateText,
			VisualToolMaskAction::Create, VisualToolMaskAction::Clear};
	else if (mode == MASK_COLOR) {
		actions = {VisualToolMaskAction::SelectionMode};
		actions.push_back(VisualToolMaskAction::Templates);
		actions.push_back(VisualToolMaskAction::AISelect);
		if (has_color_sample)
			actions.push_back(VisualToolMaskAction::AutoFill);
		actions.push_back(VisualToolMaskAction::SmoothEdges);
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
		// The slider belonging to the selection mode sits directly behind its
		// button, so the rest of the run starts after it.
		if (item == VisualToolMaskAction::SelectionMode && mode == MASK_COLOR) {
			if (pipette_mode && has_color_sample) {
				auto bounds = ToleranceBounds();
				left = bounds.second.X() + gap;
				top = bounds.first.Y();
			}
			else if (brush_mode) {
				auto bounds = BrushBounds();
				left = bounds.second.X() + gap;
				top = bounds.first.Y();
			}
		}
	}
	return {Vector2D(left, top), Vector2D(left, top + height)};
}

float VisualToolMask::TopBarHeight() const {
	if (preview_interface.HasExternalHost()) return 0.f;
	if (ai_refining) return ActionBounds(VisualToolMaskAction::Clear).second.Y() + 10.f;
	if (mode == MASK_COLOR && color_smooth_edges)
		return (color_edge_snap ? EdgeSnapRadiusBounds() : EdgeSnapBounds()).second.Y() + 10.f;
	auto last = mode == MASK_COLOR ? VisualToolMaskAction::Clear :
		VisualToolMaskAction::GenerateText;
	return ActionBounds(last).second.Y() + 10.f;
}

VisualToolMaskAction VisualToolMask::ActionAt(Vector2D point) {
	if (preview_interface.HasExternalHost()) return VisualToolMaskAction::None;
	auto hits = [&](VisualToolMaskAction action) {
		auto [top_left, bottom_right] = ActionBounds(action);
		return point.X() >= top_left.X() && point.X() <= bottom_right.X() &&
			point.Y() >= top_left.Y() && point.Y() <= bottom_right.Y();
	};
	if (mode == MASK_COLOR && !ai_refining && color_stage == ColorStage::Range &&
		hits(VisualToolMaskAction::RangeShape))
		return VisualToolMaskAction::RangeShape;
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
		actions.push_back(VisualToolMaskAction::Templates);
		actions.push_back(VisualToolMaskAction::AISelect);
		if (has_color_sample)
			actions.push_back(VisualToolMaskAction::AutoFill);
		actions.push_back(VisualToolMaskAction::SmoothEdges);
		if (color_smooth_edges) actions.push_back(VisualToolMaskAction::EdgeSnap);
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
		if (action == VisualToolMaskAction::Templates && color_stage == ColorStage::Range) continue;
		if ((action == VisualToolMaskAction::SmoothEdges ||
			action == VisualToolMaskAction::EdgeSnap) &&
			(color_stage == ColorStage::Range || color_contours.empty())) continue;
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
	color_range_path.clear();
	color_contours.clear();
	color_segmenter.Clear();
	color_contours_dirty = false;
	has_color_sample = false;
	color_frame_width = color_frame_height = 0;
	color_offset = 0;
	color_auto_fill = false;
	color_smooth_edges = false;
	color_smooth_tolerance = 10.0;
	color_smooth_angle = 35.0;
	color_edge_snap = false;
	color_edge_snap_radius = 6.0;
	color_display_dirty = true;
	color_sample_operations.clear();
	color_ai_base = false;
	drawing = false;
	tolerance_dragging = false;
	offset_dragging = false;
	smooth_tolerance_dragging = false;
	smooth_angle_dragging = false;
	edge_snap_radius_dragging = false;
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
	ColorHistoryState state;
	state.segmenter = color_segmenter;
	state.sample = color_sample;
	state.stage = color_stage;
	state.tolerance = color_tolerance;
	state.offset = color_offset;
	state.has_sample = has_color_sample;
	state.auto_fill = color_auto_fill;
	state.smooth_edges = color_smooth_edges;
	state.smooth_tolerance = color_smooth_tolerance;
	state.smooth_angle = color_smooth_angle;
	state.edge_snap = color_edge_snap;
	state.edge_snap_radius = color_edge_snap_radius;
	state.sample_operations = color_sample_operations;
	state.ai_base = color_ai_base;
	return state;
}

VisualToolMask::ColorHistoryState VisualToolMask::CaptureColorBrushHistory() const {
	ColorHistoryState state;
	state.sample = color_sample;
	state.stage = color_stage;
	state.tolerance = color_tolerance;
	state.offset = color_offset;
	state.has_sample = has_color_sample;
	state.auto_fill = color_auto_fill;
	state.smooth_edges = color_smooth_edges;
	state.smooth_tolerance = color_smooth_tolerance;
	state.smooth_angle = color_smooth_angle;
	state.edge_snap = color_edge_snap;
	state.edge_snap_radius = color_edge_snap_radius;
	state.sample_operations = color_sample_operations;
	state.ai_base = color_ai_base;
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
	color_smooth_edges = state.smooth_edges;
	color_smooth_tolerance = state.smooth_tolerance;
	color_smooth_angle = state.smooth_angle;
	color_edge_snap = state.edge_snap;
	color_edge_snap_radius = state.edge_snap_radius;
	color_sample_operations = std::move(state.sample_operations);
	color_ai_base = state.ai_base;
	color_display_dirty = true;
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
	point_curved.clear();
	edge_control_out.clear();
	edge_control_in.clear();
	mask_regions.clear();
	mask_undo_history.clear();
	mask_redo_history.clear();
	mask_brush_drawing = false;
	mask_brush_slider_dragging = false;
	ResetColorSelection();
	dragged_point = hovered_point = -1;
	tolerance_dragging = false;
	external_slider_action = VisualToolMaskAction::None;
	if (parent->HasCapture()) parent->ReleaseMouse();
	UpdateActionTooltip(VisualToolMaskAction::None);
	parent->Render();
}

void VisualToolMask::PerformPreviewAction(VisualToolMaskAction action) {
	if (action == VisualToolMaskAction::RangeShape) ShowRangeShapeMenu();
	else if (action == VisualToolMaskAction::Undo) {
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
	else if (action == VisualToolMaskAction::Templates) ShowColorTemplatesMenu();
	else if (action == VisualToolMaskAction::SmoothEdges) {
		PushColorHistory();
		color_smooth_edges = !color_smooth_edges;
		color_display_dirty = true;
		if (!color_smooth_edges && color_edge_snap) {
			color_edge_snap = false;
			if (color_contours_dirty) SyncColorSegmenterFromContours();
			RefreshColorContours();
		}
	}
	else if (action == VisualToolMaskAction::EdgeSnap) {
		if (color_contours_dirty) SyncColorSegmenterFromContours();
		PushColorHistory();
		color_edge_snap = !color_edge_snap;
		RefreshColorContours();
	}
	else if (action == VisualToolMaskAction::RemoveText ||
		action == VisualToolMaskAction::GenerateText)
		CreateAIMask(action);
	parent->Render();
}

void VisualToolMask::UpdateExternalSlider(VisualToolMaskAction action,
	double value, bool final) {
	if (external_slider_action != action) {
		external_slider_action = action;
		if (action != VisualToolMaskAction::MaskBrushSize &&
			action != VisualToolMaskAction::ColorBrushSize)
			PushColorHistory();
	}
	if (action == VisualToolMaskAction::MaskBrushSize)
		mask_brush_radius = static_cast<float>(std::lround(value));
	else if (action == VisualToolMaskAction::ColorBrushSize)
		color_brush_radius = static_cast<float>(std::lround(value));
	else if (action == VisualToolMaskAction::Tolerance) {
		if (color_contours_dirty) SyncColorSegmenterFromContours();
		color_tolerance = value;
		RefreshColorContours();
	}
	else if (action == VisualToolMaskAction::Offset) {
		if (color_contours_dirty) SyncColorSegmenterFromContours();
		color_offset = static_cast<int>(std::lround(value));
		RefreshColorContours();
	}
	else if (action == VisualToolMaskAction::SmoothTolerance) {
		color_smooth_tolerance = value;
		color_display_dirty = true;
	}
	else if (action == VisualToolMaskAction::SmoothAngle) {
		color_smooth_angle = value;
		color_display_dirty = true;
	}
	else if (action == VisualToolMaskAction::EdgeSnapRadius) {
		color_edge_snap_radius = value;
		if (color_contours_dirty) SyncColorSegmenterFromContours();
		RefreshColorContours();
	}
	if (final) external_slider_action = VisualToolMaskAction::None;
	parent->Render();
}

void VisualToolMask::UpdatePreviewInterface() {
	using Interface = VisualToolPreviewInterface;
	Interface::Page page;
	auto add = [&](VisualToolMaskAction action, Interface::ControlKind kind,
		wxString label, bool enabled = true,
		Interface::ControlStyle style = Interface::ControlStyle::Neutral) -> Interface::Control& {
		Interface::Control control;
		control.id = static_cast<int>(action);
		control.kind = kind;
		control.label = std::move(label);
		control.enabled = enabled;
		control.style = style;
		page.controls.push_back(std::move(control));
		return page.controls.back();
	};
	auto add_slider = [&](VisualToolMaskAction action, wxString label, double value,
		double minimum, double maximum, double step, wxString value_text, bool enabled = true) {
		auto& control = add(action, Interface::ControlKind::Slider, std::move(label), enabled);
		control.value = value;
		control.minimum = minimum;
		control.maximum = maximum;
		control.step = step;
		control.value_text = std::move(value_text);
		control.width = 150;
	};
	bool color_mode = mode == MASK_COLOR && !ai_refining;
	bool color_history = color_mode;
	if (color_mode && color_stage == ColorStage::Range) {
		auto& range = add(VisualToolMaskAction::RangeShape, Interface::ControlKind::Button,
			color_range_shape == ColorRangeShape::Rectangle ? _("Rectangle") : _("Freehand"));
		range.dropdown = true;
		range.icon_only = true;
		range.bitmap = MakeVisualRangeShapeBitmap(
			color_range_shape == ColorRangeShape::Freehand,
			std::max(16, static_cast<int>(OPT_GET("App/Toolbar Icon Size")->GetInt())), true);
	}
	add(VisualToolMaskAction::Undo, Interface::ControlKind::Undo, wxString(),
		color_history ? !color_undo_history.empty() : !mask_undo_history.empty());
	add(VisualToolMaskAction::Redo, Interface::ControlKind::Redo, wxString(),
		color_history ? !color_redo_history.empty() : !mask_redo_history.empty());
	if (mode == MASK_BRUSH && !ai_refining)
		add_slider(VisualToolMaskAction::MaskBrushSize, _("Size"), mask_brush_radius,
			2, 200, 1, agi::wxformat("%d", static_cast<int>(std::lround(mask_brush_radius))));
	if (color_mode) {
		add_slider(VisualToolMaskAction::Offset, _("Offset"), color_offset, -25, 25, 1,
			agi::wxformat("%d", color_offset), CanOffsetSelection());
		if (color_stage != ColorStage::Range) {
			wxString selection = color_selection_mode == VisualSelectionMode::PipetteAdd ? _("Pipette add") :
				color_selection_mode == VisualSelectionMode::PipetteSubtract ? _("Pipette subtract") :
				color_selection_mode == VisualSelectionMode::BrushAdd ? _("Brush add") : _("Brush subtract");
			auto& selection_control = add(VisualToolMaskAction::SelectionMode,
				Interface::ControlKind::Button, selection);
			selection_control.dropdown = true;
			bool pipette = color_selection_mode == VisualSelectionMode::PipetteAdd ||
				color_selection_mode == VisualSelectionMode::PipetteSubtract;
			if (pipette && has_color_sample)
				add_slider(VisualToolMaskAction::Tolerance, _("Tolerance"), color_tolerance,
					0, 20, .1, agi::wxformat("%.1f", color_tolerance));
			else if (!pipette)
				add_slider(VisualToolMaskAction::ColorBrushSize, _("Size"), color_brush_radius,
					2, 200, 1, agi::wxformat("%d", static_cast<int>(std::lround(color_brush_radius))));
			auto& templates = add(VisualToolMaskAction::Templates,
				Interface::ControlKind::Button, _("Templates"));
			templates.dropdown = true;
			add(VisualToolMaskAction::AISelect, Interface::ControlKind::Button,
				_("AI recognition"), true, Interface::ControlStyle::Warning);
			if (has_color_sample) {
				auto& fill = add(VisualToolMaskAction::AutoFill, Interface::ControlKind::Button,
					_("Auto fill"));
				fill.selected = color_auto_fill;
			}
			auto& smooth = add(VisualToolMaskAction::SmoothEdges, Interface::ControlKind::Button,
				_("Smooth edges"), !color_contours.empty());
			smooth.selected = color_smooth_edges;
			if (color_smooth_edges) {
				add_slider(VisualToolMaskAction::SmoothTolerance, _("Smooth tolerance"),
					color_smooth_tolerance, .1, 50, .1,
					agi::wxformat("%.1f", color_smooth_tolerance));
				add_slider(VisualToolMaskAction::SmoothAngle, _("Angle threshold"),
					color_smooth_angle, 0, 180, .1, agi::wxformat("%.1f", color_smooth_angle));
				auto& snap = add(VisualToolMaskAction::EdgeSnap, Interface::ControlKind::Button,
					_("Auto snap"), !color_contours.empty());
				snap.selected = color_edge_snap;
				if (color_edge_snap)
					add_slider(VisualToolMaskAction::EdgeSnapRadius, _("Edge search"),
						color_edge_snap_radius, 2, 50, 1,
						agi::wxformat("%d", static_cast<int>(std::lround(color_edge_snap_radius))));
			}
		}
	}
	bool pending = !mask_regions.empty() ||
		(points.size() >= 3 && std::abs(polygon_area(points)) > .5);
	if (ai_refining) {
		add(VisualToolMaskAction::RemoveText, Interface::ControlKind::Button, _("Erase again"),
			pending, Interface::ControlStyle::Warning);
		add(VisualToolMaskAction::GenerateText, Interface::ControlKind::Button,
			_("AI custom generation"), pending && !ai::GetApiKey().empty(),
			Interface::ControlStyle::Accent);
	}
	add(VisualToolMaskAction::Create, Interface::ControlKind::Button,
		_("Accept"), CanCreateMask(),
		Interface::ControlStyle::Accept);
	add(VisualToolMaskAction::Clear, Interface::ControlKind::Button, _("Cancel"),
		CanCancel(), Interface::ControlStyle::Cancel);
	if (!ai_refining && mode != MASK_COLOR) {
		add(VisualToolMaskAction::RemoveText, Interface::ControlKind::Button,
			_("AI text removal"), CanCreateMask(), Interface::ControlStyle::Warning);
		add(VisualToolMaskAction::GenerateText, Interface::ControlKind::Button,
			_("AI custom generation"), CanCreateMask() && !ai::GetApiKey().empty(),
			Interface::ControlStyle::Accent);
	}
	page.message = ai_refining ? _("You can refine the result again if you want.") :
		color_mode && color_stage == ColorStage::Range ?
			(color_range_shape == ColorRangeShape::Rectangle ? _("Draw the search range.") :
			_("Draw round the search range.")) :
		color_mode && color_stage == ColorStage::Sample ? _("Click the color to extract inside the range.") :
		color_mode ? agi::wxformat(_("%zu contours found."), color_contours.size()) : wxString();
	preview_interface.SetPage(std::move(page));
}

VisualToolMask::MaskHistoryState VisualToolMask::CaptureMaskHistory() const {
	return {points, point_curved, edge_control_out, edge_control_in, mask_regions};
}

void VisualToolMask::RestoreMaskHistory(MaskHistoryState state) {
	points = std::move(state.points);
	point_curved = std::move(state.point_curved);
	edge_control_out = std::move(state.edge_control_out);
	edge_control_in = std::move(state.edge_control_in);
	dragged_control = hovered_control = -1;
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
	if (ai_refining) preview_interface.PopPage();
	ai_refining = false;
	ai_working_image.reset();
	ai_selection_mask.clear();
	ai_prompt_history.clear();
	ai_session_line = nullptr;
	ai_crop_x = ai_crop_y = ai_crop_width = ai_crop_height = 0;
	if (parent->HasCapture()) parent->ReleaseMouse();
	parent->SetCursor(wxCursor(wxCURSOR_ARROW));
}

void VisualToolMask::UpdateSmoothTolerance(Vector2D point) {
	auto [top_left, bottom_right] = SmoothToleranceBounds();
	float left = top_left.X() + SliderLabelWidth(_("Smooth tolerance"));
	float right = bottom_right.X() - 12.f;
	double ratio = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f);
	color_smooth_tolerance = std::round((.1 + ratio * 49.9) * 100.0) / 100.0;
	color_display_dirty = true;
}

void VisualToolMask::UpdateSmoothAngle(Vector2D point) {
	auto [top_left, bottom_right] = SmoothAngleBounds();
	float left = top_left.X() + SliderLabelWidth(_("Angle threshold"));
	float right = bottom_right.X() - 12.f;
	double ratio = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f);
	color_smooth_angle = std::round(ratio * 1800.0) / 10.0;
	color_display_dirty = true;
}

void VisualToolMask::UpdateEdgeSnapRadius(Vector2D point) {
	auto [top_left, bottom_right] = EdgeSnapRadiusBounds();
	float left = top_left.X() + SliderLabelWidth(_("Edge search"));
	float right = bottom_right.X() - 12.f;
	double ratio = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f);
	double radius = std::round(2.0 + ratio * 48.0);
	if (radius == color_edge_snap_radius) return;
	color_edge_snap_radius = radius;
	if (color_contours_dirty) SyncColorSegmenterFromContours();
	RefreshColorContours();
}

void VisualToolMask::UpdateColorTolerance(Vector2D point) {
	if (color_contours_dirty) SyncColorSegmenterFromContours();
	auto [top_left, bottom_right] = ToleranceBounds();
	float left = top_left.X() + SliderLabelWidth(_("Tolerance"));
	float right = bottom_right.X() - 12.f;
	double value = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f) * 20.0;
	color_tolerance = std::round(value * 10.0) / 10.0;
	RefreshColorContours();
}

void VisualToolMask::UpdateColorOffset(Vector2D point) {
	if (color_contours_dirty) SyncColorSegmenterFromContours();
	auto [top_left, bottom_right] = OffsetBounds();
	float left = top_left.X() + SliderLabelWidth(_("Offset"));
	float right = bottom_right.X() - 12.f;
	double ratio = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f);
	color_offset = static_cast<int>(std::lround(ratio * 50.0 - 25.0));
	RefreshColorContours();
}

void VisualToolMask::UpdateColorBrushSize(Vector2D point) {
	auto [top_left, bottom_right] = BrushBounds();
	float left = top_left.X() + SliderLabelWidth(_("Size"));
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
		if (!color_range_path.empty()) {
			std::vector<Vector2D> lasso;
			lasso.reserve(color_range_path.size());
			for (auto point : color_range_path) lasso.push_back(to_frame(point));
			color_segmenter.SetRangeMask(lasso);
		}
		color_frame_width = frame->width;
		color_frame_height = frame->height;
		return true;
	}
	catch (...) { return false; }
}

void VisualToolMask::ShowRangeShapeMenu() {
	constexpr int rectangle_id = 17321;
	constexpr int freehand_id = 17322;
	int icon_size = OPT_GET("App/Toolbar Icon Size")->GetInt();
	wxMenu menu;
	// Plain items rather than check items: a checked item is drawn with a tick in
	// place of its bitmap, and both icons should stay visible. Which one is active
	// is already shown by the button itself.
	auto rectangle_item = menu.Append(rectangle_id, _("Rectangle range"));
	rectangle_item->SetBitmap(MakeVisualRangeShapeBitmap(false, icon_size));
	auto freehand_item = menu.Append(freehand_id, _("Freehand range"));
	freehand_item->SetBitmap(MakeVisualRangeShapeBitmap(true, icon_size));
	wxPoint menu_position = parent->ScreenToClient(wxGetMousePosition());
	int selected = parent->GetPopupMenuSelectionFromUser(menu, menu_position);
	if (selected == rectangle_id) color_range_shape = ColorRangeShape::Rectangle;
	else if (selected == freehand_id) color_range_shape = ColorRangeShape::Freehand;
	else return;
	color_range_path.clear();
	color_range_start = color_range_end = Vector2D();
	drawing = false;
	parent->Render();
}

std::vector<Vector2D> VisualToolMask::ColorRangeBoundary() const {
	if (!color_range_path.empty()) return color_range_path;
	Vector2D top_left = color_range_start.Min(color_range_end);
	Vector2D bottom_right = color_range_start.Max(color_range_end);
	if (!(bottom_right.X() > top_left.X()) || !(bottom_right.Y() > top_left.Y())) return {};
	return {top_left, Vector2D(bottom_right.X(), top_left.Y()), bottom_right,
		Vector2D(top_left.X(), bottom_right.Y())};
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
	wxPoint menu_position = parent->ScreenToClient(wxGetMousePosition());
	int selected = parent->GetPopupMenuSelectionFromUser(menu, menu_position);
	if (selected == pipette_add_id) color_selection_mode = VisualSelectionMode::PipetteAdd;
	else if (selected == pipette_subtract_id) color_selection_mode = VisualSelectionMode::PipetteSubtract;
	else if (selected == brush_add_id) color_selection_mode = VisualSelectionMode::BrushAdd;
	else if (selected == brush_subtract_id) color_selection_mode = VisualSelectionMode::BrushSubtract;
	UpdateColorCursor();
	parent->Render();
}

bool VisualToolMask::CanCaptureColorTemplate() const {
	return mode == MASK_COLOR && color_stage == ColorStage::Ready &&
		(color_ai_base || !color_sample_operations.empty());
}

VisualColorTemplate VisualToolMask::CaptureColorTemplate(std::string name) const {
	VisualColorTemplate color_template;
	color_template.name = std::move(name);
	color_template.sample_operations = color_sample_operations;
	color_template.ai_base = color_ai_base;
	color_template.tolerance = color_tolerance;
	color_template.offset = color_offset;
	color_template.auto_fill = color_auto_fill;
	color_template.smooth_edges = color_smooth_edges;
	color_template.smooth_tolerance = color_smooth_tolerance;
	color_template.smooth_angle = color_smooth_angle;
	color_template.edge_snap = color_edge_snap;
	color_template.edge_snap_radius = color_edge_snap_radius;
	color_template.selection_mode = color_selection_mode == VisualSelectionMode::PipetteSubtract ?
		VisualSelectionMode::PipetteSubtract : VisualSelectionMode::PipetteAdd;
	return color_template;
}

bool VisualToolMask::LoadColorTemplate(VisualColorTemplate const& color_template) {
	if (mode != MASK_COLOR || color_stage == ColorStage::Range) return false;
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

		VisualColorSegmenter segmenter;
		if (!segmenter.PrepareEmpty(*frame, left, top, right, bottom)) return false;
		if (color_template.ai_base) {
			wxImage crop = GetImage(*frame).GetSubImage(wxRect(left, top, right - left, bottom - top));
			auto contours = GenerateVisualAISelection(c->parent, crop);
			for (auto& contour : contours)
				for (auto& point : contour)
					point = point + Vector2D(static_cast<float>(left), static_cast<float>(top));
			segmenter.SetContours(contours, true);
		}
		for (auto const& operation : color_template.sample_operations)
			if (!segmenter.AddSample(*frame, operation.sample, operation.add)) return false;

		PushColorHistory();
		color_segmenter = std::move(segmenter);
		color_sample_operations = color_template.sample_operations;
		color_ai_base = color_template.ai_base;
		if (!color_sample_operations.empty())
			color_sample = color_sample_operations.back().sample;
		has_color_sample = !color_sample_operations.empty();
		color_tolerance = color_template.tolerance;
		color_offset = color_template.offset;
		color_auto_fill = color_template.auto_fill && has_color_sample;
		color_smooth_edges = color_template.smooth_edges;
		color_smooth_tolerance = color_template.smooth_tolerance;
		color_smooth_angle = color_template.smooth_angle;
		color_edge_snap = color_template.edge_snap;
		color_edge_snap_radius = color_template.edge_snap_radius;
		color_selection_mode = color_template.selection_mode;
		color_frame_width = frame->width;
		color_frame_height = frame->height;
		color_stage = ColorStage::Ready;
		color_contours_dirty = false;
		RefreshColorContours();
		UpdateColorCursor();
		parent->Render();
		return true;
	}
	catch (std::exception const& error) {
		wxMessageBox(to_wx(error.what()), _("Template loading failed"),
			wxOK | wxICON_ERROR, c->parent);
		return false;
	}
}

void VisualToolMask::ShowColorTemplatesMenu() {
	enum class TemplateMenuAction { Load, Update, Delete };
	struct TemplateMenuEntry {
		int id;
		TemplateMenuAction action;
		size_t index;
	};
	constexpr int add_id = 17601;
	int next_id = 17602;
	wxMenu menu;
	auto add_item = menu.Append(add_id, _("Add new template..."));
	add_item->Enable(CanCaptureColorTemplate());

	auto load_menu = new wxMenu;
	auto update_menu = new wxMenu;
	auto delete_menu = new wxMenu;
	std::vector<TemplateMenuEntry> entries;
	auto& templates = VisualColorTemplates();
	for (size_t i = 0; i < templates.size(); ++i) {
		wxString name = to_wx(templates[i].name);
		int load_id = next_id++;
		int update_id = next_id++;
		int delete_id = next_id++;
		load_menu->Append(load_id, name);
		auto update_entry = update_menu->Append(update_id, name);
		update_entry->Enable(CanCaptureColorTemplate());
		delete_menu->Append(delete_id, name);
		entries.push_back({load_id, TemplateMenuAction::Load, i});
		entries.push_back({update_id, TemplateMenuAction::Update, i});
		entries.push_back({delete_id, TemplateMenuAction::Delete, i});
	}
	auto load_item = menu.AppendSubMenu(load_menu, _("Load"));
	auto update_item = menu.AppendSubMenu(update_menu, _("Update"));
	auto delete_item = menu.AppendSubMenu(delete_menu, _("Delete"));
	load_item->Enable(!templates.empty() && color_stage != ColorStage::Range);
	update_item->Enable(!templates.empty() && CanCaptureColorTemplate());
	delete_item->Enable(!templates.empty());

	wxPoint menu_position = parent->ScreenToClient(wxGetMousePosition());
	int selected = parent->GetPopupMenuSelectionFromUser(menu, menu_position);
	if (selected == add_id) {
		wxString default_name = agi::wxformat(_("Template %zu"), templates.size() + 1);
		wxTextEntryDialog dialog(c->parent, _("Template name:"),
			_("Add template"), default_name);
		if (dialog.ShowModal() != wxID_OK) return;
		wxString name = dialog.GetValue();
		name.Trim(true).Trim(false);
		if (name.empty()) return;
		std::string utf8_name = from_wx(name);
		if (std::any_of(templates.begin(), templates.end(),
			[&](VisualColorTemplate const& item) { return item.name == utf8_name; })) {
			wxMessageBox(_("A template with this name already exists."),
				_("Add template"), wxOK | wxICON_INFORMATION, c->parent);
			return;
		}
		templates.insert(templates.begin(), CaptureColorTemplate(std::move(utf8_name)));
		parent->Render();
		return;
	}

	auto entry = std::find_if(entries.begin(), entries.end(),
		[&](TemplateMenuEntry const& item) { return item.id == selected; });
	if (entry == entries.end() || entry->index >= templates.size()) return;
	if (entry->action == TemplateMenuAction::Load) {
		LoadColorTemplate(templates[entry->index]);
		return;
	}
	if (entry->action == TemplateMenuAction::Update) {
		std::string name = templates[entry->index].name;
		templates.erase(templates.begin() + entry->index);
		templates.insert(templates.begin(), CaptureColorTemplate(std::move(name)));
		parent->Render();
		return;
	}
	wxString name = to_wx(templates[entry->index].name);
	if (wxMessageBox(agi::wxformat(_("Delete template '%s'?"), name),
		_("Delete template"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
		c->parent) == wxYES) {
		templates.erase(templates.begin() + entry->index);
		parent->Render();
	}
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
		color_sample_operations.clear();
		color_ai_base = true;
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
		color_sample_operations.push_back({color_sample,
			color_selection_mode == VisualSelectionMode::PipetteAdd});
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
	if (color_contours_dirty) SyncColorSegmenterFromContours();
	auto to_frame = [&](Vector2D screen) {
		Vector2D script = ToScriptCoords(screen);
		return Vector2D(script.X() * color_frame_width / script_res.X(),
			script.Y() * color_frame_height / script_res.Y());
	};
	float frame_scale_x = color_frame_width / video_size.X();
	float frame_scale_y = color_frame_height / video_size.Y();
	float frame_radius = color_brush_radius * (frame_scale_x + frame_scale_y) * .5f;
	bool add = color_selection_mode == VisualSelectionMode::BrushAdd;
	color_segmenter.PaintStroke(to_frame(from), to_frame(to), frame_radius, add);
	color_offset = 0;
	color_auto_fill = false;
	RefreshColorContours();
}

void VisualToolMask::SyncColorSegmenterFromContours() {
	if (color_segmenter.Empty() || color_frame_width <= 0 || color_frame_height <= 0) return;
	std::vector<std::vector<Vector2D>> frame_contours = color_contours;
	for (auto& contour : frame_contours) {
		for (auto& script : contour)
			script = Vector2D(script.X() * color_frame_width / script_res.X(),
				script.Y() * color_frame_height / script_res.Y());
	}
	color_segmenter.SetContours(frame_contours, true);
	color_contours_dirty = false;
}

std::vector<std::vector<Vector2D>> VisualToolMask::SnapColorContoursToEdges(
	std::vector<std::vector<Vector2D>> raw_contours) const {
	try {
		auto frame = c->videoController->GetFrame(frame_number, true);
		if (!frame || frame->width <= 0 || frame->height <= 0) return raw_contours;
		auto to_frame = [&](Vector2D script) {
			return Vector2D(script.X() * frame->width / script_res.X(),
				script.Y() * frame->height / script_res.Y());
		};
		Vector2D a = to_frame(color_range_start), b = to_frame(color_range_end);
		// Snap inside the marked range only: points landing on the crop border are
		// held in place and results are clamped to it, so the outline cannot be
		// pulled out of the range the user selected.
		int left = static_cast<int>(std::floor(std::min(a.X(), b.X())));
		int top = static_cast<int>(std::floor(std::min(a.Y(), b.Y())));
		int right = static_cast<int>(std::ceil(std::max(a.X(), b.X())));
		int bottom = static_cast<int>(std::ceil(std::max(a.Y(), b.Y())));
		return SnapContoursToImageEdges(*frame, std::move(raw_contours), left, top,
			right - left, bottom - top,
			static_cast<int>(std::lround(color_edge_snap_radius)));
	}
	catch (...) {
		return raw_contours;
	}
}

std::vector<std::vector<Vector2D>> const& VisualToolMask::ColorDisplayContours() const {
	// Rounding the corners and flattening the beziers for the preview costs about
	// as much as extracting the contours did, and the result depends on nothing that
	// a repaint changes. Rebuild it only when the geometry or the settings move.
	size_t source_points = 0;
	for (auto const& contour : color_contours) source_points += contour.size();
	if (!color_display_dirty && source_points == color_display_source_points)
		return color_display_contours;
	color_display_dirty = false;
	color_display_source_points = source_points;
	color_display_contours.clear();
	if (!color_smooth_edges) {
		color_display_contours = color_contours;
		return color_display_contours;
	}
	for (auto const& curves : BuildSmoothedColorSplines()) {
		if (curves.empty()) continue;
		auto& contour = color_display_contours.emplace_back();
		contour.push_back(curves.front().p1);
		for (auto const& curve : curves) {
			if (curve.type == SplineCurve::LINE) {
				contour.push_back(curve.p2);
				continue;
			}
			float length = (curve.p2 - curve.p1).Len() +
				(curve.p3 - curve.p2).Len() + (curve.p4 - curve.p3).Len();
			int steps = std::clamp(static_cast<int>(std::ceil(length / 4.f)), 4, 64);
			for (int step = 1; step <= steps; ++step)
				contour.push_back(curve.GetPoint(static_cast<float>(step) / steps));
		}
		if (contour.size() > 3 && (contour.front() - contour.back()).SquareLen() < 1e-5f)
			contour.pop_back();
	}
	if (color_display_contours.empty()) color_display_contours = color_contours;
	return color_display_contours;
}

std::vector<std::vector<SplineCurve>> VisualToolMask::BuildSmoothedColorSplines() const {
	std::vector<std::vector<SplineCurve>> result;
	result.reserve(color_contours.size());
	// Auto snap takes the smoothing over: it has put the outline on a real picture
	// edge, and the tolerance that decides how far the fit may stray from it is then
	// the snap's business, not a slider's. The angle threshold stays the user's, so
	// deliberate corners still survive.
	double tolerance = color_edge_snap ? snapped_smooth_tolerance :
		color_smooth_tolerance;
	for (auto const& contour : color_contours) {
		auto curves = SmoothClosedContour(contour, tolerance, color_smooth_angle);
		if (!curves.empty()) result.push_back(std::move(curves));
	}
	return result;
}

void VisualToolMask::RefreshColorContours() {
	color_contours.clear();
	color_display_dirty = true;
	if (color_segmenter.Empty()) return;
	auto raw_contours = color_segmenter.Extract(color_tolerance, color_auto_fill, color_offset);
	if (color_frame_width <= 0 || color_frame_height <= 0) return;
	if (color_smooth_edges && color_edge_snap && !raw_contours.empty())
		raw_contours = SnapColorContoursToEdges(std::move(raw_contours));
	for (auto& contour : raw_contours) {
		for (auto& point : contour)
			point = Vector2D(point.X() * script_res.X() / color_frame_width,
				point.Y() * script_res.Y() / color_frame_height);
		color_contours.push_back(std::move(contour));
	}
	color_contours_dirty = false;
	color_display_dirty = true;
}

void VisualToolMask::SeedEdgeControls() {
	size_t count = points.size();
	point_curved.resize(count, 0);
	bool grew = edge_control_out.size() != count;
	edge_control_out.resize(count);
	edge_control_in.resize(count);
	if (count < 2) return;
	// Only edges that have never had handles are seeded. An edge the user has already
	// shaped keeps what they made of it, even as its neighbours move around it.
	for (size_t edge = 0; edge < count; ++edge) {
		if (!point_curved[edge]) continue;
		if (!grew && edge_control_out[edge] && edge_control_in[edge]) continue;
		if (edge_control_out[edge] && edge_control_in[edge]) continue;
		auto [out, in] = DefaultEdgeControls(points, edge);
		edge_control_out[edge] = out;
		edge_control_in[edge] = in;
	}
}

int VisualToolMask::ControlAt(Vector2D point) const {
	for (size_t edge = 0; edge < point_curved.size(); ++edge) {
		if (!point_curved[edge] || edge >= edge_control_out.size()) continue;
		float reach = std::max(6.f, featureSize + 2.f);
		if ((point - FromScriptCoords(edge_control_out[edge])).Len() <= reach)
			return static_cast<int>(edge) * 2;
		if ((point - FromScriptCoords(edge_control_in[edge])).Len() <= reach)
			return static_cast<int>(edge) * 2 + 1;
	}
	return -1;
}

int VisualToolMask::PointAt(Vector2D point) const {
	for (int i = static_cast<int>(points.size()) - 1; i >= 0; --i) {
		if ((point - FromScriptCoords(points[i])).Len() <= std::max(6.f, featureSize + 2.f))
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
		!tolerance_dragging && !offset_dragging && !brush_slider_dragging &&
		!smooth_tolerance_dragging && !smooth_angle_dragging && !edge_snap_radius_dragging) {
		hovered_point = -1;
		UpdateActionTooltip(VisualToolMaskAction::None);
		parent->Render();
		return;
	}

	auto action = ActionAt(mouse_pos);
	hovered_control = IsPointMode() ? ControlAt(mouse_pos) : -1;
	hovered_point = mode == MASK_COLOR || hovered_control >= 0 ? -1 : PointAt(mouse_pos);
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
	if (smooth_tolerance_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateSmoothTolerance(mouse_pos);
		if (event.LeftUp()) {
			smooth_tolerance_dragging = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
			parent->SetFocus();
		}
		UpdateActionTooltip(VisualToolMaskAction::None);
		parent->Render();
		return;
	}
	if (smooth_angle_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateSmoothAngle(mouse_pos);
		if (event.LeftUp()) {
			smooth_angle_dragging = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
			parent->SetFocus();
		}
		UpdateActionTooltip(VisualToolMaskAction::None);
		parent->Render();
		return;
	}
	if (edge_snap_radius_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateEdgeSnapRadius(mouse_pos);
		if (event.LeftUp()) {
			edge_snap_radius_dragging = false;
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
		if (mode == MASK_COLOR && color_smooth_edges) {
			auto grab = [&](std::pair<Vector2D, Vector2D> const& bounds) {
				return mouse_pos.X() >= bounds.first.X() && mouse_pos.X() <= bounds.second.X() &&
					mouse_pos.Y() >= bounds.first.Y() && mouse_pos.Y() <= bounds.second.Y();
			};
			bool *target = grab(SmoothToleranceBounds()) ? &smooth_tolerance_dragging :
				grab(SmoothAngleBounds()) ? &smooth_angle_dragging :
				color_edge_snap && grab(EdgeSnapRadiusBounds()) ? &edge_snap_radius_dragging :
				nullptr;
			if (target) {
				PushColorHistory();
				*target = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
				if (target == &smooth_tolerance_dragging) UpdateSmoothTolerance(mouse_pos);
				else if (target == &smooth_angle_dragging) UpdateSmoothAngle(mouse_pos);
				else UpdateEdgeSnapRadius(mouse_pos);
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
			if (action == VisualToolMaskAction::RangeShape) ShowRangeShapeMenu();
			else if (action == VisualToolMaskAction::Undo) {
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
			else if (action == VisualToolMaskAction::Templates) ShowColorTemplatesMenu();
			else if (action == VisualToolMaskAction::SmoothEdges) {
				PushColorHistory();
				color_smooth_edges = !color_smooth_edges;
				color_display_dirty = true;
				// Snapping is only reachable through the smoothing row, so a preview
				// showing snapped contours with the row gone would be unexplainable.
				if (!color_smooth_edges && color_edge_snap) {
					color_edge_snap = false;
					if (color_contours_dirty) SyncColorSegmenterFromContours();
					RefreshColorContours();
				}
			}
			else if (action == VisualToolMaskAction::EdgeSnap) {
				if (color_contours_dirty) SyncColorSegmenterFromContours();
				PushColorHistory();
				color_edge_snap = !color_edge_snap;
				RefreshColorContours();
			}
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
			// A new stroke has nothing behind it, so nothing to match against.
			mask_brush_last_centre = Vector2D();
			// Pick the merged shape back up from whatever is on the canvas. Rebuilding it
			// here rather than keeping it alive means undo, a cleared mask and a mode
			// change all need no bookkeeping of their own: the contours are the record.
			if (!mask_brush_shape) mask_brush_shape = std::make_unique<BrushShape>();
			mask_brush_shape->region = MaskRegionFrom(mask_regions);
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
				color_range_path.clear();
				if (color_range_shape == ColorRangeShape::Freehand)
					color_range_path.push_back(script_point);
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

		// Handles are tested before points, since they sit on top of them and are the
		// smaller target: a handle resting on its own anchor would be unreachable
		// otherwise.
		if (hovered_control >= 0) {
			PushMaskHistory();
			dragged_control = hovered_control;
			if (!parent->HasCapture())
				parent->CaptureMouse();
			parent->SetFocus();
		}
		else if (hovered_point >= 0) {
			PushMaskHistory();
			dragged_point = hovered_point;
			if (!parent->HasCapture())
				parent->CaptureMouse();
			parent->SetFocus();
		}
		else {
			Vector2D script_point = ToScriptCoords(mouse_pos);
			PushMaskHistory();
			if (IsPointMode()) {
				points.push_back(script_point);
				// The tool in hand decides the edge that has just been made and the one
				// closing back to the start; earlier edges keep whatever they were drawn
				// with, which is what lets the two be mixed in one outline.
				point_curved.resize(points.size(), 0);
				if (points.size() >= 2)
					point_curved[points.size() - 2] = mode == MASK_BEZIER;
				// The edge back to the first point stays straight, so the outline always
				// closes with the line the preview has been showing all along.
				point_curved.back() = 0;
				SeedEdgeControls();
			}
			else {
				points.clear();
				point_curved.clear();
				edge_control_out.clear();
				edge_control_in.clear();
				mask_regions.clear();
				drawing = true;
				shape_start = script_point;
				points.clear();
				point_curved.clear();
				edge_control_out.clear();
				edge_control_in.clear();
				if (mode == MASK_RECTANGLE)
					UpdateRectangle(script_point);
				else
					UpdateFreehand(script_point);
				if (!parent->HasCapture())
					parent->CaptureMouse();
			}
		}
	}

	if (dragged_control >= 0 && (event.Dragging() || event.LeftUp())) {
		size_t edge = static_cast<size_t>(dragged_control) / 2;
		if (edge < edge_control_out.size()) {
			if (dragged_control % 2 == 0) edge_control_out[edge] = ToScriptCoords(mouse_pos);
			else edge_control_in[edge] = ToScriptCoords(mouse_pos);
		}
		if (event.LeftUp()) {
			dragged_control = -1;
			if (parent->HasCapture())
				parent->ReleaseMouse();
			parent->SetFocus();
			hovered_control = ControlAt(mouse_pos);
		}
		parent->Render();
		return;
	}

	if (dragged_point >= 0 && (event.Dragging() || event.LeftUp())) {
		points[dragged_point] = ToScriptCoords(mouse_pos);
		// A handle nobody has touched follows its point; one that has been shaped stays
		// where it was put, so moving an anchor does not undo the shaping.
		SeedEdgeControls();
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
			// Drop points which cannot change the outline: a lasso sampled per mouse
			// event otherwise carries thousands of them into the range rasteriser.
			if (color_range_shape == ColorRangeShape::Freehand &&
				(color_range_path.empty() ||
				 (FromScriptCoords(color_range_end) -
				  FromScriptCoords(color_range_path.back())).Len() >= 2.f))
				color_range_path.push_back(color_range_end);
		}
		else if (mode == MASK_RECTANGLE)
			UpdateRectangle(script_point);
		else
			UpdateFreehand(script_point);
	}

	if (drawing && event.LeftUp()) {
		drawing = false;
		bool ready = false;
		if (mode == MASK_COLOR && color_range_shape == ColorRangeShape::Freehand) {
			// The lasso closes itself, and its bounding box becomes the cached crop.
			if (color_range_path.size() >= 3) {
				Vector2D top_left = color_range_path.front();
				Vector2D bottom_right = top_left;
				for (auto point : color_range_path) {
					top_left = top_left.Min(point);
					bottom_right = bottom_right.Max(point);
				}
				color_range_start = top_left;
				color_range_end = bottom_right;
				ready = (FromScriptCoords(bottom_right) -
					FromScriptCoords(top_left)).Len() >= 3.f;
			}
			if (!ready) color_range_path.clear();
		}
		else if (mode == MASK_COLOR)
			ready = (FromScriptCoords(color_range_end) -
				FromScriptCoords(color_range_start)).Len() >= 3.f;
		if (ready) {
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
		if (event.IsAutoRepeat()) return true;
		// Where a tool has a twin - line and curve, brush and square - Alt flips between
		// the two of them instead of walking the whole toolbar. A single press cannot do
		// it: it would have moved on before the second press could be recognised, so the
		// pair switch waits for a quick double press and the single press does nothing.
		if (IsPointMode()) {
			auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			bool double_alt = last_alt_press_ms != 0 && now - last_alt_press_ms <= 450;
			last_alt_press_ms = double_alt ? 0 : now;
			if (double_alt)
				SetSubTool(mode == MASK_POINTS ? MASK_BEZIER : MASK_POINTS);
			return true;
		}
		if (mode == MASK_COLOR || mode == MASK_BRUSH) return true;
		SetSubTool((mode + 1) % MASK_COLOR);
		return true;
	}
	last_alt_press_ms = 0;
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
		point_curved.clear();
		edge_control_out.clear();
		edge_control_in.clear();
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
		else if (mode == MASK_BEZIER) {
			// An arc rather than the line tool's corner, so the badge says which of the
			// two will join the next point before it is placed.
			SplineCurve arc(icon + Vector2D(-4.f, 2.f), icon + Vector2D(-3.f, -4.f),
				icon + Vector2D(3.f, -4.f), icon + Vector2D(4.f, 2.f));
			Vector2D previous = arc.p1;
			for (int step = 1; step <= 8; ++step) {
				Vector2D at = arc.GetPoint(step / 8.f);
				gl.DrawLine(previous, at);
				previous = at;
			}
			gl.DrawCircle(icon + Vector2D(-4.f, 2.f), 1.f);
			gl.DrawCircle(icon + Vector2D(4.f, 2.f), 1.f);
		}
		else if (IsPointMode()) {
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
		// Preview what the mask will actually contain, rounded corners included.
		auto const& display_contours = ColorDisplayContours();
		std::vector<float> flat_points;
		std::vector<int> starts;
		std::vector<int> counts;
		for (auto const& contour : display_contours) {
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
			gl.SetLineColour(point_colour, .95f, 2);
			if (!color_range_path.empty()) {
				for (size_t i = 0; i + 1 < color_range_path.size(); ++i)
					gl.DrawDashedLine(FromScriptCoords(color_range_path[i]),
						FromScriptCoords(color_range_path[i + 1]), 6.f);
				// Show the closing leg while the lasso is still being drawn, so it is
				// clear what will be enclosed.
				gl.DrawDashedLine(FromScriptCoords(color_range_path.back()),
					FromScriptCoords(color_range_path.front()), 6.f);
			}
			else {
				Vector2D a = FromScriptCoords(color_range_start);
				Vector2D b = FromScriptCoords(color_range_end);
				Vector2D top_left = a.Min(b), bottom_right = a.Max(b);
				gl.DrawDashedLine(top_left, Vector2D(bottom_right.X(), top_left.Y()), 6.f);
				gl.DrawDashedLine(Vector2D(bottom_right.X(), top_left.Y()), bottom_right, 6.f);
				gl.DrawDashedLine(bottom_right, Vector2D(top_left.X(), bottom_right.Y()), 6.f);
				gl.DrawDashedLine(Vector2D(top_left.X(), bottom_right.Y()), top_left, 6.f);
			}
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
		// Show the curve rather than the polygon it is built from, so what is on screen is
		// what the mask will be made of.
		// Show the edges as they will be written, not as the point list behind them.
		if (points.size() >= 2 &&
			std::find(point_curved.begin(), point_curved.end(), 1) != point_curved.end())
			append_region(FlattenCurves(CurveThroughPoints(points, point_curved,
				edge_control_out, edge_control_in)));
		else
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
		// Two points enclose nothing, so the winding fill above draws nothing for
		// them and the first edge stayed invisible until a third point closed a
		// triangle. This was an "else if": with a finished region already on the
		// canvas, flat_points was not empty, so the branch never ran even though the
		// shape being built still had only its two points.
		if (screen_points.size() == 2)
			gl.DrawLine(screen_points[0], screen_points[1]);
		// Handles in screen space, kept for the direction arrows further down as well.
		std::vector<Vector2D> control_screen_out(point_curved.size());
		std::vector<Vector2D> control_screen_in(point_curved.size());
		for (size_t edge = 0; edge < point_curved.size(); ++edge) {
			if (edge >= edge_control_out.size()) break;
			control_screen_out[edge] = FromScriptCoords(edge_control_out[edge]);
			control_screen_in[edge] = FromScriptCoords(edge_control_in[edge]);
		}
		// Two points describe no curve yet, and an edge that has never been seeded has no
		// handles to show - drawing those put a pair of squares at the origin.
		if (IsPointMode() && screen_points.size() >= 2) {
			for (size_t edge = 0; edge < point_curved.size() && edge < screen_points.size();
					++edge) {
				if (!point_curved[edge] || edge >= edge_control_out.size()) continue;
				if (!edge_control_out[edge] || !edge_control_in[edge]) continue;
				Vector2D anchor = screen_points[edge];
				Vector2D far_anchor = screen_points[(edge + 1) % screen_points.size()];
				Vector2D out = FromScriptCoords(edge_control_out[edge]);
				Vector2D in = FromScriptCoords(edge_control_in[edge]);
				// The vector clip's own handles, to the pixel: dashed to the point they
				// belong to, then a square of the shared handle size.
				gl.SetLineColour(line_colour, .9f, 1);
				gl.DrawDashedLine(anchor, out, 6.f);
				gl.DrawDashedLine(far_anchor, in, 6.f);
				int first = static_cast<int>(edge) * 2;
				for (auto [handle, id] : {std::pair<Vector2D, int>{out, first},
						std::pair<Vector2D, int>{in, first + 1}}) {
					bool active = id == hovered_control || id == dragged_control;
					gl.SetFillColour(active ? point_colour : line_colour, .6f);
					gl.SetLineColour(line_colour, .5f, 1);
					gl.DrawRectangle(handle - featureSize, handle + featureSize);
				}
			}
		}
		// Points are drawn the size the vector clip draws its own, from the same setting.
		for (size_t i = 0; i < screen_points.size(); ++i) {
			bool active = static_cast<int>(i) == hovered_point ||
				static_cast<int>(i) == dragged_point;
			wxColour colour = active ? point_colour : line_colour;
			gl.SetFillColour(colour, .6f);
			gl.SetLineColour(colour, .5f, 1);
			gl.DrawCircle(screen_points[i], featureSize * 2.f / 3.f);
		}
		// The same switch the vector clip reads, so the two tools are informative or quiet
		// together rather than each having its own setting to find, and they say the same
		// things in the same way: an arrow at the middle of every edge for its direction,
		// and a crosshair with coordinates on whichever point is under the cursor.
		if (IsPointMode() && OPT_GET("Video/Clip Info")->GetBool() &&
			screen_points.size() >= 2) {
			auto screen_curves = CurveThroughPoints(screen_points, point_curved,
				control_screen_out, control_screen_in);
			gl.SetLineColour(to_wx(agi::Color(255, 255, 255)), .5f, 2);
			constexpr float arrow_length = 6.f;
			constexpr float arrow_width = 2.5f;
			for (auto const& curve : screen_curves) {
				// A curve is sampled either side of its middle for the same answer a
				// straight edge gives outright: which way it is going where the arrow sits.
				Vector2D tip, before;
				if (curve.type == SplineCurve::BICUBIC) {
					tip = curve.GetPoint(.5f);
					before = curve.GetPoint(.44f);
				}
				else {
					tip = (curve.p1 + curve.p2) * .5f;
					before = curve.p1;
				}
				Vector2D direction = tip - before;
				float length = direction.Len();
				if (length < 1e-6f) continue;
				direction = direction / length;
				Vector2D across(-direction.Y(), direction.X());
				gl.DrawLine(tip, tip - direction * arrow_length + across * arrow_width);
				gl.DrawLine(tip, tip - direction * arrow_length - across * arrow_width);
			}

			int marked = dragged_point >= 0 ? dragged_point : hovered_point;
			if (marked >= 0 && static_cast<size_t>(marked) < screen_points.size()) {
				Vector2D at = screen_points[marked];
				gl.SetLineColour(to_wx(agi::Color(255, 255, 255)), .5f, 1);
				gl.DrawDashedLine(Vector2D(0.f, at), Vector2D(canvas_size, at), 11.f);
				gl.DrawDashedLine(Vector2D(at, 0.f), Vector2D(at, canvas_size), 11.f);
				gl_text->SetFont("Verdana", 10, false, false);
				gl_text->SetColour(agi::Color(255, 255, 255, 175));
				std::string text = points[marked].Str();
				int width = 0, height = 0;
				gl_text->GetExtent(text, width, height);
				// Tuck the label into the quadrant of the cross that has room for it.
				int x = static_cast<int>(at.X());
				int y = static_cast<int>(at.Y());
				bool right = true;
				if (x + width + 14 > canvas_size.X()) { x -= width + 4; right = false; }
				else x += 4;
				y -= height + 4;
				if (y < 4) {
					y += height + 8;
					if (right) x += 10;
				}
				gl_text->Print(text, x, y);
			}
		}

		if (IsPointMode() && mouse_inside && dragged_point < 0 && !screen_points.empty() &&
			mouse_pos.Y() >= TopBarHeight() && ActionAt(mouse_pos) == VisualToolMaskAction::None) {
			// Clicking stores the raw script point, so a point can legitimately sit
			// outside the video rect. Clamping the preview to the script resolution
			// made the dashed line stop at the video edge instead of following the
			// cursor into the letterbox, which is exactly where the next point would
			// land. The screen position is the mouse position; no round trip needed.
			Vector2D screen_preview = mouse_pos;
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
	UpdatePreviewInterface();
	if (preview_interface.HasExternalHost()) return;
	preview_interface.DrawBackground(gl, canvas_size, TopBarHeight());
	if (mode == MASK_BRUSH) {
		auto [brush_top_left, brush_bottom_right] = MaskBrushBounds();
		preview_interface.DrawPanel(gl, {brush_top_left, brush_bottom_right});
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		std::string brush_label = from_wx(_("Size"));
		int text_width, text_height;
		gl_text->GetExtent(brush_label, text_width, text_height);
		gl_text->Print(brush_label, static_cast<int>(brush_top_left.X() + 10.f),
			static_cast<int>((brush_top_left.Y() + brush_bottom_right.Y() - text_height) * .5f));
		float slider_left = brush_top_left.X() + SliderLabelWidth(_("Size"));
		float slider_right = brush_bottom_right.X() - 12.f;
		float slider_y = (brush_top_left.Y() + brush_bottom_right.Y()) * .5f;
		gl.SetLineColour(wxColour(130, 135, 140), 1.f, 3);
		gl.DrawLine(Vector2D(slider_left, slider_y), Vector2D(slider_right, slider_y));
		float knob_x = slider_left + (mask_brush_radius - 2.f) / 198.f * (slider_right - slider_left);
		gl.SetFillColour(wxColour(190, 115, 245), 1.f);
		gl.DrawCircle(Vector2D(knob_x, slider_y), 5.f);
	}
	if (mode == MASK_COLOR && !ai_refining && color_stage == ColorStage::Range) {
		auto [top_left, bottom_right] = ActionBounds(VisualToolMaskAction::RangeShape);
		preview_interface.DrawPanel(gl, {top_left, bottom_right}, true,
			hovered_action == VisualToolMaskAction::RangeShape);
		Vector2D centre((top_left.X() + bottom_right.X()) * .5f - 5.f,
			(top_left.Y() + bottom_right.Y()) * .5f);
		gl.SetLineColour(*wxWHITE, 1.f, 2);
		gl.SetFillColour(*wxWHITE, 1.f);
		if (color_range_shape == ColorRangeShape::Rectangle) {
			Vector2D half(7.f, 6.f);
			gl.DrawDashedLine(centre - half, Vector2D(centre.X() + half.X(), centre.Y() - half.Y()), 4.f);
			gl.DrawDashedLine(Vector2D(centre.X() + half.X(), centre.Y() - half.Y()), centre + half, 4.f);
			gl.DrawDashedLine(centre + half, Vector2D(centre.X() - half.X(), centre.Y() + half.Y()), 4.f);
			gl.DrawDashedLine(Vector2D(centre.X() - half.X(), centre.Y() + half.Y()), centre - half, 4.f);
		}
		else {
			// A mouse outline: body, split top and cable.
			gl.DrawCircle(Vector2D(centre.X(), centre.Y() + 3.f), 5.f);
			gl.DrawLine(Vector2D(centre.X() - 5.f, centre.Y() + 1.f),
				Vector2D(centre.X() + 5.f, centre.Y() + 1.f));
			gl.DrawLine(Vector2D(centre.X(), centre.Y() - 2.f),
				Vector2D(centre.X(), centre.Y() + 1.f));
			gl.DrawLine(Vector2D(centre.X(), centre.Y() - 2.f),
				Vector2D(centre.X(), centre.Y() - 7.f));
		}
		float icon_y = (top_left.Y() + bottom_right.Y()) * .5f;
		gl.SetFillColour(*wxWHITE, 1.f);
		gl.DrawTriangle(Vector2D(bottom_right.X() - 12.f, icon_y - 2.f),
			Vector2D(bottom_right.X() - 4.f, icon_y - 2.f),
			Vector2D(bottom_right.X() - 8.f, icon_y + 3.f));
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
			preview_interface.DrawHistory(gl, {top_left, bottom_right},
				action == VisualToolMaskAction::Redo, enabled, hovered_action == action);
		}
	}

	bool pipette_mode = color_selection_mode == VisualSelectionMode::PipetteAdd ||
		color_selection_mode == VisualSelectionMode::PipetteSubtract;
	if (mode == MASK_COLOR && pipette_mode && has_color_sample) {
		auto [top_left, bottom_right] = ToleranceBounds();
		preview_interface.DrawPanel(gl, {top_left, bottom_right});
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		std::string label = from_wx(_("Tolerance"));
		int text_width, text_height;
		gl_text->GetExtent(label, text_width, text_height);
		gl_text->Print(label, static_cast<int>(top_left.X() + 10.f),
			static_cast<int>((top_left.Y() + bottom_right.Y() - text_height) * .5f));
		float slider_left = top_left.X() + SliderLabelWidth(_("Tolerance"));
		float slider_right = bottom_right.X() - 12.f;
		float slider_y = (top_left.Y() + bottom_right.Y()) * .5f;
		gl.SetLineColour(wxColour(130, 135, 140), 1.f, 3);
		gl.DrawLine(Vector2D(slider_left, slider_y), Vector2D(slider_right, slider_y));
		float knob_x = slider_left + static_cast<float>(color_tolerance / 20.0) * (slider_right - slider_left);
		gl.SetFillColour(wxColour(80, 220, 255), 1.f);
		gl.DrawCircle(Vector2D(knob_x, slider_y), 5.f);

	}
	if (mode == MASK_COLOR) {
		bool offset_enabled = CanOffsetSelection();
		int text_width, text_height;
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(offset_enabled ? agi::Color(255, 255, 255, 255) :
			agi::Color(145, 148, 152, 255));
		auto [offset_top_left, offset_bottom_right] = OffsetBounds();
		preview_interface.DrawPanel(gl, {offset_top_left, offset_bottom_right}, offset_enabled);
		std::string offset_label = from_wx(_("Offset"));
		gl_text->GetExtent(offset_label, text_width, text_height);
		gl_text->Print(offset_label, static_cast<int>(offset_top_left.X() + 10.f),
			static_cast<int>((offset_top_left.Y() + offset_bottom_right.Y() - text_height) * .5f));
		float offset_slider_left = offset_top_left.X() + SliderLabelWidth(_("Offset"));
		float offset_slider_right = offset_bottom_right.X() - 12.f;
		float offset_slider_y = (offset_top_left.Y() + offset_bottom_right.Y()) * .5f;
		gl.SetLineColour(offset_enabled ? wxColour(130, 135, 140) : wxColour(96, 100, 104), 1.f, 3);
		gl.DrawLine(Vector2D(offset_slider_left, offset_slider_y),
			Vector2D(offset_slider_right, offset_slider_y));
		float offset_knob_x = offset_slider_left + (color_offset + 25.f) / 50.f *
			(offset_slider_right - offset_slider_left);
		gl.SetFillColour(offset_enabled ? wxColour(80, 220, 255) : wxColour(120, 124, 128), 1.f);
		gl.DrawCircle(Vector2D(offset_knob_x, offset_slider_y), 5.f);
	}
	bool brush_mode = mode == MASK_COLOR &&
		(color_selection_mode == VisualSelectionMode::BrushAdd ||
		 color_selection_mode == VisualSelectionMode::BrushSubtract);
	if (brush_mode) {
		auto [brush_top_left, brush_bottom_right] = BrushBounds();
		preview_interface.DrawPanel(gl, {brush_top_left, brush_bottom_right});
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		std::string brush_label = from_wx(_("Size"));
		int text_width, text_height;
		gl_text->GetExtent(brush_label, text_width, text_height);
		gl_text->Print(brush_label, static_cast<int>(brush_top_left.X() + 10.f),
			static_cast<int>((brush_top_left.Y() + brush_bottom_right.Y() - text_height) * .5f));
		float slider_left = brush_top_left.X() + SliderLabelWidth(_("Size"));
		float slider_right = brush_bottom_right.X() - 12.f;
		float slider_y = (brush_top_left.Y() + brush_bottom_right.Y()) * .5f;
		gl.SetLineColour(wxColour(130, 135, 140), 1.f, 3);
		gl.DrawLine(Vector2D(slider_left, slider_y), Vector2D(slider_right, slider_y));
		float knob_x = slider_left + (color_brush_radius - 2.f) / 198.f *
			(slider_right - slider_left);
		gl.SetFillColour(wxColour(80, 220, 255), 1.f);
		gl.DrawCircle(Vector2D(knob_x, slider_y), 5.f);
	}

	if (mode == MASK_COLOR && color_smooth_edges) {
		auto draw_slider = [&](std::pair<Vector2D, Vector2D> bounds, wxString label,
			double ratio) {
			auto [top_left, bottom_right] = bounds;
			preview_interface.DrawPanel(gl, {top_left, bottom_right});
			gl_text->SetFont("Verdana", 9, false, false);
			gl_text->SetColour(agi::Color(255, 255, 255, 255));
			std::string text = from_wx(label);
			int text_width, text_height;
			gl_text->GetExtent(text, text_width, text_height);
			gl_text->Print(text, static_cast<int>(top_left.X() + 10.f),
				static_cast<int>((top_left.Y() + bottom_right.Y() - text_height) * .5f));
			float slider_left = top_left.X() + SliderLabelWidth(label);
			float slider_right = bottom_right.X() - 12.f;
			float slider_y = (top_left.Y() + bottom_right.Y()) * .5f;
			gl.SetLineColour(wxColour(130, 135, 140), 1.f, 3);
			gl.DrawLine(Vector2D(slider_left, slider_y), Vector2D(slider_right, slider_y));
			gl.SetFillColour(wxColour(80, 220, 255), 1.f);
			gl.DrawCircle(Vector2D(slider_left + static_cast<float>(std::clamp(ratio, 0.0, 1.0)) *
				(slider_right - slider_left), slider_y), 5.f);
		};
		draw_slider(SmoothToleranceBounds(), _("Smooth tolerance"),
			(color_smooth_tolerance - .1) / 49.9);
		draw_slider(SmoothAngleBounds(), _("Angle threshold"),
			color_smooth_angle / 180.0);
		if (color_edge_snap)
			draw_slider(EdgeSnapRadiusBounds(), _("Edge search"),
				(color_edge_snap_radius - 2.0) / 48.0);
	}

	auto label_for = [&](VisualToolMaskAction action) -> wxString {
		switch (action) {
			case VisualToolMaskAction::Create: return _("Accept");
			case VisualToolMaskAction::Clear: return _("Cancel");
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
			case VisualToolMaskAction::Templates: return _("Templates");
			case VisualToolMaskAction::SmoothEdges: return _("Smooth edges");
			case VisualToolMaskAction::EdgeSnap: return _("Auto snap");
			default: return wxString();
		}
	};
	std::vector<VisualToolMaskAction> actions;
	if (ai_refining)
		actions = {VisualToolMaskAction::RemoveText, VisualToolMaskAction::GenerateText,
			VisualToolMaskAction::Create, VisualToolMaskAction::Clear};
	else if (mode == MASK_COLOR) {
		actions = {VisualToolMaskAction::SelectionMode};
		actions.push_back(VisualToolMaskAction::Templates);
		actions.push_back(VisualToolMaskAction::AISelect);
		if (has_color_sample)
			actions.push_back(VisualToolMaskAction::AutoFill);
		actions.push_back(VisualToolMaskAction::SmoothEdges);
		if (color_smooth_edges) actions.push_back(VisualToolMaskAction::EdgeSnap);
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
		if (action == VisualToolMaskAction::Templates)
			enabled = color_stage != ColorStage::Range;
		if (action == VisualToolMaskAction::SmoothEdges ||
			action == VisualToolMaskAction::EdgeSnap)
			enabled = color_stage != ColorStage::Range && !color_contours.empty();
		auto style = VisualToolPreviewInterface::ControlStyle::Neutral;
		if (action == VisualToolMaskAction::Create)
			style = VisualToolPreviewInterface::ControlStyle::Accept;
		else if (action == VisualToolMaskAction::Clear)
			style = VisualToolPreviewInterface::ControlStyle::Cancel;
		else if (action == VisualToolMaskAction::RemoveText ||
			action == VisualToolMaskAction::AISelect)
			style = VisualToolPreviewInterface::ControlStyle::Warning;
		else if (action == VisualToolMaskAction::GenerateText)
			style = VisualToolPreviewInterface::ControlStyle::Accent;
		bool selected = (action == VisualToolMaskAction::AutoFill && color_auto_fill) ||
			(action == VisualToolMaskAction::SmoothEdges && color_smooth_edges) ||
			(action == VisualToolMaskAction::EdgeSnap && color_edge_snap);
		bool dropdown = action == VisualToolMaskAction::SelectionMode ||
			action == VisualToolMaskAction::Templates;
		preview_interface.DrawButton(gl, *gl_text, {top_left, bottom_right}, label_for(action),
			style, enabled, hovered_action == action, selected, dropdown);
	}
	if (ai_refining) {
		auto clear_bounds = ActionBounds(VisualToolMaskAction::Clear);
		preview_interface.DrawMessage(*gl_text, clear_bounds, clear_bounds.second.X() + 12.f,
			_("You can refine the result again if you want."));
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
	if (mode == MASK_COLOR && color_smooth_edges) {
		// Rounded corners need bezier segments, so this path cannot reuse the
		// straight m/l writer below.
		std::string encoded;
		for (auto const& curves : BuildSmoothedColorSplines()) {
			if (curves.empty()) continue;
			if (!encoded.empty()) encoded += " ";
			encoded += "m " + curves.front().p1.Str(' ', 1);
			char last = 'm';
			for (auto const& curve : curves) {
				if (curve.type == SplineCurve::LINE) {
					if (last != 'l') { encoded += " l"; last = 'l'; }
					encoded += " " + curve.p2.Str(' ', 1);
				}
				else if (curve.type == SplineCurve::BICUBIC) {
					if (last != 'b') { encoded += " b"; last = 'b'; }
					encoded += " " + curve.p2.Str(' ', 1) + " " + curve.p3.Str(' ', 1) +
						" " + curve.p4.Str(' ', 1);
				}
			}
		}
		if (!encoded.empty()) return encoded;
	}
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
		// The brush stamps a 48-sided polygon per step, so a stroke reaches the file as
		// thousands of straight segments describing edges that were round to begin with.
		// Fitting them here, where the line is written, keeps the painting itself working
		// on the plain polygons it always has.
		if (mode == MASK_BRUSH) {
			auto curves = FitClosedContour(region, mask_brush_fit_tolerance);
			if (!curves.empty()) {
				encoded += "m " + curves.front().p1.Str(' ', 1);
				char last = 'm';
				for (auto const& curve : curves) {
					if (curve.type == SplineCurve::LINE) {
						if (last != 'l') { encoded += " l"; last = 'l'; }
						encoded += " " + curve.p2.Str(' ', 1);
					}
					else if (curve.type == SplineCurve::BICUBIC) {
						if (last != 'b') { encoded += " b"; last = 'b'; }
						encoded += " " + curve.p2.Str(' ', 1) + " " + curve.p3.Str(' ', 1) +
							" " + curve.p4.Str(' ', 1);
					}
				}
				return;
			}
		}
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
	// Edges drawn with the curve tool are written as bezier segments and the rest as
	// lines, in the order they were placed. The points are still the points; only the way
	// consecutive ones are joined differs, and that is recorded per edge.
	if (points.size() >= 2 &&
		std::find(point_curved.begin(), point_curved.end(), 1) != point_curved.end()) {
		auto curves = CurveThroughPoints(points, point_curved, edge_control_out, edge_control_in);
		if (!encoded.empty()) encoded += " ";
		encoded += "m " + curves.front().p1.Str(' ', 1);
		char last = 'm';
		for (auto const& curve : curves) {
			if (curve.type == SplineCurve::BICUBIC) {
				if (last != 'b') { encoded += " b"; last = 'b'; }
				encoded += " " + curve.p2.Str(' ', 1) + " " + curve.p3.Str(' ', 1) +
					" " + curve.p4.Str(' ', 1);
			}
			else {
				if (last != 'l') { encoded += " l"; last = 'l'; }
				encoded += " " + curve.p2.Str(' ', 1);
			}
		}
	}
	else
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

	// Work on the integer script-pixel grid before encoding. The shared codec is
	// also used by motion, so a tracked image returns through exactly the same path.
	float image_x_scale = static_cast<float>(image_width) / crop_width;
	float image_y_scale = static_cast<float>(image_height) / crop_height;
	std::vector<Vector2D> image_points;
	image_points.reserve(points.size());
	for (auto point : points)
		image_points.emplace_back((point.X() - crop_x) * image_x_scale,
			(point.Y() - crop_y) * image_y_scale);

	int frame_time = c->videoController->TimeAtFrame(frame_number, agi::vfr::EXACT);
	agi::Time start = source->Start;
	agi::Time end = source->End;
	if (!(source->Start <= frame_time && frame_time < source->End)) {
		start = c->videoController->TimeAtFrame(frame_number, agi::vfr::START);
		end = c->videoController->TimeAtFrame(frame_number, agi::vfr::END);
	}

	imagemask::Raster raster;
	raster.x = crop_x;
	raster.y = crop_y;
	raster.width = image_width;
	raster.height = image_height;
	raster.rgba.resize(static_cast<size_t>(image_width) * image_height * 4);
	auto pixels = image.GetData();
	auto source_alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
	std::vector<unsigned char> polygon_alpha;
	if (!source_alpha) {
		polygon_alpha.assign(static_cast<size_t>(image_width) * image_height, 0);
		for (int y = 0; y < image_height; ++y)
			for (auto [span_start, span_end] : polygon_spans(image_points, y + .5f, 1.f, image_width))
				std::fill(polygon_alpha.begin() + static_cast<size_t>(y) * image_width + span_start,
					polygon_alpha.begin() + static_cast<size_t>(y) * image_width + span_end, 255);
	}
	for (int y = 0; y < image_height; ++y) {
		for (int x = 0; x < image_width; ++x) {
			size_t pixel = static_cast<size_t>(y) * image_width + x;
			raster.rgba[pixel * 4] = pixels[pixel * 3];
			raster.rgba[pixel * 4 + 1] = pixels[pixel * 3 + 1];
			raster.rgba[pixel * 4 + 2] = pixels[pixel * 3 + 2];
			raster.rgba[pixel * 4 + 3] = source_alpha ? source_alpha[pixel] : polygon_alpha[pixel];
		}
	}

	auto encoded = imagemask::Encode(raster, *source, static_cast<int>(start),
		static_cast<int>(end), image_colour_tolerance);
	std::vector<AssDialogue *> lines;
	lines.reserve(encoded.size());
	for (auto& line : encoded) lines.push_back(new AssDialogue(std::move(line)));
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
	point_curved.clear();
	edge_control_out.clear();
	edge_control_in.clear();
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

	if (!ai_refining) {
		VisualToolPreviewInterface::Page refine_page;
		refine_page.message = _("You can refine the result again if you want.");
		preview_interface.PushPage(std::move(refine_page));
	}
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
	mask_regions.clear();
	points.clear();
	point_curved.clear();
	edge_control_out.clear();
	edge_control_in.clear();
	mask_undo_history.clear();
	mask_redo_history.clear();
	drawing = false;
	gl.InvalidateImageCache();
	parent->Render();
}
