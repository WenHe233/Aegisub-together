// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

/// @file dialog_gradient.cpp
/// @brief Modal editor for native typesetting gradients

#include "dialog_gradient.h"

#include "command/command.h"
#include "dialogs.h"
#include "include/aegisub/context.h"
#include "typesetting_gradient.h"
#include "video_controller.h"
#include "video_display.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include <wx/button.h>
#include <wx/bmpbuttn.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/collpane.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/radiobox.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/settings.h>
#include <wx/stattext.h>
#include <wx/timer.h>

namespace {

using typesetting::gradient::Channel;
using typesetting::gradient::Kind;
using typesetting::gradient::Motion;
using typesetting::gradient::MotionMode;
using typesetting::gradient::Output;
using typesetting::gradient::Settings;
using typesetting::gradient::Stop;

bool UsesRectangularClip(Settings const& settings) {
	return settings.output == Output::Clips && settings.kind == Kind::Linear &&
		settings.angle % 90 == 0;
}

class GradientSlider final : public wxPanel {
	std::vector<Stop>& stops;
	std::function<void(bool)> changed;
	int active = -1;
	wxPoint pressed;
	bool dragged = false;

	wxRect Bar() const {
		int margin = FromDIP(12);
		int height = FromDIP(24);
		wxSize size = GetClientSize();
		return {margin, (size.y - height) / 2, std::max(1, size.x - margin * 2), height};
	}

	int StopX(Stop const& stop) const {
		auto bar = Bar();
		return bar.x + std::lround((bar.width - 1) * std::clamp(stop.position, 0, 100) / 100.0);
	}

	int HitStop(wxPoint point) const {
		auto bar = Bar();
		int half = FromDIP(11);
		if (point.y < bar.y - half || point.y > bar.GetBottom() + half) return -1;
		int found = -1, nearest = half + 1;
		for (size_t i = 0; i < stops.size(); ++i) {
			int distance = std::abs(point.x - StopX(stops[i]));
			if (distance <= half && distance < nearest) {
				nearest = distance;
				found = static_cast<int>(i);
			}
		}
		return found;
	}

	int PositionAt(int x) const {
		auto bar = Bar();
		return std::clamp(static_cast<int>(std::lround(
			(x - bar.x) * 100.0 / std::max(1, bar.width - 1))), 0, 100);
	}

	static wxColour Composite(agi::Color colour, int background) {
		double opacity = 1.0 - colour.a / 255.0;
		auto blend = [&](int component) {
			return static_cast<unsigned char>(std::lround(
				component * opacity + background * (1.0 - opacity)));
		};
		return {blend(colour.r), blend(colour.g), blend(colour.b)};
	}

	void OnPaint(wxPaintEvent&) {
		wxAutoBufferedPaintDC dc(this);
		dc.SetBackground(wxBrush(GetBackgroundColour()));
		dc.Clear();
		auto bar = Bar();

		int checker = FromDIP(6);
		wxImage image(bar.width, bar.height, false);
		auto pixels = image.GetData();
		for (int x = 0; x < bar.width; ++x) {
			auto colour = typesetting::gradient::Sample(stops,
				bar.width <= 1 ? 0.0 : static_cast<double>(x) / (bar.width - 1));
			for (int y = 0; y < bar.height; ++y) {
				bool dark = (x / checker + y / checker) % 2;
				auto composite = Composite(colour, dark ? 185 : 235);
				auto at = (static_cast<size_t>(y) * bar.width + x) * 3;
				pixels[at] = composite.Red();
				pixels[at + 1] = composite.Green();
				pixels[at + 2] = composite.Blue();
			}
		}
		dc.DrawBitmap(wxBitmap(image), bar.GetPosition(), false);
		dc.SetBrush(*wxTRANSPARENT_BRUSH);
		wxColour edge(9, 38, 55);
		dc.SetPen(wxPen(edge, FromDIP(2)));
		dc.DrawRoundedRectangle(bar, FromDIP(4));

		int handle_width = FromDIP(18);
		int extension = FromDIP(8);
		for (size_t i = 0; i < stops.size(); ++i) {
			int x = StopX(stops[i]);
			wxRect outer(x - handle_width / 2, bar.y - extension,
				handle_width, bar.height + extension * 2);
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.SetBrush(wxBrush(i == static_cast<size_t>(active) ? wxColour(80, 105, 118) : edge));
			dc.DrawRoundedRectangle(outer, FromDIP(6));
			wxRect middle = outer;
			middle.Deflate(FromDIP(2));
			dc.SetBrush(*wxWHITE_BRUSH);
			dc.DrawRoundedRectangle(middle, FromDIP(4));
			wxRect inner = outer;
			inner.Deflate(FromDIP(4));
			dc.SetBrush(wxBrush(Composite(stops[i].colour, 225)));
			dc.DrawRoundedRectangle(inner, FromDIP(3));
		}
	}

