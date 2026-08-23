// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

/// @file dialog_glitch.cpp
/// @brief Previewed editor for deterministic ASS glitch slices

#include "dialog_glitch.h"

#include "ass_dialogue.h"
#include "command/command.h"
#include "compat.h"
#include "dialogs.h"
#include "include/aegisub/context.h"
#include "selection_controller.h"
#include "typesetting_glitch.h"
#include "video_controller.h"
#include "video_display.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>

#include <wx/button.h>
#include <wx/bmpbuttn.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/dcbuffer.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/timer.h>

namespace {

using typesetting::glitch::Animation;
using typesetting::glitch::Settings;
using typesetting::glitch::Values;

class AngleDial final : public wxPanel {
	enum class MeasureState { Inactive, Armed, Dragging };

	agi::Context *context;
	int value;
	int value_before_click;
	bool dragging = false;
	MeasureState measure_state = MeasureState::Inactive;
	wxPoint measure_start;
	std::function<void(int, bool)> changed;

	void SetMeasuredValue(wxPoint end) {
		if (end == measure_start) return;
		double degrees = std::atan2(end.y - measure_start.y, end.x - measure_start.x) *
			180.0 / 3.14159265358979323846;
		int next = static_cast<int>(std::lround(degrees));
		if (next < 0) next += 360;
		if (next == value) return;
		value = next;
		Refresh();
		changed(value, false);
	}

	void ResetMeasurement(bool release_capture) {
		measure_state = MeasureState::Inactive;
		if (context->videoDisplay) context->videoDisplay->ClearAngleMeasurementLine();
		SetCursor(wxNullCursor);
		Refresh();
		if (release_capture && HasCapture()) ReleaseMouse();
	}

	void OnPaint(wxPaintEvent&) {
		wxAutoBufferedPaintDC dc(this);
		dc.SetBackground(wxBrush(GetBackgroundColour()));
		dc.Clear();
		wxSize size = GetClientSize();
		wxPoint centre(size.x / 2, size.y / 2);
		int radius = std::max(2, std::min(size.x, size.y) / 2 - FromDIP(3));
		wxColour edge = measure_state == MeasureState::Inactive ?
			wxColour(9, 38, 55) : wxColour(53, 180, 22);
		dc.SetPen(wxPen(edge, FromDIP(2)));
		dc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
		dc.DrawCircle(centre, radius);
		double radians = value * 3.14159265358979323846 / 180.0;
		wxPoint marker(centre.x + std::lround(std::cos(radians) * radius * .62),
			centre.y + std::lround(std::sin(radians) * radius * .62));
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(edge));
		dc.DrawCircle(marker, FromDIP(3));
	}

	void UpdateFrom(wxPoint point) {
		wxSize size = GetClientSize();
		double degrees = std::atan2(point.y - size.y / 2.0, point.x - size.x / 2.0) *
			180.0 / 3.14159265358979323846;
		int next = static_cast<int>(std::lround(degrees));
		if (next < 0) next += 360;
		if (next == value) return;
		value = next;
		Refresh();
		changed(value, false);
	}

	void OnLeftDown(wxMouseEvent& event) {
		if (measure_state == MeasureState::Armed) {
			wxPoint point = ClientToScreen(event.GetPosition());
			measure_start = point;
			measure_state = MeasureState::Dragging;
			if (context->videoDisplay)
				context->videoDisplay->SetAngleMeasurementLine(point, point);
			Refresh();
			return;
		}
		if (measure_state == MeasureState::Dragging) return;
		value_before_click = value;
		dragging = true;
		CaptureMouse();
		UpdateFrom(event.GetPosition());
	}

	void OnMotion(wxMouseEvent& event) {
		if (measure_state == MeasureState::Dragging && event.Dragging() && event.LeftIsDown()) {
			wxPoint point = ClientToScreen(event.GetPosition());
			if (context->videoDisplay)
				context->videoDisplay->SetAngleMeasurementLine(measure_start, point);
			SetMeasuredValue(point);
			return;
		}
		if (dragging && event.Dragging() && event.LeftIsDown()) UpdateFrom(event.GetPosition());
	}

