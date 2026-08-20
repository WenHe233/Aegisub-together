// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "visual_tool_preview.h"

#include "compat.h"
#include "gl_text.h"
#include "gl_wrap.h"
#include "options.h"

#include <libaegisub/color.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include <wx/colour.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/settings.h>

namespace {
	constexpr float margin = 12.f;
	constexpr float top = 10.f;
	constexpr float control_height = 34.f;
	constexpr float gap = 8.f;
	constexpr float row_gap = 8.f;

	void NormaliseControlOrder(VisualToolPreviewInterface::Page& page) {
		for (auto& control : page.controls) {
			using Style = VisualToolPreviewInterface::ControlStyle;
			if (control.style == Style::Accept &&
				control.label.StartsWith(_("Accept") + " ("))
				control.label = _("Accept");
			else if (control.style == Style::Cancel &&
				control.label.StartsWith(_("Cancel") + " ("))
				control.label = _("Cancel");
		}
		auto priority = [](VisualToolPreviewInterface::Control const& control) {
			using Kind = VisualToolPreviewInterface::ControlKind;
			using Style = VisualToolPreviewInterface::ControlStyle;
			if (control.kind == Kind::Undo || control.kind == Kind::Redo) return 0;
			if (control.before_accept) return 1;
			if (control.style == Style::Accept || control.style == Style::Cancel) return 2;
			return 3;
		};
		std::stable_sort(page.controls.begin(), page.controls.end(),
			[&](auto const& left, auto const& right) {
				return priority(left) < priority(right);
			});
	}
}

VisualToolPreviewInterface::~VisualToolPreviewInterface() {
	DetachHost();
}

void VisualToolPreviewInterface::AttachHost(VisualToolPreviewBar *new_host,
	std::function<void (int)> on_action,
	std::function<void (int, double, bool)> on_value, bool visible) {
	host_visible = visible;
	if (host == new_host) {
		action_handler = std::move(on_action);
		value_handler = std::move(on_value);
		NotifyHost();
		return;
	}
	DetachHost();
	host = new_host;
	action_handler = std::move(on_action);
	value_handler = std::move(on_value);
	if (host) host->Attach(this);
}

void VisualToolPreviewInterface::DetachHost() {
	if (host) {
		auto *old_host = host;
		host = nullptr;
		old_host->Detach(this);
	}
	action_handler = {};
	value_handler = {};
}

void VisualToolPreviewInterface::HostDestroyed(VisualToolPreviewBar *destroyed_host) {
	if (host != destroyed_host) return;
	host = nullptr;
	action_handler = {};
	value_handler = {};
}

void VisualToolPreviewInterface::Activate(int id) const {
	// A terminal action can replace and destroy the active visual tool. Invoke a copy so the
	// callable remains alive even when that also destroys this interface and its stored handler.
	auto handler = action_handler;
	if (handler) handler(id);
}

void VisualToolPreviewInterface::ActivateValue(int id, double value, bool final) const {
	auto handler = value_handler;
	if (handler) handler(id, value, final);
}

void VisualToolPreviewInterface::NotifyHost() {
	if (host) host->RefreshFromSource();
}

void VisualToolPreviewInterface::SetPage(Page page) {
	NormaliseControlOrder(page);
	if (pages.empty()) pages.push_back(std::move(page));
	else pages.back() = std::move(page);
	NotifyHost();
}

void VisualToolPreviewInterface::ClearPages() {
	if (pages.empty()) return;
	pages.clear();
	NotifyHost();
}

void VisualToolPreviewInterface::PushPage(Page page) {
	// A tool which only uses the shared drawing primitives still has an implicit root page.
	// Creating it here lets a nested stage (AI refinement, for example) pop cleanly back to
	// that tool-specific surface without forcing it into the automatic page layout.
	if (pages.empty()) pages.emplace_back();
	NormaliseControlOrder(page);
	pages.push_back(std::move(page));
	NotifyHost();
}

bool VisualToolPreviewInterface::PopPage() {
	if (pages.size() <= 1) return false;
	pages.pop_back();
	NotifyHost();
	return true;
}

void VisualToolPreviewInterface::Clear() {
	pages.clear();
	text_width_cache.clear();
	NotifyHost();
}

float VisualToolPreviewInterface::TextWidth(OpenGLText& text, wxString const& label,
	bool bold) const {
	std::string key = (bold ? "b:" : "r:") + from_wx(label);
	auto found = text_width_cache.find(key);
	if (found != text_width_cache.end()) return found->second;
	text.SetFont("Verdana", 9, bold, false);
	int width = 0, height = 0;
	text.GetExtent(from_wx(label), width, height);
	text_width_cache.emplace(std::move(key), static_cast<float>(width));
	return static_cast<float>(width);
}

