// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "ai_client.h"

#include <vector>

#include <libaegisub/fs.h>

class AssDialogue;
namespace agi { struct Context; }

/// Shows a modeless, read-only AI subtitle review conversation. Its input is
/// snapshotted so the subtitle editor remains available while the review runs.
void ShowAIReviewDialog(agi::Context *context,
	std::vector<AssDialogue *> lines,
	std::vector<ai::SubtitleLine> input,
	agi::fs::path audio_file);