	void OpenColour(size_t at) {
		if (at >= stops.size()) return;
		auto original = stops[at].colour;
		GetColorFromUser(this, original, true, [this, at](agi::Color colour) {
			if (at >= stops.size()) return;
			stops[at].colour = colour;
			Refresh();
			changed(false);
		});
		changed(true);
	}

	void OnLeftDown(wxMouseEvent& event) {
		active = HitStop(event.GetPosition());
		if (active < 0) {
			if (!Bar().Contains(event.GetPosition())) return;
			int position = PositionAt(event.GetX());
			auto colour = typesetting::gradient::Sample(stops, position / 100.0);
			stops.push_back({position, colour});
			active = static_cast<int>(stops.size() - 1);
			Refresh();
			OpenColour(active);
			return;
		}
		pressed = event.GetPosition();
		dragged = false;
		CaptureMouse();
		Refresh();
	}

	void OnMotion(wxMouseEvent& event) {
		if (active < 0 || !event.Dragging() || !event.LeftIsDown()) return;
		if (std::abs(event.GetX() - pressed.x) >= FromDIP(2)) dragged = true;
		int position = PositionAt(event.GetX());
		if (stops[active].position == position) return;
		stops[active].position = position;
		Refresh();
		changed(false);
	}

	void OnLeftUp(wxMouseEvent&) {
		if (HasCapture()) ReleaseMouse();
		if (active >= 0 && !dragged) OpenColour(active);
		else if (active >= 0) changed(true);
	}

	void OnRightDown(wxMouseEvent& event) {
		int found = HitStop(event.GetPosition());
		if (found < 0 || stops.size() <= 2) return;
		stops.erase(stops.begin() + found);
		active = -1;
		Refresh();
		changed(true);
	}