VisualToolPreviewInterface::Layout VisualToolPreviewInterface::BuildLayout(
	OpenGLText& text, Vector2D canvas) const {
	Layout out;
	if (pages.empty()) return out;

	float left = margin;
	float row_top = top;
	float right_limit = std::max(margin + 34.f, canvas.X() - 8.f);
	auto next_row = [&] {
		left = margin;
		row_top += control_height + row_gap;
	};

	for (auto const& control : pages.back().controls) {
		if (control.kind == ControlKind::Spacer) {
			left += gap;
			continue;
		}
		float width = control.kind == ControlKind::Undo || control.kind == ControlKind::Redo ?
			34.f : control.kind == ControlKind::Slider ? 170.f :
			TextWidth(text, control.label, control.kind != ControlKind::Toggle) +
				(control.kind == ControlKind::Toggle ? 28.f : control.dropdown ? 34.f : 24.f);
		if (left + width > right_limit && left > margin) next_row();
		out.controls.push_back({&control,
			{Vector2D(left, row_top), Vector2D(left + width, row_top + control_height)}});
		left += width + gap;
	}

	if (!pages.back().message.empty()) {
		float message_width = TextWidth(text, pages.back().message, false);
		float message_left = left > margin ? left + 2.f : left;
		if (message_left + message_width > right_limit && message_left > margin) {
			next_row();
			message_left = left;
		}
		out.message_bounds = {Vector2D(message_left, row_top),
			Vector2D(std::min(right_limit, message_left + message_width),
				row_top + control_height)};
	}
	out.height = row_top + control_height + 10.f;
	return out;
}

bool VisualToolPreviewInterface::Contains(
	std::pair<Vector2D, Vector2D> const& bounds, Vector2D point) {
	return point.X() >= bounds.first.X() && point.X() <= bounds.second.X() &&
		point.Y() >= bounds.first.Y() && point.Y() <= bounds.second.Y();
}

float VisualToolPreviewInterface::Height(OpenGLText& text, Vector2D canvas) const {
	return BuildLayout(text, canvas).height;
}

std::pair<Vector2D, Vector2D> VisualToolPreviewInterface::BoundsFor(int id,
	OpenGLText& text, Vector2D canvas) const {
	auto layout = BuildLayout(text, canvas);
	for (auto const& found : layout.controls)
		if (found.control->id == id) return found.bounds;
	return {Vector2D(margin, top), Vector2D(margin, top + control_height)};
}

int VisualToolPreviewInterface::HitTest(Vector2D point, OpenGLText& text,
	Vector2D canvas) const {
	auto layout = BuildLayout(text, canvas);
	for (auto const& found : layout.controls)
		if (found.control->enabled && Contains(found.bounds, point)) return found.control->id;
	return 0;
}

void VisualToolPreviewInterface::RoundedRectangle(OpenGLWrapper& gl, Vector2D first,
	Vector2D second, float radius, unsigned char red, unsigned char green,
	unsigned char blue, float alpha) {
	float safe_radius = std::min({radius, (second.X() - first.X()) * .5f,
		(second.Y() - first.Y()) * .5f});
	wxColour colour(red, green, blue);
	gl.SetFillColour(colour, alpha);
	gl.SetLineColour(colour, 0.f, 1);
	gl.DrawRectangle(first + Vector2D(safe_radius, 0.f),
		second - Vector2D(safe_radius, 0.f));
	gl.DrawRectangle(first + Vector2D(0.f, safe_radius),
		second - Vector2D(0.f, safe_radius));
	gl.DrawCircle(first + Vector2D(safe_radius, safe_radius), safe_radius);
	gl.DrawCircle(Vector2D(second.X() - safe_radius, first.Y() + safe_radius), safe_radius);
	gl.DrawCircle(Vector2D(first.X() + safe_radius, second.Y() - safe_radius), safe_radius);
	gl.DrawCircle(second - Vector2D(safe_radius, safe_radius), safe_radius);
}

void VisualToolPreviewInterface::DrawBackground(OpenGLWrapper& gl, Vector2D canvas,
	float height) const {
	gl.SetFillColour(*wxBLACK, .72f);
	gl.SetLineColour(*wxBLACK, 0.f, 1);
	gl.DrawRectangle(Vector2D(0.f, 0.f), Vector2D(canvas.X(), height));
}

void VisualToolPreviewInterface::DrawPanel(OpenGLWrapper& gl,
	std::pair<Vector2D, Vector2D> bounds, bool enabled, bool hovered,
	bool selected) const {
	wxColour colour = !enabled ? wxColour(66, 69, 73) :
		selected ? wxColour(35, 125, 153) : wxColour(55, 59, 64);
	if (enabled && hovered) colour = colour.ChangeLightness(118);
	RoundedRectangle(gl, bounds.first, bounds.second, 7.f,
		colour.Red(), colour.Green(), colour.Blue());
}

