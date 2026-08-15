// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include "visual_tool.h"

#include <memory>
#include <string>

class OpenGLText;

enum class AutoMotionAction {
	None,
	Accept,
	Cancel,
	TrackX,
	TrackY,
	Scale,
	Rotate,
	Perspective
};

class VisualToolAutoMotion final : public VisualToolBase {
	std::unique_ptr<OpenGLText> gl_text;
	Vector2D region_start;
	Vector2D region_end;
	bool selecting = false;
	bool has_region = false;
	bool busy = false;
	bool leaving = false;
	bool track_x = true;
	bool track_y = true;
	bool track_scale = false;
	bool track_rotate = false;
	bool track_perspective = false;
	AutoMotionAction hovered_action = AutoMotionAction::None;
	agi::signal::Connection selection_connection;
	std::string return_tool;

	void ExitTool();
	void RunTracking();
	Vector2D ClampToScript(Vector2D point) const;
	wxString LabelFor(AutoMotionAction action) const;
	float MeasuredTextWidth(wxString const& label, bool bold) const;
	std::pair<Vector2D, Vector2D> ActionBounds(AutoMotionAction action) const;
	AutoMotionAction ActionAt(Vector2D point) const;
	bool HasOutputComponent() const;
	bool ActionEnabled(AutoMotionAction action) const;
	bool ActionChecked(AutoMotionAction action) const;
	void Perform(AutoMotionAction action);
	void DrawTopBar();
	bool HandleKey(int key);
	void OnCharHook(wxKeyEvent& event);

	void OnLineChanged() override;
	void Draw() override;

public:
	VisualToolAutoMotion(VideoDisplay *parent, agi::Context *context,
		std::string return_tool);
	~VisualToolAutoMotion();

	void OnMouseEvent(wxMouseEvent& event) override;
	bool OnMouseWheel(wxMouseEvent& event) override;
	bool OnKeyEvent(wxKeyEvent& event) override;
};
