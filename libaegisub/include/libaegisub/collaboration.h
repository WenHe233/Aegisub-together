// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agi { namespace collab {

constexpr char IdExtradataKey[] = "_aegi_collab_id";
constexpr char PositionExtradataKey[] = "_aegi_collab_pos";
constexpr std::size_t PositionWidth = 16;

struct LineFields {
	bool comment = false;
	int layer = 0;
	std::int64_t start_ms = 0;
	std::int64_t end_ms = 5000;
	std::string style = "Default";
	std::string actor;
	std::string effect;
	std::array<int, 3> margins{{0, 0, 0}};
	std::string text;

	bool operator==(LineFields const& other) const;
	bool operator!=(LineFields const& other) const { return !(*this == other); }
};

struct FieldPatch {
	std::optional<bool> comment;
	std::optional<int> layer;
	std::optional<std::int64_t> start_ms;
	std::optional<std::int64_t> end_ms;
	std::optional<std::string> style;
	std::optional<std::string> actor;
	std::optional<std::string> effect;
	std::optional<std::array<int, 3>> margins;
	std::optional<std::string> text;

	bool empty() const;
};

struct Line {
	std::string id;
	std::string position;
	std::int64_t version = 1;
	LineFields fields;
};

struct ScriptInfoEntry {
	std::string key;
	std::string value;
	bool operator==(ScriptInfoEntry const& other) const { return key == other.key && value == other.value; }
};

struct Snapshot {
	std::vector<Line> lines;
	std::vector<std::string> styles;
	std::int64_t styles_version = 1;
	std::vector<ScriptInfoEntry> script_info;
	std::int64_t script_info_version = 1;
};

struct DocumentState {
	Snapshot snapshot;
	std::unordered_map<std::string, Line> tombstones;
};

enum class OperationKind { Modify, Insert, Delete, Move, Restore, ReplaceStyles, ReplaceScriptInfo };

struct Operation {
	OperationKind kind = OperationKind::Modify;
	std::string line_id;
	std::optional<std::string> left_id;
	std::optional<std::string> right_id;
	std::int64_t base_version = 0;
	FieldPatch fields;
	LineFields data;
	std::vector<std::string> styles;
	std::vector<ScriptInfoEntry> script_info;
};

class IdAllocator {
	std::string prefix;
	std::uint64_t next_counter;
public:
	IdAllocator(std::string prefix, std::uint64_t next_counter);
	std::string Mint();
	std::string const& Prefix() const { return prefix; }
	std::uint64_t NextCounter() const { return next_counter; }
};

std::string EncodeClientPrefix(std::uint64_t value);
bool IsValidLineId(std::string const& value);
bool IsLocalLineId(std::string const& value, std::string const& prefix);
bool IsValidPosition(std::string const& value);
bool IsValidLineFields(LineFields const& value);
void ReindexPositions(std::vector<Line>& lines);

struct MetadataLine {
	std::string id;
	std::string position;
};

struct SanitizeContext {
	std::unordered_set<std::string> known_live_ids;
	std::unordered_set<std::string> tombstoned_ids;
	bool accept_unknown_ids = false;
};

struct SanitizeResult {
	std::vector<std::size_t> reminted_lines;
	std::vector<std::string> resurrected_ids;
	bool positions_reindexed = false;
	bool changed() const { return !reminted_lines.empty() || !resurrected_ids.empty() || positions_reindexed; }
};

SanitizeResult SanitizeMetadata(std::vector<MetadataLine>& lines, IdAllocator& allocator, SanitizeContext const& context);

std::vector<Operation> DiffSnapshots(DocumentState const& confirmed, Snapshot const& projected);
/// Build a selective transition for collaborative undo/redo. Only entities which differ
/// between expected and desired are changed; unrelated current remote changes are retained.
std::vector<Operation> BuildSelectiveTransition(DocumentState const& current, Snapshot const& expected,
	Snapshot const& desired, std::string* error = nullptr);
bool ApplyOperation(DocumentState& state, Operation const& operation, std::string* error = nullptr);
bool ApplyOperations(DocumentState& state, std::vector<Operation> const& operations, std::string* error = nullptr);

} }
