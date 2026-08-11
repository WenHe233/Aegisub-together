// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

/// @file typesetting_gradient.h
/// @brief Native colour gradients for selected subtitle lines

#pragma once

#include <libaegisub/color.h>

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

struct Stop {
	int position = 0; // percent, 0..100
	agi::Color colour;
};

struct Channel {
	bool enabled = false;
	std::vector<Stop> stops;
};

struct Settings {
	Kind kind = Kind::Linear;
	Output output = Output::Clips;
	int angle = 0;
	int pixels_per_strip = 3;
	double anti_strip_overlap = 0.4;
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

} // namespace typesetting::gradient
