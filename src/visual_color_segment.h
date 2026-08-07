// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include "vector2d.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct VideoFrame;
class wxCursor;
class wxBitmap;
class wxImage;
class wxWindow;

struct VisualColorSample {
	unsigned char red = 0;
	unsigned char green = 0;
	unsigned char blue = 0;
};

enum class VisualSelectionMode {
	PipetteAdd,
	PipetteSubtract,
	BrushAdd,
	BrushSubtract
};

/// Cached colour distances and contour extraction for a rectangular part of a
/// video frame. Prepare() reads the pixels once; changing the tolerance only
/// thresholds the compact cached crop and traces its boundaries.
class VisualColorSegmenter final {
	int left = 0;
	int top = 0;
	int width = 0;
	int height = 0;
	int source_width = 0;
	int source_height = 0;
	std::vector<uint16_t> distances;
	std::vector<uint16_t> subtract_distances;
	std::vector<unsigned char> base_selected;
	std::vector<signed char> painted;
	std::vector<std::vector<Vector2D>> stored_contours;
	bool contours_are_pristine = false;

public:
	void Clear();
	bool Empty() const { return distances.empty(); }
	bool PrepareEmpty(VideoFrame const& frame, int x1, int y1, int x2, int y2);
	bool Prepare(VideoFrame const& frame, int x1, int y1, int x2, int y2,
		VisualColorSample sample);
	bool AddSample(VideoFrame const& frame, VisualColorSample sample, bool add = true);
	void SetContours(std::vector<std::vector<Vector2D>> const& contours,
		bool normalize_to_pixels = false);
	void Paint(int frame_x, int frame_y, float radius, bool add);
	std::vector<std::vector<Vector2D>> Extract(double tolerance, bool fill_holes = false,
		int offset_pixels = 0) const;

	static VisualColorSample Sample(VideoFrame const& frame, int x, int y);
};

wxCursor MakeVisualColorPickerCursor();
wxBitmap MakeVisualSelectionModeBitmap(VisualSelectionMode mode, int size = 20);
wxBitmap MakeVisualVectorClipBrushBitmap(bool add, int size = 20, bool dropdown = true);
wxBitmap MakeVisualAISelectionBitmap(int size = 20);

/// Return whether a screen-space brush stroke can change the current selection.
bool WouldVectorBrushStrokeChange(std::vector<std::vector<Vector2D>> const& contours,
	std::vector<Vector2D> const& stroke, float radius, bool add);

/// Apply one circular stamp using the same geometry path as VCLIP_BRUSH.
void ApplyVectorBrushStamp(std::vector<std::vector<Vector2D>>& contours,
	Vector2D centre, float radius, bool add);

/// Apply a screen-space circular brush. Contours which do not touch the brush
/// are returned byte-for-byte unchanged.
std::vector<std::vector<Vector2D>> ApplyVectorBrushStroke(
	std::vector<std::vector<Vector2D>> contours, std::vector<Vector2D> const& stroke,
	float radius, bool add);

/// Ask the configured image model for a pixel-aligned black/white selection
/// matte and return every contour in crop-local pixel coordinates.
std::vector<std::vector<Vector2D>> GenerateVisualAISelection(
	wxWindow *parent, wxImage const& crop, std::string const& subject_prompt);
