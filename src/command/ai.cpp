// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#include "command.h"

#include "../ai_client.h"
#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../compat.h"
#include "../dialog_ai_connection.h"
#include "../dialog_ai_proofread.h"
#include "../dialog_ai_translate.h"
#include "../format.h"
#include "../include/aegisub/context.h"
#include "../options.h"
#include "../project.h"
#include "../selection_controller.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/character_count.h>
#include <libaegisub/option.h>
#include <libaegisub/util.h>

#include <algorithm>
#include <climits>
#include <set>
#include <utility>
#include <vector>

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>

namespace {

constexpr int max_scene_duration_ms = 120000;
constexpr int audio_padding_ms = 200;

bool is_default_dialogue_style(std::string const& style) {
	return style == "Default" || style.compare(0, 10, "Default - ") == 0;
}

struct TemporaryFile final {
	wxString path;
	~TemporaryFile() {
		if (!path.empty() && wxFileExists(path))
			wxRemoveFile(path);
	}
};

struct ai_configure final : public cmd::Command {
	CMD_NAME("ai/configure")
	STR_MENU("Configure AI connection...")
	STR_DISP("Configure AI connection")
	STR_HELP("Configure the per-user OpenAI API connection")

	void operator()(agi::Context *c) override {
		ShowAIConnectionDialog(c->parent, false);
	}
};

struct ai_review final : public cmd::Command {
	CMD_NAME("ai/review")
	STR_MENU("Review selected lines with AI...")
	STR_DISP("Review selected lines with AI")
	STR_HELP("Check up to two minutes of Hungarian subtitles against Japanese audio and English source lines")
	CMD_TYPE(cmd::COMMAND_VALIDATE)

	bool Validate(agi::Context const *c) override {
		return c->project->AudioProvider() && !c->selectionController->GetSelectedSet().empty();
	}

	void operator()(agi::Context *c) override {
		if (ai::GetApiKey().empty() && !ShowAIConnectionDialog(c->parent, true))
			return;

		auto lines = c->selectionController->GetSortedSelection();
		if (lines.empty() || !c->project->AudioProvider()) return;

		int start = INT_MAX;
		int end = 0;
		for (auto line : lines) {
			start = std::min(start, static_cast<int>(line->Start));
			end = std::max(end, static_cast<int>(line->End));
			if (agi::util::clean_ass_text(line->SourceLineText.get()).empty()) {
				wxMessageBox(agi::format(_("Selected line %d has no English source text."), line->Row + 1),
					_("AI subtitle review"), wxOK | wxICON_WARNING, c->parent);
				return;
			}
		}

		if (end - start > max_scene_duration_ms) {
			auto duration = end - start;
			wxMessageBox(agi::format(
				_("The selected scene is %d:%02d long. The maximum for one AI conversation is 2:00."),
				duration / 60000, (duration / 1000) % 60),
				_("AI subtitle review"), wxOK | wxICON_WARNING, c->parent);
			return;
		}

		int clip_start = std::max(0, start - audio_padding_ms);
		int clip_end = end + audio_padding_ms;
		TemporaryFile temporary;
		auto base = wxFileName::CreateTempFileName("aegisub-ai-");
		if (base.empty()) {
			wxMessageBox(_("A temporary audio file could not be created."),
				_("AI subtitle review"), wxOK | wxICON_ERROR, c->parent);
			return;
		}
		temporary.path = base + ".wav";
		if (!wxRenameFile(base, temporary.path, true)) {
			wxRemoveFile(base);
			wxMessageBox(_("The temporary audio file could not be prepared."),
				_("AI subtitle review"), wxOK | wxICON_ERROR, c->parent);
			return;
		}

		try {
			agi::SaveAudioClip(*c->project->AudioProvider(),
				agi::fs::path(from_wx(temporary.path)), clip_start, clip_end);
		}
		catch (std::exception const& error) {
			wxMessageBox(to_wx(error.what()), _("The audio clip could not be created"),
				wxOK | wxICON_ERROR, c->parent);
			return;
		}

		std::vector<ai::SubtitleLine> input;
		input.reserve(lines.size());
		for (auto line : lines) {
			input.push_back({
				line->Id,
				static_cast<int>(line->Start) - clip_start,
				static_cast<int>(line->End) - clip_start,
				agi::util::clean_ass_text(line->SourceLineText.get()),
				agi::util::clean_ass_text(line->GetStrippedText()),
				line->Actor.get(),
				line->Style.get()
			});
		}

		// ShowModal inside this function locks the rest of Aegisub for the whole
		// conversation. Therefore the captured line pointers and selection cannot
		// drift before the user explicitly applies the final suggestions.
		ShowAIReviewDialog(c, std::move(lines), std::move(input),
			agi::fs::path(from_wx(temporary.path)));
	}
};

struct ai_proofread final : public cmd::Command {
	CMD_NAME("ai/proofread")
	STR_MENU("AI post-check...")
	STR_DISP("AI post-check")
	STR_HELP("Check Hungarian spelling, wording and consistency, then approve corrections one by one")
	CMD_TYPE(cmd::COMMAND_VALIDATE)

	bool Validate(agi::Context const *c) override {
		return c->ass && !c->ass->Events.empty();
	}

	void operator()(agi::Context *c) override {
		if (ai::GetApiKey().empty() && !ShowAIConnectionDialog(c->parent, true))
			return;

		auto selected = c->selectionController->GetSortedSelection();
		bool use_selection = selected.size() >= 100;
		selected.erase(std::remove_if(selected.begin(), selected.end(),
			[](AssDialogue *line) { return line->Comment; }), selected.end());

		std::vector<AssDialogue *> default_lines;
		for (auto& line : c->ass->Events) {
			if (!line.Comment && is_default_dialogue_style(line.Style.get()))
				default_lines.push_back(&line);
		}

		auto target_lines = use_selection ? selected : default_lines;
		if (target_lines.empty()) {
			wxMessageBox(_("The chosen scope contains no dialogue lines to check."),
				_("AI post-check"), wxOK | wxICON_INFORMATION, c->parent);
			return;
		}

		std::set<int> target_ids;
		for (auto line : target_lines) target_ids.insert(line->Id);
		std::vector<ai::SubtitleLine> context_lines;
		context_lines.reserve(c->ass->Events.size());
		for (auto& line : c->ass->Events) {
			if (line.Comment) continue;
			ai::SubtitleLine input;
			input.id = line.Id;
			input.start_ms = static_cast<int>(line.Start);
			input.end_ms = static_cast<int>(line.End);
			input.source_text = agi::util::clean_ass_text(line.SourceLineText.get());
			input.current_text = agi::util::clean_ass_text(line.GetStrippedText());
			input.actor = line.Actor.get();
			input.style = line.Style.get();
			input.ass_text = line.Text.get();
			input.target = target_ids.count(line.Id) != 0;
			context_lines.push_back(std::move(input));
		}

		// The modal dialog keeps line pointers stable and blocks all other Aegisub
		// editing while analysis and the complete approve/skip walk-through run.
		ShowAIProofreadDialog(c, std::move(target_lines), std::move(context_lines));
	}
};

} // namespace

namespace cmd {
void init_ai() {
	reg(std::make_unique<ai_configure>());
	reg(std::make_unique<ai_review>());
	reg(std::make_unique<ai_proofread>());
}
}