	void OnCaptureLost(wxMouseCaptureLostEvent&) {
		active = -1;
		dragged = false;
		Refresh();
	}

public:
	GradientSlider(wxWindow *parent, std::vector<Stop>& stops, std::function<void(bool)> changed)
	: wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(560, 58)))
	, stops(stops)
	, changed(std::move(changed)) {
		SetBackgroundStyle(wxBG_STYLE_PAINT);
		SetToolTip(_("Click the bar to add a stop. Drag a square to move it; click it to choose a color; right-click it to remove it."));
		Bind(wxEVT_PAINT, &GradientSlider::OnPaint, this);
		Bind(wxEVT_LEFT_DOWN, &GradientSlider::OnLeftDown, this);
		Bind(wxEVT_LEFT_UP, &GradientSlider::OnLeftUp, this);
		Bind(wxEVT_RIGHT_DOWN, &GradientSlider::OnRightDown, this);
		Bind(wxEVT_MOTION, &GradientSlider::OnMotion, this);
		Bind(wxEVT_MOUSE_CAPTURE_LOST, &GradientSlider::OnCaptureLost, this);
	}
};

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
		double angle = std::atan2(end.y - measure_start.y, end.x - measure_start.x) *
			180.0 / 3.14159265358979323846;
		if (end == measure_start) return;
		int next = static_cast<int>(std::lround(angle));
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
		double angle = std::atan2(point.y - size.y / 2.0, point.x - size.x / 2.0) *
			180.0 / 3.14159265358979323846;
		int next = static_cast<int>(std::lround(angle));
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
		if (measure_state == MeasureState::Armed) return;
		if (!dragging) return;
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
	, context(context)
	, value(value)
	, value_before_click(value)
	, changed(std::move(changed)) {
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

class MotionEditor {
	wxCollapsiblePane *pane;
	Motion& motion;
	std::function<void()> layout_changed;
	std::function<void(bool)> settings_changed;
	wxCheckBox *enabled = nullptr;
	wxChoice *mode = nullptr;
	wxCheckBox *end_at_line = nullptr;
	wxSpinCtrl *start_time = nullptr;
	wxSpinCtrl *end_time = nullptr;
	wxSpinCtrl *cycle_time = nullptr;
	wxSpinCtrlDouble *accel = nullptr;
	wxSpinCtrlDouble *start_position = nullptr;
	wxSpinCtrlDouble *end_position = nullptr;
	wxSpinCtrlDouble *start_width = nullptr;
	wxSpinCtrlDouble *middle_width = nullptr;
	wxSpinCtrlDouble *end_width = nullptr;
	bool syncing = false;

	void UpdateAvailability() {
		bool active = enabled->GetValue();
		auto selected = static_cast<MotionMode>(mode->GetSelection());
		bool once = selected == MotionMode::Once;
		bool repeating = selected == MotionMode::Loop || selected == MotionMode::PingPong;
		bool fit = selected == MotionMode::FitLine;
		mode->Enable(active);
		start_time->Enable(active && !fit);
		end_at_line->Enable(active && once);
		end_time->Enable(active && once && !end_at_line->GetValue());
		cycle_time->Enable(active && repeating);
		accel->Enable(active);
		start_position->Enable(active);
		end_position->Enable(active);
		start_width->Enable(active);
		middle_width->Enable(active);
		end_width->Enable(active);
	}

	void Read(bool immediate) {
		if (syncing) return;
		motion.enabled = enabled->GetValue();
		motion.mode = static_cast<MotionMode>(mode->GetSelection());
		motion.end_at_line = end_at_line->GetValue();
		motion.start_time = start_time->GetValue();
		motion.end_time = end_time->GetValue();
		motion.cycle_time = cycle_time->GetValue();
		motion.accel = accel->GetValue();
		motion.start_position = start_position->GetValue();
		motion.end_position = end_position->GetValue();
		motion.start_width = start_width->GetValue();
		motion.middle_width = middle_width->GetValue();
		motion.end_width = end_width->GetValue();
		UpdateAvailability();
		settings_changed(immediate);
	}

public:
	MotionEditor(wxWindow *parent, wxString const& title, Motion& motion,
		bool collapsed, std::function<void()> layout_changed,
		std::function<void(bool)> settings_changed)
	: pane(new wxCollapsiblePane(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
		wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE))
	, motion(motion)
	, layout_changed(std::move(layout_changed))
	, settings_changed(std::move(settings_changed)) {
		auto content = pane->GetPane();
		auto main = new wxBoxSizer(wxVERTICAL);
		auto top = new wxBoxSizer(wxHORIZONTAL);
		enabled = new wxCheckBox(content, wxID_ANY, _("Move colors"));
		top->Add(enabled, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
		top->Add(new wxStaticText(content, wxID_ANY, _("Motion type:")), 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		mode = new wxChoice(content, wxID_ANY);
		mode->Append(_("Once"));
		mode->Append(_("Loop until line end"));
		mode->Append(_("Ping-pong until line end"));
		mode->Append(_("Fit once to line"));
		top->Add(mode, 0, wxALIGN_CENTER_VERTICAL);
		main->Add(top, 0, wxEXPAND | wxBOTTOM, 5);

		auto grid = new wxFlexGridSizer(4, 5, 10);
		auto spin = [&](wxString const& label, int value, int minimum, int maximum) {
			grid->Add(new wxStaticText(content, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
			auto control = new wxSpinCtrl(content, wxID_ANY, "", wxDefaultPosition,
				content->FromDIP(wxSize(90, -1)), wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,
				minimum, maximum, value);
			grid->Add(control, 0, wxALIGN_CENTER_VERTICAL);
			return control;
		};
		auto decimal = [&](wxString const& label, double value, double minimum,
				double maximum, double increment) {
			grid->Add(new wxStaticText(content, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
			auto control = new wxSpinCtrlDouble(content, wxID_ANY, "", wxDefaultPosition,
				content->FromDIP(wxSize(90, -1)), wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,
				minimum, maximum, value, increment);
			control->SetDigits(2);
			grid->Add(control, 0, wxALIGN_CENTER_VERTICAL);
			return control;
		};
		start_time = spin(_("Start time (ms):"), motion.start_time, 0, 3600000);
		end_time = spin(_("End time (ms):"), motion.end_time, 0, 3600000);
		cycle_time = spin(_("Cycle/leg (ms):"), motion.cycle_time, 1, 3600000);
		accel = decimal(_("Accel:"), motion.accel, 0.01, 100.0, 0.05);
		start_position = decimal(_("Start position (%):"), motion.start_position,
			-1000.0, 1000.0, 1.0);
		end_position = decimal(_("End position (%):"), motion.end_position,
			-1000.0, 1000.0, 1.0);
		start_width = decimal(_("Start width (%):"), motion.start_width, 0.1, 1000.0, 1.0);
		middle_width = decimal(_("Middle width (%):"), motion.middle_width,
			0.1, 1000.0, 1.0);
		end_width = decimal(_("End width (%):"), motion.end_width, 0.1, 1000.0, 1.0);
		main->Add(grid, 0, wxEXPAND | wxBOTTOM, 5);

		auto bottom = new wxBoxSizer(wxHORIZONTAL);
		end_at_line = new wxCheckBox(content, wxID_ANY, _("Use line end"));
		bottom->Add(end_at_line, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
		auto forward = new wxButton(content, wxID_ANY, _("Before to after"));
		auto backward = new wxButton(content, wxID_ANY, _("After to before"));
		bottom->Add(forward, 0, wxRIGHT, 5);
		bottom->Add(backward, 0);
		main->Add(bottom, 0, wxEXPAND);
		content->SetSizer(main);
		pane->Collapse(collapsed);

		auto changed = [this](auto&) { Read(false); };
		enabled->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Read(true); });
		mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Read(true); });
		end_at_line->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Read(true); });
		for (auto control : {start_time, end_time, cycle_time}) {
			control->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { Read(true); });
			control->Bind(wxEVT_TEXT, changed);
			control->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Read(true); });
		}
		for (auto control : {accel, start_position, end_position, start_width,
				middle_width, end_width}) {
			control->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { Read(true); });
			control->Bind(wxEVT_TEXT, changed);
			control->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Read(true); });
		}
		forward->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			start_position->SetValue(-50.0);
			end_position->SetValue(150.0);
			Read(true);
		});
		backward->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			start_position->SetValue(150.0);
			end_position->SetValue(-50.0);
			Read(true);
		});
		pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent&) {
			this->layout_changed();
		});
		RefreshValues();
	}

	wxCollapsiblePane *Window() const { return pane; }
	void Show(bool shown) { pane->Show(shown); }
	void RefreshValues() {
		syncing = true;
		enabled->SetValue(motion.enabled);
		mode->SetSelection(static_cast<int>(motion.mode));
		end_at_line->SetValue(motion.end_at_line);
		start_time->SetValue(motion.start_time);
		end_time->SetValue(motion.end_time);
		cycle_time->SetValue(motion.cycle_time);
		accel->SetValue(motion.accel);
		start_position->SetValue(motion.start_position);
		end_position->SetValue(motion.end_position);
		start_width->SetValue(motion.start_width);
		middle_width->SetValue(motion.middle_width);
		end_width->SetValue(motion.end_width);
		syncing = false;
		UpdateAvailability();
	}
};

