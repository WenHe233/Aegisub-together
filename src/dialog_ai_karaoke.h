// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "ai_client.h"

#include <vector>

class AssDialogue;
namespace agi { struct Context; }

/// Creates a temporary audio clip, runs the selected AI karaoke workflow, and
/// applies the reviewed result as one undoable subtitle operation.
void ShowAIKaraokeDialog(agi::Context *context, ai::KaraokeMode mode,
	std::vector<AssDialogue *> lines);
