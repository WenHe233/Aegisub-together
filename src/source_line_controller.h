// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <libaegisub/signal.h>
#include "ass_file.h"

namespace agi { struct Context; }
class AssDialogue;

extern const char *source_line_key;

class SourceLineController {
	agi::Context *context;
	agi::signal::Connection pre_commit_listener;
	agi::signal::Connection save_listener;

	void OnPreCommit(int type, const AssDialogue *single_line);
	void OnBeforeSave(AssFile& file);

	void LoadFromExtradata();
	void BuildFromStrippedText();
	void ClearExtradata(AssFile& file);
	void SaveToExtradata(AssFile& file);

public:
	explicit SourceLineController(agi::Context *context);
};