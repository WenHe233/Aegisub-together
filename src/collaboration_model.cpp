// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include "collaboration_model.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"

#include <iterator>
#include <memory>
#include <stdexcept>
#include <unordered_set>

namespace agi { namespace collab { namespace ass {

std::string GetMetadataValue(AssFile const& file, std::vector<std::uint32_t> const& ids, std::string const& key) {
	std::string value;
	for (auto const& entry : file.GetExtradata(ids))
		if (entry.key == key) value = entry.value;
	return value;
}

SanitizeResult SanitizeFileMetadata(AssFile& file, IdAllocator& allocator, SanitizeContext const& context) {
	std::vector<MetadataLine> metadata;
	metadata.reserve(std::distance(file.Events.begin(), file.Events.end()));
	for (auto const& line : file.Events) {
		metadata.push_back({
			GetMetadataValue(file, line.ExtradataIds.get(), IdExtradataKey),
			GetMetadataValue(file, line.ExtradataIds.get(), PositionExtradataKey)
		});
	}
	auto result = SanitizeMetadata(metadata, allocator, context);
	std::size_t index = 0;
	for (auto& line : file.Events) {
		if (GetMetadataValue(file, line.ExtradataIds.get(), IdExtradataKey) != metadata[index].id)
			file.SetExtradataValue(line, IdExtradataKey, metadata[index].id);
		if (GetMetadataValue(file, line.ExtradataIds.get(), PositionExtradataKey) != metadata[index].position)
			file.SetExtradataValue(line, PositionExtradataKey, metadata[index].position);
		++index;
	}
	return result;
}

Snapshot CaptureSnapshot(AssFile const& file, std::unordered_map<std::string, std::int64_t> const& line_versions,
	std::int64_t styles_version, std::int64_t script_info_version) {
	Snapshot snapshot;
	snapshot.styles_version = styles_version;
	snapshot.script_info_version = script_info_version;
	for (auto const& source : file.Events) {
		Line line;
		line.id = GetMetadataValue(file, source.ExtradataIds.get(), IdExtradataKey);
		line.position = GetMetadataValue(file, source.ExtradataIds.get(), PositionExtradataKey);
		if (!IsValidLineId(line.id) || !IsValidPosition(line.position))
			throw std::runtime_error("cannot capture collaboration snapshot before metadata sanitization");
		auto version = line_versions.find(line.id);
		line.version = version == line_versions.end() ? 1 : version->second;
		if (line.version < 1) throw std::runtime_error("collaboration line version must be positive");
		line.fields.comment = source.Comment;
		line.fields.layer = source.Layer;
		line.fields.start_ms = static_cast<int>(source.Start);
		line.fields.end_ms = static_cast<int>(source.End);
		line.fields.style = source.Style.get();
		line.fields.actor = source.Actor.get();
		line.fields.effect = source.Effect.get();
		line.fields.margins = source.Margin;
		line.fields.text = source.Text.get();
		if (!IsValidLineFields(line.fields)) throw std::runtime_error("dialogue fields exceed collaboration protocol limits");
		snapshot.lines.push_back(std::move(line));
	}
	for (auto const& style : file.Styles) snapshot.styles.push_back(style.GetEntryData());
	for (auto const& info : file.Info) snapshot.script_info.push_back({info.Key(), info.Value()});
	return snapshot;
}

void LoadSnapshot(AssFile& file, Snapshot const& snapshot) {
	if (snapshot.styles.empty() || snapshot.styles_version < 1 || snapshot.script_info_version < 1)
		throw std::invalid_argument("invalid collaboration snapshot sections");
	std::unordered_set<std::string> ids;
	std::string previous_position;
	for (auto const& line : snapshot.lines) {
		if (!IsValidLineId(line.id) || !ids.insert(line.id).second || !IsValidPosition(line.position) ||
			(!previous_position.empty() && line.position <= previous_position) || line.version < 1 || !IsValidLineFields(line.fields))
			throw std::invalid_argument("invalid collaboration snapshot line");
		previous_position = line.position;
	}
	std::vector<std::unique_ptr<AssStyle>> parsed_styles;
	parsed_styles.reserve(snapshot.styles.size());
	for (auto const& style : snapshot.styles) parsed_styles.emplace_back(new AssStyle(style));
	std::vector<std::unique_ptr<AssDialogue>> parsed_lines;
	parsed_lines.reserve(snapshot.lines.size());
	for (auto const& source : snapshot.lines) {
		auto line = std::make_unique<AssDialogue>();
		line->Comment = source.fields.comment;
		line->Layer = source.fields.layer;
		line->Start = static_cast<int>(source.fields.start_ms);
		line->End = static_cast<int>(source.fields.end_ms);
		line->Style = source.fields.style;
		line->Actor = source.fields.actor;
		line->Effect = source.fields.effect;
		line->Margin = source.fields.margins;
		line->Text = source.fields.text;
		parsed_lines.push_back(std::move(line));
	}
	file.Info.clear();
	for (auto const& info : snapshot.script_info) file.Info.emplace_back(info.key, info.value);
	file.Styles.clear_and_dispose([](AssStyle* style) { delete style; });
	for (auto& style : parsed_styles) file.Styles.push_back(*style.release());
	file.Events.clear_and_dispose([](AssDialogue* line) { delete line; });
	for (std::size_t index = 0; index < snapshot.lines.size(); ++index) {
		auto& line = parsed_lines[index];
		file.SetExtradataValue(*line, IdExtradataKey, snapshot.lines[index].id);
		file.SetExtradataValue(*line, PositionExtradataKey, snapshot.lines[index].position);
		file.Events.push_back(*line.release());
	}
}

} } }
