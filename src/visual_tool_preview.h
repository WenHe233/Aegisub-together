// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include "vector2d.h"

#include <libaegisub/signal.h>

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>
#include <wx/bitmap.h>
#include <wx/colour.h>
#include <wx/panel.h>

class OpenGLText;
class OpenGLWrapper;
class VisualToolPreviewBar;
class wxDC;
class wxMouseEvent;
class wxPaintEvent;
class wxSizeEvent;

/// Common lifetime contract for previews which are hosted outside the video tool itself.
/// Modal editors such as Gradient use this to guarantee that closing, cancelling or changing
/// context hands the video its original subtitle lines again.
class NonDestructivePreviewSession {
public:
	virtual ~NonDestructivePreviewSession() = default;
	virtual void Clear() = 0;
};

/// Shared model and renderer for a visual tool's non-destructive preview controls.
///
/// Tools own the preview behavior, while this class owns the presentation contract:
/// controls, helper text, layout, hit testing and the common visual style. Pages can be
/// pushed for a nested preview stage (AI refinement, for example) and popped without the
/// parent tool having to rebuild its own bar implementation.
class VisualToolPreviewInterface final {
public:
	enum class ControlKind {
		Button,
		Toggle,
		Slider,
		Undo,
		Redo,
		Spacer
	};

	enum class ControlStyle {
		Neutral,
		Accept,
		Cancel,
		Accent,
		Warning
	};

	enum class ControlIcon {
		None,
		Font,
		AlignLeft,
		AlignCentre,
		AlignRight,
		AlignJustified
	};

	struct Control {
		int id = 0;
		ControlKind kind = ControlKind::Button;
		wxString label;
		ControlStyle style = ControlStyle::Neutral;
		bool enabled = true;
		bool selected = false;
		bool dropdown = false;
		double value = 0.0;
		double minimum = 0.0;
		double maximum = 1.0;
		double step = 0.0;
		wxString value_text;
		int width = 0;
		wxColour swatch;
		wxBitmap bitmap;
		ControlIcon icon = ControlIcon::None;
		bool icon_only = false;
	};

	struct Page {
		std::vector<Control> controls;
		wxString message;
	};

	~VisualToolPreviewInterface();

	void SetPage(Page page);
	void PushPage(Page page);
	/// Drop every page, which leaves the host with nothing to show and hides it. A later
	/// SetPage brings it back, so a tool can use this for the modes that want the canvas
	/// to itself.
	void ClearPages();
	bool PopPage();
	void Clear();
	bool HasPage() const { return !pages.empty(); }
	Page const *CurrentPage() const { return pages.empty() ? nullptr : &pages.back(); }

	/// Move the automatic page to a wxWidgets host outside the video canvas. Bespoke tools
	/// can continue using the OpenGL primitives until their controls are expressed as a Page.
	void AttachHost(VisualToolPreviewBar *new_host, std::function<void (int)> on_action,
		std::function<void (int, double, bool)> on_value = {}, bool visible = true);
	void DetachHost();
	bool HasExternalHost() const { return host != nullptr; }

	float Height(OpenGLText& text, Vector2D canvas) const;
	std::pair<Vector2D, Vector2D> BoundsFor(int id, OpenGLText& text,
		Vector2D canvas) const;
	int HitTest(Vector2D point, OpenGLText& text, Vector2D canvas) const;
	void Draw(OpenGLWrapper& gl, OpenGLText& text, Vector2D canvas, int hovered_id) const;