void VisualToolPreviewInterface::DrawButton(OpenGLWrapper& gl, OpenGLText& text,
	std::pair<Vector2D, Vector2D> bounds, wxString const& label, ControlStyle style,
	bool enabled, bool hovered, bool selected, bool dropdown) const {
	wxColour colour;
	if (!enabled) colour = wxColour(66, 69, 73);
	else if (selected || style == ControlStyle::Accent) colour = wxColour(35, 125, 153);
	else if (style == ControlStyle::Accept) colour = wxColour(31, 153, 76);
	else if (style == ControlStyle::Cancel) colour = wxColour(183, 54, 61);
	else if (style == ControlStyle::Warning) colour = wxColour(180, 105, 43);
	else colour = wxColour(55, 59, 64);
	if (enabled && hovered) colour = colour.ChangeLightness(118);
	RoundedRectangle(gl, bounds.first, bounds.second, 7.f,
		colour.Red(), colour.Green(), colour.Blue());

	wxColour content = enabled ? *wxWHITE : wxColour(145, 148, 152);
	text.SetFont("Verdana", 9, true, false);
	text.SetColour(agi::Color(content.Red(), content.Green(), content.Blue(), 255));
	std::string utf8 = from_wx(label);
	int text_width = 0, text_height = 0;
	text.GetExtent(utf8, text_width, text_height);
	text.Print(utf8, static_cast<int>(bounds.first.X() + 12.f),
		static_cast<int>((bounds.first.Y() + bounds.second.Y() - text_height) * .5f));
	if (dropdown) {
		float y = (bounds.first.Y() + bounds.second.Y()) * .5f;
		gl.SetFillColour(content, 1.f);
		gl.DrawTriangle(Vector2D(bounds.second.X() - 14.f, y - 2.f),
			Vector2D(bounds.second.X() - 6.f, y - 2.f),
			Vector2D(bounds.second.X() - 10.f, y + 3.f));
	}
}

void VisualToolPreviewInterface::DrawHistory(OpenGLWrapper& gl,
	std::pair<Vector2D, Vector2D> bounds, bool redo, bool enabled, bool hovered) const {
	DrawPanel(gl, bounds, enabled, hovered);
	wxColour content = enabled ? *wxWHITE : wxColour(145, 148, 152);
	gl.SetLineColour(content, 1.f, 3);
	float direction = redo ? 1.f : -1.f;
	Vector2D centre = (bounds.first + bounds.second) * .5f;
	Vector2D tip = centre + Vector2D(direction * 7.f, 0.f);
	gl.DrawLine(centre - Vector2D(direction * 7.f, 0.f), tip);
	gl.DrawLine(tip, tip - Vector2D(direction * 5.f, 5.f));
	gl.DrawLine(tip, tip - Vector2D(direction * 5.f, -5.f));
}

void VisualToolPreviewInterface::DrawToggle(OpenGLWrapper& gl, OpenGLText& text,
	std::pair<Vector2D, Vector2D> bounds, wxString const& label, bool checked,
	bool enabled, bool hovered) const {
	float middle = (bounds.first.Y() + bounds.second.Y()) * .5f;
	Vector2D mark(bounds.first.X() + 8.f, middle);
	gl.SetLineColour(enabled && hovered ? *wxWHITE : wxColour(190, 194, 198), 1.f, 1);
	gl.SetFillColour(*wxBLACK, .45f);
	gl.DrawRectangle(mark - Vector2D(7.f, 7.f), mark + Vector2D(7.f, 7.f));
	if (checked) {
		gl.SetLineColour(wxColour(120, 220, 140), 1.f, 2);
		gl.DrawLine(mark + Vector2D(-4.f, 0.f), mark + Vector2D(-1.f, 4.f));
		gl.DrawLine(mark + Vector2D(-1.f, 4.f), mark + Vector2D(5.f, -5.f));
	}
	text.SetFont("Verdana", 9, false, false);
	wxColour content = enabled ? checked ? *wxWHITE : wxColour(170, 174, 178) :
		wxColour(125, 128, 132);
	text.SetColour(agi::Color(content.Red(), content.Green(), content.Blue(), 255));
	std::string utf8 = from_wx(label);
	int width = 0, height = 0;
	text.GetExtent(utf8, width, height);
	text.Print(utf8, static_cast<int>(mark.X() + 12.f),
		static_cast<int>(middle - height * .5f));
}

void VisualToolPreviewInterface::DrawMessage(OpenGLText& text,
	std::pair<Vector2D, Vector2D> row, float left, wxString const& message) const {
	if (message.empty()) return;
	text.SetFont("Verdana", 9, false, false);
	text.SetColour(agi::Color(225, 225, 225, 255));
	std::string utf8 = from_wx(message);
	int width = 0, height = 0;
	text.GetExtent(utf8, width, height);
	text.Print(utf8, static_cast<int>(left),
		static_cast<int>((row.first.Y() + row.second.Y() - height) * .5f));
}

