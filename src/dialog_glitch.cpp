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
#include "utils.h"
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
#include <wx/image.h>
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
using typesetting::glitch::AnimationTiming;
using typesetting::glitch::ColorStyle;
using typesetting::glitch::EffectType;
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

class OptionalColourButton final : public wxButton {
	std::optional<agi::Color> colour;
	wxImage bitmap;
	std::function<void()> changed;

	void UpdateBitmap() {
		auto data = bitmap.GetData();
		for (int y = 0; y < bitmap.GetHeight(); ++y) {
			for (int x = 0; x < bitmap.GetWidth(); ++x) {
				size_t at = (static_cast<size_t>(y) * bitmap.GetWidth() + x) * 3;
				if (colour) {
					data[at] = colour->r;
					data[at + 1] = colour->g;
					data[at + 2] = colour->b;
				}
				else {
					unsigned char shade = ((x / 4 + y / 4) & 1) ? 210 : 245;
					data[at] = data[at + 1] = data[at + 2] = shade;
					if (x == y || x == bitmap.GetWidth() - y - 1) {
						data[at] = 130;
						data[at + 1] = 130;
						data[at + 2] = 130;
					}
				}
			}
		}
		SetBitmapLabel(wxBitmap(bitmap));
		SetToolTip(colour ? to_wx(colour->GetHexFormatted()) : _("No color selected"));
	}

public:
	OptionalColourButton(wxWindow *parent, wxSize size,
			std::optional<agi::Color> initial, std::function<void()> changed)
	: wxButton(parent, wxID_ANY, "", wxDefaultPosition,
		wxSize(size.GetWidth() + 6, size.GetHeight() + 6))
	, colour(std::move(initial))
	, bitmap(size)
	, changed(std::move(changed)) {
		UpdateBitmap();
		Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			GetOptionalColorFromUser(GetParent(), colour,
				[this](std::optional<agi::Color> selected) {
					colour = std::move(selected);
					UpdateBitmap();
					if (this->changed) this->changed();
				});
		});
	}

	std::optional<agi::Color> GetColor() const { return colour; }
	void SetColor(std::optional<agi::Color> value) {
		colour = std::move(value);
		UpdateBitmap();
	}
};

class DialogGlitch final : public wxDialog {
	agi::Context *context;
	Settings settings;
	typesetting::glitch::PreviewSession preview_session;
	wxTimer preview_timer;
	wxTimer playback_timer;
	std::chrono::steady_clock::time_point last_preview;

	wxChoice *effect_type = nullptr;
	wxChoice *color_style = nullptr;
	std::array<OptionalColourButton *, 3> custom_colors{};
	AngleDial *angle_dial = nullptr;
	wxSpinCtrlDouble *angle = nullptr;
	std::array<wxButton *, 4> angle_presets{};
	wxSlider *amount_slider = nullptr;
	wxSpinCtrlDouble *amount = nullptr;
	wxSlider *offset_slider = nullptr;
	wxSpinCtrlDouble *offset = nullptr;
	wxSlider *opacity_slider = nullptr;
	wxSpinCtrlDouble *opacity = nullptr;
	wxSlider *height_slider = nullptr;
	wxSpinCtrlDouble *height = nullptr;
	wxSlider *width_slider = nullptr;
	wxSpinCtrlDouble *width = nullptr;
	wxCheckBox *show_base = nullptr;

