// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include <functional>
#include <string>

#include <wx/string.h>

namespace agi { struct Context; }

/// Encode the selected source frames directly inside Aegisub. No command-line
/// encoder, temporary script or external process is involved.
bool EncodeMotionTrimH264(agi::Context *context, wxString const& output,
	int first_frame, int last_frame, int quality,
	std::function<bool(int, int)> progress, std::string& error);
