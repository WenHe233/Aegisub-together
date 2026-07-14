// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <libaegisub/collaboration.h>

#include <algorithm>
#include <charconv>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace agi { namespace collab {
namespace {
char constexpr Base32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
char constexpr Base62[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
using Rank = std::array<unsigned char, PositionWidth>;

Rank rank_maximum() {
	Rank value;
	value.fill(61);
	return value;
}

bool decode_rank(std::string const& key, Rank& value) {
	if (key.size() != PositionWidth) return false;
	for (std::size_t index = 0; index < key.size(); ++index) {
		char character = key[index];
		auto digit = std::find(std::begin(Base62), std::end(Base62) - 1, character);
		if (digit == std::end(Base62) - 1) return false;
		value[index] = static_cast<unsigned char>(digit - std::begin(Base62));
	}
	return true;
}

std::string encode_rank(Rank const& value) {
	std::string output(PositionWidth, '0');
	for (std::size_t index = 0; index < PositionWidth; ++index) output[index] = Base62[value[index]];
	return output;
}

Rank divide_rank(Rank const& value, std::size_t divisor) {
	Rank quotient{};
	std::size_t remainder = 0;
	for (std::size_t index = 0; index < PositionWidth; ++index) {
		remainder = remainder * 62 + value[index];
		quotient[index] = static_cast<unsigned char>(remainder / divisor);
		remainder %= divisor;
	}
	return quotient;
}

Rank add_rank(Rank const& left, Rank const& right) {
	Rank result{};
	unsigned carry = 0;
	for (std::size_t offset = 0; offset < PositionWidth; ++offset) {
		auto index = PositionWidth - offset - 1;
		unsigned sum = left[index] + right[index] + carry;
		result[index] = static_cast<unsigned char>(sum % 62);
		carry = sum / 62;
	}
	if (carry) throw std::overflow_error("collaboration rank addition overflowed");
	return result;
}

Rank increment_rank(Rank value) {
	for (std::size_t offset = 0; offset < PositionWidth; ++offset) {
		auto index = PositionWidth - offset - 1;
		if (++value[index] < 62) return value;
		value[index] = 0;
	}
	return value;
}

Rank midpoint_rank(Rank const& left, Rank const& right) {
	std::array<unsigned char, PositionWidth + 1> sum{};
	unsigned carry = 0;
	for (std::size_t offset = 0; offset < PositionWidth; ++offset) {
		auto index = PositionWidth - offset;
		unsigned value = left[index - 1] + right[index - 1] + carry;
		sum[index] = static_cast<unsigned char>(value % 62);
		carry = value / 62;
	}
	sum[0] = static_cast<unsigned char>(carry);
	Rank result{};
	unsigned remainder = 0;
	for (std::size_t index = 0; index < sum.size(); ++index) {
		unsigned value = remainder * 62 + sum[index];
		if (index) result[index - 1] = static_cast<unsigned char>(value / 2);
		remainder = value % 2;
	}
	return result;
}

std::size_t find_line(std::vector<Line> const& lines, std::string const& id) {
	for (std::size_t index = 0; index < lines.size(); ++index)
		if (lines[index].id == id) return index;
	return lines.size();
}

std::size_t insertion_index(std::vector<Line> const& lines, std::optional<std::string> const& left, std::optional<std::string> const& right) {
	auto left_index = left ? find_line(lines, *left) : lines.size();
	auto right_index = right ? find_line(lines, *right) : lines.size();
	if (left_index < lines.size() && right_index < lines.size() && left_index < right_index) return right_index;
	if (left_index < lines.size()) return left_index + 1;
	if (right_index < lines.size()) return right_index;
	return lines.size();
}

std::string position_for_insert(std::vector<Line>& lines, std::size_t index) {
	Rank left{};
	Rank right = rank_maximum();
	if (index > 0 && !decode_rank(lines[index - 1].position, left)) {
		ReindexPositions(lines);
		return position_for_insert(lines, index);
	}
	if (index < lines.size() && !decode_rank(lines[index].position, right)) {
		ReindexPositions(lines);
		return position_for_insert(lines, index);
	}
	if (right <= left || increment_rank(left) >= right) {
		ReindexPositions(lines);
		return position_for_insert(lines, index);
	}
	return encode_rank(midpoint_rank(left, right));
}

void set_error(std::string* error, std::string message) {
	if (error) *error = std::move(message);
}

FieldPatch difference(LineFields const& from, LineFields const& to) {
	FieldPatch patch;
	if (from.comment != to.comment) patch.comment = to.comment;
	if (from.layer != to.layer) patch.layer = to.layer;
	if (from.start_ms != to.start_ms) patch.start_ms = to.start_ms;
	if (from.end_ms != to.end_ms) patch.end_ms = to.end_ms;
	if (from.style != to.style) patch.style = to.style;
	if (from.actor != to.actor) patch.actor = to.actor;
	if (from.effect != to.effect) patch.effect = to.effect;
	if (from.margins != to.margins) patch.margins = to.margins;
	if (from.text != to.text) patch.text = to.text;
	return patch;
}

void apply_patch(LineFields& target, FieldPatch const& patch) {
	if (patch.comment) target.comment = *patch.comment;
	if (patch.layer) target.layer = *patch.layer;
	if (patch.start_ms) target.start_ms = *patch.start_ms;
	if (patch.end_ms) target.end_ms = *patch.end_ms;
	if (patch.style) target.style = *patch.style;
	if (patch.actor) target.actor = *patch.actor;
	if (patch.effect) target.effect = *patch.effect;
	if (patch.margins) target.margins = *patch.margins;
	if (patch.text) target.text = *patch.text;
}
}

bool LineFields::operator==(LineFields const& other) const {
	return comment == other.comment && layer == other.layer && start_ms == other.start_ms && end_ms == other.end_ms &&
		style == other.style && actor == other.actor && effect == other.effect && margins == other.margins && text == other.text;
}

bool FieldPatch::empty() const {
	return !comment && !layer && !start_ms && !end_ms && !style && !actor && !effect && !margins && !text;
}

std::string EncodeClientPrefix(std::uint64_t value) {
	if (value >= (std::uint64_t{1} << 50)) throw std::out_of_range("client prefix exceeds 50 bits");
	std::string output(10, '0');
	for (std::size_t index = output.size(); index > 0; --index) {
		output[index - 1] = Base32[value & 31];
		value >>= 5;
	}
	return output;
}

bool IsValidLineId(std::string const& value) {
	if (value.size() < 12 || value[10] != '-') return false;
	for (std::size_t index = 0; index < 10; ++index)
		if (std::find(std::begin(Base32), std::end(Base32) - 1, value[index]) == std::end(Base32) - 1) return false;
	std::uint64_t counter = 0;
	auto parsed = std::from_chars(value.data() + 11, value.data() + value.size(), counter);
	return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && counter > 0;
}

bool IsLocalLineId(std::string const& value, std::string const& prefix) {
	return IsValidLineId(value) && value.compare(0, 10, prefix) == 0;
}

IdAllocator::IdAllocator(std::string prefix, std::uint64_t next_counter)
:	prefix(std::move(prefix)), next_counter(next_counter) {
	if (this->prefix.size() != 10 || !IsValidLineId(this->prefix + "-1") || next_counter == 0)
		throw std::invalid_argument("invalid collaboration ID allocator state");
}

std::string IdAllocator::Mint() {
	if (next_counter == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("collaboration ID counter exhausted");
	return prefix + "-" + std::to_string(next_counter++);
}

bool IsValidPosition(std::string const& value) {
	Rank decoded;
	return decode_rank(value, decoded);
}

bool IsValidLineFields(LineFields const& value) {
	if (value.start_ms < 0 || value.end_ms < 0) return false;
	return std::all_of(value.margins.begin(), value.margins.end(), [](int margin) { return margin >= 0 && margin <= 9999; });
}

void ReindexPositions(std::vector<Line>& lines) {
	Rank step = divide_rank(rank_maximum(), lines.size() + 1);
	Rank current{};
	for (auto& line : lines) {
		current = add_rank(current, step);
		line.position = encode_rank(current);
	}
}

SanitizeResult SanitizeMetadata(std::vector<MetadataLine>& lines, IdAllocator& allocator, SanitizeContext const& context) {
	SanitizeResult result;
	std::unordered_set<std::string> seen;
	for (std::size_t index = 0; index < lines.size(); ++index) {
		auto& line = lines[index];
		bool known = context.known_live_ids.count(line.id) != 0;
		bool tombstoned = context.tombstoned_ids.count(line.id) != 0;
		bool unknown = !known && !tombstoned;
		if (!IsValidLineId(line.id) || seen.count(line.id) || (unknown && !context.accept_unknown_ids)) {
			line.id = allocator.Mint();
			result.reminted_lines.push_back(index);
			tombstoned = false;
		}
		if (tombstoned) result.resurrected_ids.push_back(line.id);
		seen.insert(line.id);
	}

	Rank previous{};
	bool have_previous = false;
	bool positions_valid = true;
	for (auto const& line : lines) {
		Rank current;
		if (!decode_rank(line.position, current) || (have_previous && current <= previous)) {
			positions_valid = false;
			break;
		}
		previous = current;
		have_previous = true;
	}
	if (!positions_valid) {
		std::vector<Line> ranked(lines.size());
		ReindexPositions(ranked);
		for (std::size_t index = 0; index < lines.size(); ++index) lines[index].position = ranked[index].position;
		result.positions_reindexed = true;
	}
	return result;
}

bool ApplyOperation(DocumentState& state, Operation const& operation, std::string* error) {
	auto& lines = state.snapshot.lines;
	auto index = find_line(lines, operation.line_id);
	switch (operation.kind) {
	case OperationKind::Modify:
		if (index == lines.size() || lines[index].version != operation.base_version || operation.fields.empty()) {
			set_error(error, "modify version mismatch or empty patch");
			return false;
		}
		{
			LineFields updated = lines[index].fields;
			apply_patch(updated, operation.fields);
			if (!IsValidLineFields(updated)) {
				set_error(error, "modify produced invalid line fields");
				return false;
			}
			lines[index].fields = std::move(updated);
		}
		++lines[index].version;
		return true;
	case OperationKind::Insert: {
		if (!IsValidLineId(operation.line_id) || !IsValidLineFields(operation.data) || index != lines.size() || state.tombstones.count(operation.line_id)) {
			set_error(error, "insert ID is invalid or already used");
			return false;
		}
		auto target = insertion_index(lines, operation.left_id, operation.right_id);
		Line inserted{operation.line_id, position_for_insert(lines, target), 1, operation.data};
		lines.insert(lines.begin() + target, std::move(inserted));
		return true;
	}
	case OperationKind::Delete:
		if (index == lines.size() || lines[index].version != operation.base_version) {
			set_error(error, "delete version mismatch");
			return false;
		}
		state.tombstones[operation.line_id] = lines[index];
		lines.erase(lines.begin() + index);
		return true;
	case OperationKind::Move: {
		if (index == lines.size() || lines[index].version != operation.base_version) {
			set_error(error, "move version mismatch");
			return false;
		}
		Line moved = lines[index];
		lines.erase(lines.begin() + index);
		auto target = insertion_index(lines, operation.left_id, operation.right_id);
		moved.position = position_for_insert(lines, target);
		++moved.version;
		lines.insert(lines.begin() + target, std::move(moved));
		return true;
	}
	case OperationKind::Restore: {
		if (index != lines.size()) {
			set_error(error, "restore line is already live");
			return false;
		}
		auto tombstone = state.tombstones.find(operation.line_id);
		if (tombstone == state.tombstones.end()) {
			set_error(error, "restore tombstone is missing");
			return false;
		}
		Line restored = tombstone->second;
		++restored.version;
		auto target = std::lower_bound(lines.begin(), lines.end(), restored.position, [](Line const& line, std::string const& position) { return line.position < position; });
		if (target != lines.end() && target->position == restored.position) {
			auto target_index = static_cast<std::size_t>(target - lines.begin());
			restored.position = position_for_insert(lines, target_index);
			target = lines.begin() + target_index;
		}
		lines.insert(target, std::move(restored));
		state.tombstones.erase(tombstone);
		return true;
	}
	case OperationKind::ReplaceStyles:
		if (operation.base_version != state.snapshot.styles_version || operation.styles.empty()) {
			set_error(error, "styles version mismatch or empty section");
			return false;
		}
		state.snapshot.styles = operation.styles;
		++state.snapshot.styles_version;
		return true;
	case OperationKind::ReplaceScriptInfo:
		if (operation.base_version != state.snapshot.script_info_version) {
			set_error(error, "script info version mismatch");
			return false;
		}
		state.snapshot.script_info = operation.script_info;
		++state.snapshot.script_info_version;
		return true;
	}
	set_error(error, "unknown operation");
	return false;
}

bool ApplyOperations(DocumentState& state, std::vector<Operation> const& operations, std::string* error) {
	DocumentState working = state;
	for (auto const& operation : operations) {
		if (!ApplyOperation(working, operation, error)) return false;
	}
	state = std::move(working);
	return true;
}

std::vector<Operation> DiffSnapshots(DocumentState const& confirmed, Snapshot const& projected) {
	DocumentState working = confirmed;
	std::vector<Operation> operations;
	std::unordered_set<std::string> target_ids;
	for (auto const& line : projected.lines) {
		if (!IsValidLineId(line.id) || !IsValidLineFields(line.fields)) throw std::invalid_argument("projected snapshot contains an invalid line");
		if (!target_ids.insert(line.id).second) throw std::invalid_argument("projected snapshot contains duplicate line IDs");
	}
	if (projected.styles.empty()) throw std::invalid_argument("projected snapshot contains no styles");
	for (auto const& line : confirmed.snapshot.lines) {
		if (!target_ids.count(line.id)) {
			Operation operation;
			operation.kind = OperationKind::Delete;
			operation.line_id = line.id;
			operation.base_version = line.version;
			operations.push_back(operation);
			ApplyOperation(working, operation);
		}
	}

	for (std::size_t target = 0; target < projected.lines.size(); ++target) {
		auto const& desired = projected.lines[target];
		auto current = find_line(working.snapshot.lines, desired.id);
		if (current == working.snapshot.lines.size()) {
			Operation operation;
			operation.line_id = desired.id;
			if (working.tombstones.count(desired.id)) {
				operation.kind = OperationKind::Restore;
			}
			else {
				operation.kind = OperationKind::Insert;
				operation.data = desired.fields;
				if (target > 0)
					operation.left_id = projected.lines[target - 1].id;
				else if (!working.snapshot.lines.empty())
					operation.right_id = working.snapshot.lines.front().id;
			}
			operations.push_back(operation);
			ApplyOperation(working, operation);
			current = find_line(working.snapshot.lines, desired.id);
		}
		if (current != target) {
			Operation operation;
			operation.kind = OperationKind::Move;
			operation.line_id = desired.id;
			operation.base_version = working.snapshot.lines[current].version;
			if (target > 0)
				operation.left_id = projected.lines[target - 1].id;
			else
				operation.right_id = working.snapshot.lines.front().id;
			operations.push_back(operation);
			ApplyOperation(working, operation);
		}
		current = find_line(working.snapshot.lines, desired.id);
		auto patch = difference(working.snapshot.lines[current].fields, desired.fields);
		if (!patch.empty()) {
			Operation operation;
			operation.kind = OperationKind::Modify;
			operation.line_id = desired.id;
			operation.base_version = working.snapshot.lines[current].version;
			operation.fields = std::move(patch);
			operations.push_back(operation);
			ApplyOperation(working, operation);
		}
	}
	if (confirmed.snapshot.styles != projected.styles) {
		Operation operation;
		operation.kind = OperationKind::ReplaceStyles;
		operation.base_version = working.snapshot.styles_version;
		operation.styles = projected.styles;
		operations.push_back(operation);
		ApplyOperation(working, operation);
	}
	if (confirmed.snapshot.script_info != projected.script_info) {
		Operation operation;
		operation.kind = OperationKind::ReplaceScriptInfo;
		operation.base_version = working.snapshot.script_info_version;
		operation.script_info = projected.script_info;
		operations.push_back(operation);
	}
	return operations;
}

} }
