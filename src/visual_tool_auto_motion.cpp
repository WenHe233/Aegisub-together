// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "visual_tool_auto_motion.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "command/command.h"
#include "compat.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "typesetting_auto_motion.h"
#include "typesetting_motion.h"
#include "video_controller.h"
#include "video_display.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>

#include <libaegisub/color.h>

#include <wx/cursor.h>
#include <wx/event.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>

namespace {

double PolygonArea(std::vector<Vector2D> const& points) {
	if (points.size() < 3) return 0;
	double area = 0;
	for (size_t index = 0; index < points.size(); ++index)
		area += points[index].Cross(points[(index + 1) % points.size()]);
	return area * .5;
}

} // namespace

VisualToolAutoMotion::VisualToolAutoMotion(VideoDisplay *parent,
	agi::Context *context, std::string return_tool)
: VisualToolBase(parent, context)
, gl_text(std::make_unique<OpenGLText>())
, featureSize(OPT_GET("Tool/Visual/Shape Handle Size")->GetInt())
, return_tool(std::move(return_tool))
{
	selection_connection = context->selectionController->AddSelectionListener(
		[this] { ExitTool(); });
	if (context->parent)
		context->parent->Bind(wxEVT_CHAR_HOOK, &VisualToolAutoMotion::OnCharHook, this);
	preview_interface.AttachHost(parent->GetPreviewBar(), [this](int id) {
		this->parent->SetFocus();
		Perform(static_cast<AutoMotionAction>(id));
	});
	parent->SetCursor(wxCursor(wxCURSOR_CROSS));
	AsyncVideoProvider::SetDisplaySubtitlesSuppressed(true);
	subtitles_suppressed = true;
	if (auto provider = context->project->VideoProvider())
		provider->ResetCurrentFrame();
	UpdatePreviewInterface();
}

VisualToolAutoMotion::~VisualToolAutoMotion() {
	if (subtitles_suppressed) {
		AsyncVideoProvider::SetDisplaySubtitlesSuppressed(false);
		if (auto provider = c->project->VideoProvider())
			provider->ResetCurrentFrame();
	}
	if (c->parent)
		c->parent->Unbind(wxEVT_CHAR_HOOK, &VisualToolAutoMotion::OnCharHook, this);
	if (parent->HasCapture()) parent->ReleaseMouse();
	parent->SetCursor(wxNullCursor);
}

void VisualToolAutoMotion::ExitTool() {
	if (busy || leaving) return;
	leaving = true;
	if (parent->HasCapture()) parent->ReleaseMouse();
	if (parent->IsBeingDeleted()) return;
	agi::Context *context = c;
	std::string command = return_tool.empty() ? "video/tool/cross" : return_tool;
	parent->CallAfter([context, command = std::move(command)] {
		if (!context->videoDisplay || context->videoDisplay->IsBeingDeleted() ||
			!context->project->VideoProvider()) return;
		cmd::call(command, context);
	});
}

void VisualToolAutoMotion::ResetRegion() {
	if (parent->HasCapture()) parent->ReleaseMouse();
	hovered_point = -1;
	dragged_point = -1;
	has_region = false;
	region.clear();
	region_reference_frame = -1;
	UpdatePreviewInterface();
	parent->Render();
}

void VisualToolAutoMotion::UpdateRegionValidity() {
	std::vector<Vector2D> screen_region;
	screen_region.reserve(region.size());
	for (auto region_point : region)
		screen_region.push_back(FromScriptCoords(region_point));
	has_region = region.size() >= 3 && std::abs(PolygonArea(screen_region)) >= 16.0;
	UpdatePreviewInterface();
}

int VisualToolAutoMotion::PointAt(Vector2D point) const {
	float reach = std::max(6.f, featureSize + 2.f);
	for (int index = static_cast<int>(region.size()) - 1; index >= 0; --index)
		if ((FromScriptCoords(region[static_cast<size_t>(index)]) - point).Len() <= reach)
			return index;
	return -1;
}

typesetting::motion::AutoTrackDirection VisualToolAutoMotion::TrackingDirection(
	int first_frame, int last_frame) const {
	if (region_reference_frame <= first_frame)
		return typesetting::motion::AutoTrackDirection::Forward;
	if (region_reference_frame >= last_frame)
		return typesetting::motion::AutoTrackDirection::Backward;
	return typesetting::motion::AutoTrackDirection::Both;
}

