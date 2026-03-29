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

#include "source_line_controller.h"

#include "ass_dialogue.h"
#include "include/aegisub/context.h"
#include "subs_controller.h"

#include <algorithm>

const char *source_line_key = "_aegi_source_line";

SourceLineController::SourceLineController(agi::Context *context)
: context(context)
, pre_commit_listener(context->ass->AddPreCommitListener(&SourceLineController::OnPreCommit, this))
, save_listener(context->subsController->AddBeforeSaveListener(&SourceLineController::OnBeforeSave, this))
{
}

void SourceLineController::OnPreCommit(int type, const AssDialogue * /*single_line*/) {
	if (type == AssFile::COMMIT_NEW) {
		LoadFromExtradata();

		auto ext = context->subsController->Filename().extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });

		if (ext == ".mkv")
			BuildFromStrippedText();
	}
}

void SourceLineController::OnBeforeSave(AssFile& file) {
	ClearExtradata(file);
	SaveToExtradata(file);
}

void SourceLineController::LoadFromExtradata() {
	for (auto line = context->ass->Events.begin(); line != context->ass->Events.end(); ++line) {
		for (auto const& extra : context->ass->GetExtradata(line->ExtradataIds)) {
			if (extra.key == source_line_key) {
				line->SourceLineText = extra.value;
				break;
			}
		}
	}
}

void SourceLineController::BuildFromStrippedText() {
	for (auto line = context->ass->Events.begin(); line != context->ass->Events.end(); ++line)
		line->SourceLineText = line->GetStrippedText();
}

void SourceLineController::ClearExtradata(AssFile& file) {
	for (AssDialogue &line : file.Events) {
		file.DeleteExtradataValue(line, source_line_key);
	}
}

void SourceLineController::SaveToExtradata(AssFile& file) {
	for (AssDialogue &line : file.Events) {
		std::string const& value = line.SourceLineText.get();
		if (!value.empty())
			file.SetExtradataValue(line, source_line_key, value);
	}
}