	void OnLeftUp(wxMouseEvent& event) {
		if (measure_state == MeasureState::Dragging) {
			wxPoint point = ClientToScreen(event.GetPosition());
			if (context->videoDisplay)
				context->videoDisplay->SetAngleMeasurementLine(measure_start, point);
			SetMeasuredValue(point);
			ResetMeasurement(true);
			changed(value, true);
			return;
		}
		if (measure_state == MeasureState::Armed || !dragging) return;
		UpdateFrom(event.GetPosition());
		dragging = false;
		if (HasCapture()) ReleaseMouse();
		changed(value, true);
	}

	void OnDoubleClick(wxMouseEvent&) {
		if (measure_state != MeasureState::Inactive) {
			ResetMeasurement(true);
			return;
		}
		dragging = false;
		if (value != value_before_click) {
			value = value_before_click;
			changed(value, false);
		}
		measure_state = MeasureState::Armed;
		SetCursor(wxCursor(wxCURSOR_CROSS));
		SetFocus();
		Refresh();
		if (!HasCapture()) CaptureMouse();
	}

	void OnRightDown(wxMouseEvent& event) {
		if (CancelMeasurement()) return;
		event.Skip();
	}

	void OnCaptureLost(wxMouseCaptureLostEvent&) {
		dragging = false;
		if (measure_state != MeasureState::Inactive) ResetMeasurement(false);
	}

public:
	AngleDial(wxWindow *parent, agi::Context *context, int value,
		std::function<void(int, bool)> changed)
	: wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(34, 34)))
	, context(context), value(value), value_before_click(value), changed(std::move(changed)) {
		SetBackgroundStyle(wxBG_STYLE_PAINT);
		SetToolTip(_("Drag the angle dial or enter an exact value. Double-click the dial to measure an angle on the video."));
		Bind(wxEVT_PAINT, &AngleDial::OnPaint, this);
		Bind(wxEVT_LEFT_DCLICK, &AngleDial::OnDoubleClick, this);
		Bind(wxEVT_LEFT_DOWN, &AngleDial::OnLeftDown, this);
		Bind(wxEVT_LEFT_UP, &AngleDial::OnLeftUp, this);
		Bind(wxEVT_RIGHT_DOWN, &AngleDial::OnRightDown, this);
		Bind(wxEVT_MOTION, &AngleDial::OnMotion, this);
		Bind(wxEVT_MOUSE_CAPTURE_LOST, &AngleDial::OnCaptureLost, this);
	}

	~AngleDial() override { ResetMeasurement(true); }

	bool CancelMeasurement() {
		if (measure_state == MeasureState::Inactive) return false;
		ResetMeasurement(true);
		return true;
	}

	void SetValue(int next) {
		if (next == value) return;
		value = next;
		Refresh();
	}
};

class DialogGlitch final : public wxDialog {
	agi::Context *context;
	Settings settings;
	typesetting::glitch::PreviewSession preview_session;
	VisualToolPreviewInterface preview_interface;
	wxTimer preview_timer;
	wxTimer playback_timer;
	std::chrono::steady_clock::time_point last_preview;

	wxChoice *mode = nullptr;
	AngleDial *angle_dial = nullptr;
	wxSpinCtrl *angle = nullptr;
	std::array<wxButton *, 4> angle_presets{};
	wxSlider *amount_slider = nullptr;
	wxSpinCtrl *amount = nullptr;
	wxSlider *offset_slider = nullptr;
	wxSpinCtrl *offset = nullptr;
	wxSlider *opacity_slider = nullptr;
	wxSpinCtrlDouble *opacity = nullptr;
	wxSlider *height_slider = nullptr;
	wxSpinCtrl *height = nullptr;
	wxSlider *width_slider = nullptr;
	wxSpinCtrl *width = nullptr;
	wxCheckBox *show_base = nullptr;
	wxStaticText *seed_label = nullptr;

	wxListBox *animation_list = nullptr;
	wxCheckBox *animation_enabled = nullptr;
	wxSpinCtrl *animation_start = nullptr;
	wxSpinCtrl *animation_end = nullptr;
	wxSpinCtrlDouble *from_amount = nullptr;
	wxSpinCtrlDouble *to_amount = nullptr;
	wxSpinCtrlDouble *from_offset = nullptr;
	wxSpinCtrlDouble *to_offset = nullptr;
	wxSpinCtrlDouble *from_opacity = nullptr;
	wxSpinCtrlDouble *to_opacity = nullptr;
	wxSpinCtrl *from_height = nullptr;
	wxSpinCtrl *to_height = nullptr;
	wxSpinCtrlDouble *from_width = nullptr;
	wxSpinCtrlDouble *to_width = nullptr;
	wxSpinCtrl *from_angle = nullptr;
	wxSpinCtrl *to_angle = nullptr;
	int active_animation = -1;
	bool syncing = false;
	bool accepted = false;
	bool preview_pending = false;
	bool loop_playback = false;