	wxListBox *animation_list = nullptr;
	wxCheckBox *animation_enabled = nullptr;
	wxChoice *animation_effect_type = nullptr;
	wxChoice *animation_color_style = nullptr;
	std::array<OptionalColourButton *, 3> animation_custom_colors{};
	wxChoice *animation_timing = nullptr;
	wxPanel *animation_frame_panel = nullptr;
	wxPanel *animation_time_panel = nullptr;
	wxSpinCtrlDouble *animation_start = nullptr;
	wxSpinCtrlDouble *animation_end = nullptr;
	wxSpinCtrlDouble *animation_frame = nullptr;
	wxSpinCtrlDouble *from_amount = nullptr;
	wxSpinCtrlDouble *to_amount = nullptr;
	wxSpinCtrlDouble *from_offset = nullptr;
	wxSpinCtrlDouble *to_offset = nullptr;
	wxSpinCtrlDouble *from_opacity = nullptr;
	wxSpinCtrlDouble *to_opacity = nullptr;
	wxSpinCtrlDouble *from_height = nullptr;
	wxSpinCtrlDouble *to_height = nullptr;
	wxSpinCtrlDouble *from_width = nullptr;
	wxSpinCtrlDouble *to_width = nullptr;
	wxCheckBox *animation_show_base = nullptr;
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
			static_cast<double>(offset->GetValue()), opacity->GetValue(),
			static_cast<int>(std::lround(height->GetValue())),
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

	void UpdateCustomColorVisibility() {
		bool base_custom = color_style &&
			color_style->GetSelection() == static_cast<int>(ColorStyle::Custom);
		for (auto button : custom_colors)
			if (button) button->Show(base_custom);

		bool animation_selected = active_animation >= 0 &&
			active_animation < static_cast<int>(settings.animations.size());
		bool animation_custom = animation_selected && animation_color_style &&
			animation_color_style->GetSelection() ==
				static_cast<int>(ColorStyle::Custom) + 1;
		for (auto button : animation_custom_colors)
			if (button) button->Show(animation_custom);
		Layout();
	}

	void SaveAnimation() {
		if (syncing || active_animation < 0 ||
			active_animation >= static_cast<int>(settings.animations.size())) return;
		auto& animation = settings.animations[static_cast<size_t>(active_animation)];
		animation.enabled = animation_enabled->GetValue();
		animation.use_default_effect_type = animation_effect_type->GetSelection() <= 0;
		if (!animation.use_default_effect_type)
			animation.effect_type = static_cast<EffectType>(
				animation_effect_type->GetSelection() - 1);
		animation.use_default_color_style = animation_color_style->GetSelection() <= 0;
		if (!animation.use_default_color_style)
			animation.color_style = static_cast<ColorStyle>(
				animation_color_style->GetSelection() - 1);
		for (size_t i = 0; i < animation_custom_colors.size(); ++i)
			animation.custom_colors[i] = animation_custom_colors[i]->GetColor();
		animation.timing = animation_timing->GetSelection() == 1 ?
			AnimationTiming::Frame : AnimationTiming::Range;
		animation.start_time = static_cast<int>(std::lround(animation_start->GetValue()));
		animation.end_time = static_cast<int>(std::lround(animation_end->GetValue()));
		animation.frame = static_cast<int>(std::lround(animation_frame->GetValue()));
		animation.show_base = animation_show_base->GetValue();
		animation.from = {from_amount->GetValue(), from_offset->GetValue(),
			from_opacity->GetValue(), static_cast<int>(std::lround(from_height->GetValue())),
			from_width->GetValue(),
			static_cast<double>(angle->GetValue())};
		animation.to = {to_amount->GetValue(), to_offset->GetValue(),
			to_opacity->GetValue(), static_cast<int>(std::lround(to_height->GetValue())),
			to_width->GetValue(),
			static_cast<double>(angle->GetValue())};
	}

	wxString AnimationLabel(Animation const& animation, size_t index) const {
		wxString suffix = animation.enabled ? wxString() : " " + _("(off)");
		if (animation.timing == AnimationTiming::Frame)
			return wxString::Format(_("Animation %zu: frame %d%s"), index + 1,
				animation.frame, suffix);
		return wxString::Format(_("Animation %zu: %d-%d ms%s"), index + 1,
			animation.start_time, animation.end_time,
			suffix);
	}

