// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include "spline_curve.h"
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

struct VisualColorSampleOperation {
	VisualColorSample sample;
	bool add = true;
};

/// A reusable colour extraction recipe. Shared between the vector clip and the
/// mask tools so a template captured in one is offered by the other as well.
struct VisualColorTemplate {
	std::string name;
	std::vector<VisualColorSampleOperation> sample_operations;
	bool ai_base = false;
	double tolerance = 2.0;
	int offset = 0;
	bool auto_fill = false;
	bool smooth_edges = false;
	double smooth_tolerance = 10.0;
	double smooth_angle = 35.0;
	bool edge_snap = false;
	double edge_snap_radius = 6.0;
	VisualSelectionMode selection_mode = VisualSelectionMode::PipetteAdd;
};

std::vector<VisualColorTemplate>& VisualColorTemplates();

/// Split pixel-traced figure-eight contours at repeated vertices. The resulting
/// simple loops have exactly the same non-zero/even-odd fill, but polygon boolean
/// libraries can process them independently.
std::vector<std::vector<Vector2D>> SplitSelfTouchingContours(
	std::vector<std::vector<Vector2D>> contours);

/// Corner radius edge smoothing uses while auto snap is on. Large enough to take
/// the pixel staircase off a snapped outline and follow its curve, small enough
/// not to cut across the outline band the snap just enclosed.
constexpr double snapped_smooth_tolerance = 4.0;

/// Round a traced contour into line and bezier segments. `tolerance` is the
/// corner radius in pixels and `angle_threshold` the interior angle at or below
/// which a corner counts as deliberate and is left sharp.
std::vector<SplineCurve> SmoothClosedContour(std::vector<Vector2D> points,
	double tolerance, double angle_threshold);

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
	/// Empty while the range is the whole crop; otherwise one byte per pixel
	/// marking what the freehand range encloses.
	std::vector<unsigned char> range_mask;
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
	/// Restrict everything this segmenter may select to the inside of a polygon,
	/// given in absolute frame pixels. Used by the freehand range: the cached crop
	/// stays rectangular, but only what the lasso encloses can be picked.
	void SetRangeMask(std::vector<Vector2D> const& polygon);
	void Paint(int frame_x, int frame_y, float radius, bool add);
	/// Paint a continuous frame-space stroke into the manual override mask.
	/// Pixels outside the stroke are never reclassified or retraced here.
	void PaintStroke(Vector2D from, Vector2D to, float radius, bool add);
	std::vector<std::vector<Vector2D>> Extract(double tolerance, bool fill_holes = false,
		int offset_pixels = 0) const;

	static VisualColorSample Sample(VideoFrame const& frame, int x, int y);
};

wxCursor MakeVisualColorPickerCursor();
wxBitmap MakeVisualSelectionModeBitmap(VisualSelectionMode mode, int size = 20);
wxBitmap MakeVisualVectorClipBrushBitmap(bool add, int size = 20, bool dropdown = true);
wxBitmap MakeVisualAISelectionBitmap(int size = 20);
/// Icon for the range shape chooser: a dashed rectangle, or a mouse for freehand.
wxBitmap MakeVisualRangeShapeBitmap(bool freehand, int size = 20, bool dark = false);

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

/// Cut every contour down to an axis-aligned rectangle, holes included. Used to
/// keep a colour selection inside the range the user marked out.
void ClipVectorContoursToRect(std::vector<std::vector<Vector2D>>& contours,
	Vector2D top_left, Vector2D bottom_right);

/// The same for a freehand range: clip to an arbitrary simple polygon.
void ClipVectorContoursToPolygon(std::vector<std::vector<Vector2D>>& contours,
	std::vector<Vector2D> const& boundary);

/// Move every contour onto the picture edge it is tracing, in absolute frame
/// pixel coordinates matching VisualColorSegmenter::Extract. Each point may only
/// travel along its own normal, at most `search_radius` pixels, and neighbouring
/// points are pushed towards the same displacement - so a stretch where the
/// sampled colour bled across an outline is lined up with its surroundings
/// instead of chasing the bleed. Points on the crop border stay put and results
/// never leave the crop, which keeps the selection inside the marked range. A
/// contour the snap cannot improve confidently is returned unchanged.
std::vector<std::vector<Vector2D>> SnapContoursToImageEdges(VideoFrame const& frame,
	std::vector<std::vector<Vector2D>> contours, int crop_left, int crop_top,
	int crop_width, int crop_height, int search_radius);

/// Ask the configured image model for a pixel-aligned black/white selection
/// matte and return every contour in crop-local pixel coordinates.
std::vector<std::vector<Vector2D>> GenerateVisualAISelection(
	wxWindow *parent, wxImage const& crop);