	void RunPreview() {
		preview_timer.Stop();
		preview_pending = false;
		last_preview = std::chrono::steady_clock::now();
		preview_session.Update(settings);
	}

	void SchedulePreview(bool immediate = false) {
		constexpr auto interval = std::chrono::milliseconds(70);
		auto now = std::chrono::steady_clock::now();
		if (immediate || last_preview.time_since_epoch().count() == 0 ||
			now - last_preview >= interval) {
			RunPreview();
			return;
		}
		preview_pending = true;
		if (!preview_timer.IsRunning()) {
			auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
				interval - (now - last_preview));
			preview_timer.StartOnce(std::max(1, static_cast<int>(remaining.count())));
		}
	}

	Values BaseValues() const {
		return {static_cast<double>(amount->GetValue()),
			static_cast<double>(offset->GetValue()), opacity->GetValue(), height->GetValue(),
			static_cast<double>(width->GetValue()), static_cast<double>(angle->GetValue())};
	}

	int LineDuration() const {
		if (auto line = context->selectionController->GetActiveLine())
			return std::max(0, static_cast<int>(line->End) - static_cast<int>(line->Start));
		return 0;
	}

	void StopPlayback() {
		loop_playback = false;
		playback_timer.Stop();
		context->videoController->Stop();
	}

	void PlayLine(bool loop) {
		StopPlayback();
		loop_playback = loop;
		context->videoController->PlayLine();
		if (loop_playback && context->videoController->IsPlaying())
			playback_timer.Start(25);
		else if (loop_playback)
			loop_playback = false;
	}

	void SaveAnimation() {
		if (syncing || active_animation < 0 ||
			active_animation >= static_cast<int>(settings.animations.size())) return;
		auto& animation = settings.animations[static_cast<size_t>(active_animation)];
		animation.enabled = animation_enabled->GetValue();
		animation.start_time = animation_start->GetValue();
		animation.end_time = animation_end->GetValue();
		animation.from = {from_amount->GetValue(), from_offset->GetValue(),
			from_opacity->GetValue(), from_height->GetValue(), from_width->GetValue(),
			static_cast<double>(from_angle->GetValue())};
		animation.to = {to_amount->GetValue(), to_offset->GetValue(),
			to_opacity->GetValue(), to_height->GetValue(), to_width->GetValue(),
			static_cast<double>(to_angle->GetValue())};
	}

	wxString AnimationLabel(Animation const& animation, size_t index) const {
		wxString suffix = animation.enabled ? wxString() : " " + _("(off)");
		return wxString::Format(_("Animation %zu: %d-%d ms%s"), index + 1,
			animation.start_time, animation.end_time,
			suffix);
	}

	void RefreshAnimationList() {
		int selection = active_animation;
		animation_list->Freeze();
		animation_list->Clear();
		for (size_t i = 0; i < settings.animations.size(); ++i)
			animation_list->Append(AnimationLabel(settings.animations[i], i));
		animation_list->Thaw();
		if (selection >= static_cast<int>(settings.animations.size()))
			selection = static_cast<int>(settings.animations.size()) - 1;
		active_animation = selection;
		if (selection >= 0) animation_list->SetSelection(selection);
	}

	void LoadAnimation(int selection) {
		SaveAnimation();
		active_animation = selection;
		bool enabled = selection >= 0 &&
			selection < static_cast<int>(settings.animations.size());
		std::array<wxWindow *, 15> animation_controls = {animation_enabled,
			animation_start, animation_end, from_amount, to_amount, from_offset, to_offset,
			from_opacity, to_opacity, from_height, to_height, from_width, to_width,
			from_angle, to_angle};
		for (wxWindow *control : animation_controls)
			control->Enable(enabled);
		if (!enabled) return;
		auto const& animation = settings.animations[static_cast<size_t>(selection)];
		syncing = true;
		animation_enabled->SetValue(animation.enabled);
		animation_start->SetValue(animation.start_time);
		animation_end->SetValue(animation.end_time);
		from_amount->SetValue(animation.from.amount);
		to_amount->SetValue(animation.to.amount);
		from_offset->SetValue(animation.from.offset);
		to_offset->SetValue(animation.to.offset);
		from_opacity->SetValue(animation.from.opacity);
		to_opacity->SetValue(animation.to.opacity);
		from_height->SetValue(animation.from.height);
		to_height->SetValue(animation.to.height);
		from_width->SetValue(animation.from.width);
		to_width->SetValue(animation.to.width);
		from_angle->SetValue(static_cast<int>(std::lround(animation.from.angle)));
		to_angle->SetValue(static_cast<int>(std::lround(animation.to.angle)));
		syncing = false;
	}

	void Read(bool immediate = false) {
		if (syncing) return;
		settings.mode = static_cast<typesetting::glitch::Mode>(mode->GetSelection());
		settings.base = BaseValues();
		settings.show_base = show_base->GetValue();
		SaveAnimation();
		RefreshAnimationList();
		SchedulePreview(immediate);
	}

	void NewPattern() {
		settings.seed = settings.seed * 1664525U + 1013904223U;
		seed_label->SetLabel(wxString::Format("%08X", settings.seed));
		SchedulePreview(true);
	}

	void AddAnimation() {
		SaveAnimation();
		Animation animation;
		animation.from = BaseValues();
		animation.to = animation.from;
		animation.to.amount = std::min(100.0, animation.from.amount + 25.0);
		animation.to.offset = std::min(100.0, animation.from.offset + 25.0);
		animation.to.opacity = std::max(0.0, animation.from.opacity - .3);
		animation.to.height = std::max(1, animation.from.height / 2);
		int duration = std::max(1, LineDuration());
		animation.end_time = duration;
		settings.animations.push_back(animation);
		active_animation = static_cast<int>(settings.animations.size()) - 1;
		RefreshAnimationList();
		LoadAnimation(active_animation);
		SchedulePreview(true);
	}

	void RemoveAnimation() {
		if (active_animation < 0 ||
			active_animation >= static_cast<int>(settings.animations.size())) return;
		settings.animations.erase(settings.animations.begin() + active_animation);
		if (active_animation >= static_cast<int>(settings.animations.size()))
			active_animation = static_cast<int>(settings.animations.size()) - 1;
		RefreshAnimationList();
		LoadAnimation(active_animation);
		SchedulePreview(true);
	}

	void Accept() {
		Read(true);
		preview_timer.Stop();
		preview_pending = false;
		for (auto const& animation : settings.animations) {
			if (animation.enabled && animation.end_time <= animation.start_time) {
				wxMessageBox(_("An animation end time must be after its start time."),
					_("Glitch effect"), wxOK | wxICON_WARNING, this);
				return;
			}
		}
		if (!typesetting::glitch::Apply(context, settings)) {
			wxMessageBox(_("No glitch effect could be generated for the selected lines."),
				_("Glitch effect"), wxOK | wxICON_WARNING, this);
			return;
		}
		accepted = true;
		EndModal(wxID_OK);
	}

	wxSpinCtrlDouble *DoubleControl(wxWindow *parent, double minimum, double maximum,
		double value, double step, int digits = 2) {
		auto control = new wxSpinCtrlDouble(parent, wxID_ANY, "", wxDefaultPosition,
			FromDIP(wxSize(88, -1)), wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,
			minimum, maximum, value, step);
		control->SetDigits(digits);
		return control;
	}