class GradientBlock {
	wxCollapsiblePane *pane;
	wxWindow *content;
	Channel& channel;
	int index;
	std::function<void()> layout_changed;
	std::function<void(int, int)> copy_stops;
	std::function<void(bool)> settings_changed;
	GradientSlider *slider = nullptr;

public:
	GradientBlock(wxWindow *parent, wxString const& title, Channel& channel, int index,
		bool collapsed, std::function<void()> layout_changed,
		std::function<void(int, int)> copy_stops,
		std::function<void(bool)> settings_changed)
	: pane(new wxCollapsiblePane(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
		wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE))
	, content(pane->GetPane())
	, channel(channel)
	, index(index)
	, layout_changed(std::move(layout_changed))
	, copy_stops(std::move(copy_stops))
	, settings_changed(std::move(settings_changed)) {
		auto main = new wxBoxSizer(wxVERTICAL);
		auto header = new wxBoxSizer(wxHORIZONTAL);
		auto enabled = new wxCheckBox(content, wxID_ANY, _("Apply to this color"));
		enabled->SetValue(this->channel.enabled);
		enabled->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
			this->channel.enabled = event.IsChecked();
			this->settings_changed(true);
		});
		header->Add(enabled, 0, wxALIGN_CENTER_VERTICAL);
		header->AddStretchSpacer();
		auto copy = new wxButton(content, wxID_ANY, _("Copy stops from..."));
		copy->Bind(wxEVT_BUTTON, [this, copy](wxCommandEvent&) {
			wxMenu menu;
			std::array<wxString, 3> labels = {_("Color"), _("Bord color"),
				_("Shadow color")};
			for (int source = 0; source < 3; ++source) {
				if (source == this->index) continue;
				menu.Append(2000 + source, labels[source]);
			}
			menu.Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
				this->copy_stops(event.GetId() - 2000, this->index);
			});
			copy->PopupMenu(&menu);
		});
		header->Add(copy, 0, wxLEFT, 6);
		main->Add(header, 0, wxEXPAND | wxBOTTOM, 5);

		slider = new GradientSlider(content, this->channel.stops, this->settings_changed);
		main->Add(slider, 0, wxEXPAND | wxBOTTOM, 4);
		auto instructions = new wxStaticText(content, wxID_ANY,
			_("Click the bar to add a stop. Drag a square to move it; click it to choose a color; right-click it to remove it."));
		instructions->Wrap(content->FromDIP(700));
		main->Add(instructions, 0, wxEXPAND);
		content->SetSizer(main);
		pane->Collapse(collapsed);
		pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent&) {
			this->layout_changed();
		});
	}

	wxCollapsiblePane *Window() const { return pane; }
	void RefreshStops() { slider->Refresh(); }
};