int VisualToolAutoMotion::TrackingSteps(int first_frame, int last_frame) const {
	auto direction = TrackingDirection(first_frame, last_frame);
	int backward = region_reference_frame - first_frame;
	int forward = last_frame - region_reference_frame;
	switch (direction) {
		case typesetting::motion::AutoTrackDirection::Backward: return backward;
		case typesetting::motion::AutoTrackDirection::Forward: return forward;
		case typesetting::motion::AutoTrackDirection::Both: return backward + forward;
	}
	return 0;
}

void VisualToolAutoMotion::RunTracking() {
	if (busy || !has_region || !HasOutputComponent()) return;
	auto selected = c->selectionController->GetSelectedSet();
	if (selected.empty()) return;
	int first_frame = std::numeric_limits<int>::max();
	int last_frame = 0;
	for (auto line : selected) {
		first_frame = std::min(first_frame,
			c->videoController->FrameAtTime(line->Start, agi::vfr::START));
		last_frame = std::max(last_frame,
			c->videoController->FrameAtTime(line->End, agi::vfr::END));
	}
	int reference_frame = region_reference_frame;
	if (reference_frame < first_frame || reference_frame > last_frame) {
		wxMessageBox(_("The frame on which the tracking area was drawn must be inside the selected lines."),
			_("Auto motion"), wxOK | wxICON_WARNING, c->parent);
		return;
	}

	busy = true;
	std::string error;
	typesetting::motion::AutoTrackSettings settings;
	settings.track_x = track_x;
	settings.track_y = track_y;
	settings.scale = track_scale;
	settings.rotate = track_rotate;
	settings.linear = linear;
	settings.direction = TrackingDirection(first_frame, last_frame);
	UpdatePreviewInterface();
	std::optional<typesetting::motion::Track> track;
	{
		wxProgressDialog progress(_("Auto motion"), _("Preparing tracker..."),
			std::max(1, TrackingSteps(first_frame, last_frame)), c->parent,
			wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
		track = typesetting::motion::TrackRegion(c, region,
			first_frame, last_frame, reference_frame, settings,
			[&](int complete, int total) {
				return progress.Update(complete,
					wxString::Format(_("Tracking frame %d of %d"), complete, total));
			}, error);
	}
	if (track) {
		typesetting::motion::ApplyOptions options;
		options.reference_sample = static_cast<size_t>(reference_frame - first_frame);
		options.main = {track_x, track_y, track_scale, track_rotate, false};
		options.linear = linear;
		wxProgressDialog progress(_("Auto motion"), _("Preparing motion..."),
			1000, c->parent, wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);
		int last_progress_value = -1;
		int last_progress_stage = -1;
		auto update_progress = [&](typesetting::motion::ApplyProgressStage stage,
			size_t complete, size_t total) {
			total = std::max<size_t>(1, total);
			int value = 0;
			wxString message;
			switch (stage) {
				case typesetting::motion::ApplyProgressStage::Preparing:
					value = static_cast<int>(complete * 50 / total);
					message = _("Preparing motion...");
					break;
				case typesetting::motion::ApplyProgressStage::Applying:
					value = 50 + static_cast<int>(complete * 800 / total);
					message = wxString::Format(_("Applying frame %zu of %zu"),
						complete, total);
					break;
				case typesetting::motion::ApplyProgressStage::Writing:
					value = 850 + static_cast<int>(complete * 150 / total);
					message = wxString::Format(_("Writing subtitle row %zu of %zu"),
						complete, total);
					break;
			}
			int stage_number = static_cast<int>(stage);
			if (value == last_progress_value && stage_number == last_progress_stage &&
				complete < total) return;
			last_progress_value = value;
			last_progress_stage = stage_number;
			progress.Update(std::clamp(value, 0, 1000), message);
		};
		if (typesetting::motion::Apply(c, *track, std::nullopt, std::nullopt,
			std::nullopt, options, error, update_progress)) {
			busy = false;
			ExitTool();
			return;
		}
	}
	busy = false;
	UpdatePreviewInterface();
	if (!error.empty() && error != "Auto motion cancelled.")
		wxMessageBox(to_wx(error), _("Auto motion"), wxOK | wxICON_WARNING, c->parent);
}

void VisualToolAutoMotion::OnMouseEvent(wxMouseEvent& event) {
	if (busy || leaving) return;
	mouse_pos = event.GetPosition();
	if (dragged_point >= 0 && (event.Dragging() || event.LeftUp())) {
		if (static_cast<size_t>(dragged_point) < region.size())
			region[static_cast<size_t>(dragged_point)] = ToScriptCoords(mouse_pos);
		UpdateRegionValidity();
		if (event.LeftUp()) {
			dragged_point = -1;
			if (parent->HasCapture()) parent->ReleaseMouse();
			hovered_point = PointAt(mouse_pos);
			parent->SetFocus();
		}
		parent->Render();
		return;
	}
	auto action = ActionAt(mouse_pos);
	if (action != AutoMotionAction::None || mouse_pos.Y() < TopBarHeight()) {
		if (hovered_action != action) {
			hovered_action = action;
			hovered_point = -1;
			parent->Render();
		}
		if (event.LeftDown() && action != AutoMotionAction::None)
			Perform(action);
		return;
	}
	if (hovered_action != AutoMotionAction::None) {
		hovered_action = AutoMotionAction::None;
		parent->Render();
	}
	int point = PointAt(mouse_pos);
	if (hovered_point != point) {
		hovered_point = point;
		parent->Render();
	}
	if (event.LeftDown() && !event.LeftDClick()) {
		if (hovered_point >= 0) {
			dragged_point = hovered_point;
			if (!parent->HasCapture()) parent->CaptureMouse();
		}
		else {
			if (region.empty())
				region_reference_frame = c->videoController->GetFrameN();
			region.push_back(ToScriptCoords(mouse_pos));
			hovered_point = static_cast<int>(region.size()) - 1;
			UpdateRegionValidity();
		}
		parent->SetFocus();
	}
	parent->Render();
}

bool VisualToolAutoMotion::OnMouseWheel(wxMouseEvent&) {
	return true;
}

bool VisualToolAutoMotion::OnKeyEvent(wxKeyEvent& event) {
	return HandleKey(event.GetKeyCode());
}

void VisualToolAutoMotion::Draw() {
	if (!region.empty()) {
		std::vector<float> points;
		points.reserve(region.size() * 2 + 2);
		for (auto region_point : region) {
			auto screen = FromScriptCoords(region_point);
			points.push_back(screen.X());
			points.push_back(screen.Y());
		}
		wxColour colour(255, 72, 72);
		gl.SetLineColour(colour, 1.f, 2);
		if (has_region) {
			std::vector<int> starts{0};
			std::vector<int> counts{static_cast<int>(region.size())};
			gl.SetFillColour(colour, .12f);
			gl.DrawMultiPolygon(points, starts, counts, video_pos, video_size, false);
		}
		if (region.size() >= 2) {
			points.push_back(points[0]);
			points.push_back(points[1]);
			gl.DrawLineStrip(2, points);
		}
		for (size_t index = 0; index < region.size(); ++index) {
			bool highlighted = static_cast<int>(index) == hovered_point ||
				static_cast<int>(index) == dragged_point;
			wxColour point_colour = highlighted ? wxColour(255, 218, 72) : colour;
			gl.SetFillColour(point_colour, highlighted ? 1.f : .85f);
			gl.SetLineColour(highlighted ? *wxWHITE : colour, 1.f,
				highlighted ? 2 : 1);
			gl.DrawCircle(FromScriptCoords(region[index]), featureSize * 2.f / 3.f);
		}
		if (dragged_point < 0 && mouse_pos.Y() >= TopBarHeight() &&
			ActionAt(mouse_pos) == AutoMotionAction::None) {
			gl.SetLineColour(colour, .85f, 2);
			gl.DrawDashedLine(mouse_pos, FromScriptCoords(region.back()), 6.f);
			if (region.size() > 1)
				gl.DrawDashedLine(mouse_pos, FromScriptCoords(region.front()), 6.f);
		}
	}
	DrawTopBar();
}

wxString VisualToolAutoMotion::LabelFor(AutoMotionAction action) const {
	switch (action) {
		case AutoMotionAction::Accept: return _("Accept (ENTER)");
		case AutoMotionAction::Cancel: return _("Cancel");
		case AutoMotionAction::TrackX: return _("X");
		case AutoMotionAction::TrackY: return _("Y");
		case AutoMotionAction::Scale: return _("Scale");
		case AutoMotionAction::Rotate: return _("Rotate");
		case AutoMotionAction::Linear: return _("Linear");
		default: return {};
	}
}

std::pair<Vector2D, Vector2D> VisualToolAutoMotion::ActionBounds(
	AutoMotionAction action) const {
	UpdatePreviewInterface();
	return preview_interface.BoundsFor(static_cast<int>(action), *gl_text, canvas_size);
}

float VisualToolAutoMotion::TopBarHeight() const {
	UpdatePreviewInterface();
	if (preview_interface.HasExternalHost()) return 0.f;
	return preview_interface.Height(*gl_text, canvas_size);
}

bool VisualToolAutoMotion::ActionEnabled(AutoMotionAction action) const {
	if (busy || leaving) return false;
	if (action == AutoMotionAction::Accept) return has_region && HasOutputComponent();
	return action != AutoMotionAction::None;
}

bool VisualToolAutoMotion::HasOutputComponent() const {
	return track_x || track_y || track_scale || track_rotate;
}

bool VisualToolAutoMotion::ActionChecked(AutoMotionAction action) const {
	switch (action) {
		case AutoMotionAction::TrackX: return track_x;
		case AutoMotionAction::TrackY: return track_y;
		case AutoMotionAction::Scale: return track_scale;
		case AutoMotionAction::Rotate: return track_rotate;
		case AutoMotionAction::Linear: return linear;
		default: return false;
	}
}

AutoMotionAction VisualToolAutoMotion::ActionAt(Vector2D point) const {
	UpdatePreviewInterface();
	if (preview_interface.HasExternalHost()) return AutoMotionAction::None;
	for (auto action : {AutoMotionAction::Accept, AutoMotionAction::Cancel,
		AutoMotionAction::TrackX,
		AutoMotionAction::TrackY, AutoMotionAction::Scale,
		AutoMotionAction::Rotate, AutoMotionAction::Linear}) {
		if (!ActionEnabled(action)) continue;
		auto [top_left, bottom_right] = ActionBounds(action);
		if (point.X() >= top_left.X() && point.X() <= bottom_right.X() &&
			point.Y() >= top_left.Y() && point.Y() <= bottom_right.Y())
			return action;
	}
	return AutoMotionAction::None;
}

void VisualToolAutoMotion::Perform(AutoMotionAction action) {
	if (!ActionEnabled(action)) return;
	switch (action) {
		case AutoMotionAction::Accept: RunTracking(); return;
		case AutoMotionAction::Cancel: ExitTool(); return;
		case AutoMotionAction::TrackX: track_x = !track_x; break;
		case AutoMotionAction::TrackY: track_y = !track_y; break;
		case AutoMotionAction::Scale: track_scale = !track_scale; break;
		case AutoMotionAction::Rotate: track_rotate = !track_rotate; break;
		case AutoMotionAction::Linear: linear = !linear; break;
		default: return;
	}
	UpdatePreviewInterface();
	parent->Render();
}

void VisualToolAutoMotion::UpdatePreviewInterface() const {
	using Interface = VisualToolPreviewInterface;
	Interface::Page page;
	auto add = [&](AutoMotionAction action, Interface::ControlKind kind,
		Interface::ControlStyle style = Interface::ControlStyle::Neutral) {
		Interface::Control control;
		control.id = static_cast<int>(action);
		control.kind = kind;
		control.label = LabelFor(action);
		control.style = style;
		control.enabled = ActionEnabled(action);
		control.selected = ActionChecked(action);
		page.controls.push_back(std::move(control));
	};
	add(AutoMotionAction::Accept, Interface::ControlKind::Button,
		Interface::ControlStyle::Accept);
	add(AutoMotionAction::Cancel, Interface::ControlKind::Button,
		Interface::ControlStyle::Cancel);
	page.controls.push_back({0, Interface::ControlKind::Spacer});
	for (auto action : {AutoMotionAction::TrackX, AutoMotionAction::TrackY,
		AutoMotionAction::Scale, AutoMotionAction::Rotate, AutoMotionAction::Linear})
		add(action, Interface::ControlKind::Toggle);
	page.message = busy ? _("Tracking the selected region...") :
		has_region && !HasOutputComponent() ? _("Select at least one motion component.") :
		has_region ? wxString::Format(
			_("Tracking area ready on frame %d. The direction is automatic; press Esc to redraw."),
			region_reference_frame + 1) :
		region.empty() ? _("Add points around stable details on the reference frame.") :
		_("Add at least three points to define the tracking area.");
	preview_interface.SetPage(std::move(page));
}

void VisualToolAutoMotion::DrawTopBar() {
	UpdatePreviewInterface();
	if (preview_interface.HasExternalHost()) return;
	preview_interface.Draw(gl, *gl_text, canvas_size, static_cast<int>(hovered_action));
}

bool VisualToolAutoMotion::HandleKey(int key) {
	if (busy || leaving) return false;
	if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
		Perform(AutoMotionAction::Accept);
		return true;
	}
	if (key == WXK_ESCAPE) {
		ResetRegion();
		return true;
	}
	return false;
}

void VisualToolAutoMotion::OnCharHook(wxKeyEvent& event) {
	if (!HandleKey(event.GetKeyCode())) event.Skip();
}

void VisualToolAutoMotion::OnLineChanged() {
	ExitTool();
}
