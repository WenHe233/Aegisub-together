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

#include "visual_tool_vector_clip.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ai_client.h"
#include "compat.h"
#include "format.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "selection_controller.h"
#include "video_display.h"
#include "video_controller.h"
#include "video_frame.h"

#include <libaegisub/color.h>

#include <algorithm>
#include <array>
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <boost/range/algorithm/set_algorithm.hpp>
#include <cmath>
#include <functional>
#include <limits>
#include <wx/event.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/popupwin.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/toolbar.h>

int BUTTON_ID_BASE = 1300;

namespace {
	namespace bg = boost::geometry;
	using BrushPoint = bg::model::d2::point_xy<double>;
	using BrushPolygon = bg::model::polygon<BrushPoint>;
	using BrushMultiPolygon = bg::model::multi_polygon<BrushPolygon>;

	ai::CloudinaryCredentials configured_cloudinary() {
		return {OPT_GET("AI/Cloudinary/Cloud Name")->GetString(),
			OPT_GET("AI/Cloudinary/API Key")->GetString(), ai::GetCloudinarySecret()};
	}

	class BrushSettingsPopup final : public wxPopupTransientWindow {
	public:
		BrushSettingsPopup(wxWindow *parent, bool add, int radius,
			std::function<void(bool)> set_mode, std::function<void(int)> set_size)
		: wxPopupTransientWindow(parent, wxBORDER_SIMPLE) {
			auto sizer = new wxBoxSizer(wxVERTICAL);
			auto modes = new wxBoxSizer(wxHORIZONTAL);
			auto add_button = new wxRadioButton(this, wxID_ANY, _("Add"),
				wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
			auto erase_button = new wxRadioButton(this, wxID_ANY, _("Delete"));
			add_button->SetValue(add);
			erase_button->SetValue(!add);
			modes->Add(add_button, 1, wxRIGHT, 12);
			modes->Add(erase_button, 1);
			sizer->Add(modes, 0, wxEXPAND | wxALL, 10);

			auto size_label = new wxStaticText(this, wxID_ANY,
				agi::wxformat(_("Brush size: %d px"), radius));
			sizer->Add(size_label, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
			auto slider = new wxSlider(this, wxID_ANY, radius, 2, 200,
				wxDefaultPosition, wxSize(220, -1), wxSL_HORIZONTAL);
			sizer->Add(slider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
			SetSizerAndFit(sizer);

			add_button->Bind(wxEVT_RADIOBUTTON,
				[set_mode](wxCommandEvent&) { set_mode(true); });
			erase_button->Bind(wxEVT_RADIOBUTTON,
				[set_mode](wxCommandEvent&) { set_mode(false); });
			slider->Bind(wxEVT_SLIDER, [size_label, set_size](wxCommandEvent& event) {
				int value = event.GetInt();
				size_label->SetLabel(agi::wxformat(_("Brush size: %d px"), value));
				set_size(value);
			});
		}
	};

	bool point_in_polygon(Vector2D point, std::vector<float> const& polygon,
		size_t first, size_t count) {
		if (count < 3 || (first + count) * 2 > polygon.size()) return false;
		bool inside = false;
		for (size_t i = 0, j = count - 1; i < count; j = i++) {
			Vector2D a(polygon[(first + i) * 2], polygon[(first + i) * 2 + 1]);
			Vector2D b(polygon[(first + j) * 2], polygon[(first + j) * 2 + 1]);
			bool crosses = (a.Y() > point.Y()) != (b.Y() > point.Y());
			if (crosses && point.X() < (b.X() - a.X()) * (point.Y() - a.Y()) /
				(b.Y() - a.Y()) + a.X())
				inside = !inside;
		}
		return inside;
	}

	bool point_in_polygon(Vector2D point, std::vector<float> const& polygon) {
		return point_in_polygon(point, polygon, 0, polygon.size() / 2);
	}

	float distance_to_segment_squared(Vector2D point, Vector2D a, Vector2D b) {
		Vector2D delta = b - a;
		float length_squared = delta.SquareLen();
		if (length_squared <= .0001f) return (point - a).SquareLen();
		float factor = std::clamp((point - a).Dot(delta) / length_squared, 0.f, 1.f);
		return (point - (a + delta * factor)).SquareLen();
	}

	bool circle_touches_polygon(Vector2D centre, float radius,
		std::vector<float> const& polygon, size_t first, size_t count) {
		if (count < 3 || (first + count) * 2 > polygon.size()) return false;
		if (point_in_polygon(centre, polygon, first, count)) return true;
		float radius_squared = radius * radius;
		for (size_t i = 0, previous = count - 1; i < count; previous = i++) {
			Vector2D a(polygon[(first + previous) * 2], polygon[(first + previous) * 2 + 1]);
			Vector2D b(polygon[(first + i) * 2], polygon[(first + i) * 2 + 1]);
			if (distance_to_segment_squared(centre, a, b) <= radius_squared) return true;
		}
		return false;
	}

	BrushPolygon make_brush_polygon(std::vector<float> const& points,
		size_t first, size_t count) {
		BrushPolygon polygon;
		if (count < 3 || (first + count) * 2 > points.size()) return polygon;
		auto& ring = polygon.outer();
		ring.reserve(count + 1);
		Vector2D previous;
		bool has_previous = false;
		for (size_t i = 0; i < count; ++i) {
			Vector2D point(points[(first + i) * 2], points[(first + i) * 2 + 1]);
			if (!std::isfinite(point.X()) || !std::isfinite(point.Y())) continue;
			if (has_previous && (point - previous).SquareLen() <= 1e-8f) continue;
			ring.emplace_back(point.X(), point.Y());
			previous = point;
			has_previous = true;
		}
		if (ring.size() > 2 && bg::equals(ring.front(), ring.back())) ring.pop_back();
		if (ring.size() < 3) {
			ring.clear();
			return polygon;
		}
		ring.push_back(ring.front());
		bg::correct(polygon);
		return polygon;
	}

	BrushPolygon make_brush_circle(Vector2D centre, float radius) {
		constexpr int segments = 64;
		BrushPolygon polygon;
		auto& ring = polygon.outer();
		ring.reserve(segments + 1);
		for (int i = 0; i < segments; ++i) {
			double angle = i * 2.0 * M_PI / segments;
			ring.emplace_back(centre.X() + std::cos(angle) * radius,
				centre.Y() + std::sin(angle) * radius);
		}
		ring.push_back(ring.front());
		bg::correct(polygon);
		return polygon;
	}

	void simplify_smooth_open(std::vector<Vector2D> const& points, size_t first,
		size_t last, double epsilon_squared, std::vector<unsigned char>& keep) {
		if (last <= first + 1) return;
		double maximum_distance = epsilon_squared;
		size_t split = first;
		Vector2D segment = points[last] - points[first];
		float segment_length_squared = segment.SquareLen();
		for (size_t i = first + 1; i < last; ++i) {
			float factor = segment_length_squared <= 1e-6f ? 0.f :
				std::clamp((points[i] - points[first]).Dot(segment) /
					segment_length_squared, 0.f, 1.f);
			double distance = (points[i] - (points[first] + segment * factor)).SquareLen();
			if (distance > maximum_distance) {
				maximum_distance = distance;
				split = i;
			}
		}
		if (split == first) return;
		keep[split] = 1;
		simplify_smooth_open(points, first, split, epsilon_squared, keep);
		simplify_smooth_open(points, split, last, epsilon_squared, keep);
	}

	std::vector<Vector2D> simplify_smooth_closed(std::vector<Vector2D> points,
		double epsilon) {
		std::vector<Vector2D> clean;
		for (auto point : points)
			if (clean.empty() || (point - clean.back()).SquareLen() > .25f)
				clean.push_back(point);
		if (clean.size() > 2 && (clean.front() - clean.back()).SquareLen() <= .25f)
			clean.pop_back();
		if (clean.size() < 4) return clean;
		size_t opposite = 1;
		for (size_t i = 2; i < clean.size(); ++i)
			if ((clean[i] - clean.front()).SquareLen() >
				(clean[opposite] - clean.front()).SquareLen()) opposite = i;
		std::vector<Vector2D> opened = clean;
		opened.push_back(clean.front());
		std::vector<unsigned char> keep(opened.size());
		keep[0] = keep[opposite] = keep.back() = 1;
		double epsilon_squared = epsilon * epsilon;
		simplify_smooth_open(opened, 0, opposite, epsilon_squared, keep);
		simplify_smooth_open(opened, opposite, opened.size() - 1, epsilon_squared, keep);
		std::vector<Vector2D> result;
		for (size_t i = 0; i + 1 < opened.size(); ++i)
			if (keep[i]) result.push_back(opened[i]);
		return result.size() >= 3 ? result : clean;
	}

	std::vector<SplineCurve> smooth_closed_contour(std::vector<Vector2D> points,
		double tolerance, double angle_threshold) {
		if (points.size() < 3) return {};
		tolerance = std::clamp(tolerance, .1, 50.0);
		angle_threshold = std::clamp(angle_threshold, 0.0, 180.0);
		auto vertices = simplify_smooth_closed(std::move(points),
			std::max(.05, tolerance * .35));
		if (vertices.size() < 3) return {};

		struct RoundedVertex {
			Vector2D entry;
			Vector2D control_in;
			Vector2D control_out;
			Vector2D exit;
			bool rounded = false;
		};
		std::vector<RoundedVertex> rounded(vertices.size());
		for (size_t i = 0; i < vertices.size(); ++i) {
			Vector2D previous = vertices[(i + vertices.size() - 1) % vertices.size()];
			Vector2D current = vertices[i];
			Vector2D next = vertices[(i + 1) % vertices.size()];
			Vector2D to_previous = previous - current;
			Vector2D to_next = next - current;
			float previous_length = to_previous.Len();
			float next_length = to_next.Len();
			auto& corner = rounded[i];
			corner.entry = corner.exit = current;
			if (previous_length <= 1e-4f || next_length <= 1e-4f) continue;
			double cosine = std::clamp(static_cast<double>(to_previous.Dot(to_next)) /
				(previous_length * next_length), -1.0, 1.0);
			double interior_angle = std::acos(cosine) * 180.0 / M_PI;
			// Angles at or below the threshold are deliberate sharp corners.
			// Everything above it is rounded, so a low threshold also removes
			// one-pixel staircase corners instead of preserving every 90-degree turn.
			if (interior_angle <= angle_threshold) continue;
			float radius = static_cast<float>(std::min({tolerance,
				previous_length * .45, next_length * .45}));
			if (radius <= .05f) continue;
			Vector2D previous_unit = to_previous / previous_length;
			Vector2D next_unit = to_next / next_length;
			corner.entry = current + previous_unit * radius;
			corner.exit = current + next_unit * radius;
			float handle = radius * .55228475f;
			corner.control_in = corner.entry - previous_unit * handle;
			corner.control_out = corner.exit - next_unit * handle;
			corner.rounded = true;
		}

		std::vector<SplineCurve> result;
		Vector2D cursor = rounded.back().exit;
		for (auto const& corner : rounded) {
			if ((corner.entry - cursor).SquareLen() > 1e-5f)
				result.emplace_back(cursor, corner.entry);
			if (corner.rounded)
				result.emplace_back(corner.entry, corner.control_in,
					corner.control_out, corner.exit);
			cursor = corner.exit;
		}
		return result;
	}

	Vector2D rotate_point(Vector2D point, float angle) {
		float sine = std::sin(angle);
		float cosine = std::cos(angle);
		return Vector2D(point.X() * cosine - point.Y() * sine,
			point.X() * sine + point.Y() * cosine);
	}

	template<typename Func>
	void transform_curve(SplineCurve& curve, Func&& transform) {
		curve.p1 = transform(curve.p1);
		if (curve.type == SplineCurve::LINE)
			curve.p2 = transform(curve.p2);
		else if (curve.type == SplineCurve::BICUBIC) {
			curve.p2 = transform(curve.p2);
			curve.p3 = transform(curve.p3);
			curve.p4 = transform(curve.p4);
		}
	}
}

VisualToolVectorClip::VisualToolVectorClip(VideoDisplay *parent, agi::Context *context, bool edit_drawing)
: VisualTool<VisualToolVectorClipDraggableFeature>(parent, context)
, gl_text(std::make_unique<OpenGLText>())
, drawing_mode(edit_drawing)
, spline(edit_drawing ? nullptr : this)
, featureSize(OPT_GET("Tool/Visual/Shape Handle Size")->GetInt())
, brush_add_mode(OPT_GET("Tool/Visual/Vector Clip Brush Add")->GetBool())
{
	connections.push_back(c->ass->AddCommitListener(
		&VisualToolVectorClip::OnSubtitleCommit, this));
}

VisualToolVectorClip::~VisualToolVectorClip() {
	if (brush_popup) brush_popup->Destroy();
	if (!toolBar) return;
	toolBar->Unbind(wxEVT_TOOL, &VisualToolVectorClip::OnToolbar, this);
	toolBar->Unbind(wxEVT_TOOL_RCLICKED, &VisualToolVectorClip::OnToolbar, this);
}

void VisualToolVectorClip::AddTool(std::string command_name, VisualToolVectorClipMode mode) {
	cmd::Command *command = cmd::get(command_name);
	int icon_size = OPT_GET("App/Toolbar Icon Size")->GetInt();
	toolBar->AddTool(BUTTON_ID_BASE + mode, command->StrDisplay(c), command->Icon(icon_size), command->GetTooltip("Video"), wxITEM_CHECK);
}


void VisualToolVectorClip::SetToolbar(wxToolBar *toolBar) {
	this->toolBar = toolBar;
	parent->SetCursor(wxCursor(wxCURSOR_ARROW));

	toolBar->AddSeparator();

	AddTool("video/tool/vclip/drag", VCLIP_DRAG);
	AddTool("video/tool/vclip/line", VCLIP_LINE);
	AddTool("video/tool/vclip/bicubic", VCLIP_BICUBIC);
	AddTool(brush_add_mode ? "video/tool/vclip/brush_add" :
		"video/tool/vclip/brush_delete", VCLIP_BRUSH);
	toolBar->AddSeparator();
	AddTool("video/tool/vclip/convert", VCLIP_CONVERT);
	AddTool("video/tool/vclip/append", VCLIP_APPEND);
	AddTool("video/tool/vclip/insert", VCLIP_INSERT);
	AddTool("video/tool/vclip/remove", VCLIP_REMOVE);
	toolBar->AddSeparator();
	AddTool("video/tool/vclip/freehand", VCLIP_FREEHAND);
	AddTool("video/tool/vclip/freehand_smooth", VCLIP_FREEHAND_SMOOTH);
	toolBar->AddSeparator();
	AddTool("video/tool/vclip/color", VCLIP_COLOR);

	toolBar->ToggleTool(BUTTON_ID_BASE + VCLIP_DRAG, true);
	toolBar->Realize();
	toolBar->Show(true);
	toolBar->Bind(wxEVT_TOOL, &VisualToolVectorClip::OnToolbar, this);
	toolBar->Bind(wxEVT_TOOL_RCLICKED, &VisualToolVectorClip::OnToolbar, this);
	UpdateBrushToolbar();
	SetSubTool(features.empty() ? VCLIP_LINE : VCLIP_DRAG);
}

void VisualToolVectorClip::OnToolbar(wxCommandEvent& event) {
	int subtool = event.GetId() - BUTTON_ID_BASE;
	if (subtool == VCLIP_BRUSH && event.GetEventType() == wxEVT_TOOL_RCLICKED) {
		ShowBrushSettings();
		return;
	}
	if (event.GetEventType() == wxEVT_TOOL)
		SetSubTool(subtool);
}

void VisualToolVectorClip::ShowBrushSettings() {
	if (!toolBar) return;
	if (brush_popup) {
		brush_popup->Dismiss();
		brush_popup->Destroy();
		brush_popup = nullptr;
	}
	bool add = brush_add_mode;
	brush_popup = new BrushSettingsPopup(toolBar, add,
		static_cast<int>(std::lround(color_brush_radius)),
		[this](bool use_add) {
			cmd::get(use_add ? "video/tool/vclip/brush_add" :
				"video/tool/vclip/brush_delete")->operator()(c);
		},
		[this](int value) {
			color_brush_radius = static_cast<float>(value);
			UpdateBrushToolbar();
			parent->Render();
		});
	wxSize client_size = toolBar->GetClientSize();
	int left = client_size.x, top = client_size.y, right = -1, bottom = -1;
	for (int y = 0; y < client_size.y; ++y) {
		for (int x = 0; x < client_size.x; ++x) {
			auto tool = toolBar->FindToolForPosition(x, y);
			if (!tool || tool->GetId() != BUTTON_ID_BASE + VCLIP_BRUSH) continue;
			left = std::min(left, x);
			top = std::min(top, y);
			right = std::max(right, x);
			bottom = std::max(bottom, y);
		}
	}
	wxPoint position;
	if (right >= left && bottom >= top) {
		bool vertical = client_size.y > client_size.x;
		position = toolBar->ClientToScreen(vertical ? wxPoint(right + 2, top) :
			wxPoint(left, bottom + 2));
	}
	else
		position = toolBar->ClientToScreen(wxPoint(0, client_size.y));
	brush_popup->Move(position);
	brush_popup->Popup();
}

void VisualToolVectorClip::UpdateBrushToolbar() {
	if (!toolBar) return;
	bool add = brush_add_mode;
	int icon_size = OPT_GET("App/Toolbar Icon Size")->GetInt();
	toolBar->SetToolNormalBitmap(BUTTON_ID_BASE + VCLIP_BRUSH,
		wxBitmapBundle::FromBitmap(MakeVisualVectorClipBrushBitmap(add, icon_size)));
	toolBar->SetToolShortHelp(BUTTON_ID_BASE + VCLIP_BRUSH,
		agi::wxformat(_("Brush: %s, %d px (right-click for settings)"),
			add ? _("Add") : _("Delete"), static_cast<int>(std::lround(color_brush_radius))));
	toolBar->Realize();
}

void VisualToolVectorClip::UpdateTool(int update) {
	if (update != VCLIP_BRUSH_ACTION_ADD && update != VCLIP_BRUSH_ACTION_DELETE)
		return;
	if (brush_popup) {
		auto popup = brush_popup;
		brush_popup = nullptr;
		popup->Dismiss();
		popup->Destroy();
	}
	brush_add_mode = update == VCLIP_BRUSH_ACTION_ADD;
	OPT_SET("Tool/Visual/Vector Clip Brush Add")->SetBool(brush_add_mode);
	color_selection_mode = brush_add_mode ?
		VisualSelectionMode::BrushAdd : VisualSelectionMode::BrushSubtract;
	UpdateColorCursor();
	UpdateBrushToolbar();
	parent->Render();
}

void VisualToolVectorClip::SetSubTool(int subtool) {
	if (toolBar == nullptr) {
		throw agi::InternalError("Vector clip toolbar hasn't been set yet!");
	}
	if (brush_popup && subtool != VCLIP_BRUSH) {
		auto popup = brush_popup;
		brush_popup = nullptr;
		popup->Dismiss();
		popup->Destroy();
	}
	if (subtool == VCLIP_COLOR && mode != VCLIP_COLOR) {
		color_return_mode = mode;
		ResetColorSelection();
	}
	else if (subtool == VCLIP_BRUSH && mode != VCLIP_BRUSH) {
		color_return_mode = mode;
		ResetColorSelection();
	}
	else if ((mode == VCLIP_COLOR || mode == VCLIP_BRUSH) &&
		subtool != VCLIP_COLOR && subtool != VCLIP_BRUSH) {
		ResetColorSelection();
	}

	// Manually enforce radio behavior as we want one selection in the bar
	// rather than one per group
	for (int i = 0; i < VCLIP_LAST; i++)
		toolBar->ToggleTool(BUTTON_ID_BASE + i, i == subtool);

	mode = subtool;
	if (mode == VCLIP_BRUSH)
		InitializeBrushSelection();
	held_curve_features.clear();
	drag_commit_pending = false;
	parent->Render();
}

int VisualToolVectorClip::GetSubTool() {
	return mode;
}

void VisualToolVectorClip::ResetColorSelection() {
	if (mode == VCLIP_BRUSH && color_brush_drawing &&
		color_brush_preview_changed) {
		spline.clear();
		spline.insert(spline.end(), color_brush_base_spline.begin(),
			color_brush_base_spline.end());
		MakeFeatures();
	}
	color_stage = ColorStage::Range;
	color_selection_mode = VisualSelectionMode::PipetteAdd;
	color_range_start = color_range_end = Vector2D();
	color_contours.clear();
	color_segmenter.Clear();
	color_contours_dirty = false;
	has_color_sample = false;
	color_sample_operations.clear();
	color_ai_base = false;
	color_offset = 0;
	color_auto_fill = false;
	color_smooth_edges = false;
	color_smooth_tolerance = 10.0;
	color_smooth_angle = 35.0;
	color_frame_width = color_frame_height = 0;
	color_drawing = false;
	tolerance_dragging = false;
	offset_dragging = false;
	smooth_tolerance_dragging = false;
	smooth_angle_dragging = false;
	brush_slider_dragging = false;
	color_brush_drawing = false;
	color_brush_moved = false;
	color_brush_stroke.clear();
	color_brush_base_spline.clear();
	color_brush_preview_changed = false;
	color_undo_history.clear();
	color_redo_history.clear();
	color_undo_history.reserve(16);
	color_redo_history.reserve(16);
	hovered_color_action = ColorAction::None;
	parent->SetCursor(wxCursor(wxCURSOR_ARROW));
	parent->UnsetToolTip();
	if (parent->HasCapture()) parent->ReleaseMouse();
}

VisualToolVectorClip::ColorHistoryState VisualToolVectorClip::CaptureColorHistory() const {
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
	state.sample_operations = color_sample_operations;
	state.ai_base = color_ai_base;
	return state;
}

VisualToolVectorClip::ColorHistoryState VisualToolVectorClip::CaptureColorBrushHistory() const {
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
	state.sample_operations = color_sample_operations;
	state.ai_base = color_ai_base;
	state.contours_only = true;
	state.contours = color_contours;
	return state;
}

void VisualToolVectorClip::RestoreColorHistory(ColorHistoryState state) {
	color_sample = state.sample;
	color_stage = state.stage;
	color_tolerance = state.tolerance;
	color_offset = state.offset;
	has_color_sample = state.has_sample;
	color_auto_fill = state.auto_fill;
	color_smooth_edges = state.smooth_edges;
	color_smooth_tolerance = state.smooth_tolerance;
	color_smooth_angle = state.smooth_angle;
	color_sample_operations = std::move(state.sample_operations);
	color_ai_base = state.ai_base;
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

void VisualToolVectorClip::PushColorHistory() {
	if (color_contours_dirty) SyncColorSegmenterFromContours();
	constexpr size_t maximum_history = 16;
	if (color_undo_history.size() == maximum_history)
		color_undo_history.erase(color_undo_history.begin());
	color_undo_history.push_back(CaptureColorHistory());
	color_redo_history.clear();
}

void VisualToolVectorClip::PushColorBrushHistory() {
	constexpr size_t maximum_history = 16;
	if (color_undo_history.size() == maximum_history)
		color_undo_history.erase(color_undo_history.begin());
	color_undo_history.push_back(CaptureColorBrushHistory());
	color_redo_history.clear();
}

bool VisualToolVectorClip::UndoColorHistory() {
	if (color_undo_history.empty()) return false;
	color_redo_history.push_back(color_undo_history.back().contours_only ?
		CaptureColorBrushHistory() : CaptureColorHistory());
	auto state = std::move(color_undo_history.back());
	color_undo_history.pop_back();
	RestoreColorHistory(std::move(state));
	return true;
}

bool VisualToolVectorClip::RedoColorHistory() {
	if (color_redo_history.empty()) return false;
	color_undo_history.push_back(color_redo_history.back().contours_only ?
		CaptureColorBrushHistory() : CaptureColorHistory());
	auto state = std::move(color_redo_history.back());
	color_redo_history.pop_back();
	RestoreColorHistory(std::move(state));
	return true;
}

std::vector<VisualToolVectorClip::ColorTemplate>& VisualToolVectorClip::ColorTemplates() {
	static std::vector<ColorTemplate> templates;
	return templates;
}

bool VisualToolVectorClip::CanCaptureColorTemplate() const {
	return mode == VCLIP_COLOR && color_stage == ColorStage::Ready &&
		(color_ai_base || !color_sample_operations.empty());
}

VisualToolVectorClip::ColorTemplate VisualToolVectorClip::CaptureColorTemplate(std::string name) const {
	ColorTemplate color_template;
	color_template.name = std::move(name);
	color_template.sample_operations = color_sample_operations;
	color_template.ai_base = color_ai_base;
	color_template.tolerance = color_tolerance;
	color_template.offset = color_offset;
	color_template.auto_fill = color_auto_fill;
	color_template.smooth_edges = color_smooth_edges;
	color_template.smooth_tolerance = color_smooth_tolerance;
	color_template.smooth_angle = color_smooth_angle;
	color_template.selection_mode = color_selection_mode == VisualSelectionMode::PipetteSubtract ?
		VisualSelectionMode::PipetteSubtract : VisualSelectionMode::PipetteAdd;
	return color_template;
}

bool VisualToolVectorClip::LoadColorTemplate(ColorTemplate const& color_template) {
	if (mode != VCLIP_COLOR || color_stage == ColorStage::Range) return false;
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
			segmenter.SetContours(contours);
		}
		for (auto const& operation : color_template.sample_operations) {
			if (!segmenter.AddSample(*frame, operation.sample, operation.add)) return false;
		}

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

std::pair<Vector2D, Vector2D> VisualToolVectorClip::ColorToleranceBounds() {
	auto mode_bounds = ColorActionBounds(ColorAction::SelectionMode);
	float left = mode_bounds.second.X() + 8.f;
	float top = mode_bounds.first.Y();
	constexpr float width = 145.f;
	if (left + width > canvas_size.X() - 8.f) {
		left = 12.f;
		top = mode_bounds.second.Y() + 8.f;
	}
	return {Vector2D(left, top), Vector2D(left + width, top + 34.f)};
}

bool VisualToolVectorClip::CanOffsetSelection() const {
	return (mode == VCLIP_COLOR || mode == VCLIP_BRUSH) &&
		color_stage == ColorStage::Ready && !color_segmenter.Empty();
}

std::pair<Vector2D, Vector2D> VisualToolVectorClip::ColorOffsetBounds() const {
	return {Vector2D(96.f, 10.f), Vector2D(230.f, 44.f)};
}

std::pair<Vector2D, Vector2D> VisualToolVectorClip::SmoothToleranceBounds() {
	float top = ColorActionBounds(ColorAction::Cancel).second.Y() + 8.f;
	return {Vector2D(12.f, top), Vector2D(242.f, top + 34.f)};
}

std::pair<Vector2D, Vector2D> VisualToolVectorClip::SmoothAngleBounds() {
	auto tolerance = SmoothToleranceBounds();
	float left = tolerance.second.X() + 8.f;
	float top = tolerance.first.Y();
	constexpr float width = 225.f;
	if (left + width > canvas_size.X() - 8.f) {
		left = 12.f;
		top = tolerance.second.Y() + 8.f;
	}
	return {Vector2D(left, top), Vector2D(left + width, top + 34.f)};
}

std::pair<Vector2D, Vector2D> VisualToolVectorClip::ColorBrushBounds() {
	auto mode_bounds = ColorActionBounds(ColorAction::SelectionMode);
	float left = mode_bounds.second.X() + 8.f;
	float top = mode_bounds.first.Y();
	constexpr float width = 145.f;
	if (left + width > canvas_size.X() - 8.f) {
		left = 12.f;
		top = mode_bounds.second.Y() + 8.f;
	}
	return {Vector2D(left, top), Vector2D(left + width, top + 34.f)};
}

std::pair<Vector2D, Vector2D> VisualToolVectorClip::ColorActionBounds(ColorAction action) {
	if (action == ColorAction::Undo)
		return {Vector2D(12.f, 10.f), Vector2D(46.f, 44.f)};
	if (action == ColorAction::Redo)
		return {Vector2D(54.f, 10.f), Vector2D(88.f, 44.f)};
	float top = 10.f;
	constexpr float height = 34.f, gap = 8.f;
	bool pipette_mode = color_selection_mode == VisualSelectionMode::PipetteAdd ||
		color_selection_mode == VisualSelectionMode::PipetteSubtract;
	bool brush_mode = !pipette_mode;
	float left = CanOffsetSelection() ? ColorOffsetBounds().second.X() + gap : 96.f;
	auto mode_label = [&]() -> wxString {
		switch (color_selection_mode) {
			case VisualSelectionMode::PipetteAdd: return _("Pipette add");
			case VisualSelectionMode::PipetteSubtract: return _("Pipette subtract");
			case VisualSelectionMode::BrushAdd: return _("Brush add");
			case VisualSelectionMode::BrushSubtract: return _("Brush subtract");
		}
		return wxString();
	};
	auto label_for = [&](ColorAction item) -> wxString {
		if (item == ColorAction::SelectionMode) return mode_label();
		if (item == ColorAction::Templates) return _("Templates");
		if (item == ColorAction::AISelect) return _("AI recognition");
		if (item == ColorAction::AutoFill) return _("Auto fill");
		if (item == ColorAction::SmoothEdges) return _("Smooth edges");
		if (item == ColorAction::Accept)
			return _("Accept (ENTER)");
		if (item == ColorAction::Cancel) return _("Cancel (ESC)");
		return wxString();
	};
	auto width_for = [&](ColorAction item) {
		gl_text->SetFont("Verdana", 9, true, false);
		int width, height;
		gl_text->GetExtent(from_wx(label_for(item)), width, height);
		return static_cast<float>(width + (item == ColorAction::SelectionMode ||
			item == ColorAction::Templates ? 32 :
			24));
	};
	std::vector<ColorAction> actions{ColorAction::SelectionMode};
	if (mode == VCLIP_COLOR) {
		actions.push_back(ColorAction::Templates);
		actions.push_back(ColorAction::AISelect);
		if (has_color_sample)
			actions.push_back(ColorAction::AutoFill);
		actions.push_back(ColorAction::SmoothEdges);
		actions.push_back(ColorAction::Accept);
		actions.push_back(ColorAction::Cancel);
	}
	for (auto item : actions) {
		float width = width_for(item);
		if (left + width > canvas_size.X() - 8.f && left > 12.f) {
			left = 12.f;
			top += height + gap;
		}
		if (item == action) return {Vector2D(left, top), Vector2D(left + width, top + height)};
		left += width + gap;
		if (item == ColorAction::SelectionMode && mode == VCLIP_COLOR) {
			if (pipette_mode && has_color_sample) {
				auto bounds = ColorToleranceBounds();
				left = bounds.second.X() + gap;
				top = bounds.first.Y();
			}
			else if (brush_mode) {
				auto bounds = ColorBrushBounds();
				left = bounds.second.X() + gap;
				top = bounds.first.Y();
			}
		}
	}
	return {Vector2D(left, top), Vector2D(left, top + height)};
}

float VisualToolVectorClip::ColorTopBarHeight() {
	if (mode == VCLIP_BRUSH) return 0.f;
	if (color_smooth_edges) return SmoothAngleBounds().second.Y() + 10.f;
	return ColorActionBounds(ColorAction::Cancel).second.Y() + 10.f;
}

VisualToolVectorClip::ColorAction VisualToolVectorClip::ColorActionAt(Vector2D point) {
	if (mode == VCLIP_BRUSH) return ColorAction::None;
	for (auto action : {ColorAction::Undo, ColorAction::Redo}) {
		bool enabled = action == ColorAction::Undo ? !color_undo_history.empty() :
			!color_redo_history.empty();
		if (!enabled) continue;
		auto [top_left, bottom_right] = ColorActionBounds(action);
		if (point.X() >= top_left.X() && point.X() <= bottom_right.X() &&
			point.Y() >= top_left.Y() && point.Y() <= bottom_right.Y()) return action;
	}
	std::vector<ColorAction> actions{ColorAction::SelectionMode};
	if (mode == VCLIP_COLOR) {
		actions.push_back(ColorAction::Templates);
		actions.push_back(ColorAction::AISelect);
		if (has_color_sample)
			actions.push_back(ColorAction::AutoFill);
		actions.push_back(ColorAction::SmoothEdges);
		actions.push_back(ColorAction::Accept);
		actions.push_back(ColorAction::Cancel);
	}
	for (auto action : actions) {
		if (action == ColorAction::Accept && color_contours.empty() && mode != VCLIP_BRUSH) continue;
		if (action == ColorAction::AutoFill && !has_color_sample) continue;
		if (action == ColorAction::SmoothEdges &&
			(color_stage == ColorStage::Range || color_contours.empty())) continue;
		if (action == ColorAction::Cancel && color_stage == ColorStage::Range) continue;
		if (action == ColorAction::SelectionMode && color_stage == ColorStage::Range) continue;
		if (action == ColorAction::AISelect && color_stage == ColorStage::Range) continue;
		auto [top_left, bottom_right] = ColorActionBounds(action);
		if (point.X() >= top_left.X() && point.X() <= bottom_right.X() &&
			point.Y() >= top_left.Y() && point.Y() <= bottom_right.Y()) return action;
	}
	return ColorAction::None;
}

void VisualToolVectorClip::ShowColorModeMenu() {
	if (color_stage == ColorStage::Range) return;
	constexpr int pipette_add_id = 17411;
	constexpr int pipette_subtract_id = 17412;
	constexpr int brush_add_id = 17413;
	constexpr int brush_subtract_id = 17414;
	wxMenu menu;
	auto add_mode_item = [&](int id, wxString const& label) {
		menu.Append(id, label);
	};
	if (mode != VCLIP_BRUSH) {
		add_mode_item(pipette_add_id, _("Pipette add"));
		add_mode_item(pipette_subtract_id, _("Pipette subtract"));
	}
	add_mode_item(brush_add_id, _("Brush add"));
	add_mode_item(brush_subtract_id, _("Brush subtract"));
	auto [top_left, bottom_right] = ColorActionBounds(ColorAction::SelectionMode);
	int selected = parent->GetPopupMenuSelectionFromUser(menu,
		wxPoint(static_cast<int>(top_left.X()), static_cast<int>(bottom_right.Y())));
	if (selected == pipette_add_id) color_selection_mode = VisualSelectionMode::PipetteAdd;
	else if (selected == pipette_subtract_id) color_selection_mode = VisualSelectionMode::PipetteSubtract;
	else if (selected == brush_add_id) color_selection_mode = VisualSelectionMode::BrushAdd;
	else if (selected == brush_subtract_id) color_selection_mode = VisualSelectionMode::BrushSubtract;
	UpdateColorCursor();
	parent->Render();
}

void VisualToolVectorClip::ShowColorTemplatesMenu() {
	enum class TemplateMenuAction { Load, Update, Delete };
	struct TemplateMenuEntry {
		int id;
		TemplateMenuAction action;
		size_t index;
	};
	constexpr int add_id = 17501;
	int next_id = 17502;
	wxMenu menu;
	auto add_item = menu.Append(add_id, _("Add new template..."));
	add_item->Enable(CanCaptureColorTemplate());

	auto load_menu = new wxMenu;
	auto update_menu = new wxMenu;
	auto delete_menu = new wxMenu;
	std::vector<TemplateMenuEntry> entries;
	auto& templates = ColorTemplates();
	for (size_t i = 0; i < templates.size(); ++i) {
		wxString name = to_wx(templates[i].name);
		int load_id = next_id++;
		int update_id = next_id++;
		int delete_id = next_id++;
		load_menu->Append(load_id, name);
		auto update_item = update_menu->Append(update_id, name);
		update_item->Enable(CanCaptureColorTemplate());
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

	auto [top_left, bottom_right] = ColorActionBounds(ColorAction::Templates);
	int selected = parent->GetPopupMenuSelectionFromUser(menu,
		wxPoint(static_cast<int>(top_left.X()), static_cast<int>(bottom_right.Y())));
	if (selected == add_id) {
		wxString default_name = agi::wxformat(_("Template %zu"), templates.size() + 1);
		wxTextEntryDialog dialog(c->parent, _("Template name:"),
			_("Add template"), default_name);
		if (dialog.ShowModal() != wxID_OK) return;
		wxString name = dialog.GetValue();
		name.Trim(true).Trim(false);
		if (name.empty()) return;
		std::string utf8_name = from_wx(name);
		if (std::any_of(templates.begin(), templates.end(), [&](ColorTemplate const& item) {
			return item.name == utf8_name;
		})) {
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

void VisualToolVectorClip::ShowAISelectionMenu() {
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

void VisualToolVectorClip::UpdateColorTooltip() {
	if (mode != VCLIP_COLOR) {
		parent->UnsetToolTip();
		return;
	}
	auto inside = [&](std::pair<Vector2D, Vector2D> const& bounds) {
		return mouse_pos.X() >= bounds.first.X() && mouse_pos.X() <= bounds.second.X() &&
			mouse_pos.Y() >= bounds.first.Y() && mouse_pos.Y() <= bounds.second.Y();
	};
	wxString tooltip;
	bool pipette_mode = color_selection_mode == VisualSelectionMode::PipetteAdd ||
		color_selection_mode == VisualSelectionMode::PipetteSubtract;
	bool brush_mode = color_selection_mode == VisualSelectionMode::BrushAdd ||
		color_selection_mode == VisualSelectionMode::BrushSubtract;
	if (smooth_tolerance_dragging || (color_smooth_edges && inside(SmoothToleranceBounds())))
		tooltip = agi::wxformat(_("Smooth tolerance: %.2f"), color_smooth_tolerance);
	else if (smooth_angle_dragging || (color_smooth_edges && inside(SmoothAngleBounds())))
		tooltip = agi::wxformat(_("Angle threshold: %.1f deg"), color_smooth_angle);
	else if (tolerance_dragging || (pipette_mode && has_color_sample && inside(ColorToleranceBounds())))
		tooltip = agi::wxformat(_("Tolerance: %.1f"), color_tolerance);
	else if (offset_dragging || (CanOffsetSelection() && inside(ColorOffsetBounds())))
		tooltip = agi::wxformat(_("Offset: %d px"), color_offset);
	else if (brush_slider_dragging || (brush_mode && inside(ColorBrushBounds())))
		tooltip = agi::wxformat(_("Brush: %d px"), static_cast<int>(std::lround(color_brush_radius)));
	if (tooltip.empty()) parent->UnsetToolTip();
	else parent->SetToolTip(tooltip);
}

void VisualToolVectorClip::UpdateColorTolerance(Vector2D point) {
	if (color_contours_dirty) SyncColorSegmenterFromContours();
	auto [top_left, bottom_right] = ColorToleranceBounds();
	float left = top_left.X() + 75.f;
	float right = bottom_right.X() - 12.f;
	double value = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f) * 20.0;
	color_tolerance = std::round(value * 10.0) / 10.0;
	RefreshColorContours();
}

void VisualToolVectorClip::UpdateColorOffset(Vector2D point) {
	if (color_contours_dirty) SyncColorSegmenterFromContours();
	auto [top_left, bottom_right] = ColorOffsetBounds();
	float left = top_left.X() + 60.f;
	float right = bottom_right.X() - 12.f;
	double ratio = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f);
	color_offset = static_cast<int>(std::lround(ratio * 50.0 - 25.0));
	RefreshColorContours();
}

void VisualToolVectorClip::UpdateSmoothTolerance(Vector2D point) {
	auto [top_left, bottom_right] = SmoothToleranceBounds();
	float left = top_left.X() + 125.f;
	float right = bottom_right.X() - 12.f;
	double ratio = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f);
	color_smooth_tolerance = std::round((.1 + ratio * 49.9) * 100.0) / 100.0;
}

void VisualToolVectorClip::UpdateSmoothAngle(Vector2D point) {
	auto [top_left, bottom_right] = SmoothAngleBounds();
	float left = top_left.X() + 120.f;
	float right = bottom_right.X() - 12.f;
	double ratio = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f);
	color_smooth_angle = std::round(ratio * 1800.0) / 10.0;
}

void VisualToolVectorClip::UpdateColorBrushSize(Vector2D point) {
	auto [top_left, bottom_right] = ColorBrushBounds();
	float left = top_left.X() + 70.f;
	float right = bottom_right.X() - 12.f;
	double ratio = std::clamp((point.X() - left) / std::max(1.f, right - left), 0.f, 1.f);
	color_brush_radius = static_cast<float>(std::lround(2.0 + ratio * 198.0));
}

void VisualToolVectorClip::UpdateColorCursor() {
	if (color_stage == ColorStage::Range || mouse_pos.Y() < ColorTopBarHeight()) {
		parent->SetCursor(wxCursor(wxCURSOR_ARROW));
		return;
	}
	if (color_selection_mode == VisualSelectionMode::PipetteAdd ||
		color_selection_mode == VisualSelectionMode::PipetteSubtract)
		parent->SetCursor(MakeVisualColorPickerCursor());
	else if (color_selection_mode == VisualSelectionMode::BrushAdd ||
		color_selection_mode == VisualSelectionMode::BrushSubtract)
		// Keep a real pointer visible as a fallback when a delayed OpenGL frame
		// cannot redraw the brush outline immediately.
		parent->SetCursor(wxCursor(wxCURSOR_ARROW));
	else
		parent->SetCursor(wxCursor(wxCURSOR_ARROW));
}

bool VisualToolVectorClip::EnsureColorSegmenter() {
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

bool VisualToolVectorClip::InitializeBrushSelection() {
	if (mode == VCLIP_BRUSH) {
		color_contours.clear();
		color_stage = ColorStage::Ready;
		color_selection_mode = brush_add_mode ? VisualSelectionMode::BrushAdd :
			VisualSelectionMode::BrushSubtract;
		has_color_sample = false;
		color_auto_fill = false;
		color_offset = 0;
		UpdateColorCursor();
		return true;
	}
	try {
		auto frame = c->videoController->GetFrame(frame_number, true);
		if (!frame || frame->width <= 0 || frame->height <= 0) return false;
		if (!color_segmenter.PrepareEmpty(*frame, 0, 0, frame->width, frame->height))
			return false;

		std::vector<int> starts, counts;
		auto points = spline.GetPointList(starts, counts);
		std::vector<std::vector<Vector2D>> contours;
		contours.reserve(starts.size());
		for (size_t contour_index = 0; contour_index < starts.size(); ++contour_index) {
			std::vector<Vector2D> contour;
			contour.reserve(counts[contour_index]);
			for (int i = 0; i < counts[contour_index]; ++i) {
				size_t point_index = static_cast<size_t>(starts[contour_index] + i) * 2;
				Vector2D script = ToScriptCoords(Vector2D(points[point_index], points[point_index + 1]));
				contour.emplace_back(script.X() * frame->width / script_res.X(),
					script.Y() * frame->height / script_res.Y());
			}
			if (contour.size() >= 3) contours.push_back(std::move(contour));
		}
		color_segmenter.SetContours(contours, true);
		color_range_start = Vector2D(0.f, 0.f);
		color_range_end = script_res;
		color_frame_width = frame->width;
		color_frame_height = frame->height;
		color_stage = ColorStage::Ready;
		color_selection_mode = brush_add_mode ? VisualSelectionMode::BrushAdd :
			VisualSelectionMode::BrushSubtract;
		has_color_sample = false;
		color_auto_fill = false;
		color_offset = 0;
		RefreshColorContours();
		UpdateColorCursor();
		return true;
	}
	catch (...) {
		return false;
	}
}

bool VisualToolVectorClip::PrepareAISelection() {
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
		color_segmenter.SetContours(contours);
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

bool VisualToolVectorClip::PrepareColorSelection(Vector2D sample_point) {
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

void VisualToolVectorClip::PaintColorBrush(Vector2D from, Vector2D to) {
	if (mode == VCLIP_BRUSH) return;
	if (!EnsureColorSegmenter() || color_frame_width <= 0 ||
		color_frame_height <= 0 || video_size.X() <= 0.f || video_size.Y() <= 0.f) return;
	PaintSelectionBrush(from, to);
}

void VisualToolVectorClip::PaintSelectionBrush(Vector2D from, Vector2D to) {
	if (color_frame_width <= 0 || color_frame_height <= 0 ||
		video_size.X() <= 0.f || video_size.Y() <= 0.f) return;
	std::vector<std::vector<Vector2D>> screen_contours = color_contours;
	for (auto& contour : screen_contours)
		for (auto& point : contour) point = FromScriptCoords(point);
	float distance = (to - from).Len();
	float step_size = std::max(1.f, color_brush_radius * .3f);
	int steps = distance > .01f ?
		std::max(1, static_cast<int>(std::ceil(distance / step_size))) : 0;
	bool add = color_selection_mode == VisualSelectionMode::BrushAdd;
	for (int step = 0; step <= steps; ++step)
		ApplyVectorBrushStamp(screen_contours, from + (to - from) *
			(steps ? static_cast<float>(step) / steps : 0.f),
			color_brush_radius, add);
	color_contours = std::move(screen_contours);
	for (auto& contour : color_contours)
		for (auto& point : contour) point = ToScriptCoords(point);
	color_offset = 0;
	color_auto_fill = false;
	color_contours_dirty = true;
}

void VisualToolVectorClip::SyncColorSegmenterFromContours() {
	if (color_segmenter.Empty() || color_frame_width <= 0 || color_frame_height <= 0) return;
	std::vector<std::vector<Vector2D>> frame_contours = color_contours;
	for (auto& contour : frame_contours) {
		for (auto& point : contour)
			point = Vector2D(point.X() * color_frame_width / script_res.X(),
				point.Y() * color_frame_height / script_res.Y());
	}
	color_segmenter.SetContours(frame_contours);
	color_contours_dirty = false;
}

bool VisualToolVectorClip::ApplyBrushStroke(std::vector<Vector2D> const& stroke) {
	if (stroke.empty() || color_brush_radius <= 0.f) return false;
	std::vector<int> starts, counts;
	auto points = spline.GetPointList(starts, counts);
	std::vector<size_t> path_starts;
	for (size_t i = 0; i < spline.size(); ++i)
		if (spline[i].type == SplineCurve::POINT) path_starts.push_back(i);
	if (starts.size() != path_starts.size()) return false;

	float spacing = std::max(1.f, color_brush_radius * .35f);
	std::vector<Vector2D> centres{stroke.front()};
	for (size_t i = 1; i < stroke.size(); ++i) {
		Vector2D from = centres.back();
		Vector2D to = stroke[i];
		float distance = (to - from).Len();
		int steps = std::max(1, static_cast<int>(std::ceil(distance / spacing)));
		for (int step = 1; step <= steps; ++step)
			centres.push_back(from + (to - from) * (static_cast<float>(step) / steps));
	}
	std::vector<unsigned char> touched(starts.size());
	for (size_t i = 0; i < starts.size(); ++i) {
		for (auto centre : centres) {
			if (circle_touches_polygon(centre, color_brush_radius, points,
				static_cast<size_t>(starts[i]), static_cast<size_t>(counts[i]))) {
				touched[i] = 1;
				break;
			}
		}
	}
	if (std::none_of(touched.begin(), touched.end(), [](unsigned char value) { return value != 0; })) {
		if (!brush_add_mode) return false;
	}

	try {
		// Preserve the clip's non-zero winding semantics before applying the
		// brush. Same-direction overlaps remain filled; opposite-direction
		// overlaps cancel. This is the case that a containment-only model cannot
		// represent when neither contour is wholly inside the other.
		auto signed_area = [&](size_t contour) {
			double area = 0.0;
			size_t first = static_cast<size_t>(starts[contour]);
			size_t count = static_cast<size_t>(counts[contour]);
			for (size_t i = 0, previous = count - 1; i < count; previous = i++) {
				double ax = points[(first + previous) * 2];
				double ay = points[(first + previous) * 2 + 1];
				double bx = points[(first + i) * 2];
				double by = points[(first + i) * 2 + 1];
				area += ax * by - bx * ay;
			}
			return area * .5;
		};
		std::vector<BrushPolygon> polygons;
		polygons.reserve(starts.size());
		for (size_t i = 0; i < starts.size(); ++i) {
			auto polygon = make_brush_polygon(points,
				static_cast<size_t>(starts[i]), static_cast<size_t>(counts[i]));
			if (polygon.outer().size() < 4 || !bg::is_valid(polygon)) {
				AppendBrushCircle(centres.front());
				MakeFeatures();
				return true;
			}
			polygons.push_back(std::move(polygon));
		}
		std::vector<unsigned char> affected = touched;
		bool expanded = true;
		while (expanded) {
			expanded = false;
			for (size_t i = 0; i < polygons.size(); ++i) {
				if (affected[i]) continue;
				for (size_t j = 0; j < polygons.size(); ++j) {
					if (!affected[j]) continue;
					if (bg::intersects(polygons[i], polygons[j]) ||
						bg::within(polygons[i], polygons[j]) || bg::within(polygons[j], polygons[i])) {
						affected[i] = 1;
						expanded = true;
						break;
					}
				}
			}
		}
		double reference_area = 0.0;
		for (size_t i = 0; i < starts.size(); ++i) {
			if (!affected[i]) continue;
			double area = signed_area(i);
			if (std::abs(area) > std::abs(reference_area)) reference_area = area;
		}
		auto unite = [](BrushMultiPolygon& target, BrushPolygon polygon) {
			if (polygon.outer().size() < 4) return;
			if (target.empty()) {
				target.push_back(std::move(polygon));
				return;
			}
			BrushMultiPolygon combined;
			bg::union_(target, polygon, combined);
			target = std::move(combined);
		};
		BrushMultiPolygon forward;
		BrushMultiPolygon reverse;
		for (size_t i = 0; i < starts.size(); ++i) {
			if (!affected[i]) continue;
			double area = signed_area(i);
			if (std::abs(area) <= 1e-6) continue;
			if (reference_area == 0.0 || area * reference_area >= 0.0)
				unite(forward, polygons[i]);
			else
				unite(reverse, polygons[i]);
		}
		BrushMultiPolygon selection;
		if (forward.empty()) selection = std::move(reverse);
		else if (reverse.empty()) selection = std::move(forward);
		else bg::sym_difference(forward, reverse, selection);

		BrushMultiPolygon brush;
		for (auto centre : centres) {
			auto circle = make_brush_circle(centre, color_brush_radius);
			if (brush.empty()) {
				brush.push_back(std::move(circle));
				continue;
			}
			BrushMultiPolygon combined;
			bg::union_(brush, circle, combined);
			brush = std::move(combined);
		}
		BrushMultiPolygon result;
		if (brush_add_mode && selection.empty()) result = std::move(brush);
		else if (brush_add_mode) bg::union_(selection, brush, result);
		else bg::difference(selection, brush, result);

		std::vector<SplineCurve> rebuilt;
		for (size_t i = 0; i < path_starts.size(); ++i) {
			if (affected[i]) continue;
			size_t end = i + 1 < path_starts.size() ? path_starts[i + 1] : spline.size();
			rebuilt.insert(rebuilt.end(), spline.begin() + path_starts[i], spline.begin() + end);
		}
		auto append_ring = [&](auto const& ring) {
			if (ring.size() < 4) return;
			Vector2D first(static_cast<float>(ring.front().x()),
				static_cast<float>(ring.front().y()));
			rebuilt.emplace_back(first);
			Vector2D previous = first;
			for (size_t i = 1; i + 1 < ring.size(); ++i) {
				Vector2D current(static_cast<float>(ring[i].x()),
					static_cast<float>(ring[i].y()));
				rebuilt.emplace_back(previous, current);
				previous = current;
			}
		};
		for (auto const& polygon : result) {
			append_ring(polygon.outer());
			for (auto const& inner : polygon.inners()) append_ring(inner);
		}
		spline.clear();
		spline.insert(spline.end(), rebuilt.begin(), rebuilt.end());
		MakeFeatures();
		return true;
	}
	catch (...) {
		return false;
	}
}

void VisualToolVectorClip::AppendBrushCircle(Vector2D centre) {
	constexpr int segments = 64;
	std::vector<Vector2D> points;
	points.reserve(segments);
	double reference_area = 0.0;
	std::vector<int> starts, counts;
	auto existing = spline.GetPointList(starts, counts);
	for (size_t contour = 0; contour < starts.size() && contour < counts.size(); ++contour) {
		double area = 0.0;
		size_t first = static_cast<size_t>(starts[contour]);
		size_t count = static_cast<size_t>(counts[contour]);
		for (size_t i = 0, previous = count - 1; count >= 3 && i < count; previous = i++) {
			Vector2D a(existing[(first + previous) * 2], existing[(first + previous) * 2 + 1]);
			Vector2D b(existing[(first + i) * 2], existing[(first + i) * 2 + 1]);
			area += a.Cross(b);
		}
		if (std::abs(area) > std::abs(reference_area)) reference_area = area;
	}
	bool positive = brush_add_mode ? reference_area >= 0.0 : reference_area < 0.0;
	for (int i = 0; i < segments; ++i) {
		int index = positive ? i : segments - i;
		float angle = static_cast<float>(index * 2.0 * M_PI / segments);
		points.emplace_back(centre.X() + std::cos(angle) * color_brush_radius,
			centre.Y() + std::sin(angle) * color_brush_radius);
	}
	if (points.empty()) return;
	active_path_start = spline.size();
	spline.emplace_back(points.front());
	for (size_t i = 1; i < points.size(); ++i)
		spline.emplace_back(points[i - 1], points[i]);
}

void VisualToolVectorClip::RefreshColorContours() {
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

std::vector<std::vector<SplineCurve>> VisualToolVectorClip::BuildSmoothedColorSplines() const {
	std::vector<std::vector<SplineCurve>> result;
	result.reserve(color_contours.size());
	for (auto const& contour : color_contours) {
		auto curves = smooth_closed_contour(contour,
			color_smooth_tolerance, color_smooth_angle);
		if (!curves.empty()) result.push_back(std::move(curves));
	}
	return result;
}

void VisualToolVectorClip::AcceptColorContours() {
	if (color_contours.empty() && mode != VCLIP_BRUSH) return;
	bool brush_edit = mode == VCLIP_BRUSH;
	if (brush_edit) spline.clear();
	if (color_smooth_edges && !brush_edit) {
		for (auto const& curves : BuildSmoothedColorSplines()) {
			if (curves.empty()) continue;
			active_path_start = spline.size();
			spline.emplace_back(FromScriptCoords(curves.front().p1));
			for (auto const& curve : curves) {
				if (curve.type == SplineCurve::LINE)
					spline.emplace_back(FromScriptCoords(curve.p1), FromScriptCoords(curve.p2));
				else if (curve.type == SplineCurve::BICUBIC)
					spline.emplace_back(FromScriptCoords(curve.p1), FromScriptCoords(curve.p2),
						FromScriptCoords(curve.p3), FromScriptCoords(curve.p4));
			}
		}
	}
	else {
		for (auto const& contour : color_contours) {
			if (contour.size() < 3) continue;
			std::vector<Vector2D> screen_contour;
			screen_contour.reserve(contour.size());
			for (auto point : contour) screen_contour.push_back(FromScriptCoords(point));
			active_path_start = spline.size();
			spline.emplace_back(screen_contour.front());
			for (size_t i = 1; i < screen_contour.size(); ++i)
				spline.emplace_back(screen_contour[i - 1], screen_contour[i]);
		}
	}
	MakeFeatures();
	SetSubTool(VCLIP_DRAG);
	Save(1);
	VisualToolBase::Commit(brush_edit ? _("Brush edit vector clip") : _("Add color contours"));
}

void VisualToolVectorClip::CommitBrushContours() {
	if (mode != VCLIP_BRUSH) return;
	// The whole captured stroke has already been applied as one geometry edit.
	Save();
	VisualToolBase::Commit(_("Brush edit vector clip"));
	commit_id = -1;
}

void VisualToolVectorClip::CloseColorMode() {
	int return_mode = color_return_mode == VCLIP_COLOR || color_return_mode == VCLIP_BRUSH ?
		VCLIP_DRAG : color_return_mode;
	SetSubTool(return_mode);
}

bool VisualToolVectorClip::DeleteActivePath() {
	if (mode != VCLIP_DRAG || active_path_start == no_path || active_path_start >= spline.size())
		return false;
	size_t end = PathEnd(active_path_start);
	spline.erase(spline.begin() + active_path_start, spline.begin() + end);
	active_path_start = no_path;
	MakeFeatures();
	Commit(_("Delete contour"));
	parent->Render();
	return true;
}

void VisualToolVectorClip::OnMouseEvent(wxMouseEvent& event) {
	if (!active_line) return;
	if (mode != VCLIP_COLOR && mode != VCLIP_BRUSH) {
		VisualTool<VisualToolVectorClipDraggableFeature>::OnMouseEvent(event);
		return;
	}
	mouse_pos = event.GetPosition();
	hovered_color_action = mode == VCLIP_BRUSH ? ColorAction::None :
		ColorActionAt(mouse_pos);
	UpdateColorCursor();
	UpdateColorTooltip();
	bool brush_mode = color_selection_mode == VisualSelectionMode::BrushAdd ||
		color_selection_mode == VisualSelectionMode::BrushSubtract;
	if (color_brush_drawing && (event.Dragging() || event.LeftUp())) {
		if ((mouse_pos - color_brush_last).Len() >= 1.f) {
			if (mode == VCLIP_BRUSH) {
				color_brush_stroke.push_back(mouse_pos);
				color_brush_preview_changed = ApplyBrushStroke(
					{color_brush_last, mouse_pos}) || color_brush_preview_changed;
			}
			else
				PaintSelectionBrush(color_brush_last, mouse_pos);
			color_brush_last = mouse_pos;
			color_brush_moved = true;
		}
		if (event.LeftUp()) {
			color_brush_drawing = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
			if (mode == VCLIP_BRUSH && color_brush_preview_changed) {
				CommitBrushContours();
			}
			color_brush_stroke.clear();
			color_brush_base_spline.clear();
			color_brush_preview_changed = false;
		}
		parent->Render();
		return;
	}
	if (brush_slider_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateColorBrushSize(mouse_pos);
		if (event.LeftUp()) {
			brush_slider_dragging = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
		}
		UpdateColorTooltip();
		parent->Render();
		return;
	}

	if (tolerance_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateColorTolerance(mouse_pos);
		if (event.LeftUp()) {
			tolerance_dragging = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
		}
		UpdateColorTooltip();
		parent->Render();
		return;
	}
	if (offset_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateColorOffset(mouse_pos);
		if (event.LeftUp()) {
			offset_dragging = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
		}
		UpdateColorTooltip();
		parent->Render();
		return;
	}
	if (smooth_tolerance_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateSmoothTolerance(mouse_pos);
		if (event.LeftUp()) {
			smooth_tolerance_dragging = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
		}
		UpdateColorTooltip();
		parent->Render();
		return;
	}
	if (smooth_angle_dragging && (event.Dragging() || event.LeftUp())) {
		UpdateSmoothAngle(mouse_pos);
		if (event.LeftUp()) {
			smooth_angle_dragging = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
		}
		UpdateColorTooltip();
		parent->Render();
		return;
	}
	if (event.LeftDown()) {
		if (color_smooth_edges) {
			auto [smooth_top_left, smooth_bottom_right] = SmoothToleranceBounds();
			if (mouse_pos.X() >= smooth_top_left.X() && mouse_pos.X() <= smooth_bottom_right.X() &&
				mouse_pos.Y() >= smooth_top_left.Y() && mouse_pos.Y() <= smooth_bottom_right.Y()) {
				PushColorHistory();
				smooth_tolerance_dragging = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
				UpdateSmoothTolerance(mouse_pos);
				UpdateColorTooltip();
				return;
			}
			std::tie(smooth_top_left, smooth_bottom_right) = SmoothAngleBounds();
			if (mouse_pos.X() >= smooth_top_left.X() && mouse_pos.X() <= smooth_bottom_right.X() &&
				mouse_pos.Y() >= smooth_top_left.Y() && mouse_pos.Y() <= smooth_bottom_right.Y()) {
				PushColorHistory();
				smooth_angle_dragging = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
				UpdateSmoothAngle(mouse_pos);
				UpdateColorTooltip();
				return;
			}
		}
		bool pipette_mode = color_selection_mode == VisualSelectionMode::PipetteAdd ||
			color_selection_mode == VisualSelectionMode::PipetteSubtract;
		auto [tolerance_top_left, tolerance_bottom_right] = ColorToleranceBounds();
		if (pipette_mode && has_color_sample && mouse_pos.X() >= tolerance_top_left.X() && mouse_pos.X() <= tolerance_bottom_right.X() &&
			mouse_pos.Y() >= tolerance_top_left.Y() && mouse_pos.Y() <= tolerance_bottom_right.Y()) {
			PushColorHistory();
			tolerance_dragging = true;
			if (!parent->HasCapture()) parent->CaptureMouse();
			UpdateColorTolerance(mouse_pos);
			UpdateColorTooltip();
			return;
		}
		if (CanOffsetSelection()) {
			auto [offset_top_left, offset_bottom_right] = ColorOffsetBounds();
			if (mouse_pos.X() >= offset_top_left.X() && mouse_pos.X() <= offset_bottom_right.X() &&
				mouse_pos.Y() >= offset_top_left.Y() && mouse_pos.Y() <= offset_bottom_right.Y()) {
				PushColorHistory();
				offset_dragging = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
				UpdateColorOffset(mouse_pos);
				UpdateColorTooltip();
				return;
			}
		}
		if (brush_mode && mode != VCLIP_BRUSH) {
			auto [brush_top_left, brush_bottom_right] = ColorBrushBounds();
			if (mouse_pos.X() >= brush_top_left.X() && mouse_pos.X() <= brush_bottom_right.X() &&
				mouse_pos.Y() >= brush_top_left.Y() && mouse_pos.Y() <= brush_bottom_right.Y()) {
				brush_slider_dragging = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
				UpdateColorBrushSize(mouse_pos);
				UpdateColorTooltip();
				return;
			}
		}
		if (hovered_color_action == ColorAction::Undo) UndoColorHistory();
		else if (hovered_color_action == ColorAction::Redo) RedoColorHistory();
		else if (hovered_color_action == ColorAction::SelectionMode) ShowColorModeMenu();
		else if (hovered_color_action == ColorAction::Templates) ShowColorTemplatesMenu();
		else if (hovered_color_action == ColorAction::AISelect) ShowAISelectionMenu();
		else if (hovered_color_action == ColorAction::AutoFill) {
			if (color_contours_dirty) SyncColorSegmenterFromContours();
			PushColorHistory();
			color_auto_fill = !color_auto_fill;
			RefreshColorContours();
		}
		else if (hovered_color_action == ColorAction::SmoothEdges) {
			PushColorHistory();
			color_smooth_edges = !color_smooth_edges;
		}
		else if (hovered_color_action == ColorAction::Accept) AcceptColorContours();
		else if (hovered_color_action == ColorAction::Cancel) {
			if (mode == VCLIP_BRUSH) CloseColorMode();
			else ResetColorSelection();
		}
		else if (mouse_pos.Y() >= ColorTopBarHeight()) {
			Vector2D script = ToScriptCoords(mouse_pos);
			if (color_stage == ColorStage::Range) {
				script = Vector2D(std::clamp(script.X(), 0.f, script_res.X()),
					std::clamp(script.Y(), 0.f, script_res.Y()));
				color_range_start = color_range_end = script;
				color_drawing = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
			}
			else if (color_selection_mode == VisualSelectionMode::PipetteAdd ||
				color_selection_mode == VisualSelectionMode::PipetteSubtract)
				PrepareColorSelection(script);
			else if (brush_mode) {
				if (mode == VCLIP_COLOR) PushColorBrushHistory();
				color_stage = ColorStage::Ready;
				color_brush_drawing = true;
				color_brush_moved = true;
				color_brush_last = mouse_pos;
				color_brush_stroke.clear();
				color_brush_stroke.push_back(mouse_pos);
				if (mode == VCLIP_BRUSH) {
					color_brush_base_spline.assign(spline.begin(), spline.end());
					color_brush_preview_changed = ApplyBrushStroke(color_brush_stroke);
				}
				else {
					PaintSelectionBrush(mouse_pos, mouse_pos);
				}
				if (!parent->HasCapture()) parent->CaptureMouse();
			}
		}
		parent->SetFocus();
	}
	if (color_drawing && (event.Dragging() || event.LeftUp())) {
		Vector2D script = ToScriptCoords(mouse_pos);
		color_range_end = Vector2D(std::clamp(script.X(), 0.f, script_res.X()),
			std::clamp(script.Y(), 0.f, script_res.Y()));
	}
	if (color_drawing && event.LeftUp()) {
		color_drawing = false;
		if ((FromScriptCoords(color_range_end) - FromScriptCoords(color_range_start)).Len() >= 3.f)
		{
			color_stage = ColorStage::Sample;
			EnsureColorSegmenter();
			UpdateColorCursor();
		}
		if (parent->HasCapture()) parent->ReleaseMouse();
	}
	parent->Render();
}

bool VisualToolVectorClip::OnMouseWheel(wxMouseEvent& event) {
	int wheel = event.GetWheelRotation();
	bool brush_mode = mode == VCLIP_BRUSH || (mode == VCLIP_COLOR &&
		(color_selection_mode == VisualSelectionMode::BrushAdd ||
		 color_selection_mode == VisualSelectionMode::BrushSubtract));
	if (brush_mode && event.AltDown() && wheel) {
		mouse_pos = event.GetPosition();
		int wheel_delta = std::max(1, event.GetWheelDelta());
		int steps = wheel / wheel_delta;
		if (!steps) steps = wheel > 0 ? 1 : -1;
		color_brush_radius = std::clamp(color_brush_radius + steps * 2.f, 2.f, 200.f);
		UpdateBrushToolbar();
		parent->Render();
		// false means consumed: do not let VideoDisplay zoom or pan the video.
		return false;
	}
	return VisualTool<VisualToolVectorClipDraggableFeature>::OnMouseWheel(event);
}

bool VisualToolVectorClip::OnKeyEvent(wxKeyEvent& event) {
	int key = event.GetKeyCode();
	bool alt_key = key == WXK_ALT;
#ifdef WXK_RALT
	alt_key = alt_key || key == WXK_RALT;
#endif
	if (mode == VCLIP_COLOR || mode == VCLIP_BRUSH) {
		if (alt_key)
			return true;
		if (mode == VCLIP_COLOR && event.CmdDown() &&
			(key == 'Z' || key == 'Y')) {
			return key == 'Y' || event.ShiftDown() ? RedoColorHistory() : UndoColorHistory();
		}
		if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) &&
			mode == VCLIP_COLOR && !color_contours.empty()) {
			AcceptColorContours();
			return true;
		}
		if (key == WXK_ESCAPE) {
			if (mode == VCLIP_BRUSH && color_brush_drawing) {
				spline.clear();
				spline.insert(spline.end(), color_brush_base_spline.begin(),
					color_brush_base_spline.end());
				MakeFeatures();
				color_brush_drawing = false;
				color_brush_stroke.clear();
				color_brush_base_spline.clear();
				color_brush_preview_changed = false;
				if (parent->HasCapture()) parent->ReleaseMouse();
			}
			if (mode == VCLIP_BRUSH || color_stage == ColorStage::Range) CloseColorMode();
			else ResetColorSelection();
			parent->Render();
			return true;
		}
	}
	if (key == WXK_DELETE || key == WXK_NUMPAD_DELETE)
		return DeleteActivePath();
	return false;
}

size_t VisualToolVectorClip::PathEnd(size_t path_start) const {
	if (path_start >= spline.size() || spline[path_start].type != SplineCurve::POINT)
		return spline.size();
	size_t end = path_start + 1;
	while (end < spline.size() && spline[end].type != SplineCurve::POINT)
		++end;
	return end;
}

std::vector<float> VisualToolVectorClip::PathPoints(size_t path_start) const {
	std::vector<float> points;
	for (size_t i = path_start, end = PathEnd(path_start); i < end; ++i)
		spline[i].GetPoints(points);
	return points;
}

void VisualToolVectorClip::NormalizeActivePath() {
	if (spline.empty()) {
		active_path_start = no_path;
		return;
	}
	if (active_path_start < spline.size() &&
		spline[active_path_start].type == SplineCurve::POINT)
		return;
	for (size_t i = spline.size(); i-- > 0;) {
		if (spline[i].type == SplineCurve::POINT) {
			active_path_start = i;
			return;
		}
	}
	active_path_start = no_path;
}

bool VisualToolVectorClip::SelectPathAt(Vector2D point) {
	for (size_t i = spline.size(); i-- > 0;) {
		if (spline[i].type == SplineCurve::POINT && point_in_polygon(point, PathPoints(i))) {
			active_path_start = i;
			sel_features.clear();
			return true;
		}
	}
	return false;
}

void VisualToolVectorClip::DrawColorMode() {
	float top_bar_height = ColorTopBarHeight();
	wxColour line_colour = to_wx(line_color_primary_opt->GetColor());
	wxColour highlight = to_wx(highlight_color_primary_opt->GetColor());

	std::vector<float> flat;
	std::vector<int> starts, counts;
	std::vector<std::vector<Vector2D>> display_contours = color_contours;
	if (mode == VCLIP_COLOR && color_smooth_edges) {
		display_contours.clear();
		for (auto const& curves : BuildSmoothedColorSplines()) {
			if (curves.empty()) continue;
			auto& contour = display_contours.emplace_back();
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
	}
	for (auto const& contour : display_contours) {
		starts.push_back(static_cast<int>(flat.size() / 2));
		counts.push_back(static_cast<int>(contour.size()));
		for (auto point : contour) {
			Vector2D screen = mode == VCLIP_BRUSH ? point : FromScriptCoords(point);
			flat.push_back(screen.X()); flat.push_back(screen.Y());
		}
	}
	if (mode == VCLIP_BRUSH && !spline.empty()) {
		// Brush edits keep the original curve list and add only local circular
		// paths, so draw that exact list rather than a rasterized reconstruction.
		auto clip_points = spline.GetPointList(starts, counts);
		gl.SetLineColour(line_colour, .95f, 2);
		gl.SetFillColour(line_colour, .18f);
		gl.DrawMultiPolygon(clip_points, starts, counts, video_pos, video_size, false);
	}
	else if (!flat.empty()) {
		gl.SetLineColour(line_colour, .95f, 2);
		gl.SetFillColour(line_colour, .18f);
		gl.DrawMultiPolygon(flat, starts, counts, video_pos, video_size, false);
	}
	if (mode != VCLIP_BRUSH && (color_stage != ColorStage::Range || color_drawing)) {
		Vector2D a = FromScriptCoords(color_range_start);
		Vector2D b = FromScriptCoords(color_range_end);
		Vector2D top_left = a.Min(b), bottom_right = a.Max(b);
		gl.SetLineColour(highlight, 1.f, 2);
		gl.DrawDashedLine(top_left, Vector2D(bottom_right.X(), top_left.Y()), 6.f);
		gl.DrawDashedLine(Vector2D(bottom_right.X(), top_left.Y()), bottom_right, 6.f);
		gl.DrawDashedLine(bottom_right, Vector2D(top_left.X(), bottom_right.Y()), 6.f);
		gl.DrawDashedLine(Vector2D(top_left.X(), bottom_right.Y()), top_left, 6.f);
	}
	bool brush_mode = color_selection_mode == VisualSelectionMode::BrushAdd ||
		color_selection_mode == VisualSelectionMode::BrushSubtract;
	if (brush_mode && color_stage != ColorStage::Range && mouse_pos &&
		mouse_pos.Y() >= top_bar_height) {
		wxColour brush_colour = color_selection_mode == VisualSelectionMode::BrushAdd ?
			wxColour(55, 230, 115) : wxColour(245, 80, 90);
		if (mode == VCLIP_BRUSH && color_brush_drawing && !color_brush_stroke.empty()) {
			gl.SetFillColour(brush_colour, .12f);
			gl.SetLineColour(brush_colour, 0.f, 1);
			float spacing = std::max(2.f, color_brush_radius * .45f);
			gl.DrawCircle(color_brush_stroke.front(), color_brush_radius);
			for (size_t i = 1; i < color_brush_stroke.size(); ++i) {
				Vector2D from = color_brush_stroke[i - 1];
				Vector2D to = color_brush_stroke[i];
				int steps = std::max(1, static_cast<int>(std::ceil((to - from).Len() / spacing)));
				for (int step = 1; step <= steps; ++step)
					gl.DrawCircle(from + (to - from) *
						(static_cast<float>(step) / steps), color_brush_radius);
			}
		}
		gl.SetFillColour(brush_colour, .12f);
		gl.SetLineColour(brush_colour, 1.f, 2);
		gl.DrawCircle(mouse_pos, color_brush_radius);
		gl.DrawCircle(mouse_pos, 2.f);
	}
	if (mode == VCLIP_BRUSH) return;

	gl.SetFillColour(*wxBLACK, .72f);
	gl.SetLineColour(*wxBLACK, 0.f, 1);
	gl.DrawRectangle(Vector2D(0.f, 0.f), Vector2D(canvas_size.X(), top_bar_height));
	auto rounded_rectangle = [&](Vector2D top_left, Vector2D bottom_right,
		float radius, wxColour colour) {
		float safe_radius = std::min({radius, (bottom_right.X() - top_left.X()) * .5f,
			(bottom_right.Y() - top_left.Y()) * .5f});
		gl.SetFillColour(colour, 1.f); gl.SetLineColour(colour, 0.f, 1);
		gl.DrawRectangle(top_left + Vector2D(safe_radius, 0.f), bottom_right - Vector2D(safe_radius, 0.f));
		gl.DrawRectangle(top_left + Vector2D(0.f, safe_radius), bottom_right - Vector2D(0.f, safe_radius));
		gl.DrawCircle(top_left + Vector2D(safe_radius, safe_radius), safe_radius);
		gl.DrawCircle(Vector2D(bottom_right.X() - safe_radius, top_left.Y() + safe_radius), safe_radius);
		gl.DrawCircle(Vector2D(top_left.X() + safe_radius, bottom_right.Y() - safe_radius), safe_radius);
		gl.DrawCircle(bottom_right - Vector2D(safe_radius, safe_radius), safe_radius);
	};
	for (auto action : {ColorAction::Undo, ColorAction::Redo}) {
		auto [top_left, bottom_right] = ColorActionBounds(action);
		bool enabled = action == ColorAction::Undo ? !color_undo_history.empty() :
			!color_redo_history.empty();
		wxColour colour = enabled ? wxColour(55, 59, 64) : wxColour(66, 69, 73);
		if (enabled && hovered_color_action == action)
			colour = colour.ChangeLightness(118);
		rounded_rectangle(top_left, bottom_right, 7.f, colour);
		wxColour content = enabled ? *wxWHITE : wxColour(145, 148, 152);
		gl.SetLineColour(content, 1.f, 3);
		float direction = action == ColorAction::Undo ? -1.f : 1.f;
		Vector2D centre((top_left.X() + bottom_right.X()) * .5f,
			(top_left.Y() + bottom_right.Y()) * .5f);
		Vector2D tip = centre + Vector2D(direction * 7.f, 0.f);
		gl.DrawLine(centre - Vector2D(direction * 7.f, 0.f), tip);
		gl.DrawLine(tip, tip - Vector2D(direction * 5.f, 5.f));
		gl.DrawLine(tip, tip - Vector2D(direction * 5.f, -5.f));
	}

	int text_width, text_height;
	bool pipette_mode = color_selection_mode == VisualSelectionMode::PipetteAdd ||
		color_selection_mode == VisualSelectionMode::PipetteSubtract;
	if (mode == VCLIP_COLOR && pipette_mode && has_color_sample) {
		auto [tolerance_top_left, tolerance_bottom_right] = ColorToleranceBounds();
		rounded_rectangle(tolerance_top_left, tolerance_bottom_right, 7.f, wxColour(55, 59, 64));
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		std::string tolerance_label = from_wx(_("Tolerance"));
		gl_text->GetExtent(tolerance_label, text_width, text_height);
		gl_text->Print(tolerance_label, static_cast<int>(tolerance_top_left.X() + 10.f),
			static_cast<int>((tolerance_top_left.Y() + tolerance_bottom_right.Y() - text_height) * .5f));
		float slider_left = tolerance_top_left.X() + 75.f;
		float slider_right = tolerance_bottom_right.X() - 12.f;
		float slider_y = (tolerance_top_left.Y() + tolerance_bottom_right.Y()) * .5f;
		gl.SetLineColour(wxColour(130, 135, 140), 1.f, 3);
		gl.DrawLine(Vector2D(slider_left, slider_y), Vector2D(slider_right, slider_y));
		float knob_x = slider_left + static_cast<float>(color_tolerance / 20.0) * (slider_right - slider_left);
		gl.SetFillColour(wxColour(80, 220, 255), 1.f);
		gl.DrawCircle(Vector2D(knob_x, slider_y), 5.f);

	}
	if (mode == VCLIP_COLOR && CanOffsetSelection()) {
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		auto [offset_top_left, offset_bottom_right] = ColorOffsetBounds();
		rounded_rectangle(offset_top_left, offset_bottom_right, 7.f, wxColour(55, 59, 64));
		std::string offset_label = from_wx(_("Offset"));
		gl_text->GetExtent(offset_label, text_width, text_height);
		gl_text->Print(offset_label, static_cast<int>(offset_top_left.X() + 10.f),
			static_cast<int>((offset_top_left.Y() + offset_bottom_right.Y() - text_height) * .5f));
		float offset_slider_left = offset_top_left.X() + 60.f;
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
	if (brush_mode) {
		auto [brush_top_left, brush_bottom_right] = ColorBrushBounds();
		rounded_rectangle(brush_top_left, brush_bottom_right, 7.f, wxColour(55, 59, 64));
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		std::string brush_label = from_wx(_("Brush size"));
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
	if (mode == VCLIP_COLOR && color_smooth_edges) {
		auto draw_smooth_slider = [&](std::pair<Vector2D, Vector2D> bounds,
			wxString label, double ratio, float label_width) {
			auto [top_left, bottom_right] = bounds;
			rounded_rectangle(top_left, bottom_right, 7.f, wxColour(55, 59, 64));
			gl_text->SetFont("Verdana", 9, false, false);
			gl_text->SetColour(agi::Color(255, 255, 255, 255));
			std::string text = from_wx(label);
			gl_text->GetExtent(text, text_width, text_height);
			gl_text->Print(text, static_cast<int>(top_left.X() + 10.f),
				static_cast<int>((top_left.Y() + bottom_right.Y() - text_height) * .5f));
			float slider_left = top_left.X() + label_width;
			float slider_right = bottom_right.X() - 12.f;
			float slider_y = (top_left.Y() + bottom_right.Y()) * .5f;
			gl.SetLineColour(wxColour(130, 135, 140), 1.f, 3);
			gl.DrawLine(Vector2D(slider_left, slider_y), Vector2D(slider_right, slider_y));
			gl.SetFillColour(wxColour(80, 220, 255), 1.f);
			gl.DrawCircle(Vector2D(slider_left + static_cast<float>(std::clamp(ratio, 0.0, 1.0)) *
				(slider_right - slider_left), slider_y), 5.f);
		};
		draw_smooth_slider(SmoothToleranceBounds(), _("Smooth tolerance"),
			(color_smooth_tolerance - .1) / 49.9, 125.f);
		draw_smooth_slider(SmoothAngleBounds(),
			_("Angle threshold"), color_smooth_angle / 180.0, 120.f);
	}

	auto label_for = [&](ColorAction action) -> wxString {
		if (action == ColorAction::SelectionMode) {
			switch (color_selection_mode) {
				case VisualSelectionMode::PipetteAdd: return _("Pipette add");
				case VisualSelectionMode::PipetteSubtract: return _("Pipette subtract");
				case VisualSelectionMode::BrushAdd: return _("Brush add");
				case VisualSelectionMode::BrushSubtract: return _("Brush subtract");
			}
		}
		if (action == ColorAction::Templates) return _("Templates");
		if (action == ColorAction::AISelect) return _("AI recognition");
		if (action == ColorAction::AutoFill) return _("Auto fill");
		if (action == ColorAction::SmoothEdges) return _("Smooth edges");
		if (action == ColorAction::Accept)
			return _("Accept (ENTER)");
		return _("Cancel (ESC)");
	};
	std::vector<ColorAction> actions{ColorAction::SelectionMode};
	if (mode == VCLIP_COLOR) {
		actions.push_back(ColorAction::Templates);
		actions.push_back(ColorAction::AISelect);
		if (has_color_sample)
			actions.push_back(ColorAction::AutoFill);
		actions.push_back(ColorAction::SmoothEdges);
		actions.push_back(ColorAction::Accept);
		actions.push_back(ColorAction::Cancel);
	}
	for (auto action : actions) {
		auto [top_left, bottom_right] = ColorActionBounds(action);
		bool enabled = action != ColorAction::Accept || !color_contours.empty() ||
			mode == VCLIP_BRUSH;
		if (action == ColorAction::AutoFill)
			enabled = has_color_sample;
		if (action == ColorAction::SmoothEdges)
			enabled = color_stage != ColorStage::Range && !color_contours.empty();
		if (action == ColorAction::SelectionMode)
			enabled = enabled && color_stage != ColorStage::Range;
		if (action == ColorAction::AISelect)
			enabled = color_stage != ColorStage::Range;
		if (action == ColorAction::Cancel)
			enabled = color_stage != ColorStage::Range;
		wxColour colour = action == ColorAction::Accept ? wxColour(31, 153, 76) :
			action == ColorAction::Cancel ? wxColour(183, 54, 61) : wxColour(55, 59, 64);
		if (action == ColorAction::AISelect) colour = wxColour(180, 105, 43);
		if (action == ColorAction::AutoFill && color_auto_fill)
			colour = wxColour(35, 125, 153);
		if (action == ColorAction::SmoothEdges && color_smooth_edges)
			colour = wxColour(35, 125, 153);
		if (!enabled) colour = wxColour(66, 69, 73);
		else if (hovered_color_action == action) colour = colour.ChangeLightness(118);
		rounded_rectangle(top_left, bottom_right, 7.f, colour);
		wxColour content = enabled ? *wxWHITE : wxColour(145, 148, 152);
		gl.SetLineColour(content, 1.f, 3);
		if (action == ColorAction::SelectionMode || action == ColorAction::Templates) {
			float icon_y = (top_left.Y() + bottom_right.Y()) * .5f;
			gl.SetFillColour(content, 1.f);
			gl.DrawTriangle(Vector2D(bottom_right.X() - 14.f, icon_y - 2.f),
				Vector2D(bottom_right.X() - 6.f, icon_y - 2.f),
				Vector2D(bottom_right.X() - 10.f, icon_y + 3.f));
		}
		gl_text->SetFont("Verdana", 9, true, false);
		gl_text->SetColour(enabled ? agi::Color(255, 255, 255, 255) : agi::Color(145, 148, 152, 255));
		std::string label = from_wx(label_for(action));
		gl_text->GetExtent(label, text_width, text_height);
		gl_text->Print(label, static_cast<int>(top_left.X() + 12.f),
			static_cast<int>((top_left.Y() + bottom_right.Y() - text_height) * .5f));
	}

	auto last = ColorActionBounds(mode == VCLIP_BRUSH ? ColorAction::SelectionMode :
		ColorAction::Cancel);
	wxString information = mode == VCLIP_BRUSH ?
		_("Paint to add to or erase from the current clip.") :
		color_stage == ColorStage::Range ? _("Draw the search range.") :
		color_stage == ColorStage::Sample ?
		_("Click the color to extract inside the range.") :
		agi::wxformat(_("%zu contours found. They will be added to the current clip."), color_contours.size());
	gl_text->SetFont("Verdana", 9, false, false);
	gl_text->SetColour(agi::Color(225, 225, 225, 255));
	std::string info = from_wx(information);
	gl_text->GetExtent(info, text_width, text_height);
	gl_text->Print(info, static_cast<int>(last.second.X() + 16.f),
		static_cast<int>((last.first.Y() + last.second.Y() - text_height) * .5f));
}

void VisualToolVectorClip::Draw() {
	if (!active_line) return;
	if (mode == VCLIP_COLOR || mode == VCLIP_BRUSH) {
		DrawColorMode();
		return;
	}
	if (spline.empty()) return;

	// Parse vector
	std::vector<int> start;
	std::vector<int> count;
	auto points = spline.GetPointList(start, count);
	assert(!start.empty());
	assert(!count.empty());

	// Load colors from options
	wxColour line_color = to_wx(line_color_primary_opt->GetColor());
	wxColour highlight_color_primary = to_wx(highlight_color_primary_opt->GetColor());
	wxColour highlight_color_secondary = to_wx(highlight_color_secondary_opt->GetColor());
	float shaded_alpha = static_cast<float>(shaded_area_alpha_opt->GetDouble());

	gl.SetLineColour(line_color, .5f, 2);
	gl.SetFillColour(*wxBLACK, shaded_alpha);

	// draw the shade over clipped out areas and line showing the clip
	gl.DrawMultiPolygon(points, start, count, video_pos, video_size, !inverse);

	// Never perform contour selection or highlighting work while the mouse is
	// dragging. Drag mode also highlights the contour under the idle pointer;
	// line and bicubic modes only mark the currently active contour.
	bool show_active_path = mode == VCLIP_DRAG || mode == VCLIP_LINE || mode == VCLIP_BICUBIC;
	if (show_active_path && !dragging && !holding) {
		std::vector<size_t> path_starts;
		for (size_t i = 0; i < spline.size(); ++i)
			if (spline[i].type == SplineCurve::POINT)
				path_starts.push_back(i);

		size_t hovered_path = no_path;
		if (mode == VCLIP_DRAG) {
			if (active_feature)
				hovered_path = active_feature->path_start;
			else if (mouse_pos) {
				for (size_t i = std::min(path_starts.size(), count.size()); i-- > 0;) {
					if (point_in_polygon(mouse_pos, points, start[i], count[i])) {
						hovered_path = path_starts[i];
						break;
					}
				}
			}
		}

		auto draw_path_highlight = [&](size_t path, wxColour colour) {
			auto found = std::find(path_starts.begin(), path_starts.end(), path);
			if (found == path_starts.end()) return;
			size_t index = std::distance(path_starts.begin(), found);
			if (index >= start.size() || index >= count.size() || count[index] < 2) return;
			size_t first = static_cast<size_t>(start[index]) * 2;
			size_t point_count = static_cast<size_t>(count[index]);
			gl.SetLineColour(colour, 1.f, 2);
			gl.DrawLineStrip(2, points.data() + first, point_count);
			gl.DrawLine(Vector2D(points[first + (point_count - 1) * 2], points[first + (point_count - 1) * 2 + 1]),
				Vector2D(points[first], points[first + 1]));
		};
		if (hovered_path == active_path_start)
			draw_path_highlight(active_path_start, highlight_color_primary);
		else {
			draw_path_highlight(active_path_start, highlight_color_secondary);
			if (hovered_path != no_path)
				draw_path_highlight(hovered_path, highlight_color_primary);
		}
	}

	// draw direction of the points
	if (OPT_GET("Video/Clip Info")->GetBool()) {
		gl.SetLineColour(to_wx(agi::Color(255, 255, 255)), .5f, 2);

		float arrowLength = 6.0f;
		float arrowWidth  = 2.5f;
		for (auto const& curve : spline) {
			if (curve.type == SplineCurve::LINE) {
				Vector2D dir = curve.p2 - curve.p1;

				float len = std::sqrt(dir.X()*dir.X() + dir.Y()*dir.Y());
				if (len < 1e-6f)
					continue;

				dir = dir * (1.0f / len);

				Vector2D perp(-dir.Y(), dir.X());
				Vector2D tip = (curve.p1 + curve.p2) * 0.5f;
				Vector2D left  = tip - dir * arrowLength + perp * arrowWidth;
				Vector2D right = tip - dir * arrowLength - perp * arrowWidth;

				gl.DrawLine(tip, left);
				gl.DrawLine(tip, right);
			} else if (curve.type == SplineCurve::BICUBIC) {
				float t = 0.5f;
				float u = 1.0f - t;

				Vector2D point = curve.p1 * (u*u*u) + curve.p2 * (3*u*u*t) + curve.p3 * (3*u*t*t) + curve.p4 * (t*t*t);
				Vector2D tangent = (curve.p2 - curve.p1) * (3*u*u) + (curve.p3 - curve.p2) * (6*u*t) + (curve.p4 - curve.p3) * (3*t*t);

				float len = std::sqrt(tangent.X()*tangent.X() + tangent.Y()*tangent.Y());
				if (len < 1e-6f)
					continue;

				tangent = tangent * (1.0f / len);

				Vector2D perp(-tangent.Y(), tangent.X());

				float arrowLength = 6.0f;
				float arrowWidth  = 2.5f;

				Vector2D left  = point - tangent * arrowLength + perp * arrowWidth;
				Vector2D right = point - tangent * arrowLength - perp * arrowWidth;

				gl.DrawLine(point, left);
				gl.DrawLine(point, right);
			}
		}

		gl.SetLineColour(line_color, .5f, 1);
	}

	if ((mode == VCLIP_DRAG || mode == VCLIP_REMOVE) && holding && drag_start && mouse_pos) {
		// Draw drag-select box
		Vector2D top_left = drag_start.Min(mouse_pos);
		Vector2D bottom_right = drag_start.Max(mouse_pos);
		gl.DrawDashedLine(top_left, Vector2D(top_left.X(), bottom_right.Y()), 6);
		gl.DrawDashedLine(Vector2D(top_left.X(), bottom_right.Y()), bottom_right, 6);
		gl.DrawDashedLine(bottom_right, Vector2D(bottom_right.X(), top_left.Y()), 6);
		gl.DrawDashedLine(Vector2D(bottom_right.X(), top_left.Y()), top_left, 6);
	}

	Vector2D closest_point;
	Spline::iterator highlighted_curve = spline.end();
	if (mode == VCLIP_CONVERT || mode == VCLIP_INSERT) {
		float t;
		spline.GetClosestParametricPoint(mouse_pos, highlighted_curve, t, closest_point);
	}

	// Draw highlighted line
	if ((mode == VCLIP_CONVERT || mode == VCLIP_INSERT) && !active_feature && points.size() > 2) {
		auto highlighted_points = spline.GetPointList(highlighted_curve);
		if (!highlighted_points.empty()) {
			gl.SetLineColour(highlight_color_secondary, 1.f, 2);
			gl.DrawLineStrip(2, highlighted_points);
		}
	}

	// Draw lines connecting the bicubic features
	gl.SetLineColour(line_color, 0.9f, 1);
	for (auto const& curve : spline) {
		if (curve.type == SplineCurve::BICUBIC) {
			gl.DrawDashedLine(curve.p1, curve.p2, 6);
			gl.DrawDashedLine(curve.p3, curve.p4, 6);
		}
	}

	// Draw features
	for (auto& feature : features) {
		wxColour feature_color = line_color;
		if (&feature == active_feature)
			feature_color = highlight_color_primary;
		else if (sel_features.count(&feature))
			feature_color = highlight_color_secondary;
		gl.SetFillColour(feature_color, .6f);

		if (feature.type == DRAG_SMALL_SQUARE) {
			gl.SetLineColour(line_color, .5f, 1);
			gl.DrawRectangle(feature.pos - featureSize, feature.pos + featureSize);
		}
		else {
			gl.SetLineColour(feature_color, .5f, 1);
			gl.DrawCircle(feature.pos, featureSize * 2.f / 3.f);
		}

		// draw position of the point
		if (&feature == active_feature && OPT_GET("Video/Clip Info")->GetBool()) {
			wxColour color = to_wx(agi::Color(255, 255, 255));
			agi::Color color2 = agi::Color(255, 255, 255, 175);
			int lineWidth = 11;

			if (feature.type == DRAG_SMALL_SQUARE) {
				lineWidth = 8;
				color = to_wx(agi::Color(160, 160, 160));
				color2 = agi::Color(160, 160, 160, 175);
			}

			gl.SetLineColour(color, .5f, 1);
			gl_text->SetFont("Verdana", 10, false, false);
			gl_text->SetColour(color2);

			Vector2D hlp1 = Vector2D(0.0f, feature.pos);
			Vector2D hlp2 = Vector2D(video_size, feature.pos);
			Vector2D vlp1 = Vector2D(feature.pos, 0.0f);
			Vector2D vlp2 = Vector2D(feature.pos, video_size);
			gl.DrawDashedLine(hlp1, hlp2, lineWidth);
			gl.DrawDashedLine(vlp1, vlp2, lineWidth);

			std::string text = spline.ToScript(feature.pos).Str();
			int tw, th;
			gl_text->GetExtent(text, tw, th);

			// Place the text in the corner of the cross closest to the center of the video
			int dx = feature.pos.X();
			int dy = feature.pos.Y();
			int r = 1;
			if (dx + tw + 14 > canvas_size.X()) {
				dx -= tw + 4;
				r = 0;
			}
			else {
				dx += 4;
			}

			dy -= th + 4;

			if (dy < 4) {
				dy += th + 8;

				if (r == 1) {
					dx += 10;
				}
			}

			gl_text->Print(text, dx, dy);
			gl.SetLineColour(feature_color, .5f, 1);
		}
	}

	// Draw preview of inserted line
	if ((mode == VCLIP_LINE || mode == VCLIP_BICUBIC) && !dragging) {
		if (active_path_start != no_path && mouse_pos) {
			gl.DrawDashedLine(mouse_pos, spline[active_path_start].p1, 6);
			if (!holding) {
				size_t path_end = PathEnd(active_path_start);
				gl.DrawDashedLine(mouse_pos, spline[path_end - 1].EndPoint(), 6);
			}
		}
	}

	// Draw preview of insert point
	if (mode == VCLIP_INSERT)
		gl.DrawCircle(closest_point, 4);

	// Draw preview of insert point
	if (mode == VCLIP_APPEND)
		gl.DrawCircle(mouse_pos, 3);
}

void VisualToolVectorClip::MakeFeature(size_t idx, size_t path_start) {
	auto feat = std::make_unique<Feature>();
	feat->idx = idx;
	feat->path_start = path_start;

	auto const& curve = spline[idx];
	if (curve.type == SplineCurve::POINT) {
		feat->pos = curve.p1;
		feat->type = DRAG_SMALL_CIRCLE;
		feat->point = 0;
	}
	else if (curve.type == SplineCurve::LINE) {
		feat->pos = curve.p2;
		feat->type = DRAG_SMALL_CIRCLE;
		feat->point = 1;
	}
	else if (curve.type == SplineCurve::BICUBIC) {
		// Control points
		feat->pos = curve.p2;
		feat->point = 1;
		feat->type = DRAG_SMALL_SQUARE;
		features.push_back(*feat.release());

		feat = std::make_unique<Feature>();
		feat->idx = idx;
		feat->path_start = path_start;
		feat->pos = curve.p3;
		feat->point = 2;
		feat->type = DRAG_SMALL_SQUARE;
		features.push_back(*feat.release());

		// End point
		feat = std::make_unique<Feature>();
		feat->idx = idx;
		feat->path_start = path_start;
		feat->pos = curve.p4;
		feat->point = 3;
		feat->type = DRAG_SMALL_CIRCLE;
	}
	features.push_back(*feat.release());
}

void VisualToolVectorClip::MakeFeatures() {
	held_curve_features.clear();
	sel_features.clear();
	features.clear();
	active_feature = nullptr;
	size_t path_start = 0;
	for (size_t i = 0; i < spline.size(); ++i) {
		if (spline[i].type == SplineCurve::POINT)
			path_start = i;
		MakeFeature(i, path_start);
	}
	NormalizeActivePath();
}

void VisualToolVectorClip::SyncCurveFeatures(size_t idx) {
	if (idx >= spline.size()) return;
	auto const& curve = spline[idx];
	for (auto *feature : held_curve_features) {
		if (!feature || feature->idx != idx) continue;
		switch (feature->point) {
			case 0: feature->pos = curve.p1; break;
			case 1: feature->pos = curve.p2; break;
			case 2: feature->pos = curve.p3; break;
			case 3: feature->pos = curve.p4; break;
		}
	}
}

void VisualToolVectorClip::Save(int precision_override) {
	if (drawing_mode) {
		if (!active_line) return;
		auto blocks = active_line->ParseTags();
		AssDialogueBlockDrawing *first_drawing = nullptr;
		for (auto& block : blocks) {
			if (block->GetType() != AssBlockType::DRAWING) continue;
			auto drawing = static_cast<AssDialogueBlockDrawing *>(block.get());
			if (!first_drawing) {
				first_drawing = drawing;
				first_drawing->text = EncodeDrawing();
			}
			else {
				drawing->text.clear();
			}
		}
		if (first_drawing)
			active_line->UpdateText(blocks);
		return;
	}

	std::string value = "(";
	if (spline.GetScale() != 1)
		value += std::to_string(spline.GetScale()) + ",";
	int precision = precision_override >= 0 ? precision_override :
		(mode == VCLIP_COLOR || mode == VCLIP_BRUSH ? 1 : 2);
	value += spline.EncodeToAss(precision) + ")";

	for (auto line : c->selectionController->GetSelectedSet()) {
		// This check is technically not correct as it could be outside of an
		// override block... but that's rather unlikely
		bool has_iclip = line->Text.get().find("\\iclip") != std::string::npos;
		SetOverride(line, has_iclip ? "\\iclip" : "\\clip", value);
	}
}

void VisualToolVectorClip::Commit(wxString message) {
	// Encoding the complete spline and committing the subtitle file for every
	// queued mouse-move event makes the handle trail behind the pointer. The
	// visual spline already contains the live geometry, so persist it once when
	// the interaction ends instead.
	if (dragging || holding)
		return;
	Save();
	VisualToolBase::Commit(message);
}

Vector2D VisualToolVectorClip::DrawingToScreen(Vector2D point) const {
	point = point + drawing_alignment_shift;
	point = point * (drawing_scale / 100.f);
	point = point + drawing_pos - drawing_org;
	point = rotate_point(point, -drawing_rotation);
	return FromScriptCoords(point + drawing_org);
}

Vector2D VisualToolVectorClip::ScreenToAlignedDrawing(Vector2D point) const {
	point = ToScriptCoords(point) - drawing_org;
	point = rotate_point(point, drawing_rotation);
	point = point - drawing_pos + drawing_org;
	Vector2D safe_scale(
		std::abs(drawing_scale.X()) < 0.0001f ? 1.f : drawing_scale.X() / 100.f,
		std::abs(drawing_scale.Y()) < 0.0001f ? 1.f : drawing_scale.Y() / 100.f);
	return point / safe_scale;
}

void VisualToolVectorClip::TransformSplineToScreen() {
	for (auto& curve : spline)
		transform_curve(curve, [this](Vector2D point) { return DrawingToScreen(point); });
	// The stored points are screen coordinates from this point on. Keeping the
	// spline scale at one also makes the coordinate tooltip match the display.
	spline.SetScale(1);
}

std::string VisualToolVectorClip::EncodeDrawing() {
	if (spline.empty()) return {};

	std::vector<SplineCurve> local;
	local.reserve(spline.size());
	for (auto const& screen_curve : spline) {
		local.push_back(screen_curve);
		transform_curve(local.back(), [this](Vector2D point) {
			return ScreenToAlignedDrawing(point);
		});
	}

	float left = std::numeric_limits<float>::max();
	float top = std::numeric_limits<float>::max();
	float right = -std::numeric_limits<float>::max();
	float bottom = -std::numeric_limits<float>::max();
	for (auto curve : local) {
		for (auto point : curve.AnchorPoints()) {
			left = std::min(left, point.X());
			top = std::min(top, point.Y());
			right = std::max(right, point.X());
			bottom = std::max(bottom, point.Y());
		}
	}

	Vector2D alignment_shift;
	float width = std::max(right - left, 1.f);
	float height = std::max(bottom - top, 1.f);
	int align = GetLineAlignment(active_line);
	switch ((align - 1) % 3) {
		case 1: alignment_shift = Vector2D(-width / 2.f, alignment_shift); break;
		case 2: alignment_shift = Vector2D(-width, alignment_shift); break;
		default: break;
	}
	switch ((align - 1) / 3) {
		case 0: alignment_shift = Vector2D(alignment_shift, -height); break;
		case 1: alignment_shift = Vector2D(alignment_shift, -height / 2.f); break;
		default: break;
	}

	int coordinate_scale = 1 << std::max(0, drawing_scale_level - 1);
	std::string result;
	char last = 0;
	auto append = [&](Vector2D point) {
		result += ((point - alignment_shift) * coordinate_scale).Str(' ', 1);
	};
	for (auto const& curve : local) {
		switch (curve.type) {
			case SplineCurve::POINT:
				if (last != 'm') { result += "m "; last = 'm'; }
				append(curve.p1);
				break;
			case SplineCurve::LINE:
				if (last != 'l') { result += "l "; last = 'l'; }
				append(curve.p2);
				break;
			case SplineCurve::BICUBIC:
				if (last != 'b') { result += "b "; last = 'b'; }
				append(curve.p2); result += " ";
				append(curve.p3); result += " ";
				append(curve.p4);
				break;
		}
		result += " ";
	}
	return result;
}

void VisualToolVectorClip::UpdateDrag(Feature *feature) {
	spline.MovePoint(spline.begin() + feature->idx, feature->point, feature->pos);
}

bool VisualToolVectorClip::InitializeDrag(Feature *feature) {
	active_path_start = feature->path_start;
	if (mode != VCLIP_REMOVE) {
		drag_commit_pending = true;
		return true;
	}
	drag_commit_pending = false;

	auto curve = spline.begin() + feature->idx;
	if (curve->type == SplineCurve::BICUBIC && (feature->point == 1 || feature->point == 2)) {
		// Deleting bicubic curve handles, so convert to line
		curve->type = SplineCurve::LINE;
		curve->p2 = curve->p4;
	}
	else {
		auto next = std::next(curve);
		if (next != spline.end()) {
			if (curve->type == SplineCurve::POINT) {
				next->p1 = next->EndPoint();
				next->type = SplineCurve::POINT;
			}
			else {
				next->p1 = curve->p1;
			}
		}

		spline.erase(curve);
	}
	active_feature = nullptr;

	MakeFeatures();
	Commit(_("delete control point"));

	return false;
}

void VisualToolVectorClip::EndDrag(Feature *) {
	if (!drag_commit_pending) return;
	drag_commit_pending = false;
	Save();
	VisualToolBase::Commit();
}

void VisualToolVectorClip::OnFrameChanged() {
	if (mode == VCLIP_COLOR) {
		ResetColorSelection();
		parent->Render();
		return;
	}
	if (mode == VCLIP_BRUSH) {
		InitializeBrushSelection();
		parent->Render();
		return;
	}
	if (drawing_mode)
		DoRefresh();
}

void VisualToolVectorClip::OnCoordinateSystemsChanged() {
	if (mode == VCLIP_COLOR || mode == VCLIP_BRUSH) {
		DoRefresh();
		if (mode == VCLIP_BRUSH)
			InitializeBrushSelection();
		if (mode == VCLIP_BRUSH || !color_contours_dirty)
			RefreshColorContours();
		UpdateColorCursor();
		return;
	}
	DoRefresh();
}

void VisualToolVectorClip::OnLineChanged() {
	DoRefresh();
	if (mode == VCLIP_BRUSH) {
		// Rebuild directly for the new line. Toggling tools here loses the
		// brush state and was the reason a newly selected clip stayed invisible.
		InitializeBrushSelection();
		UpdateBrushToolbar();
		parent->Render();
		return;
	}
	parent->Render();
}

void VisualToolVectorClip::OnSubtitleCommit(int type) {
	if (mode != VCLIP_BRUSH || !(type & AssFile::COMMIT_DIAG_TEXT)) return;
	bool subtract = color_selection_mode == VisualSelectionMode::BrushSubtract;
	DoRefresh();
	InitializeBrushSelection();
	color_selection_mode = subtract ? VisualSelectionMode::BrushSubtract :
		VisualSelectionMode::BrushAdd;
	UpdateColorCursor();
	UpdateBrushToolbar();
	parent->Render();
}

bool VisualToolVectorClip::InitializeHold() {
	// Box selection
	if (mode == VCLIP_DRAG) {
		if (SelectPathAt(mouse_pos))
			return false;
		if (ctrl_down)
			box_added = sel_features;
		else
			box_added.clear();

		return true;
	}
	if (mode == VCLIP_REMOVE) {
		sel_features.clear();
		return true;
	}

	// Insert line/bicubic
	if (mode == VCLIP_LINE || mode == VCLIP_BICUBIC) {
		SplineCurve curve;
		NormalizeActivePath();

		// New spline beginning at the clicked point
		if (spline.empty()) {
			curve.p1 = mouse_pos;
			curve.type = SplineCurve::POINT;
			spline.push_back(curve);
			active_path_start = 0;
			held_curve_index = 0;
		}
		else {
			// Continue the selected path, inserting before the following path.
			held_curve_index = PathEnd(active_path_start);
			curve.p1 = spline[held_curve_index - 1].EndPoint();
			curve.type = mode == VCLIP_LINE ? SplineCurve::LINE : SplineCurve::BICUBIC;
			spline.insert(spline.begin() + held_curve_index, curve);
		}

		sel_features.clear();
		MakeFeatures();
		for (auto& feature : features)
			if (feature.idx == held_curve_index)
				held_curve_features.push_back(&feature);
		UpdateHold();
		return true;
	}

	// Convert, insert and append
	if (mode == VCLIP_CONVERT || mode == VCLIP_INSERT || mode == VCLIP_APPEND) {
		// Get closest point
		Vector2D pt;
		Spline::iterator curve;
		float t;
		spline.GetClosestParametricPoint(mouse_pos, curve, t, pt);

		// Convert line <-> bicubic
		if (mode == VCLIP_CONVERT) {
			if (curve != spline.end()) {
				if (curve->type == SplineCurve::LINE) {
					curve->type = SplineCurve::BICUBIC;
					curve->p4 = curve->p2;
					curve->p2 = curve->p1 * 0.75 + curve->p4 * 0.25;
					curve->p3 = curve->p1 * 0.25 + curve->p4 * 0.75;
				}

				else if (curve->type == SplineCurve::BICUBIC) {
					curve->type = SplineCurve::LINE;
					curve->p2 = curve->p4;
				}
			}
		}
		// Append
		else if (mode == VCLIP_APPEND) {
			SplineCurve ct;
			ct.p1 = mouse_pos;
			ct.type = SplineCurve::POINT;

			spline.push_back(ct);
			active_path_start = spline.size() - 1;
		}
		// Insert
		else {
			if (spline.empty()) return false;

			// Split the curve
			if (curve == spline.end()) {
				SplineCurve ct(spline.back().EndPoint(), spline.front().p1);
				ct.p2 = ct.p1 * (1 - t) + ct.p2 * t;
				spline.push_back(ct);
			}
			else {
				std::pair<SplineCurve, SplineCurve> split = curve->Split(t);
				*curve = split.first;
				spline.insert(++curve, split.second);
			}
		}

		MakeFeatures();
		Commit();
		return false;
	}

	// Freehand spline draw
	if (mode == VCLIP_FREEHAND || mode == VCLIP_FREEHAND_SMOOTH) {
		sel_features.clear();
		features.clear();
		active_feature = nullptr;
		spline.clear();
		spline.emplace_back(mouse_pos);
		active_path_start = 0;
		held_curve_index = 0;
		return true;
	}

	// Nothing to do for mode VCLIP_REMOVE
	return false;
}

static bool in_box(Vector2D top_left, Vector2D bottom_right, Vector2D p) {
	return p.X() >= top_left.X()
		&& p.X() <= bottom_right.X()
		&& p.Y() >= top_left.Y()
		&& p.Y() <= bottom_right.Y();
}

void VisualToolVectorClip::UpdateHold() {
	// Box selection
	if ((mode == VCLIP_DRAG || mode == VCLIP_REMOVE) && holding) {
		std::set<Feature *> boxed_features;

		Vector2D p1 = drag_start.Min(mouse_pos);
		Vector2D p2 = drag_start.Max(mouse_pos);

		for (auto& feature : features) {
			if (in_box(p1, p2, feature.pos))
				boxed_features.insert(&feature);
		}

		sel_features = boxed_features;

		if (mode == VCLIP_DRAG && ctrl_down) {
			sel_features = box_added;

			for (auto* feature : boxed_features) {
				if (sel_features.count(feature))
					sel_features.erase(feature);
				else
					sel_features.insert(feature);
			}
		}

		return;
	}

	if (mode == VCLIP_LINE || mode == VCLIP_BICUBIC) {
		if (held_curve_index >= spline.size()) return;
		SplineCurve &curve = spline[held_curve_index];
		if (curve.type == SplineCurve::POINT) {
			curve.p1 = mouse_pos;
		}
		else if (mode == VCLIP_LINE) {
			curve.EndPoint() = mouse_pos;
		}
		else {
			curve.EndPoint() = mouse_pos;
			float len = (curve.p4 - curve.p1).Len();
			Vector2D direction = curve.p4 - curve.p1;
			if (held_curve_index > active_path_start) {
				auto const& previous = spline[held_curve_index - 1];
				if (previous.type == SplineCurve::LINE)
					direction = previous.p2 - previous.p1;
				else if (previous.type == SplineCurve::BICUBIC)
					direction = previous.p4 - previous.p3;
			}
			curve.p2 = direction.Unit() * (0.25f * len) + curve.p1;
			curve.p3 = curve.p1 * 0.25 + curve.p4 * 0.75;
		}
		SyncCurveFeatures(held_curve_index);
	}

	// Freehand
	else if (mode == VCLIP_FREEHAND || mode == VCLIP_FREEHAND_SMOOTH) {
		// See if distance is enough
		Vector2D const& last = spline.back().EndPoint();
		float len = (last - mouse_pos).SquareLen();
		if ((mode == VCLIP_FREEHAND && len >= 900) || (mode == VCLIP_FREEHAND_SMOOTH && len >= 3600)) {
			spline.emplace_back(last, mouse_pos);
			MakeFeature(spline.size() - 1, active_path_start);
		}
	}

	if (mode == VCLIP_CONVERT || mode == VCLIP_INSERT || mode == VCLIP_APPEND)
		return;

	// Smooth spline
	if (!holding && mode == VCLIP_FREEHAND_SMOOTH)
		spline.Smooth();

	// End freedraw
	if (!holding && (mode == VCLIP_FREEHAND || mode == VCLIP_FREEHAND_SMOOTH)) {
		SetSubTool(VCLIP_DRAG);
		MakeFeatures();
	}
}

struct FeatureKey {
	size_t idx;
	int point;

	bool operator<(FeatureKey const& other) const {
		return std::tie(idx, point) < std::tie(other.idx, other.point);
	}
};

void VisualToolVectorClip::DoRefresh() {
	if (!active_line) {
		spline.clear();
		MakeFeatures();
		return;
	}

	if (drawing_mode) {
		auto blocks = active_line->ParseTags();
		std::string drawing_text;
		drawing_scale_level = 1;
		bool found_drawing = false;
		for (auto& block : blocks) {
			if (block->GetType() != AssBlockType::DRAWING) continue;
			auto drawing = static_cast<AssDialogueBlockDrawing *>(block.get());
			if (!found_drawing) {
				drawing_scale_level = std::max(1, drawing->Scale);
				found_drawing = true;
			}
			drawing_text += drawing->text;
		}

		spline.clear();
		if (!found_drawing || drawing_text.empty()) {
			MakeFeatures();
			return;
		}

		spline.SetScale(drawing_scale_level);
		spline.DecodeFromAss(drawing_text);

		auto bbox = GetLineBaseExtents(active_line);
		float width = std::max(bbox.second.X() - bbox.first.X(), 1.f);
		float height = std::max(bbox.second.Y() - bbox.first.Y(), 1.f);
		drawing_alignment_shift = Vector2D();
		int align = GetLineAlignment(active_line);
		switch ((align - 1) % 3) {
			case 1: drawing_alignment_shift = Vector2D(-width / 2.f, drawing_alignment_shift); break;
			case 2: drawing_alignment_shift = Vector2D(-width, drawing_alignment_shift); break;
			default: break;
		}
		switch ((align - 1) / 3) {
			case 0: drawing_alignment_shift = Vector2D(drawing_alignment_shift, -height); break;
			case 1: drawing_alignment_shift = Vector2D(drawing_alignment_shift, -height / 2.f); break;
			default: break;
		}

		drawing_pos = GetLinePosition(active_line);
		Vector2D move_start, move_end;
		int move_t1, move_t2;
		if (GetLineMove(active_line, move_start, move_end, move_t1, move_t2)) {
			if (move_t2 <= move_t1) {
				move_t1 = 0;
				move_t2 = active_line->End - active_line->Start;
			}
			int now = c->videoController->TimeAtFrame(frame_number, agi::vfr::EXACT) - active_line->Start;
			float progress = move_t2 == move_t1 ? 0.f :
				std::clamp((now - move_t1) / static_cast<float>(move_t2 - move_t1), 0.f, 1.f);
			drawing_pos = move_start + (move_end - move_start) * progress;
		}

		drawing_org = GetLineOrigin(active_line);
		if (!drawing_org)
			drawing_org = drawing_pos;
		GetLineScale(active_line, drawing_scale);
		float rotation_x, rotation_y, rotation_z;
		GetLineRotation(active_line, rotation_x, rotation_y, rotation_z);
		drawing_rotation = rotation_z * 3.14159265358979323846f / 180.f;
		TransformSplineToScreen();
		inverse = false;
	}
	else {
		int scale;
		std::string vect = GetLineVectorClip(active_line, scale, inverse);
		spline.SetScale(scale);
		spline.DecodeFromAss(vect);
	}

	NormalizeActivePath();

	std::set<FeatureKey> restore_sel_features;
	for (auto* f : sel_features)
		restore_sel_features.insert({ f->idx, f->point });

	MakeFeatures();

	for (auto& f : features) {
		FeatureKey key { f.idx, f.point };

		if (restore_sel_features.count(key))
			sel_features.insert(&f);
	}
}

void VisualToolVectorClip::EndHold() {
	if (mode != VCLIP_REMOVE) return;

	// Include the final mouse-up position even if the platform did not send a
	// motion event for it.
	Vector2D p1 = drag_start.Min(mouse_pos);
	Vector2D p2 = drag_start.Max(mouse_pos);
	std::set<std::pair<size_t, int>> selected;
	for (auto& feature : features) {
		if (in_box(p1, p2, feature.pos))
			selected.emplace(feature.idx, feature.point);
	}
	if (selected.empty()) {
		sel_features.clear();
		return;
	}

	std::set<size_t, std::greater<size_t>> erase_curves;
	std::set<size_t> line_curves;
	for (auto [idx, point] : selected) {
		if (idx >= spline.size()) continue;
		auto const& curve = spline[idx];
		bool endpoint = (curve.type == SplineCurve::POINT && point == 0)
			|| (curve.type == SplineCurve::LINE && point == 1)
			|| (curve.type == SplineCurve::BICUBIC && point == 3);
		if (endpoint)
			erase_curves.insert(idx);
		else if (curve.type == SplineCurve::BICUBIC && (point == 1 || point == 2))
			line_curves.insert(idx);
	}

	for (size_t idx : line_curves) {
		if (idx >= spline.size() || erase_curves.count(idx)) continue;
		auto& curve = spline[idx];
		curve.type = SplineCurve::LINE;
		curve.p2 = curve.p4;
	}
	for (size_t idx : erase_curves) {
		if (idx >= spline.size()) continue;
		auto curve = spline.begin() + idx;
		auto next = std::next(curve);
		if (next != spline.end()) {
			if (curve->type == SplineCurve::POINT) {
				next->p1 = next->EndPoint();
				next->type = SplineCurve::POINT;
			}
			else
				next->p1 = curve->p1;
		}
		spline.erase(curve);
	}

	active_path_start = no_path;
	sel_features.clear();
	MakeFeatures();
}