void VisualToolPreviewInterface::Draw(OpenGLWrapper& gl, OpenGLText& text,
	Vector2D canvas, int hovered_id) const {
	auto layout = BuildLayout(text, canvas);
	DrawBackground(gl, canvas, layout.height);
	for (auto const& found : layout.controls) {
		auto const& control = *found.control;
		bool hovered = control.id == hovered_id;
		switch (control.kind) {
			case ControlKind::Undo:
			case ControlKind::Redo:
				DrawHistory(gl, found.bounds, control.kind == ControlKind::Redo,
					control.enabled, hovered);
				break;
			case ControlKind::Toggle:
				DrawToggle(gl, text, found.bounds, control.label, control.selected,
					control.enabled, hovered);
				break;
			case ControlKind::Slider:
				DrawPanel(gl, found.bounds, control.enabled, hovered);
				DrawMessage(text, found.bounds, found.bounds.first.X() + 8.f,
					control.label + ": " + control.value_text);
				break;
			case ControlKind::Button:
				DrawButton(gl, text, found.bounds, control.label, control.style,
					control.enabled, hovered, control.selected, control.dropdown);
				break;
			case ControlKind::Spacer: break;
		}
	}
	if (!pages.empty() && !pages.back().message.empty())
		DrawMessage(text, layout.message_bounds, layout.message_bounds.first.X(),
			pages.back().message);
}

// ---------------------------------------------------------------- native preview host

VisualToolPreviewBar::VisualToolPreviewBar(wxWindow *parent)
: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	Hide();
	Bind(wxEVT_PAINT, &VisualToolPreviewBar::OnPaint, this);
	Bind(wxEVT_MOTION, &VisualToolPreviewBar::OnMouseMove, this);
	Bind(wxEVT_LEAVE_WINDOW, &VisualToolPreviewBar::OnMouseLeave, this);
	Bind(wxEVT_LEFT_DOWN, &VisualToolPreviewBar::OnLeftDown, this);
	Bind(wxEVT_LEFT_UP, &VisualToolPreviewBar::OnLeftUp, this);
	Bind(wxEVT_MOUSEWHEEL, &VisualToolPreviewBar::OnMouseWheel, this);
	Bind(wxEVT_SIZE, &VisualToolPreviewBar::OnSize, this);
	icon_size_connection = OPT_SUB("App/Toolbar Icon Size", [this] {
		RefreshFromSource();
	});
}

VisualToolPreviewBar::~VisualToolPreviewBar() {
	auto attached = sources;
	for (auto *item : attached)
		if (item) item->HostDestroyed(this);
}

int VisualToolPreviewBar::ToolbarIconSize() const {
	return std::max(16, static_cast<int>(OPT_GET("App/Toolbar Icon Size")->GetInt()));
}

int VisualToolPreviewBar::Dip(int value) const {
	return FromDIP(value);
}

VisualToolPreviewInterface::Page const *VisualToolPreviewBar::DisplayPage() const {
	if (shutting_down) return has_frozen_page ? &frozen_page : nullptr;
	return source ? source->CurrentPage() : nullptr;
}

void VisualToolPreviewBar::Attach(VisualToolPreviewInterface *new_source) {
	if (shutting_down) return;
	sources.erase(std::remove(sources.begin(), sources.end(), new_source), sources.end());
	sources.push_back(new_source);
	source = new_source;
	hovered_id = 0;
	dragging_id = 0;
	dragging_track = wxRect();
	RefreshFromSource();
}

void VisualToolPreviewBar::Detach(VisualToolPreviewInterface *old_source) {
	auto previous_source = source;
	sources.erase(std::remove(sources.begin(), sources.end(), old_source), sources.end());
	source = sources.empty() ? nullptr : sources.back();
	if (shutting_down) return;
	if (source == previous_source) return;
	if (HasCapture()) ReleaseMouse();
	controls.clear();
	message_bounds = wxRect();
	hovered_id = 0;
	dragging_id = 0;
	dragging_track = wxRect();
	RefreshFromSource();
}

void VisualToolPreviewBar::UpdateHeight(int height) {
	if (height == content_height) return;
	content_height = height;
	SetMinSize(wxSize(-1, height));
	SetMaxSize(wxSize(-1, height));
	SetSize(GetSize().GetWidth(), height);
	InvalidateBestSize();
	RequestParentLayout();
}

void VisualToolPreviewBar::RequestParentLayout() {
	if (layout_pending) return;
	layout_pending = true;
	CallAfter([this] {
		layout_pending = false;
		if (auto *parent = GetParent()) parent->Layout();
	});
}

