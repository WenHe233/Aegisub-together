// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

/// @file typesetting_gradient.h
/// @brief Native colour gradients for selected subtitle lines

#pragma once

#include <libaegisub/color.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace agi { struct Context; }
class AssDialogue;
class AssFile;

namespace typesetting::gradient {

enum class Kind {
	Linear,
	Radial
};

enum class Output {
	Clips,
	Characters,
	Shapes
};

enum class MotionMode {
	Once,
	Loop,
	PingPong,
	FitLine
};

enum class MotionOutside {
	Clamp,
	Repeat,
	BaseColor
};

struct Motion {
	bool enabled = false;
	MotionMode mode = MotionMode::Once;
	MotionOutside outside = MotionOutside::Clamp;
	bool end_at_line = true;
	int start_time = 0;
	int end_time = 1000;
	int cycle_time = 1000;
	double accel = 1.0;
	double start_position = -50.0;
	double end_position = 150.0;
	double start_width = 100.0;
	double middle_width = 100.0;
	double end_width = 100.0;
};

struct Stop {
	int position = 0; // percent, 0..100
	agi::Color colour;
};

struct Channel {
	bool enabled = false;
	std::vector<Stop> stops;
	Motion motion;
};

struct GeometrySnapshot {
	bool valid = false;
	int script_w = 1;
	int script_h = 1;
	double centre_x = 0;
	double centre_y = 0;
	std::array<double, 8> corners{};
};

struct Settings {
	Kind kind = Kind::Linear;
	Output output = Output::Clips;
	int angle = 0;
	int pixels_per_strip = 3;
	double anti_strip_overlap = 0.4;
	bool shared_motion = true;
	Motion motion;
	GeometrySnapshot geometry;
	Channel primary;
	Channel outline;
	Channel shadow;
};

/// Prefer the saved data of a selected gradient group. Ordinary subtitle lines start
/// with fixed defaults and two stops using their effective fill, outline and shadow colours.
Settings LoadSettingsForSelection(agi::Context *c);

/// Non-destructive video preview for one open gradient dialog. The selection geometry is
/// collected once so dragging a stop or the angle dial remains responsive.
class PreviewSession {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	explicit PreviewSession(agi::Context *c);
	~PreviewSession();

	void Update(Settings const& settings);
	void Clear();
};

/// Apply the settings to the current selection and create one undo step.
/// Returns the number of resulting selected dialogue lines, or zero when none of
/// the selected lines contained a character which could be changed.
size_t Apply(agi::Context *c, Settings const& settings);

/// Colour at a normalized position. Exposed for the gradient preview control.
agi::Color Sample(std::vector<Stop> const& stops, double position);

/// Serialize the editable gradient metadata into ASS comment markers for the clipboard.
/// The generated rows themselves do not carry file-local extradata identifiers there.
std::string ClipboardMetadata(AssFile const& file, AssDialogue const& line);
/// Remove clipboard markers from a pasted row and recreate its gradient extradata.
bool RestoreClipboardMetadata(AssFile& file, AssDialogue& line);

/// Move the settings and original-source metadata from a gradient group's deleted
/// header row to its first surviving generated row.
bool TransferGroupMetadata(AssFile& file, AssDialogue const& from, AssDialogue& to);

/// Serialized original subtitle row used to edit a generated gradient as text.
std::string GroupSourceEntry(AssFile const& file, AssDialogue const& line);
/// Replace one generated gradient group from an edited original source row. Geometry,
/// motion and all output rows are recalculated and committed as one change.
bool RegenerateGroupText(agi::Context *c, AssDialogue const& anchor,
	AssDialogue const& edited_source);

/// Compact description for a collapsed gradient group in the subtitle grid, listing
/// the colour/alpha channels which actually vary and whether motion is enabled.
std::string GroupDescription(AssFile const& file, AssDialogue const& line);

/// Rebuild time-dependent color transforms when a generated gradient group's timing
/// changes. Called before the timing edit is committed so it remains one undo step.
/// Returns true when generated dialogue text or group timing was updated.
bool RegenerateMotionForTiming(agi::Context *c, int type,
	AssDialogue const *changed_line);

} // namespace typesetting::gradient