class DialogGradient final : public wxDialog {
	agi::Context *context;
	Settings settings;
	typesetting::gradient::PreviewSession preview_session;
	wxTimer preview_timer;
	wxTimer playback_timer;
	wxRadioBox *kind = nullptr;
	std::array<wxRadioButton *, 3> output_choices{};
	AngleDial *angle_dial = nullptr;
	wxSpinCtrl *angle = nullptr;
	std::array<wxButton *, 4> angle_presets{};
	wxSpinCtrl *pixels_per_strip = nullptr;
	wxSpinCtrlDouble *anti_strip_overlap = nullptr;
	std::unique_ptr<MotionEditor> motion_editor;
	wxPanel *sections_panel = nullptr;
	std::array<std::unique_ptr<GradientBlock>, 3> blocks;
	bool accepted = false;
	bool preview_pending = false;
	bool syncing_angle = false;
	bool loop_playback = false;
	double non_rect_overlap = 0.4;
	std::chrono::steady_clock::time_point last_preview;

	void LayoutContent() {
		if (!sections_panel || !GetSizer()) return;
		Freeze();
		sections_panel->GetSizer()->Layout();
		GetSizer()->Layout();
		Fit();
		Thaw();
		Refresh(true);
	}

	void RunPreview() {
		preview_timer.Stop();
		preview_pending = false;
		last_preview = std::chrono::steady_clock::now();
		preview_session.Update(settings);
	}

