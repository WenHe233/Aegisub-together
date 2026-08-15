// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "visual_tool_auto_motion.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "command/command.h"
#include "compat.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
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

#include <libaegisub/color.h>

#include <wx/cursor.h>
#include <wx/event.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>

VisualToolAutoMotion::VisualToolAutoMotion(VideoDisplay *parent,
	agi::Context *context, std::string return_tool)
: VisualToolBase(parent, context)
, gl_text(std::make_unique<OpenGLText>())
, return_tool(std::move(return_tool))
{
	selection_connection = context->selectionController->AddSelectionListener(
		[this] { ExitTool(); });
	if (context->parent)
		context->parent->Bind(wxEVT_CHAR_HOOK, &VisualToolAutoMotion::OnCharHook, this);
	parent->SetCursor(wxCursor(wxCURSOR_CROSS));
}

VisualToolAutoMotion::~VisualToolAutoMotion() {
	if (c->parent)
		c->parent->Unbind(wxEVT_CHAR_HOOK, &VisualToolAutoMotion::OnCharHook, this);
	if (parent->HasCapture()) parent->ReleaseMouse();
	parent->SetCursor(wxNullCursor);
}

Vector2D VisualToolAutoMotion::ClampToScript(Vector2D point) const {
	return Vector2D(std::clamp(point.X(), 0.f, script_res.X()),
		std::clamp(point.Y(), 0.f, script_res.Y()));
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

void VisualToolAutoMotion::RunTracking() {
	if (busy || !has_region || !HasOutputComponent()) return;
	Vector2D top_left(std::min(region_start.X(), region_end.X()),
		std::min(region_start.Y(), region_end.Y()));
	Vector2D bottom_right(std::max(region_start.X(), region_end.X()),
		std::max(region_start.Y(), region_end.Y()));
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
	int reference_frame = c->videoController->GetFrameN();
	if (reference_frame < first_frame || reference_frame > last_frame) {
		wxMessageBox(_("The current video frame must be inside the selected lines."),
			_("Auto motion"), wxOK | wxICON_WARNING, c->parent);
		return;
	}
	int frames = last_frame - first_frame + 1;

	busy = true;
	wxProgressDialog progress(_("Auto motion"), _("Preparing tracker..."),
		std::max(1, frames - 1), c->parent,
		wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
	std::string error;
	typesetting::motion::AutoTrackSettings settings;
	settings.track_x = track_x;
	settings.track_y = track_y;
	settings.scale = track_scale;
	settings.rotate = track_rotate;
	settings.perspective = track_perspective;
	auto track = typesetting::motion::TrackRegion(c, top_left, bottom_right,
		first_frame, last_frame, reference_frame, settings,
		[&](int complete, int total) {
			return progress.Update(complete,
				wxString::Format(_("Tracking frame %d of %d"), complete, total));
		}, error);
	if (track) {
		typesetting::motion::ApplyOptions options;
		options.reference_sample = static_cast<size_t>(reference_frame - first_frame);
		if (typesetting::motion::Apply(c, *track, std::nullopt, options, error)) {
			busy = false;
			ExitTool();
			return;
		}
	}
	busy = false;
	if (!error.empty() && error != "Auto motion cancelled.")
		wxMessageBox(to_wx(error), _("Auto motion"), wxOK | wxICON_WARNING, c->parent);
}

void VisualToolAutoMotion::OnMouseEvent(wxMouseEvent& event) {
	if (busy || leaving) return;
	Vector2D point(event.GetPosition());
	auto action = ActionAt(point);
	if (action != AutoMotionAction::None || point.Y() < 70.f) {
		if (hovered_action != action) {
			hovered_action = action;
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
	mouse_pos = event.GetPosition();
	if (event.LeftDown() && !event.LeftDClick()) {
		region_start = region_end = ClampToScript(ToScriptCoords(mouse_pos));
		selecting = true;
		has_region = false;
		if (!parent->HasCapture()) parent->CaptureMouse();
		parent->SetFocus();
	}
	if (selecting && (event.Dragging() || event.LeftUp()))
		region_end = ClampToScript(ToScriptCoords(mouse_pos));
	if (selecting && event.LeftUp()) {
		selecting = false;
		if (parent->HasCapture()) parent->ReleaseMouse();
		has_region = std::abs(region_end.X() - region_start.X()) >= 2.f &&
			std::abs(region_end.Y() - region_start.Y()) >= 2.f;
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
	if (selecting || has_region) {
		Vector2D first = FromScriptCoords(region_start);
		Vector2D second = FromScriptCoords(region_end);
		wxColour colour(255, 72, 72);
		gl.SetFillColour(colour, .12f);
		gl.SetLineColour(colour, 1.f, 2);
		gl.DrawRectangle(first, second);
	}
	DrawTopBar();
}

wxString VisualToolAutoMotion::LabelFor(AutoMotionAction action) const {
	switch (action) {
		case AutoMotionAction::Accept: return _("Accept (ENTER)");
		case AutoMotionAction::Cancel: return _("Cancel (ESC)");
		case AutoMotionAction::TrackX: return _("X");
		case AutoMotionAction::TrackY: return _("Y");
		case AutoMotionAction::Scale: return _("Scale");
		case AutoMotionAction::Rotate: return _("Rotate");
		case AutoMotionAction::Perspective: return _("Perspective");
		default: return {};
	}
}

float VisualToolAutoMotion::MeasuredTextWidth(wxString const& label, bool bold) const {
	gl_text->SetFont("Verdana", 9, bold, false);
	int width = 0, height = 0;
	gl_text->GetExtent(from_wx(label), width, height);
	return static_cast<float>(width);
}

std::pair<Vector2D, Vector2D> VisualToolAutoMotion::ActionBounds(
	AutoMotionAction action) const {
	constexpr float top = 10.f;
	constexpr float height = 34.f;
	constexpr float gap = 8.f;
	float left = 12.f;
	for (auto item : {AutoMotionAction::Accept, AutoMotionAction::Cancel}) {
		float width = MeasuredTextWidth(LabelFor(item), true) + 24.f;
		if (item == action) return {Vector2D(left, top), Vector2D(left + width, top + height)};
		left += width + gap;
	}
	left += 10.f;
	for (auto item : {AutoMotionAction::TrackX, AutoMotionAction::TrackY,
		AutoMotionAction::Scale, AutoMotionAction::Rotate, AutoMotionAction::Perspective}) {
		float width = MeasuredTextWidth(LabelFor(item), false) + 28.f;
		if (item == action) return {Vector2D(left, top), Vector2D(left + width, top + height)};
		left += width + gap;
	}
	return {Vector2D(left, top), Vector2D(left, top + height)};
}

bool VisualToolAutoMotion::ActionEnabled(AutoMotionAction action) const {
	if (busy || leaving) return false;
	if (action == AutoMotionAction::Accept) return has_region && HasOutputComponent();
	return action != AutoMotionAction::None;
}

bool VisualToolAutoMotion::HasOutputComponent() const {
	return track_x || track_y || track_scale || track_rotate || track_perspective;
}

bool VisualToolAutoMotion::ActionChecked(AutoMotionAction action) const {
	switch (action) {
		case AutoMotionAction::TrackX: return track_x;
		case AutoMotionAction::TrackY: return track_y;
		case AutoMotionAction::Scale: return track_scale;
		case AutoMotionAction::Rotate: return track_rotate;
		case AutoMotionAction::Perspective: return track_perspective;
		default: return false;
	}
}

AutoMotionAction VisualToolAutoMotion::ActionAt(Vector2D point) const {
	for (auto action : {AutoMotionAction::Accept, AutoMotionAction::Cancel,
		AutoMotionAction::TrackX, AutoMotionAction::TrackY, AutoMotionAction::Scale,
		AutoMotionAction::Rotate, AutoMotionAction::Perspective}) {
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
		case AutoMotionAction::Perspective: track_perspective = !track_perspective; break;
		default: return;
	}
	parent->Render();
}

void VisualToolAutoMotion::DrawTopBar() {
	gl.SetFillColour(*wxBLACK, .72f);
	gl.SetLineColour(*wxBLACK, 0.f, 1);
	gl.DrawRectangle(Vector2D(0.f, 0.f), Vector2D(canvas_size.X(), 70.f));

	auto rounded_rectangle = [&](Vector2D top_left, Vector2D bottom_right,
		float radius, wxColour colour) {
		float safe = std::min({radius, (bottom_right.X() - top_left.X()) * .5f,
			(bottom_right.Y() - top_left.Y()) * .5f});
		gl.SetFillColour(colour, 1.f);
		gl.SetLineColour(colour, 0.f, 1);
		gl.DrawRectangle(top_left + Vector2D(safe, 0), bottom_right - Vector2D(safe, 0));
		gl.DrawRectangle(top_left + Vector2D(0, safe), bottom_right - Vector2D(0, safe));
		gl.DrawCircle(top_left + Vector2D(safe, safe), safe);
		gl.DrawCircle(Vector2D(bottom_right.X() - safe, top_left.Y() + safe), safe);
		gl.DrawCircle(Vector2D(top_left.X() + safe, bottom_right.Y() - safe), safe);
		gl.DrawCircle(bottom_right - Vector2D(safe, safe), safe);
	};

	for (auto action : {AutoMotionAction::Accept, AutoMotionAction::Cancel}) {
		auto [top_left, bottom_right] = ActionBounds(action);
		bool enabled = ActionEnabled(action);
		wxColour colour = action == AutoMotionAction::Accept ?
			wxColour(31, 153, 76) : wxColour(183, 54, 61);
		if (!enabled) colour = wxColour(66, 69, 73);
		else if (hovered_action == action) colour = colour.ChangeLightness(118);
		rounded_rectangle(top_left, bottom_right, 7.f, colour);
		gl_text->SetFont("Verdana", 9, true, false);
		gl_text->SetColour(enabled ? agi::Color(255, 255, 255, 255) :
			agi::Color(145, 148, 152, 255));
		std::string text = from_wx(LabelFor(action));
		int width = 0, height = 0;
		gl_text->GetExtent(text, width, height);
		gl_text->Print(text, static_cast<int>(top_left.X() + 12.f),
			static_cast<int>((top_left.Y() + bottom_right.Y() - height) * .5f));
	}

	for (auto action : {AutoMotionAction::TrackX, AutoMotionAction::TrackY,
		AutoMotionAction::Scale, AutoMotionAction::Rotate, AutoMotionAction::Perspective}) {
		auto [top_left, bottom_right] = ActionBounds(action);
		float middle = (top_left.Y() + bottom_right.Y()) * .5f;
		Vector2D mark(top_left.X() + 8.f, middle);
		gl.SetLineColour(hovered_action == action ? *wxWHITE : wxColour(190, 194, 198),
			1.f, 1);
		gl.SetFillColour(*wxBLACK, .45f);
		gl.DrawRectangle(mark - Vector2D(7, 7), mark + Vector2D(7, 7));
		if (ActionChecked(action)) {
			gl.SetLineColour(wxColour(120, 220, 140), 1.f, 2);
			gl.DrawLine(mark + Vector2D(-4, 0), mark + Vector2D(-1, 4));
			gl.DrawLine(mark + Vector2D(-1, 4), mark + Vector2D(5, -4));
		}
		gl_text->SetFont("Verdana", 9, false, false);
		gl_text->SetColour(agi::Color(230, 232, 235, 255));
		std::string label = from_wx(LabelFor(action));
		int width = 0, height = 0;
		gl_text->GetExtent(label, width, height);
		gl_text->Print(label, static_cast<int>(top_left.X() + 20.f),
			static_cast<int>((top_left.Y() + bottom_right.Y() - height) * .5f));
	}

	gl_text->SetFont("Verdana", 9, false, false);
	gl_text->SetColour(agi::Color(220, 223, 226, 255));
	wxString status = has_region && !HasOutputComponent() ?
		_("Select at least one motion component.") : has_region ?
		_("Tracking region ready. Adjust the motion components or select a new region.") :
		_("Select a stable tracking region, choose the motion components, then accept.");
	std::string message = from_wx(status);
	gl_text->Print(message, 12, 50);
}

bool VisualToolAutoMotion::HandleKey(int key) {
	if (busy || leaving) return false;
	if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
		Perform(AutoMotionAction::Accept);
		return true;
	}
	if (key == WXK_ESCAPE) {
		Perform(AutoMotionAction::Cancel);
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
