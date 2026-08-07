// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include "visual_feature.h"
#include "visual_tool.h"

#include <memory>
#include <utility>
#include <vector>

#include <wx/colour.h>

class wxCommandEvent;
class wxToolBar;
class OpenGLText;

enum class VisualShapeKind {
	Line,
	Rectangle,
	Ellipse,
	Triangle,
	Diamond,
	Hexagon,
	Heart,
	WideHeart,
	Star5,
	Star6,
	Star8,
	Arrow,
	Freehand
};

class VisualToolShape final : public VisualTool<VisualDraggableFeature> {
	enum class Action {
		None,
		Undo,
		Redo,
		Blur,
		Colour,
		Accept,
		Clear
	};

	struct PendingShape {
		std::vector<Vector2D> geometry;
		VisualShapeKind kind = VisualShapeKind::Rectangle;
		bool closed = false;
		bool filled = false;
		double stroke_size = 1.0;
		int corner_radius = 0;
	};

	wxToolBar *toolBar = nullptr;
	std::unique_ptr<OpenGLText> gl_text;
	VisualShapeKind shape = VisualShapeKind::Rectangle;
	VisualShapeKind last_geometric_shape = VisualShapeKind::Rectangle;
	std::vector<PendingShape> pending_shapes;
	std::vector<std::vector<PendingShape>> undo_history;
	std::vector<std::vector<PendingShape>> redo_history;
	std::vector<Vector2D> freehand_points;
	Vector2D shape_start;
	Vector2D shape_end;
	bool drawing = false;
	bool filled = true;
	double stroke_size = 4.0;
	int corner_radius = 0;
	double blur = .5;
	wxColour selected_colour;
	bool has_selected_colour = false;
	Action hovered_action = Action::None;

	void OnToolbar(wxCommandEvent& event);
	void ShowShapeMenu(Vector2D position);
	void ShowSizeMenu(Vector2D position);
	void ShowRadiusMenu(Vector2D position);
	void ShowBlurMenu(Vector2D position);
	void ShowColourPicker();
	void UpdateToolbar();
	std::vector<Vector2D> Geometry() const;
	bool IsClosedShape() const;
	bool CanCreate() const;
	std::pair<Vector2D, Vector2D> ActionBounds(Action action);
	Action ActionAt(Vector2D position);
	void FinishCurrentShape();
	void ResetCurrentShape();
	void PushHistory();
	bool UndoHistory();
	bool RedoHistory();
	void DrawShape(PendingShape const& item);
	void CreateShape();
	void ClearPreview();

	void Draw() override;

public:
	VisualToolShape(VideoDisplay *parent, agi::Context *context);
	~VisualToolShape();

	void OnMouseEvent(wxMouseEvent& event) override;
	bool OnKeyEvent(wxKeyEvent& event) override;
	void SetToolbar(wxToolBar *toolbar) override;
};