void VisualToolPreviewBar::RebuildLayout(wxDC& dc, int width) {
	controls.clear();
	message_bounds = wxRect();
	auto const *page = DisplayPage();
	if (!page) {
		UpdateHeight(0);
		return;
	}

	double ratio = std::max(1.0, ToolbarIconSize() / 16.0);
	int bar_margin = Dip(4);
	int bar_gap = std::max(Dip(6), static_cast<int>(std::round(5 * ratio)));
	int bar_row_gap = Dip(3);
	int bar_control_height = ToolbarIconSize() + Dip(10);
	int right_limit = std::max(bar_margin + bar_control_height, width - bar_margin);
	int left = bar_margin;
	int row_top = bar_margin;

	wxFont regular = GetFont();
	regular.SetFaceName("Verdana");
	regular.SetPointSize(std::max(regular.GetPointSize(),
		static_cast<int>(std::round(9.0 * std::sqrt(ratio)))));
	wxFont bold = regular;
	bold.SetWeight(wxFONTWEIGHT_BOLD);

	auto next_row = [&] {
		left = bar_margin;
		row_top += bar_control_height + bar_row_gap;
	};
	for (auto const& control : page->controls) {
		if (control.kind == VisualToolPreviewInterface::ControlKind::Spacer) {
			if (left + bar_gap <= right_limit) left += bar_gap;
			continue;
		}
		dc.SetFont(control.kind == VisualToolPreviewInterface::ControlKind::Toggle ? regular : bold);
		int item_width = control.kind == VisualToolPreviewInterface::ControlKind::Undo ||
			control.kind == VisualToolPreviewInterface::ControlKind::Redo ? bar_control_height :
			control.icon_only ? bar_control_height + (control.dropdown ? Dip(10) : 0) :
			control.kind == VisualToolPreviewInterface::ControlKind::Slider ?
				(control.width > 0 ? Dip(control.width) : Dip(170)) :
			dc.GetTextExtent(control.label).GetWidth() +
				(control.kind == VisualToolPreviewInterface::ControlKind::Toggle ? Dip(28) :
				 control.dropdown ? Dip(30) : Dip(20)) + (control.swatch.IsOk() ? Dip(18) : 0);
		int slider_value_width = 0;
		if (control.kind == VisualToolPreviewInterface::ControlKind::Slider) {
			dc.SetFont(regular);
			slider_value_width = dc.GetTextExtent(control.value_text).GetWidth();
			if (!control.value_text_sample.empty())
				slider_value_width = std::max(slider_value_width,
					dc.GetTextExtent(control.value_text_sample).GetWidth());
			int content_width = dc.GetTextExtent(control.label).GetWidth() +
				slider_value_width + Dip(55);
			item_width = std::max(item_width, content_width);
		}
		item_width = std::min(item_width, std::max(bar_control_height,
			right_limit - bar_margin));
		if (left + item_width > right_limit && left > bar_margin) next_row();
		wxRect bounds(left, row_top, item_width, bar_control_height);
		wxRect track;
		if (control.kind == VisualToolPreviewInterface::ControlKind::Slider) {
			dc.SetFont(regular);
			int label_width = dc.GetTextExtent(control.label).GetWidth();
			int track_left = bounds.x + label_width + Dip(13);
			int track_right = bounds.GetRight() - slider_value_width - Dip(14);
			track = wxRect(track_left, bounds.y, std::max(Dip(24), track_right - track_left),
				bounds.height);
		}
		controls.push_back({&control, bounds, track});
		left += item_width + bar_gap;
	}

	if (!page->message.empty()) {
		dc.SetFont(regular);
		int message_width = dc.GetTextExtent(page->message).GetWidth();
		int message_left = left > bar_margin ? left + bar_gap : left;
		if (message_left + message_width > right_limit && message_left > bar_margin) {
			next_row();
			message_left = left;
		}
		message_bounds = wxRect(message_left, row_top,
			std::max(0, right_limit - message_left), bar_control_height);
	}
	UpdateHeight(row_top + bar_control_height + bar_margin);
}

void VisualToolPreviewBar::RefreshFromSource() {
	if (shutting_down) return;
	bool should_show = DisplayPage() && source && source->host_visible;
	if (!should_show) {
		dragging_id = 0;
		if (HasCapture()) ReleaseMouse();
		controls.clear();
		message_bounds = wxRect();
		hovered_id = 0;
		dragging_track = wxRect();
		SetCursor(wxCursor(wxCURSOR_ARROW));
		UpdateHeight(0);
		if (IsShown()) {
			Hide();
			RequestParentLayout();
		}
		return;
	}

	if (!IsShown()) {
		Show();
		RequestParentLayout();
	}
	wxClientDC dc(this);
	RebuildLayout(dc, std::max(Dip(120), GetClientSize().GetWidth()));
	Refresh(false);
}

int VisualToolPreviewBar::HitTest(wxPoint point) const {
	for (auto const& found : controls)
		if (found.control->enabled && found.bounds.Contains(point)) return found.control->id;
	return 0;
}