	/// Shared rendering primitives for tools with bespoke layouts such as sliders or icons.
	/// They use the same palette and geometry as page controls while leaving placement to the
	/// tool. This lets complex existing tools join the common interface incrementally.
	void DrawBackground(OpenGLWrapper& gl, Vector2D canvas, float height) const;
	void DrawPanel(OpenGLWrapper& gl, std::pair<Vector2D, Vector2D> bounds,
		bool enabled = true, bool hovered = false, bool selected = false) const;
	void DrawButton(OpenGLWrapper& gl, OpenGLText& text,
		std::pair<Vector2D, Vector2D> bounds, wxString const& label,
		ControlStyle style, bool enabled, bool hovered, bool selected = false,
		bool dropdown = false) const;
	void DrawHistory(OpenGLWrapper& gl, std::pair<Vector2D, Vector2D> bounds,
		bool redo, bool enabled, bool hovered) const;
	void DrawToggle(OpenGLWrapper& gl, OpenGLText& text,
		std::pair<Vector2D, Vector2D> bounds, wxString const& label,
		bool checked, bool enabled, bool hovered) const;
	void DrawMessage(OpenGLText& text, std::pair<Vector2D, Vector2D> row,
		float left, wxString const& message) const;

private:
	struct LocatedControl {
		Control const *control = nullptr;
		std::pair<Vector2D, Vector2D> bounds;
	};
	struct Layout {
		std::vector<LocatedControl> controls;
		std::pair<Vector2D, Vector2D> message_bounds;
		float height = 54.f;
	};

	std::vector<Page> pages;
	mutable std::map<std::string, float> text_width_cache;
	VisualToolPreviewBar *host = nullptr;
	bool host_visible = true;
	std::function<void (int)> action_handler;
	std::function<void (int, double, bool)> value_handler;

	void HostDestroyed(VisualToolPreviewBar *destroyed_host);
	void Activate(int id) const;
	void ActivateValue(int id, double value, bool final) const;
	void NotifyHost();

	float TextWidth(OpenGLText& text, wxString const& label, bool bold) const;
	Layout BuildLayout(OpenGLText& text, Vector2D canvas) const;
	static bool Contains(std::pair<Vector2D, Vector2D> const& bounds, Vector2D point);
	static void RoundedRectangle(OpenGLWrapper& gl, Vector2D first, Vector2D second,
		float radius, unsigned char red, unsigned char green, unsigned char blue,
		float alpha = 1.f);

	friend class VisualToolPreviewBar;
};

/// Native host for a preview page, placed outside and above VideoBox by its owning window.
/// Its geometry and platform colours follow the main toolbar, while a slightly darker
/// background and separator lines distinguish temporary preview actions.
class VisualToolPreviewBar final : public wxPanel {
	struct LocatedControl {
		VisualToolPreviewInterface::Control const *control = nullptr;
		wxRect bounds;
		wxRect track;
	};

	VisualToolPreviewInterface *source = nullptr;
	std::vector<VisualToolPreviewInterface *> sources;
	std::vector<LocatedControl> controls;
	wxRect message_bounds;
	int hovered_id = 0;
	int dragging_id = 0;
	int content_height = 0;
	bool layout_pending = false;
	bool shutting_down = false;
	bool has_frozen_page = false;
	VisualToolPreviewInterface::Page frozen_page;
	agi::signal::Connection icon_size_connection;

	int ToolbarIconSize() const;
	int Dip(int value) const;
	VisualToolPreviewInterface::Page const *DisplayPage() const;
	void RebuildLayout(wxDC& dc, int width);
	void RequestParentLayout();
	int HitTest(wxPoint point) const;
	void UpdateHeight(int height);
	void OnPaint(wxPaintEvent& event);
	void OnMouseMove(wxMouseEvent& event);
	void OnMouseLeave(wxMouseEvent& event);
	void OnLeftDown(wxMouseEvent& event);
	void OnLeftUp(wxMouseEvent& event);
	void OnSize(wxSizeEvent& event);
	void UpdateSlider(int id, int x, bool final = false);

public:
	explicit VisualToolPreviewBar(wxWindow *parent);
	~VisualToolPreviewBar() override;

	void Attach(VisualToolPreviewInterface *new_source);
	void Detach(VisualToolPreviewInterface *old_source);
	void RefreshFromSource();
	/// Keep the last complete preview visible while the containing application window is
	/// being destroyed. Detaching tools must not collapse the layout for one final frame.
	void BeginShutdown();
};
