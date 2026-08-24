// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include "visual_tool_preview.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace agi { struct Context; }
class AssDialogue;
class AssFile;

namespace typesetting::glitch {

enum class Mode {
	Difference,
	SourceAtop,
	DestinationOut,
	Lighter,
	Multiply,
	Screen,
	Overlay,
	Darken,
	Lighten,
	ColorDodge,
	ColorBurn,
	HardLight,
	SoftLight,
	Exclusion,
	Hue,
	Color,
	Luminosity
};

struct Values {
	double amount = 100.0;
	double offset = 50.0;
	double opacity = 1.0;
	int height = 10;
	double width = 100.0;
	double angle = 90.0;
};

enum class AnimationTiming {
	Range,
	Frame
};

struct Animation {
	bool enabled = true;
	AnimationTiming timing = AnimationTiming::Range;
	int start_time = 0;
	int end_time = 1000;
	int frame = 0;
	bool use_default_mode = true;
	Mode mode = Mode::SourceAtop;
	bool show_base = true;
	Values from;
	Values to{100.0, 50.0, 0.7, 10, 100.0, 90.0};
};

struct Settings {
	Mode mode = Mode::SourceAtop;
	Values base;
	bool show_base = true;
	uint32_t seed = 0x5a17c9e3U;
	std::vector<Animation> animations;
};

std::vector<std::string> ModeNames();
Settings LoadSettingsForSelection(agi::Context *c);
bool SettingsFromClipboard(std::string clipboard, Settings& settings);

std::string ClipboardMetadata(AssFile const& file, AssDialogue const& line);
bool RestoreClipboardMetadata(AssFile& file, AssDialogue& line);
void ClearGroupMetadata(AssFile& file, AssDialogue& line);

class PreviewSession final : public NonDestructivePreviewSession {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	explicit PreviewSession(agi::Context *c);
	~PreviewSession() override;

	void Update(Settings const& settings);
	void Clear() override;
};

size_t Apply(agi::Context *c, Settings const& settings);

bool IsEffect(AssDialogue const *line);
bool IsSource(AssFile const& file, AssDialogue const *line);
std::string Label(AssFile const& file, AssDialogue const& line);
std::string Description(AssFile const& file, AssDialogue const& line);

} // namespace typesetting::glitch
