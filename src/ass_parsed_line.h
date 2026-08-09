// Copyright (c) 2026, Muteki Aegisub
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

/// @file ass_parsed_line.h
/// @brief Writing override tags into a dialogue line at a cursor position
///
/// Shared because three things do it: the colour picker and the font picker in
/// command/edit.cpp, and picking a colour off the video. It used to live in that
/// one command file; nothing about it changed in the move.

#pragma once

#include "ass_dialogue.h"

#include <libaegisub/of_type_adaptor.h>
#include <libaegisub/string.h>

#include <boost/range/adaptor/reversed.hpp>
#include <boost/range/adaptor/sliced.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace boost::adaptors;

struct parsed_line {
	AssDialogue *line;
	std::vector<std::unique_ptr<AssDialogueBlock>> blocks;

	parsed_line(AssDialogue *line) : line(line), blocks(line->ParseTags()) { }
	parsed_line(parsed_line&& r) = default;

	const AssOverrideTag *find_tag(int blockn, std::string const& tag_name, std::string const& alt) const {
		for (auto ovr : blocks | sliced(0, blockn + 1) | reversed | agi::of_type<AssDialogueBlockOverride>()) {
			for (auto const& tag : ovr->Tags | reversed) {
				if (tag.Name == tag_name || tag.Name == alt)
					return &tag;
			}
		}
		return nullptr;
	}

	template<typename T>
	T get_value(int blockn, T initial, std::string const& tag_name, std::string const& alt = "") const {
		auto tag = find_tag(blockn, tag_name, alt);
		if (tag)
			return tag->Params[0].template Get<T>(initial);
		return initial;
	}

	int block_at_pos(int pos) const {
		auto const& text = line->Text.get();
		int n = 0;
		int max = text.size() - 1;
		bool in_block = false;

		for (int i = 0; i <= max; ++i) {
			if (text[i] == '{') {
				if (!in_block && i > 0 && pos >= 0)
					++n;
				in_block = true;
			}
			else if (text[i] == '}' && in_block) {
				in_block = false;
				if (pos > 0 && (i + 1 == max || text[i + 1] != '{'))
					n++;
			}
			else if (!in_block) {
				if (--pos == 0)
					return n + (i < max && text[i + 1] == '{');
			}
		}

		return n - in_block;
	}

	int set_tag(std::string const& tag, std::string const& value, int norm_pos, int orig_pos) {
		int blockn = block_at_pos(norm_pos);

		AssDialogueBlockPlain *plain = nullptr;
		AssDialogueBlockOverride *ovr = nullptr;
		while (blockn >= 0 && !plain && !ovr) {
			AssDialogueBlock *block = blocks[blockn].get();
			switch (block->GetType()) {
			case AssBlockType::PLAIN:
				plain = static_cast<AssDialogueBlockPlain *>(block);
				break;
			case AssBlockType::DRAWING:
				--blockn;
				break;
			case AssBlockType::COMMENT:
				--blockn;
				orig_pos = line->Text.get().rfind('{', orig_pos);
				break;
			case AssBlockType::OVERRIDE:
				ovr = static_cast<AssDialogueBlockOverride*>(block);
				break;
			}
		}

		// If we didn't hit a suitable block for inserting the override just put
		// it at the beginning of the line
		if (blockn < 0)
			orig_pos = 0;

		std::string insert(tag + value);
		int shift = insert.size();
		if (plain || blockn < 0) {
			std::string_view text = line->Text.get();
			line->Text = agi::Str(text.substr(0, orig_pos), "{", insert, "}", text.substr(orig_pos));
			shift += 2;
			blocks = line->ParseTags();
		}
		else if (ovr) {
			std::string alt;
			if (tag == "\\c") alt = "\\1c";
			// Remove old of same
			bool found = false;
			for (size_t i = 0; i < ovr->Tags.size(); i++) {
				std::string const& name = ovr->Tags[i].Name;
				if (tag == name || alt == name) {
					shift -= ((std::string)ovr->Tags[i]).size();
					if (found) {
						ovr->Tags.erase(ovr->Tags.begin() + i);
						i--;
					}
					else {
						ovr->Tags[i].Params[0].Set(value);
						found = true;
					}
				}
			}
			if (!found)
				ovr->AddTag(insert);

			line->UpdateText(blocks);
		}
		else
			assert(false);

		return shift;
	}
};

inline int normalize_pos(std::string const& text, int pos) {
	int plain_len = 0;
	bool in_block = false;

	for (int i = 0, max = text.size() - 1; i < pos && i <= max; ++i) {
		if (text[i] == '{')
			in_block = true;
		if (!in_block)
			++plain_len;
		if (text[i] == '}' && in_block)
			in_block = false;
	}

	return plain_len;
}
