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
	std::string error;
	typesetting::motion::AutoTrackSettings settings;
	settings.track_x = track_x;
	settings.track_y = track_y;
	settings.scale = track_scale;
	settings.rotate = track_rotate;
	settings.linear = linear;
	UpdatePreviewInterface();
	std::optional<typesetting::motion::Track> track;
	{
		wxProgressDialog progress(_("Auto motion"), _("Preparing tracker..."),
			std::max(1, frames - 1), c->parent,
			wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
		track = typesetting::motion::TrackRegion(c, top_left, bottom_right,
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
	Vector2D point(event.GetPosition());
	auto action = ActionAt(point);
	if (action != AutoMotionAction::None || point.Y() < TopBarHeight()) {
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
		UpdatePreviewInterface();
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
		AutoMotionAction::TrackX, AutoMotionAction::TrackY, AutoMotionAction::Scale,
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
		has_region ? _("Tracking region ready. Adjust the motion components or select a new region.") :
		_("Select a stable tracking region, choose the motion components, then accept.");
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