	void UpdateAnimationTimingAvailability() {
		bool selected = active_animation >= 0 &&
			active_animation < static_cast<int>(settings.animations.size());
		bool single_frame = selected && animation_timing->GetSelection() == 1;
		animation_start->Enable(selected && !single_frame);
		animation_end->Enable(selected && !single_frame);
		animation_frame->Enable(selected && single_frame);
		animation_time_panel->Show(selected && !single_frame);
		animation_frame_panel->Show(selected && single_frame);
		std::array<wxWindow *, 5> from_controls = {from_amount, from_offset,
			from_opacity, from_height, from_width};
		for (wxWindow *control : from_controls)
			control->Enable(selected && !single_frame);
		Layout();
		if (GetSizer()) GetSizer()->Fit(this);
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
		std::array<wxWindow *, 21> animation_controls = {animation_enabled,
			animation_effect_type, animation_color_style, animation_timing,
			animation_start, animation_end, animation_frame,
			from_amount, to_amount, from_offset, to_offset,
			from_opacity, to_opacity, from_height, to_height, from_width, to_width,
			animation_custom_colors[0], animation_custom_colors[1],
			animation_custom_colors[2], animation_show_base};
		for (wxWindow *control : animation_controls)
			control->Enable(enabled);
		if (!enabled) {
			UpdateCustomColorVisibility();
			UpdateAnimationTimingAvailability();
			return;
		}
		auto const& animation = settings.animations[static_cast<size_t>(selection)];
		syncing = true;
		animation_enabled->SetValue(animation.enabled);
		animation_effect_type->SetSelection(animation.use_default_effect_type ? 0 :
			static_cast<int>(animation.effect_type) + 1);
		animation_color_style->SetSelection(animation.use_default_color_style ? 0 :
			static_cast<int>(animation.color_style) + 1);
		for (size_t i = 0; i < animation_custom_colors.size(); ++i)
			animation_custom_colors[i]->SetColor(animation.custom_colors[i]);
		animation_timing->SetSelection(animation.timing == AnimationTiming::Frame ? 1 : 0);
		animation_start->SetValue(animation.start_time);
		animation_end->SetValue(animation.end_time);
		animation_frame->SetValue(animation.frame);
		animation_show_base->SetValue(animation.show_base);
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
		syncing = false;
		UpdateCustomColorVisibility();
		UpdateAnimationTimingAvailability();
	}

	void Read(bool immediate = false) {
		if (syncing) return;
		settings.effect_type = static_cast<EffectType>(effect_type->GetSelection());
		settings.color_style = static_cast<ColorStyle>(color_style->GetSelection());
		for (size_t i = 0; i < custom_colors.size(); ++i)
			settings.custom_colors[i] = custom_colors[i]->GetColor();
		settings.base = BaseValues();
		settings.show_base = show_base->GetValue();
		SaveAnimation();
		RefreshAnimationList();
		SchedulePreview(immediate);
	}

	void AddAnimation() {
		SaveAnimation();
		Animation animation;
		animation.effect_type = settings.effect_type;
		animation.color_style = settings.color_style;
		animation.show_base = settings.show_base;
		animation.from = BaseValues();
		animation.to = animation.from;
		animation.to.opacity = std::max(0.0, animation.from.opacity - .3);
		int duration = std::max(1, LineDuration());
		animation.end_time = duration;
		settings.animations.push_back(animation);
		active_animation = static_cast<int>(settings.animations.size()) - 1;
		RefreshAnimationList();
		LoadAnimation(active_animation);
		SchedulePreview(true);
	}