	void SchedulePreview(bool immediate) {
		constexpr auto interval = std::chrono::milliseconds(50);
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

	void CopyStops(int source, int target) {
		if (source < 0 || source >= 3 || target < 0 || target >= 3 || source == target) return;
		Channel *channels[] = {&settings.primary, &settings.outline, &settings.shadow};
		channels[target]->stops = channels[source]->stops;
		blocks[target]->RefreshStops();
		SchedulePreview(true);
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

	void ShowHelp() {
		wxMessageBox(_(
			"Gradient type\n"
			"Linear follows the selected angle. Radial expands from the center of the selected text.\n\n"
			"Output\n"
			"Clip creates clipped subtitle copies. By character assigns the gradient separately to characters. With shapes converts text, border and shadow to editable vector shapes without clips.\n\n"
			"Pixels per strip controls spatial detail. Smaller values are smoother but create more generated rows. Anti-strip overlap slightly overlaps oblique, radial and shape bands; rectangular clips always use zero.\n\n"
			"Color stops\n"
			"Click the bar to add a stop, drag its square to position it, click the square to edit color and alpha, or right-click it to remove it.\n\n"
			"Motion\n"
			"Once moves between the two positions during the chosen interval. Loop restarts each cycle. Ping-pong alternates direction. Fit once to line uses the full line duration. Use line end makes Once finish at the subtitle end.\n\n"
			"Accel changes the timing curve: 1 is linear, above 1 starts slower, and below 1 starts faster. Positions are percentages of the gradient range; values below 0 or above 100 place it outside the text. Colors outside the moving range keep the nearest edge color.\n\n"
			"Start, middle and end width set the size of the moving gradient at the beginning, halfway point and end. This can create a thin-wide-thin light beam.\n\n"
			"Playback\n"
			"The framed Play button plays the line once. The plain Play button repeats it. Pause stops playback. The arrow buttons step one video frame."),
			_("Gradient help"), wxOK | wxICON_INFORMATION, this);
	}

	int OutputSelection() const {
		for (size_t i = 0; i < output_choices.size(); ++i) {
			if (output_choices[i]->GetValue()) return static_cast<int>(i);
		}
		return 0;
	}

	void UpdateControls(bool immediate = false) {
		bool was_rectangular = UsesRectangularClip(settings);
		if (!was_rectangular && anti_strip_overlap->IsEnabled())
			non_rect_overlap = anti_strip_overlap->GetValue();

		settings.kind = kind->GetSelection() == 1 ? Kind::Radial : Kind::Linear;
		settings.output = static_cast<Output>(OutputSelection());
		settings.angle = angle->GetValue();
		settings.pixels_per_strip = pixels_per_strip->GetValue();
		bool rectangular = UsesRectangularClip(settings);
		if (rectangular) {
			settings.anti_strip_overlap = 0.0;
			anti_strip_overlap->SetValue(0.0);
		}
		else {
			if (was_rectangular) anti_strip_overlap->SetValue(non_rect_overlap);
			settings.anti_strip_overlap = anti_strip_overlap->GetValue();
			non_rect_overlap = settings.anti_strip_overlap;
		}
		bool characters = settings.output == Output::Characters;
		kind->Enable(!characters);
		angle->Enable(!characters && settings.kind == Kind::Linear);
		angle_dial->Enable(!characters && settings.kind == Kind::Linear);
		for (auto button : angle_presets)
			button->Enable(!characters && settings.kind == Kind::Linear);
		angle_dial->SetValue(settings.angle);
		pixels_per_strip->Enable(!characters);
		anti_strip_overlap->Enable(!characters && !rectangular);
		Layout();
		SchedulePreview(immediate);
	}

	void Accept() {
		UpdateControls();
		preview_timer.Stop();
		preview_pending = false;
		if (!settings.primary.enabled && !settings.outline.enabled && !settings.shadow.enabled) {
			wxMessageBox(_("Enable at least one color block."), _("Gradient"),
				wxOK | wxICON_WARNING, this);
			return;
		}
		if (!typesetting::gradient::Apply(context, settings)) {
			wxMessageBox(settings.output == Output::Shapes ?
				_("No selected lines could be converted to shapes.") :
				_("No characters were found in the selected lines."), _("Gradient"),
				wxOK | wxICON_WARNING, this);
			return;
		}
		accepted = true;
		EndModal(wxID_OK);
	}

public:
	explicit DialogGradient(agi::Context *context)
	: wxDialog(context->parent, wxID_ANY, _("Gradient"), wxDefaultPosition, wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, context(context)
	, settings(typesetting::gradient::LoadSettingsForSelection(context))
	, preview_session(context)
	, preview_timer(this)
	, playback_timer(this) {
		settings.shared_motion = true;
		bool rectangular = UsesRectangularClip(settings);
		non_rect_overlap = rectangular ? 0.4 : settings.anti_strip_overlap;
		if (rectangular) settings.anti_strip_overlap = 0.0;

		auto main = new wxBoxSizer(wxVERTICAL);
		auto controls = new wxBoxSizer(wxHORIZONTAL);
		auto mode_controls = new wxBoxSizer(wxVERTICAL);
		auto mode_choices = new wxBoxSizer(wxHORIZONTAL);
		wxString kind_labels[] = {_("Linear"), _("Radial")};
		kind = new wxRadioBox(this, wxID_ANY, _("Gradient type"), wxDefaultPosition,
			wxDefaultSize, 2, kind_labels, 2, wxRA_SPECIFY_COLS);
		kind->SetSelection(settings.kind == Kind::Radial ? 1 : 0);
		mode_choices->Add(kind, 0, wxRIGHT, 8);

		auto output_box = new wxStaticBoxSizer(wxHORIZONTAL, this, _("Output"));
		auto output_parent = output_box->GetStaticBox();
		std::array<wxString, 3> output_labels = {"Clip", _("By character"), _("With shapes")};
		for (size_t i = 0; i < output_choices.size(); ++i) {
			output_choices[i] = new wxRadioButton(output_parent, wxID_ANY, output_labels[i],
				wxDefaultPosition, wxDefaultSize, i == 0 ? wxRB_GROUP : 0);
			output_box->Add(output_choices[i], 0, wxALIGN_CENTER_VERTICAL);
			if (i + 1 < output_choices.size()) output_box->AddStretchSpacer();
		}
		output_choices[static_cast<int>(settings.output)]->SetValue(true);
		output_box->SetMinSize(FromDIP(wxSize(294, -1)));
		mode_choices->Add(output_box, 0, wxEXPAND);
		mode_controls->Add(mode_choices);

		auto overlap_row = new wxBoxSizer(wxHORIZONTAL);
		auto overlap_label = new wxStaticText(this, wxID_ANY, _("Anti-strip overlap:"));
		overlap_row->Add(overlap_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		anti_strip_overlap = new wxSpinCtrlDouble(this, wxID_ANY, "", wxDefaultPosition,
			FromDIP(wxSize(85, -1)), wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,
			0.0, 100.0, settings.anti_strip_overlap, 0.1);
		anti_strip_overlap->SetDigits(2);
		auto overlap_tip = _(
			"Overlap on each side of oblique, radial, and shape bands, in script units. Rectangular clips force this value to 0.");
		overlap_label->SetToolTip(overlap_tip);
		anti_strip_overlap->SetToolTip(overlap_tip);
		overlap_row->Add(anti_strip_overlap, 0, wxALIGN_CENTER_VERTICAL);
		mode_controls->Add(overlap_row, 0, wxTOP, 5);
		controls->Add(mode_controls, 0, wxRIGHT, 12);

		auto numeric = new wxFlexGridSizer(2, 5, 6);
		numeric->Add(new wxStaticText(this, wxID_ANY, _("Angle:")), 0, wxALIGN_CENTER_VERTICAL);
		auto angle_controls = new wxBoxSizer(wxHORIZONTAL);
		angle_dial = new AngleDial(this, context, settings.angle, [this](int value, bool final) {
			syncing_angle = true;
			angle->SetValue(value);
			syncing_angle = false;
			UpdateControls(final);
		});
		angle_controls->Add(angle_dial, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		angle = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(85, -1)),
			wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER, 0, 359, settings.angle);
		angle_controls->Add(angle, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		std::array<wxString, 4> direction_labels = {
			wxString::FromUTF8("→"), wxString::FromUTF8("↓"),
			wxString::FromUTF8("←"), wxString::FromUTF8("↑")};
		constexpr std::array<int, 4> direction_angles = {0, 90, 180, 270};
		for (size_t i = 0; i < angle_presets.size(); ++i) {
			angle_presets[i] = new wxButton(this, wxID_ANY, direction_labels[i],
				wxDefaultPosition, FromDIP(wxSize(28, 26)), wxBU_EXACTFIT);
			angle_presets[i]->SetToolTip(wxString::Format("%d", direction_angles[i]) +
				wxString::FromUTF8("°"));
			angle_presets[i]->Bind(wxEVT_BUTTON, [this, value = direction_angles[i]](wxCommandEvent&) {
				syncing_angle = true;
				angle->SetValue(value);
				syncing_angle = false;
				angle_dial->SetValue(value);
				UpdateControls(true);
			});
			angle_controls->Add(angle_presets[i], 0, wxALIGN_CENTER_VERTICAL |
				(i + 1 == angle_presets.size() ? 0 : wxRIGHT), 2);
		}
		numeric->Add(angle_controls, 0, wxALIGN_CENTER_VERTICAL);
		numeric->Add(new wxStaticText(this, wxID_ANY, _("Pixels per strip:")), 0,
			wxALIGN_CENTER_VERTICAL);
		pixels_per_strip = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition,
			FromDIP(wxSize(85, -1)), wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER, 1, 100,
			settings.pixels_per_strip);
		numeric->Add(pixels_per_strip, 0, wxALIGN_CENTER_VERTICAL);
		controls->Add(numeric, 0, wxALIGN_CENTER_VERTICAL);
		main->Add(controls, 0, wxEXPAND | wxALL, 8);

		auto explanation = new wxStaticText(this, wxID_ANY,
			_("With shapes there will be no clips and you can give additional clip, blur, etc. You can edit the gradient after saving."));
		explanation->Wrap(controls->GetMinSize().x);
		main->Add(explanation, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		auto relayout = [this] { CallAfter([this] { LayoutContent(); }); };
		auto utility_row = new wxBoxSizer(wxHORIZONTAL);
		auto info = new wxButton(this, wxID_ANY, _("Info"));
		info->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowHelp(); });
		utility_row->Add(info, 0, wxALIGN_CENTER_VERTICAL);
		utility_row->AddStretchSpacer();
		auto add_transport = [&](char const *command_name, wxString const& tooltip,
				std::function<void()> action) {
			auto button = new wxBitmapButton(this, wxID_ANY, cmd::get(command_name)->Icon(16),
				wxDefaultPosition, FromDIP(wxSize(30, 28)));
			button->SetToolTip(tooltip);
			button->Bind(wxEVT_BUTTON, [action = std::move(action)](wxCommandEvent&) {
				action();
			});
			utility_row->Add(button, 0, wxLEFT, 3);
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
			utility_row->Add(button, 0, wxLEFT, 3);
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
		main->Add(utility_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		motion_editor = std::make_unique<MotionEditor>(this, _("Motion"),
			settings.motion, !settings.motion.enabled, relayout,
			[this](bool immediate) { SchedulePreview(immediate); });
		main->Add(motion_editor->Window(), 0,
			wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		sections_panel = new wxPanel(this, wxID_ANY);
		auto sections = new wxBoxSizer(wxVERTICAL);
		sections_panel->SetSizer(sections);
		auto copier = [this](int source, int target) {
			CopyStops(source, target);
		};
		auto preview = [this](bool immediate) { SchedulePreview(immediate); };
		blocks[0] = std::make_unique<GradientBlock>(sections_panel, _("Color"),
			settings.primary, 0, false, relayout, copier, preview);
		blocks[1] = std::make_unique<GradientBlock>(sections_panel, _("Bord color"),
			settings.outline, 1, !settings.outline.enabled, relayout, copier, preview);
		blocks[2] = std::make_unique<GradientBlock>(sections_panel, _("Shadow color"),
			settings.shadow, 2, !settings.shadow.enabled, relayout, copier, preview);
		for (auto const& block : blocks)
			sections->Add(block->Window(), 0, wxEXPAND | wxBOTTOM, 6);
		main->Add(sections_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

		auto buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
		main->Add(buttons, 0, wxEXPAND | wxALL, 8);
		SetSizer(main);

		preview_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
			if (preview_pending) RunPreview();
		});
		playback_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
			if (!loop_playback || this->context->videoController->IsPlaying()) return;
			this->context->videoController->PlayLine();
			if (!this->context->videoController->IsPlaying()) StopPlayback();
		});
		kind->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent&) { UpdateControls(true); });
		for (auto choice : output_choices)
			choice->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) { UpdateControls(true); });
		angle->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { UpdateControls(true); });
		angle->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
			if (!syncing_angle) UpdateControls();
		});
		angle->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { UpdateControls(true); });
		pixels_per_strip->Bind(wxEVT_SPINCTRL,
			[this](wxSpinEvent&) { UpdateControls(true); });
		pixels_per_strip->Bind(wxEVT_TEXT,
			[this](wxCommandEvent&) { UpdateControls(); });
		pixels_per_strip->Bind(wxEVT_TEXT_ENTER,
			[this](wxCommandEvent&) { UpdateControls(true); });
		anti_strip_overlap->Bind(wxEVT_SPINCTRLDOUBLE,
			[this](wxSpinDoubleEvent&) { UpdateControls(true); });
		anti_strip_overlap->Bind(wxEVT_TEXT,
			[this](wxCommandEvent&) { UpdateControls(); });
		anti_strip_overlap->Bind(wxEVT_TEXT_ENTER,
			[this](wxCommandEvent&) { UpdateControls(true); });
		Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Accept(); }, wxID_OK);
		Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
			if (event.GetKeyCode() == WXK_ESCAPE && angle_dial->CancelMeasurement()) return;
			event.Skip();
		});
		UpdateControls(true);
		LayoutContent();
		CentreOnParent();
	}

	~DialogGradient() override {
		StopPlayback();
		preview_timer.Stop();
		preview_pending = false;
		// Restoring the original preview while the application is already closing only
		// requests one last video render and causes the same shutdown flash that the
		// Transformation tool avoids.
		bool window_going = context->parent && context->parent->IsBeingDeleted();
		if (!accepted && !window_going) preview_session.Clear();
	}
};

} // namespace

void ShowGradientDialog(agi::Context *c) {
	DialogGradient(c).ShowModal();
}