void VisualToolPreviewBar::OnMouseMove(wxMouseEvent& event) {
	if (dragging_id && event.LeftIsDown()) {
		UpdateSlider(dragging_id, event.GetX());
		return;
	}
	int next = HitTest(event.GetPosition());
	if (next != hovered_id) {
		hovered_id = next;
		SetCursor(wxCursor(next ? wxCURSOR_HAND : wxCURSOR_ARROW));
		Refresh(false);
	}
}

void VisualToolPreviewBar::OnMouseLeave(wxMouseEvent&) {
	if (!hovered_id) return;
	hovered_id = 0;
	SetCursor(wxCursor(wxCURSOR_ARROW));
	Refresh(false);
}

void VisualToolPreviewBar::OnLeftDown(wxMouseEvent& event) {
	int id = HitTest(event.GetPosition());
	if (!id || !source) return;
	for (auto const& found : controls) {
		if (found.control->id != id ||
			found.control->kind != VisualToolPreviewInterface::ControlKind::Slider) continue;
		dragging_id = id;
		dragging_control = *found.control;
		dragging_track = found.track;
		if (!HasCapture()) CaptureMouse();
		UpdateSlider(id, event.GetX());
		return;
	}
	auto *active_source = source;
	CallAfter([this, active_source, id] {
		if (source == active_source &&
			std::find(sources.begin(), sources.end(), active_source) != sources.end())
			active_source->Activate(id);
	});
}

void VisualToolPreviewBar::OnLeftUp(wxMouseEvent& event) {
	if (!dragging_id) return;
	int id = dragging_id;
	UpdateSlider(id, event.GetX(), true);
	dragging_id = 0;
	dragging_track = wxRect();
	if (HasCapture()) ReleaseMouse();
}

void VisualToolPreviewBar::UpdateSlider(int id, int x, bool final) {
	if (!source) return;
	auto update = [&](VisualToolPreviewInterface::Control const& control, wxRect const& track) {
		double ratio = std::clamp((x - track.x) /
			static_cast<double>(std::max(1, track.width)), 0.0, 1.0);
		double value = control.minimum + ratio * (control.maximum - control.minimum);
		if (control.step > 0.0) value = std::round(value / control.step) * control.step;
		source->ActivateValue(id, std::clamp(value, control.minimum, control.maximum), final);
	};
	// Updating a preview value can rebuild the page and slightly change the track geometry
	// (for example when the value text gains a digit). Keep the geometry captured on mouse-down
	// for the whole gesture, including mouse-up, so the last event cannot move the value again.
	if (dragging_id == id && dragging_track.width > 0) {
		update(dragging_control, dragging_track);
		return;
	}
	for (auto const& found : controls) {
		if (found.control->id != id || found.track.width <= 0) continue;
		update(*found.control, found.track);
		return;
	}
}

void VisualToolPreviewBar::OnMouseWheel(wxMouseEvent& event) {
	if (!source || event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL) {
		event.Skip();
		return;
	}
	int id = HitTest(event.GetPosition());
	for (auto const& found : controls) {
		if (found.control->id != id ||
			found.control->kind != VisualToolPreviewInterface::ControlKind::Slider) continue;
		auto control = *found.control;
		int rotation = event.GetWheelRotation();
		int delta = event.GetWheelDelta();
		if (!rotation || !delta) return;
		int notches = rotation / delta;
		if (!notches) notches = rotation > 0 ? 1 : -1;
		double step = control.step > 0.0 ? control.step :
			(control.maximum - control.minimum) / 100.0;
		double value = std::clamp(control.value + notches * step,
			control.minimum, control.maximum);
		if (control.step > 0.0) value = std::round(value / control.step) * control.step;
		source->ActivateValue(id, value, true);
		return;
	}
	event.Skip();
}

void VisualToolPreviewBar::OnSize(wxSizeEvent& event) {
	if (DisplayPage()) {
		wxClientDC dc(this);
		RebuildLayout(dc, std::max(Dip(120), event.GetSize().GetWidth()));
		Refresh(false);
	}
	event.Skip();
}