public:
	explicit DialogGlitch(agi::Context *context)
	: wxDialog(context->parent, wxID_ANY, _("Glitch effect"), wxDefaultPosition,
		wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, context(context)
	, settings(typesetting::glitch::LoadSettingsForSelection(context))
	, preview_session(context)
	, preview_timer(this)
	, playback_timer(this) {
		preview_interface.AttachHost(context->videoDisplay->GetPreviewBar(), [this](int id) {
			if (id == wxID_OK) Accept();
			else if (id == wxID_CANCEL) EndModal(wxID_CANCEL);
		}, {}, false);
		VisualToolPreviewInterface::Page page;
		page.controls = {
			{wxID_OK, VisualToolPreviewInterface::ControlKind::Button, _("Accept"),
				VisualToolPreviewInterface::ControlStyle::Accept},
			{wxID_CANCEL, VisualToolPreviewInterface::ControlKind::Button, _("Cancel"),
				VisualToolPreviewInterface::ControlStyle::Cancel}
		};
		page.message = _("Glitch effect");
		preview_interface.SetPage(std::move(page));

		auto main = new wxBoxSizer(wxVERTICAL);
		auto basic = new wxStaticBoxSizer(wxVERTICAL, this, _("Glitch settings"));
		auto mode_row = new wxBoxSizer(wxHORIZONTAL);
		mode_row->Add(new wxStaticText(basic->GetStaticBox(), wxID_ANY, _("Mode:")), 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		mode = new wxChoice(basic->GetStaticBox(), wxID_ANY);
		for (auto const& name : typesetting::glitch::ModeNames()) mode->Append(to_wx(name));
		mode->SetSelection(static_cast<int>(settings.mode));
		mode_row->Add(mode, 1, wxEXPAND);
		basic->Add(mode_row, 0, wxEXPAND | wxALL, 8);

		auto angle_row = new wxBoxSizer(wxHORIZONTAL);
		angle_row->Add(new wxStaticText(basic->GetStaticBox(), wxID_ANY, _("Angle:")), 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		auto angle_controls = new wxBoxSizer(wxHORIZONTAL);
		angle = new wxSpinCtrl(basic->GetStaticBox(), wxID_ANY, "", wxDefaultPosition,
			FromDIP(wxSize(76, -1)), wxSP_ARROW_KEYS, 0, 359,
			static_cast<int>(std::lround(settings.base.angle)));
		angle_dial = new AngleDial(basic->GetStaticBox(), context, angle->GetValue(),
			[this](int value, bool immediate) {
				syncing = true;
				angle->SetValue(value);
				syncing = false;
				Read(immediate);
			});
		angle_controls->Add(angle_dial, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		angle_controls->Add(angle, 0, wxALIGN_CENTER_VERTICAL);
		angle_controls->Add(new wxStaticText(basic->GetStaticBox(), wxID_ANY, wxString::FromUTF8("°")),
			0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
		angle_row->Add(angle_controls, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		std::array<wxString, 4> arrows = {wxString::FromUTF8("→"), wxString::FromUTF8("↓"),
			wxString::FromUTF8("←"), wxString::FromUTF8("↑")};
		for (size_t i = 0; i < angle_presets.size(); ++i) {
			angle_presets[i] = new wxButton(basic->GetStaticBox(), wxID_ANY, arrows[i],
				wxDefaultPosition, FromDIP(wxSize(34, 28)));
			angle_row->Add(angle_presets[i], 0, wxALIGN_CENTER_VERTICAL);
		}
		basic->Add(angle_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		auto values = new wxFlexGridSizer(3, 4, 7);
		values->AddGrowableCol(1, 1);
		auto add_value = [&](wxString const& label, wxSlider *& slider, wxWindow *spin) {
			values->Add(new wxStaticText(basic->GetStaticBox(), wxID_ANY, label), 0,
				wxALIGN_CENTER_VERTICAL);
			values->Add(slider, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
			values->Add(spin, 0, wxALIGN_CENTER_VERTICAL);
		};
		amount_slider = new wxSlider(basic->GetStaticBox(), wxID_ANY,
			static_cast<int>(settings.base.amount), 0, 100);
		amount = new wxSpinCtrl(basic->GetStaticBox(), wxID_ANY, "", wxDefaultPosition,
			FromDIP(wxSize(88, -1)), wxSP_ARROW_KEYS, 0, 100,
			static_cast<int>(settings.base.amount));
		add_value(_("Amount (%):"), amount_slider, amount);
		offset_slider = new wxSlider(basic->GetStaticBox(), wxID_ANY,
			static_cast<int>(settings.base.offset), 0, 100);
		offset = new wxSpinCtrl(basic->GetStaticBox(), wxID_ANY, "", wxDefaultPosition,
			FromDIP(wxSize(88, -1)), wxSP_ARROW_KEYS, 0, 100,
			static_cast<int>(settings.base.offset));
		add_value(_("Offset:"), offset_slider, offset);
		opacity_slider = new wxSlider(basic->GetStaticBox(), wxID_ANY,
			static_cast<int>(std::lround(settings.base.opacity * 100)), 0, 100);
		opacity = DoubleControl(basic->GetStaticBox(), 0.0, 1.0, settings.base.opacity, .01);
		add_value(_("Opacity:"), opacity_slider, opacity);
		height_slider = new wxSlider(basic->GetStaticBox(), wxID_ANY,
			settings.base.height, 1, 200);
		height = new wxSpinCtrl(basic->GetStaticBox(), wxID_ANY, "", wxDefaultPosition,
			FromDIP(wxSize(88, -1)), wxSP_ARROW_KEYS, 1, 200, settings.base.height);
		add_value(_("Height:"), height_slider, height);
		width_slider = new wxSlider(basic->GetStaticBox(), wxID_ANY,
			static_cast<int>(settings.base.width), 1, 100);
		width = new wxSpinCtrl(basic->GetStaticBox(), wxID_ANY, "", wxDefaultPosition,
			FromDIP(wxSize(88, -1)), wxSP_ARROW_KEYS, 1, 100,
			static_cast<int>(settings.base.width));
		add_value(_("Width (%):"), width_slider, width);
		basic->Add(values, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		auto pattern_row = new wxBoxSizer(wxHORIZONTAL);
		auto reroll = new wxButton(basic->GetStaticBox(), wxID_ANY, _("New pattern"));
		pattern_row->Add(reroll, 0, wxRIGHT, 8);
		pattern_row->Add(new wxStaticText(basic->GetStaticBox(), wxID_ANY, _("Pattern:")),
			0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		seed_label = new wxStaticText(basic->GetStaticBox(), wxID_ANY,
			wxString::Format("%08X", settings.seed));
		pattern_row->Add(seed_label, 0, wxALIGN_CENTER_VERTICAL);
		basic->Add(pattern_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
		show_base = new wxCheckBox(basic->GetStaticBox(), wxID_ANY, _("Show base layer"));
		show_base->SetValue(settings.show_base);
		basic->Add(show_base, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
		main->Add(basic, 0, wxEXPAND | wxALL, 8);

		auto transport = new wxBoxSizer(wxHORIZONTAL);
		transport->Add(new wxStaticText(this, wxID_ANY, _("Playback")), 0,
			wxALIGN_CENTER_VERTICAL);
		transport->AddStretchSpacer();
		auto add_transport = [&](char const *command_name, wxString const& tooltip,
				std::function<void()> action) {
			auto button = new wxBitmapButton(this, wxID_ANY, cmd::get(command_name)->Icon(16),
				wxDefaultPosition, FromDIP(wxSize(30, 28)));
			button->SetToolTip(tooltip);
			button->Bind(wxEVT_BUTTON, [action = std::move(action)](wxCommandEvent&) {
				action();
			});
			transport->Add(button, 0, wxLEFT, 3);
		};
		add_transport("video/play/line", _("Play line once"), [this] { PlayLine(false); });
		add_transport("video/play", _("Play line repeatedly"), [this] { PlayLine(true); });
		add_transport("video/stop", _("Stop playback"), [this] { StopPlayback(); });
		auto add_step = [&](wxString const& label, wxString const& tooltip,
				std::function<void()> action) {
			auto button = new wxButton(this, wxID_ANY, label, wxDefaultPosition,
				FromDIP(wxSize(30, 28)));
			button->SetToolTip(tooltip);
			button->Bind(wxEVT_BUTTON, [action = std::move(action)](wxCommandEvent&) {
				action();
			});
			transport->Add(button, 0, wxLEFT, 3);
		};
		add_step("|<", _("Jump to line start"), [this] {
			StopPlayback();
			cmd::call("video/jump/start", this->context);
		});
		add_step("<", _("Previous frame"), [this] {
			StopPlayback();
			this->context->videoController->PrevFrame();
		});
		add_step(">", _("Next frame"), [this] {
			StopPlayback();
			this->context->videoController->NextFrame();
		});
		add_step(">|", _("Jump to line end"), [this] {
			StopPlayback();
			cmd::call("video/jump/end", this->context);
		});
		main->Add(transport, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		auto animation_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Animations"));
		auto total_duration = new wxStaticText(animation_box->GetStaticBox(), wxID_ANY,
			wxString::Format(_("Total line duration: %d ms"), LineDuration()));
		animation_box->Add(total_duration, 0, wxLEFT | wxRIGHT | wxTOP, 8);
		auto animation_top = new wxBoxSizer(wxHORIZONTAL);
		animation_list = new wxListBox(animation_box->GetStaticBox(), wxID_ANY,
			wxDefaultPosition, FromDIP(wxSize(300, 110)));
		animation_top->Add(animation_list, 1, wxEXPAND | wxRIGHT, 7);
		auto animation_buttons = new wxBoxSizer(wxVERTICAL);
		auto add_animation = new wxButton(animation_box->GetStaticBox(), wxID_ANY,
			_("Add animation"));
		auto remove_animation = new wxButton(animation_box->GetStaticBox(), wxID_ANY,
			_("Remove animation"));
		animation_buttons->Add(add_animation, 0, wxEXPAND | wxBOTTOM, 5);
		animation_buttons->Add(remove_animation, 0, wxEXPAND);
		animation_top->Add(animation_buttons, 0, wxEXPAND);
		animation_box->Add(animation_top, 0, wxEXPAND | wxALL, 8);

		auto editor = new wxFlexGridSizer(3, 7, 7);
		editor->AddGrowableCol(1, 1);
		editor->AddGrowableCol(2, 1);
		editor->Add(new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, ""));
		editor->Add(new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, _("From")),
			0, wxALIGN_CENTER);
		editor->Add(new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, _("To")),
			0, wxALIGN_CENTER);
		animation_enabled = new wxCheckBox(animation_box->GetStaticBox(), wxID_ANY,
			_("Enabled"));
		editor->Add(animation_enabled, 0, wxALIGN_CENTER_VERTICAL);
		editor->AddSpacer(0);
		editor->AddSpacer(0);
		animation_start = new wxSpinCtrl(animation_box->GetStaticBox(), wxID_ANY, "",
			wxDefaultPosition, FromDIP(wxSize(95, -1)), wxSP_ARROW_KEYS, 0, 3600000, 0);
		animation_end = new wxSpinCtrl(animation_box->GetStaticBox(), wxID_ANY, "",
			wxDefaultPosition, FromDIP(wxSize(95, -1)), wxSP_ARROW_KEYS, 0, 3600000, 1000);
		editor->Add(new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, _("Time (ms):")),
			0, wxALIGN_CENTER_VERTICAL);
		editor->Add(animation_start, 0, wxEXPAND);
		editor->Add(animation_end, 0, wxEXPAND);
		from_amount = DoubleControl(animation_box->GetStaticBox(), 0, 100,
			settings.base.amount, 1, 0);
		to_amount = DoubleControl(animation_box->GetStaticBox(), 0, 100,
			settings.base.amount, 1, 0);
		editor->Add(new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, _("Amount (%):")),
			0, wxALIGN_CENTER_VERTICAL);
		editor->Add(from_amount, 0, wxEXPAND);
		editor->Add(to_amount, 0, wxEXPAND);
		from_offset = DoubleControl(animation_box->GetStaticBox(), 0, 100,
			settings.base.offset, 1, 0);
		to_offset = DoubleControl(animation_box->GetStaticBox(), 0, 100,
			settings.base.offset, 1, 0);
		editor->Add(new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, _("Offset:")),
			0, wxALIGN_CENTER_VERTICAL);
		editor->Add(from_offset, 0, wxEXPAND);
		editor->Add(to_offset, 0, wxEXPAND);
		from_opacity = DoubleControl(animation_box->GetStaticBox(), 0, 1,
			settings.base.opacity, .01);
		to_opacity = DoubleControl(animation_box->GetStaticBox(), 0, 1,
			settings.base.opacity, .01);
		editor->Add(new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, _("Opacity:")),
			0, wxALIGN_CENTER_VERTICAL);
		editor->Add(from_opacity, 0, wxEXPAND);
		editor->Add(to_opacity, 0, wxEXPAND);
		from_height = new wxSpinCtrl(animation_box->GetStaticBox(), wxID_ANY, "",
			wxDefaultPosition, FromDIP(wxSize(88, -1)), wxSP_ARROW_KEYS, 1, 200,
			settings.base.height);
		to_height = new wxSpinCtrl(animation_box->GetStaticBox(), wxID_ANY, "",
			wxDefaultPosition, FromDIP(wxSize(88, -1)), wxSP_ARROW_KEYS, 1, 200,
			settings.base.height);
		editor->Add(new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, _("Height:")),
			0, wxALIGN_CENTER_VERTICAL);
		editor->Add(from_height, 0, wxEXPAND);
		editor->Add(to_height, 0, wxEXPAND);
		from_width = DoubleControl(animation_box->GetStaticBox(), 1, 100,
			settings.base.width, 1, 0);
		to_width = DoubleControl(animation_box->GetStaticBox(), 1, 100,
			settings.base.width, 1, 0);
		editor->Add(new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, _("Width (%):")),
			0, wxALIGN_CENTER_VERTICAL);
		editor->Add(from_width, 0, wxEXPAND);
		editor->Add(to_width, 0, wxEXPAND);
		from_angle = new wxSpinCtrl(animation_box->GetStaticBox(), wxID_ANY, "",
			wxDefaultPosition, FromDIP(wxSize(88, -1)), wxSP_ARROW_KEYS, 0, 359,
			static_cast<int>(std::lround(settings.base.angle)));
		to_angle = new wxSpinCtrl(animation_box->GetStaticBox(), wxID_ANY, "",
			wxDefaultPosition, FromDIP(wxSize(88, -1)), wxSP_ARROW_KEYS, 0, 359,
			static_cast<int>(std::lround(settings.base.angle)));
		editor->Add(new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, _("Angle:")),
			0, wxALIGN_CENTER_VERTICAL);
		editor->Add(from_angle, 0, wxEXPAND);
		editor->Add(to_angle, 0, wxEXPAND);
		animation_box->Add(editor, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		main->Add(animation_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		main->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
			wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		SetSizerAndFit(main);
		SetMinSize(FromDIP(wxSize(720, -1)));

		preview_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
			if (preview_pending) RunPreview();
		});
		playback_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
			if (!loop_playback || this->context->videoController->IsPlaying()) return;
			this->context->videoController->PlayLine();
			if (!this->context->videoController->IsPlaying()) StopPlayback();
		});
		mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Read(true); });
		angle->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
			angle_dial->SetValue(angle->GetValue());
			Read(true);
		});
		for (size_t i = 0; i < angle_presets.size(); ++i) {
			angle_presets[i]->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
				int value = static_cast<int>(i) * 90;
				angle->SetValue(value);
				angle_dial->SetValue(value);
				Read(true);
			});
		}
		amount_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
			syncing = true; amount->SetValue(amount_slider->GetValue()); syncing = false; Read();
		});
		amount->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
			syncing = true; amount_slider->SetValue(amount->GetValue()); syncing = false; Read(true);
		});
		offset_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
			syncing = true; offset->SetValue(offset_slider->GetValue()); syncing = false; Read();
		});
		offset->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
			syncing = true; offset_slider->SetValue(offset->GetValue()); syncing = false; Read(true);
		});
		opacity_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
			syncing = true; opacity->SetValue(opacity_slider->GetValue() / 100.0); syncing = false; Read();
		});
		opacity->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
			syncing = true; opacity_slider->SetValue(static_cast<int>(std::lround(opacity->GetValue() * 100))); syncing = false; Read(true);
		});
		height_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
			syncing = true; height->SetValue(height_slider->GetValue()); syncing = false; Read();
		});
		height->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
			syncing = true; height_slider->SetValue(height->GetValue()); syncing = false; Read(true);
		});
		width_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
			syncing = true; width->SetValue(width_slider->GetValue()); syncing = false; Read();
		});
		width->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
			syncing = true; width_slider->SetValue(width->GetValue()); syncing = false; Read(true);
		});
		show_base->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Read(true); });
		reroll->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { NewPattern(); });
		add_animation->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddAnimation(); });
		remove_animation->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RemoveAnimation(); });
		animation_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& event) {
			LoadAnimation(event.GetSelection());
		});
		auto animation_changed = [this](auto&) { SaveAnimation(); RefreshAnimationList(); SchedulePreview(); };
		animation_enabled->Bind(wxEVT_CHECKBOX, animation_changed);
		for (auto control : {animation_start, animation_end, from_height, to_height,
			from_angle, to_angle})
			control->Bind(wxEVT_SPINCTRL, animation_changed);
		for (auto control : {from_amount, to_amount, from_offset, to_offset,
			from_opacity, to_opacity, from_width, to_width})
			control->Bind(wxEVT_SPINCTRLDOUBLE, animation_changed);
		Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Accept(); }, wxID_OK);
		Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
			if (event.GetKeyCode() == WXK_ESCAPE && angle_dial->CancelMeasurement()) return;
			event.Skip();
		});

		RefreshAnimationList();
		LoadAnimation(settings.animations.empty() ? -1 : 0);
		SchedulePreview(true);
		CentreOnParent();
	}

	~DialogGlitch() override {
		StopPlayback();
		preview_timer.Stop();
		preview_pending = false;
		bool window_going = context->parent && context->parent->IsBeingDeleted();
		if (!accepted && !window_going) preview_session.Clear();
	}
};

} // namespace

void ShowGlitchDialog(agi::Context *c) {
	DialogGlitch(c).ShowModal();
}