	void LoadClipboardSettings(Settings pasted) {
		settings = std::move(pasted);
		syncing = true;
		effect_type->SetSelection(static_cast<int>(settings.effect_type));
		color_style->SetSelection(static_cast<int>(settings.color_style));
		for (size_t i = 0; i < custom_colors.size(); ++i)
			custom_colors[i]->SetColor(settings.custom_colors[i]);
		angle->SetValue(static_cast<int>(std::lround(settings.base.angle)));
		angle_dial->SetValue(static_cast<int>(std::lround(angle->GetValue())));
		amount->SetValue(static_cast<int>(std::lround(settings.base.amount)));
		amount_slider->SetValue(amount->GetValue());
		offset->SetValue(static_cast<int>(std::lround(settings.base.offset)));
		offset_slider->SetValue(offset->GetValue());
		opacity->SetValue(settings.base.opacity);
		opacity_slider->SetValue(static_cast<int>(std::lround(settings.base.opacity * 100)));
		height->SetValue(settings.base.height);
		height_slider->SetValue(settings.base.height);
		width->SetValue(static_cast<int>(std::lround(settings.base.width)));
		width_slider->SetValue(width->GetValue());
		show_base->SetValue(settings.show_base);
		syncing = false;
		int const selection = settings.animations.empty() ? -1 : 0;
		active_animation = -1;
		RefreshAnimationList();
		LoadAnimation(selection);
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
			if (animation.enabled && animation.timing == AnimationTiming::Range &&
				animation.end_time <= animation.start_time) {
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
		double value, double step, int digits = 2, int control_width = 88) {
		auto control = new wxSpinCtrlDouble(parent, wxID_ANY, "", wxDefaultPosition,
			FromDIP(wxSize(control_width, -1)),
			wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,
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
		auto main = new wxBoxSizer(wxVERTICAL);
		auto basic = new wxStaticBoxSizer(wxVERTICAL, this, _("Glitch settings"));
		auto effect_row = new wxBoxSizer(wxHORIZONTAL);
		effect_row->Add(new wxStaticText(basic->GetStaticBox(), wxID_ANY,
			_("Effect:")), 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		effect_type = new wxChoice(basic->GetStaticBox(), wxID_ANY);
		for (auto const& name : typesetting::glitch::EffectTypeNames())
			effect_type->Append(to_wx(name));
		effect_type->SetSelection(static_cast<int>(settings.effect_type));
		effect_type->SetMinSize(FromDIP(wxSize(180, -1)));
		effect_row->Add(effect_type, 1, wxEXPAND | wxRIGHT, 8);
		color_style = new wxChoice(basic->GetStaticBox(), wxID_ANY);
		for (auto const& name : typesetting::glitch::ColorStyleNames())
		color_style->Append(to_wx(name));
		color_style->SetSelection(static_cast<int>(settings.color_style));
		color_style->SetMinSize(FromDIP(wxSize(160, -1)));
		effect_row->Add(color_style, 1, wxEXPAND | wxRIGHT, 5);
		for (size_t i = 0; i < custom_colors.size(); ++i) {
			custom_colors[i] = new OptionalColourButton(basic->GetStaticBox(),
				FromDIP(wxSize(24, 16)), settings.custom_colors[i],
				[this] { Read(true); });
			custom_colors[i]->SetToolTip(wxString::Format(_("Custom color %zu"), i + 1));
			effect_row->Add(custom_colors[i], 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 3);
		}
		basic->Add(effect_row, 0, wxEXPAND | wxALL, 8);

		auto angle_row = new wxBoxSizer(wxHORIZONTAL);
		angle_row->Add(new wxStaticText(basic->GetStaticBox(), wxID_ANY, _("Angle:")), 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		auto angle_controls = new wxBoxSizer(wxHORIZONTAL);
		angle = DoubleControl(basic->GetStaticBox(), 0, 359, settings.base.angle, 1, 0, 68);
		angle_dial = new AngleDial(basic->GetStaticBox(), context,
			static_cast<int>(std::lround(angle->GetValue())),
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

		auto values = new wxFlexGridSizer(6, 4, 7);
		values->AddGrowableCol(1, 1);
		values->AddGrowableCol(4, 1);
		auto add_value = [&](wxString const& label, wxSlider *& slider, wxWindow *spin,
				bool right_aligned = false) {
			auto text = new wxStaticText(basic->GetStaticBox(), wxID_ANY, label,
				wxDefaultPosition, wxDefaultSize, right_aligned ? wxALIGN_RIGHT : 0);
			values->Add(text, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
			values->Add(slider, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
			values->Add(spin, 0, wxALIGN_CENTER_VERTICAL);
		};
		amount_slider = new wxSlider(basic->GetStaticBox(), wxID_ANY,
			static_cast<int>(settings.base.amount), 0, 100);
		amount = DoubleControl(basic->GetStaticBox(), 0, 100, settings.base.amount, 1, 0);
		offset_slider = new wxSlider(basic->GetStaticBox(), wxID_ANY,
			static_cast<int>(settings.base.offset), 0, 100);
		offset = DoubleControl(basic->GetStaticBox(), 0, 100, settings.base.offset, 1, 0);
		add_value(_("Amount (%):"), amount_slider, amount);
		add_value(_("Offset:"), offset_slider, offset, true);
		opacity_slider = new wxSlider(basic->GetStaticBox(), wxID_ANY,
			static_cast<int>(std::lround(settings.base.opacity * 100)), 0, 100);
		opacity = DoubleControl(basic->GetStaticBox(), 0.0, 1.0, settings.base.opacity, .01);
		height_slider = new wxSlider(basic->GetStaticBox(), wxID_ANY,
			settings.base.height, 1, 200);
		height = DoubleControl(basic->GetStaticBox(), 1, 200, settings.base.height, 1, 0);
		width_slider = new wxSlider(basic->GetStaticBox(), wxID_ANY,
			static_cast<int>(settings.base.width), 1, 100);
		width = DoubleControl(basic->GetStaticBox(), 1, 100, settings.base.width, 1, 0);
		add_value(_("Height:"), height_slider, height);
		add_value(_("Width (%):"), width_slider, width, true);
		add_value(_("Opacity:"), opacity_slider, opacity);
		show_base = new wxCheckBox(basic->GetStaticBox(), wxID_ANY, _("Show base layer"));
		show_base->SetValue(settings.show_base);
		values->Add(show_base, 0, wxALIGN_CENTER_VERTICAL);
		values->AddSpacer(0);
		values->AddSpacer(0);
		basic->Add(values, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
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

		auto animation_timing_row = new wxBoxSizer(wxHORIZONTAL);
		animation_enabled = new wxCheckBox(animation_box->GetStaticBox(), wxID_ANY,
			_("Enabled"));
		animation_timing_row->Add(animation_enabled, 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		animation_timing = new wxChoice(animation_box->GetStaticBox(), wxID_ANY);
		animation_timing->Append(_("Time"));
		animation_timing->Append(_("Frame"));
		animation_timing->SetSelection(0);
		animation_timing->SetMinSize(FromDIP(wxSize(82, -1)));
		animation_timing->SetToolTip(_("Timing"));
		animation_timing_row->Add(animation_timing, 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		animation_frame_panel = new wxPanel(animation_box->GetStaticBox());
		auto frame_sizer = new wxBoxSizer(wxHORIZONTAL);
		animation_frame = DoubleControl(animation_frame_panel, 0, 1000000, 0, 1, 0, 72);
		frame_sizer->Add(new wxStaticText(animation_frame_panel, wxID_ANY, _("Frame:")),
			0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		frame_sizer->Add(animation_frame, 0, wxALIGN_CENTER_VERTICAL);
		animation_frame_panel->SetSizer(frame_sizer);
		animation_timing_row->Add(animation_frame_panel, 0, wxALIGN_CENTER_VERTICAL);

		animation_time_panel = new wxPanel(animation_box->GetStaticBox());
		auto time_sizer = new wxBoxSizer(wxHORIZONTAL);
		animation_start = DoubleControl(animation_time_panel, 0, 3600000, 0, 1, 0, 78);
		animation_end = DoubleControl(animation_time_panel, 0, 3600000, 1000, 1, 0, 78);
		time_sizer->Add(new wxStaticText(animation_time_panel, wxID_ANY, _("Time (ms):")),
			0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		time_sizer->Add(animation_start, 0, wxALIGN_CENTER_VERTICAL);
		time_sizer->Add(new wxStaticText(animation_time_panel, wxID_ANY, "->"), 0,
			wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 7);
		time_sizer->Add(animation_end, 0, wxALIGN_CENTER_VERTICAL);
		animation_time_panel->SetSizer(time_sizer);
		animation_timing_row->Add(animation_time_panel, 0, wxALIGN_CENTER_VERTICAL);
		animation_box->Add(animation_timing_row, 0,
			wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		auto animation_effect_row = new wxBoxSizer(wxHORIZONTAL);
		animation_effect_row->Add(new wxStaticText(animation_box->GetStaticBox(),
			wxID_ANY, _("Effect:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		animation_effect_type = new wxChoice(animation_box->GetStaticBox(), wxID_ANY);
		animation_effect_type->Append(_("Default"));
		for (auto const& name : typesetting::glitch::EffectTypeNames())
			animation_effect_type->Append(to_wx(name));
		animation_effect_type->SetSelection(0);
		animation_effect_type->SetMinSize(FromDIP(wxSize(135, -1)));
		animation_effect_type->SetToolTip(_("Effect type"));
		animation_effect_row->Add(animation_effect_type, 1,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		animation_color_style = new wxChoice(animation_box->GetStaticBox(), wxID_ANY);
		animation_color_style->Append(_("Default"));
		for (auto const& name : typesetting::glitch::ColorStyleNames())
			animation_color_style->Append(to_wx(name));
		animation_color_style->SetSelection(0);
		animation_color_style->SetMinSize(FromDIP(wxSize(135, -1)));
		animation_color_style->SetToolTip(_("Color style"));
		animation_effect_row->Add(animation_color_style, 1,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		for (size_t i = 0; i < animation_custom_colors.size(); ++i) {
			animation_custom_colors[i] = new OptionalColourButton(
				animation_box->GetStaticBox(), FromDIP(wxSize(20, 14)),
				std::nullopt, [this] {
					SaveAnimation();
					RefreshAnimationList();
					SchedulePreview(true);
				});
			animation_effect_row->Add(animation_custom_colors[i], 0,
				wxALIGN_CENTER_VERTICAL | wxLEFT, 3);
		}
		animation_box->Add(animation_effect_row, 0,
			wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		auto editor = new wxFlexGridSizer(2, 7, 12);
		editor->AddGrowableCol(0, 1);
		editor->AddGrowableCol(1, 1);
		auto transition = [&](wxString const& label, wxWindow *from, wxWindow *to,
				bool right_aligned = false) {
			auto row = new wxBoxSizer(wxHORIZONTAL);
			auto text = new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, label,
				wxDefaultPosition, FromDIP(wxSize(82, -1)),
				right_aligned ? wxALIGN_RIGHT : 0);
			row->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			row->Add(from, 0, wxALIGN_CENTER_VERTICAL);
			row->Add(new wxStaticText(animation_box->GetStaticBox(), wxID_ANY, "->"), 0,
				wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
			row->Add(to, 0, wxALIGN_CENTER_VERTICAL);
			return row;
		};
		from_amount = DoubleControl(animation_box->GetStaticBox(), 0, 100,
			settings.base.amount, 1, 0);
		to_amount = DoubleControl(animation_box->GetStaticBox(), 0, 100,
			settings.base.amount, 1, 0);
		from_offset = DoubleControl(animation_box->GetStaticBox(), 0, 100,
			settings.base.offset, 1, 0);
		to_offset = DoubleControl(animation_box->GetStaticBox(), 0, 100,
			settings.base.offset, 1, 0);
		from_opacity = DoubleControl(animation_box->GetStaticBox(), 0, 1,
			settings.base.opacity, .01);
		to_opacity = DoubleControl(animation_box->GetStaticBox(), 0, 1,
			settings.base.opacity, .01);
		from_height = DoubleControl(animation_box->GetStaticBox(), 1, 200,
			settings.base.height, 1, 0);
		to_height = DoubleControl(animation_box->GetStaticBox(), 1, 200,
			settings.base.height, 1, 0);
		from_width = DoubleControl(animation_box->GetStaticBox(), 1, 100,
			settings.base.width, 1, 0);
		to_width = DoubleControl(animation_box->GetStaticBox(), 1, 100,
			settings.base.width, 1, 0);
		editor->Add(transition(_("Amount (%):"), from_amount, to_amount), 1, wxEXPAND);
		editor->Add(transition(_("Offset:"), from_offset, to_offset, true), 1, wxEXPAND);
		editor->Add(transition(_("Height:"), from_height, to_height), 1, wxEXPAND);
		editor->Add(transition(_("Width (%):"), from_width, to_width, true), 1, wxEXPAND);
		editor->Add(transition(_("Opacity:"), from_opacity, to_opacity), 1, wxEXPAND);
		animation_show_base = new wxCheckBox(animation_box->GetStaticBox(), wxID_ANY,
			_("Show base layer"));
		animation_show_base->SetValue(settings.show_base);
		editor->Add(animation_show_base, 0, wxALIGN_CENTER_VERTICAL);
		animation_box->Add(editor, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		main->Add(animation_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		main->Add(transport, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		auto bottom = new wxBoxSizer(wxHORIZONTAL);
		Settings clipboard_settings;
		if (typesetting::glitch::SettingsFromClipboard(GetClipboard(), clipboard_settings)) {
			auto load_effect = new wxButton(this, wxID_ANY, _("Paste effect"));
			load_effect->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
				Settings pasted;
				if (typesetting::glitch::SettingsFromClipboard(GetClipboard(), pasted))
					LoadClipboardSettings(std::move(pasted));
			});
			bottom->Add(load_effect, 0, wxALIGN_CENTER_VERTICAL);
		}
		bottom->AddStretchSpacer();
		bottom->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALIGN_CENTER_VERTICAL);
		main->Add(bottom, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		UpdateCustomColorVisibility();
		SetSizerAndFit(main);
		SetMinSize(FromDIP(wxSize(620, -1)));

		preview_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
			if (preview_pending) RunPreview();
		});
		playback_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
			if (!loop_playback || this->context->videoController->IsPlaying()) return;
			this->context->videoController->PlayLine();
			if (!this->context->videoController->IsPlaying()) StopPlayback();
		});
		effect_type->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Read(true); });
		color_style->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
			UpdateCustomColorVisibility();
			Read(true);
		});
		angle->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
			angle_dial->SetValue(static_cast<int>(std::lround(angle->GetValue())));
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
		amount->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
			syncing = true; amount_slider->SetValue(static_cast<int>(std::lround(amount->GetValue()))); syncing = false; Read(true);
		});
		offset_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
			syncing = true; offset->SetValue(offset_slider->GetValue()); syncing = false; Read();
		});
		offset->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
			syncing = true; offset_slider->SetValue(static_cast<int>(std::lround(offset->GetValue()))); syncing = false; Read(true);
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
		height->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
			syncing = true; height_slider->SetValue(static_cast<int>(std::lround(height->GetValue()))); syncing = false; Read(true);
		});
		width_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
			syncing = true; width->SetValue(width_slider->GetValue()); syncing = false; Read();
		});
		width->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
			syncing = true; width_slider->SetValue(static_cast<int>(std::lround(width->GetValue()))); syncing = false; Read(true);
		});
		show_base->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Read(true); });
		add_animation->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddAnimation(); });
		remove_animation->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RemoveAnimation(); });
		animation_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& event) {
			LoadAnimation(event.GetSelection());
		});
		auto animation_changed = [this](auto&) { SaveAnimation(); RefreshAnimationList(); SchedulePreview(); };
		animation_enabled->Bind(wxEVT_CHECKBOX, animation_changed);
		animation_show_base->Bind(wxEVT_CHECKBOX, animation_changed);
		animation_effect_type->Bind(wxEVT_CHOICE, animation_changed);
		animation_color_style->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
			SaveAnimation();
			RefreshAnimationList();
			UpdateCustomColorVisibility();
			SchedulePreview();
		});
		animation_timing->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
			SaveAnimation();
			RefreshAnimationList();
			UpdateAnimationTimingAvailability();
			SchedulePreview();
		});
		for (auto control : {animation_start, animation_end, animation_frame,
			from_amount, to_amount, from_offset, to_offset,
			from_opacity, to_opacity, from_height, to_height, from_width, to_width})
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