void VisualToolPreviewBar::OnPaint(wxPaintEvent&) {
	wxAutoBufferedPaintDC dc(this);
	// Keep the established in-video preview palette even though the common host now lives
	// above the video. The dimensions are deliberately more compact than the old overlay.
	wxColour background(30, 32, 35);
	dc.SetBackground(wxBrush(background));
	dc.Clear();
	dc.SetPen(wxPen(wxColour(49, 53, 58), 1));
	dc.DrawLine(0, 0, GetClientSize().GetWidth(), 0);
	dc.SetPen(wxPen(wxColour(8, 9, 10), 1));
	dc.DrawLine(0, GetClientSize().GetHeight() - 1,
		GetClientSize().GetWidth(), GetClientSize().GetHeight() - 1);

	auto const *page = DisplayPage();
	if (!page) return;

	double ratio = std::max(1.0, ToolbarIconSize() / 16.0);
	wxFont regular = GetFont();
	regular.SetFaceName("Verdana");
	regular.SetPointSize(std::max(regular.GetPointSize(),
		static_cast<int>(std::round(9.0 * std::sqrt(ratio)))));
	wxFont bold = regular;
	bold.SetWeight(wxFONTWEIGHT_BOLD);
	std::unique_ptr<wxGraphicsContext> graphics(wxGraphicsContext::Create(dc));

	auto rounded = [&](wxRect const& bounds, wxColour colour) {
		if (!graphics) {
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.SetBrush(wxBrush(colour));
			dc.DrawRectangle(bounds);
			return;
		}
		graphics->SetPen(wxPen(colour));
		graphics->SetBrush(wxBrush(colour));
		graphics->DrawRoundedRectangle(bounds.x, bounds.y, bounds.width, bounds.height, Dip(5));
	};

	for (auto const& found : controls) {
		auto const& control = *found.control;
		bool hovered = hovered_id == control.id;
		wxColour colour;
		if (!control.enabled) colour = wxColour(66, 69, 73);
		else if (control.selected || control.style == VisualToolPreviewInterface::ControlStyle::Accent)
			colour = wxColour(35, 125, 153);
		else if (control.style == VisualToolPreviewInterface::ControlStyle::Accept)
			colour = wxColour(31, 153, 76);
		else if (control.style == VisualToolPreviewInterface::ControlStyle::Cancel)
			colour = wxColour(183, 54, 61);
		else if (control.style == VisualToolPreviewInterface::ControlStyle::Warning)
			colour = wxColour(180, 105, 43);
		else colour = wxColour(55, 59, 64);
		if (control.enabled && hovered) colour = colour.ChangeLightness(118);
		if (control.kind != VisualToolPreviewInterface::ControlKind::Toggle)
			rounded(found.bounds, colour);

		wxColour content = control.enabled ? wxColour(255, 255, 255) : wxColour(145, 148, 152);
		if (control.kind == VisualToolPreviewInterface::ControlKind::Undo ||
			control.kind == VisualToolPreviewInterface::ControlKind::Redo) {
			int direction = control.kind == VisualToolPreviewInterface::ControlKind::Redo ? 1 : -1;
			wxPoint centre = found.bounds.GetPosition() +
				wxPoint(found.bounds.width / 2, found.bounds.height / 2);
			int reach = Dip(7);
			dc.SetPen(wxPen(content, std::max(2, Dip(2))));
			wxPoint tip = centre + wxPoint(direction * reach, 0);
			dc.DrawLine(centre - wxPoint(direction * reach, 0), tip);
			dc.DrawLine(tip, tip - wxPoint(direction * Dip(5), Dip(5)));
			dc.DrawLine(tip, tip - wxPoint(direction * Dip(5), -Dip(5)));
			continue;
		}
		if (control.kind == VisualToolPreviewInterface::ControlKind::Slider) {
			dc.SetFont(regular);
			dc.SetTextForeground(content);
			wxSize label_extent = dc.GetTextExtent(control.label);
			dc.DrawText(control.label, found.bounds.x + Dip(7),
				found.bounds.y + (found.bounds.height - label_extent.GetHeight()) / 2);
			int y = found.bounds.y + found.bounds.height / 2;
			dc.SetPen(wxPen(control.enabled ? wxColour(130, 135, 140) :
				wxColour(82, 86, 90), std::max(1, Dip(2))));
			dc.DrawLine(found.track.x, y, found.track.GetRight(), y);
			double span = std::max(1e-12, control.maximum - control.minimum);
			double ratio_value = std::clamp((control.value - control.minimum) / span, 0.0, 1.0);
			int knob_x = found.track.x + static_cast<int>(std::round(ratio_value * found.track.width));
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.SetBrush(wxBrush(control.enabled ? wxColour(80, 220, 255) :
				wxColour(145, 148, 152)));
			dc.DrawCircle(wxPoint(knob_x, y), Dip(4));
			wxSize value_extent = dc.GetTextExtent(control.value_text);
			dc.SetTextForeground(content);
			dc.DrawText(control.value_text, found.bounds.GetRight() - value_extent.GetWidth() - Dip(6),
				found.bounds.y + (found.bounds.height - value_extent.GetHeight()) / 2);
			continue;
		}

		dc.SetFont(control.kind == VisualToolPreviewInterface::ControlKind::Toggle ? regular : bold);
		dc.SetTextForeground(content);
		wxSize extent = dc.GetTextExtent(control.label);
		int text_left = found.bounds.x + Dip(10);
		if (control.icon_only) {
			int dropdown_space = control.dropdown ? Dip(10) : 0;
			wxRect icon_area = found.bounds;
			icon_area.width -= dropdown_space;
			if (control.bitmap.IsOk()) {
				int target = std::min(ToolbarIconSize(), found.bounds.height - Dip(6));
				wxImage image = control.bitmap.ConvertToImage();
				if (image.GetWidth() != target || image.GetHeight() != target)
					image.Rescale(target, target, wxIMAGE_QUALITY_HIGH);
				dc.DrawBitmap(wxBitmap(image), icon_area.x + (icon_area.width - target) / 2,
					icon_area.y + (icon_area.height - target) / 2, true);
			}
			else if (control.icon == VisualToolPreviewInterface::ControlIcon::Font) {
				wxString label = "fn";
				wxSize icon_extent = dc.GetTextExtent(label);
				dc.DrawText(label, icon_area.x + (icon_area.width - icon_extent.GetWidth()) / 2,
					icon_area.y + (icon_area.height - icon_extent.GetHeight()) / 2);
			}
			else if (control.icon != VisualToolPreviewInterface::ControlIcon::None) {
				int centre_x = icon_area.x + icon_area.width / 2;
				int icon_top = icon_area.y + (icon_area.height - Dip(13)) / 2;
				dc.SetPen(wxPen(content, std::max(1, Dip(2))));
				for (int row = 0; row < 4; ++row) {
					bool justified = control.icon == VisualToolPreviewInterface::ControlIcon::AlignJustified;
					int line_width = justified || row % 2 == 0 ? Dip(12) : Dip(8);
					int line_left = control.icon == VisualToolPreviewInterface::ControlIcon::AlignLeft ?
						centre_x - Dip(6) :
						control.icon == VisualToolPreviewInterface::ControlIcon::AlignRight ?
						centre_x + Dip(6) - line_width : centre_x - line_width / 2;
					dc.DrawLine(line_left, icon_top + row * Dip(4),
						line_left + line_width, icon_top + row * Dip(4));
				}
			}
		}
		if (control.swatch.IsOk()) {
			int swatch_size = std::min(Dip(14), found.bounds.height - Dip(8));
			wxRect swatch(found.bounds.x + Dip(6), found.bounds.y +
				(found.bounds.height - swatch_size) / 2, swatch_size, swatch_size);
			dc.SetPen(wxPen(wxColour(14, 16, 18), 1));
			dc.SetBrush(wxBrush(control.swatch));
			dc.DrawRectangle(swatch);
			text_left = swatch.GetRight() + Dip(6);
		}
		if (control.kind == VisualToolPreviewInterface::ControlKind::Toggle) {
			int box_size = Dip(14);
			wxRect mark(found.bounds.x + Dip(8), found.bounds.y +
				(found.bounds.height - box_size) / 2, box_size, box_size);
			dc.SetPen(wxPen(wxColour(190, 194, 198), 1));
			dc.SetBrush(wxBrush(wxColour(18, 20, 22)));
			dc.DrawRectangle(mark);
			if (control.selected) {
				dc.SetPen(wxPen(wxColour(120, 220, 140), std::max(2, Dip(2))));
				dc.DrawLine(mark.x + Dip(3), mark.y + box_size / 2,
					mark.x + Dip(6), mark.y + box_size - Dip(3));
				dc.DrawLine(mark.x + Dip(6), mark.y + box_size - Dip(3),
					mark.x + box_size - Dip(2), mark.y + Dip(2));
			}
			text_left = mark.GetRight() + Dip(7);
		}
		if (!control.icon_only)
			dc.DrawText(control.label, text_left,
				found.bounds.y + (found.bounds.height - extent.GetHeight()) / 2);
		if (control.dropdown) {
			wxPoint points[] = {
				wxPoint(found.bounds.GetRight() - Dip(14), found.bounds.y + found.bounds.height / 2 - Dip(2)),
				wxPoint(found.bounds.GetRight() - Dip(6), found.bounds.y + found.bounds.height / 2 - Dip(2)),
				wxPoint(found.bounds.GetRight() - Dip(10), found.bounds.y + found.bounds.height / 2 + Dip(3))
			};
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.SetBrush(wxBrush(content));
			dc.DrawPolygon(3, points);
		}
	}

	if (!page->message.empty() && !message_bounds.IsEmpty()) {
		dc.SetFont(regular);
		dc.SetTextForeground(wxColour(225, 225, 225));
		wxSize extent = dc.GetTextExtent(page->message);
		dc.SetClippingRegion(message_bounds);
		dc.DrawText(page->message, message_bounds.x,
			message_bounds.y + (message_bounds.height - extent.GetHeight()) / 2);
		dc.DestroyClippingRegion();
	}
}

void VisualToolPreviewBar::BeginShutdown() {
	if (shutting_down) return;
	if (auto const *page = DisplayPage()) {
		frozen_page = *page;
		has_frozen_page = true;
	}
	shutting_down = true;
	if (has_frozen_page) {
		wxClientDC dc(this);
		RebuildLayout(dc, std::max(Dip(120), GetClientSize().GetWidth()));
		Refresh(false);
	}
}
