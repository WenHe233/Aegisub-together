// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <libaegisub/collaboration_sync.h>

#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace agi { namespace collab {
namespace {
json::UnknownElement parse(std::string const& input) {
	std::istringstream stream(input);
	json::UnknownElement value;
	json::Reader::Read(value, stream);
	return value;
}

std::string write(json::UnknownElement const& value) {
	std::ostringstream stream;
	JsonWriter::Write(value, stream);
	return stream.str();
}

json::UnknownElement const& required(json::Object const& object, char const* name) {
	auto value = object.find(name);
	if (value == object.end()) throw std::invalid_argument(std::string("missing synchronization payload field: ") + name);
	return value->second;
}

json::UnknownElement const* optional(json::Object const& object, char const* name) {
	auto value = object.find(name);
	return value == object.end() ? nullptr : &value->second;
}

bool is_null(json::UnknownElement const& value) {
	try {
		static_cast<void>(static_cast<json::Null const&>(value));
		return true;
	}
	catch (json::Exception const&) {
		return false;
	}
}

void encode_anchor(json::Object& object, char const* name, std::optional<std::string> const& value) {
	if (value) object[name] = *value;
	else object[name] = json::Null{};
}

json::Object encode_full_fields(LineFields const& fields) {
	json::Object object;
	object["comment"] = fields.comment;
	object["layer"] = static_cast<std::int64_t>(fields.layer);
	object["start_ms"] = fields.start_ms;
	object["end_ms"] = fields.end_ms;
	object["style"] = fields.style;
	object["actor"] = fields.actor;
	object["effect"] = fields.effect;
	json::Array margins;
	for (auto margin : fields.margins) margins.emplace_back(static_cast<std::int64_t>(margin));
	object["margins"] = std::move(margins);
	object["text"] = fields.text;
	return object;
}

json::Object encode_patch(FieldPatch const& fields) {
	json::Object object;
	if (fields.comment) object["comment"] = *fields.comment;
	if (fields.layer) object["layer"] = static_cast<std::int64_t>(*fields.layer);
	if (fields.start_ms) object["start_ms"] = *fields.start_ms;
	if (fields.end_ms) object["end_ms"] = *fields.end_ms;
	if (fields.style) object["style"] = *fields.style;
	if (fields.actor) object["actor"] = *fields.actor;
	if (fields.effect) object["effect"] = *fields.effect;
	if (fields.margins) {
		json::Array margins;
		for (auto margin : *fields.margins) margins.emplace_back(static_cast<std::int64_t>(margin));
		object["margins"] = std::move(margins);
	}
	if (fields.text) object["text"] = *fields.text;
	return object;
}

json::Object encode_operation(Operation const& operation) {
	json::Object object;
	switch (operation.kind) {
	case OperationKind::Modify:
		object["op"] = "modify";
		object["line_id"] = operation.line_id;
		object["base_version"] = operation.base_version;
		object["fields"] = encode_patch(operation.fields);
		break;
	case OperationKind::Insert:
		object["op"] = "insert";
		object["line_id"] = operation.line_id;
		encode_anchor(object, "left_id", operation.left_id);
		encode_anchor(object, "right_id", operation.right_id);
		object["fields"] = encode_full_fields(operation.data);
		break;
	case OperationKind::Delete:
		object["op"] = "delete";
		object["line_id"] = operation.line_id;
		object["base_version"] = operation.base_version;
		break;
	case OperationKind::Move:
		object["op"] = "move";
		object["line_id"] = operation.line_id;
		encode_anchor(object, "left_id", operation.left_id);
		encode_anchor(object, "right_id", operation.right_id);
		object["base_version"] = operation.base_version;
		break;
	case OperationKind::Restore:
		object["op"] = "restore";
		object["line_id"] = operation.line_id;
		break;
	case OperationKind::ReplaceStyles: {
		object["op"] = "replace_styles";
		object["base_version"] = operation.base_version;
		json::Array styles;
		for (auto const& style : operation.styles) styles.emplace_back(style);
		object["styles"] = std::move(styles);
		break;
	}
	case OperationKind::ReplaceScriptInfo: {
		object["op"] = "replace_script_info";
		object["base_version"] = operation.base_version;
		json::Array entries;
		for (auto const& entry : operation.script_info) {
			json::Object encoded;
			encoded["key"] = entry.key;
			encoded["value"] = entry.value;
			entries.emplace_back(std::move(encoded));
		}
		object["entries"] = std::move(entries);
		break;
	}
	}
	return object;
}

std::optional<std::string> decode_anchor(json::Object const& object, char const* name) {
	auto const& value = required(object, name);
	if (is_null(value)) return std::nullopt;
	return static_cast<json::String const&>(value);
}

std::array<int, 3> decode_margins(json::UnknownElement const& value) {
	auto const& margins = static_cast<json::Array const&>(value);
	if (margins.size() != 3) throw std::invalid_argument("collaboration line must have three margins");
	return {{
		static_cast<int>(static_cast<json::Integer const&>(margins[0])),
		static_cast<int>(static_cast<json::Integer const&>(margins[1])),
		static_cast<int>(static_cast<json::Integer const&>(margins[2]))
	}};
}

LineFields decode_full_fields(json::UnknownElement const& value) {
	auto const& object = static_cast<json::Object const&>(value);
	LineFields fields;
	fields.comment = static_cast<json::Boolean const&>(required(object, "comment"));
	fields.layer = static_cast<int>(static_cast<json::Integer const&>(required(object, "layer")));
	fields.start_ms = static_cast<json::Integer const&>(required(object, "start_ms"));
	fields.end_ms = static_cast<json::Integer const&>(required(object, "end_ms"));
	fields.style = static_cast<json::String const&>(required(object, "style"));
	fields.actor = static_cast<json::String const&>(required(object, "actor"));
	fields.effect = static_cast<json::String const&>(required(object, "effect"));
	fields.margins = decode_margins(required(object, "margins"));
	fields.text = static_cast<json::String const&>(required(object, "text"));
	if (!IsValidLineFields(fields)) throw std::invalid_argument("invalid collaboration line fields");
	return fields;
}

FieldPatch decode_patch(json::UnknownElement const& value) {
	auto const& object = static_cast<json::Object const&>(value);
	FieldPatch fields;
	if (auto item = optional(object, "comment")) fields.comment = static_cast<json::Boolean const&>(*item);
	if (auto item = optional(object, "layer")) fields.layer = static_cast<int>(static_cast<json::Integer const&>(*item));
	if (auto item = optional(object, "start_ms")) fields.start_ms = static_cast<json::Integer const&>(*item);
	if (auto item = optional(object, "end_ms")) fields.end_ms = static_cast<json::Integer const&>(*item);
	if (auto item = optional(object, "style")) fields.style = static_cast<json::String const&>(*item);
	if (auto item = optional(object, "actor")) fields.actor = static_cast<json::String const&>(*item);
	if (auto item = optional(object, "effect")) fields.effect = static_cast<json::String const&>(*item);
	if (auto item = optional(object, "margins")) fields.margins = decode_margins(*item);
	if (auto item = optional(object, "text")) fields.text = static_cast<json::String const&>(*item);
	return fields;
}

Operation decode_operation(json::UnknownElement const& value) {
	auto const& object = static_cast<json::Object const&>(value);
	auto name = static_cast<json::String const&>(required(object, "op"));
	Operation operation;
	if (name == "modify") {
		operation.kind = OperationKind::Modify;
		operation.line_id = static_cast<json::String const&>(required(object, "line_id"));
		operation.base_version = static_cast<json::Integer const&>(required(object, "base_version"));
		operation.fields = decode_patch(required(object, "fields"));
	}
	else if (name == "insert") {
		operation.kind = OperationKind::Insert;
		operation.line_id = static_cast<json::String const&>(required(object, "line_id"));
		operation.left_id = decode_anchor(object, "left_id");
		operation.right_id = decode_anchor(object, "right_id");
		operation.data = decode_full_fields(required(object, "fields"));
	}
	else if (name == "delete") {
		operation.kind = OperationKind::Delete;
		operation.line_id = static_cast<json::String const&>(required(object, "line_id"));
		operation.base_version = static_cast<json::Integer const&>(required(object, "base_version"));
	}
	else if (name == "move") {
		operation.kind = OperationKind::Move;
		operation.line_id = static_cast<json::String const&>(required(object, "line_id"));
		operation.left_id = decode_anchor(object, "left_id");
		operation.right_id = decode_anchor(object, "right_id");
		operation.base_version = static_cast<json::Integer const&>(required(object, "base_version"));
	}
	else if (name == "restore") {
		operation.kind = OperationKind::Restore;
		operation.line_id = static_cast<json::String const&>(required(object, "line_id"));
	}
	else if (name == "replace_styles") {
		operation.kind = OperationKind::ReplaceStyles;
		operation.base_version = static_cast<json::Integer const&>(required(object, "base_version"));
		for (auto const& style : static_cast<json::Array const&>(required(object, "styles")))
			operation.styles.emplace_back(static_cast<json::String const&>(style));
	}
	else if (name == "replace_script_info") {
		operation.kind = OperationKind::ReplaceScriptInfo;
		operation.base_version = static_cast<json::Integer const&>(required(object, "base_version"));
		for (auto const& item : static_cast<json::Array const&>(required(object, "entries"))) {
			auto const& entry = static_cast<json::Object const&>(item);
			operation.script_info.push_back({
				static_cast<json::String const&>(required(entry, "key")),
				static_cast<json::String const&>(required(entry, "value"))
			});
		}
	}
	else throw std::invalid_argument("unknown collaboration operation");
	return operation;
}

Line decode_line(json::UnknownElement const& value) {
	auto const& object = static_cast<json::Object const&>(value);
	Line line;
	line.id = static_cast<json::String const&>(required(object, "line_id"));
	line.position = static_cast<json::String const&>(required(object, "pos_key"));
	line.version = static_cast<json::Integer const&>(required(object, "version"));
	line.fields = decode_full_fields(required(object, "fields"));
	if (!IsValidLineId(line.id) || !IsValidPosition(line.position) || line.version < 1)
		throw std::invalid_argument("invalid canonical collaboration line");
	return line;
}

json::Object encode_line(Line const& line) {
	json::Object object;
	object["line_id"] = line.id;
	object["pos_key"] = line.position;
	object["version"] = line.version;
	object["fields"] = encode_full_fields(line.fields);
	return object;
}

json::Object encode_snapshot(Snapshot const& snapshot) {
	json::Object object;
	json::Array lines;
	for (auto const& line : snapshot.lines) lines.emplace_back(encode_line(line));
	object["lines"] = std::move(lines);
	json::Array styles;
	for (auto const& style : snapshot.styles) styles.emplace_back(style);
	object["styles"] = std::move(styles);
	object["styles_version"] = snapshot.styles_version;
	json::Array script_info;
	for (auto const& item : snapshot.script_info) {
		json::Object entry;
		entry["key"] = item.key;
		entry["value"] = item.value;
		script_info.emplace_back(std::move(entry));
	}
	object["script_info"] = std::move(script_info);
	object["script_info_version"] = snapshot.script_info_version;
	return object;
}

Snapshot decode_snapshot(json::UnknownElement const& value) {
	auto const& object = static_cast<json::Object const&>(value);
	Snapshot snapshot;
	for (auto const& line : static_cast<json::Array const&>(required(object, "lines"))) snapshot.lines.push_back(decode_line(line));
	for (auto const& style : static_cast<json::Array const&>(required(object, "styles")))
		snapshot.styles.emplace_back(static_cast<json::String const&>(style));
	snapshot.styles_version = static_cast<json::Integer const&>(required(object, "styles_version"));
	for (auto const& item : static_cast<json::Array const&>(required(object, "script_info"))) {
		auto const& entry = static_cast<json::Object const&>(item);
		snapshot.script_info.push_back({
			static_cast<json::String const&>(required(entry, "key")),
			static_cast<json::String const&>(required(entry, "value"))
		});
	}
	snapshot.script_info_version = static_cast<json::Integer const&>(required(object, "script_info_version"));
	return snapshot;
}

std::size_t find_line(std::vector<Line> const& lines, std::string const& id) {
	for (std::size_t index = 0; index < lines.size(); ++index)
		if (lines[index].id == id) return index;
	return lines.size();
}

bool validate_snapshot(Snapshot const& snapshot) {
	if (snapshot.styles.empty() || snapshot.styles_version < 1 || snapshot.script_info_version < 1) return false;
	std::unordered_set<std::string> ids;
	std::string previous;
	for (auto const& line : snapshot.lines) {
		if (!IsValidLineId(line.id) || !ids.insert(line.id).second || !IsValidPosition(line.position) ||
			(!previous.empty() && line.position <= previous) || line.version < 1 || !IsValidLineFields(line.fields)) return false;
		previous = line.position;
	}
	return true;
}

bool apply_canonical(DocumentState& state, AppliedOperation const& applied) {
	auto& lines = state.snapshot.lines;
	auto index = find_line(lines, applied.operation.line_id);
	switch (applied.operation.kind) {
	case OperationKind::Delete:
		if (index == lines.size()) return false;
		state.tombstones[applied.operation.line_id] = lines[index];
		lines.erase(lines.begin() + index);
		break;
	case OperationKind::ReplaceStyles:
		if (applied.operation.styles.empty() || applied.styles_version < 1) return false;
		state.snapshot.styles = applied.operation.styles;
		state.snapshot.styles_version = applied.styles_version;
		break;
	case OperationKind::ReplaceScriptInfo:
		if (applied.script_info_version < 1) return false;
		state.snapshot.script_info = applied.operation.script_info;
		state.snapshot.script_info_version = applied.script_info_version;
		break;
	case OperationKind::Insert:
		if (!applied.has_line || index != lines.size()) return false;
		lines.push_back(applied.line);
		break;
	case OperationKind::Restore:
		if (!applied.has_line || index != lines.size()) return false;
		state.tombstones.erase(applied.operation.line_id);
		lines.push_back(applied.line);
		break;
	case OperationKind::Modify:
	case OperationKind::Move:
		if (!applied.has_line || index == lines.size()) return false;
		lines[index] = applied.line;
		break;
	}
	std::sort(lines.begin(), lines.end(), [](Line const& left, Line const& right) { return left.position < right.position; });
	return true;
}

void remap_value(std::string& value, std::unordered_map<std::string, std::string> const& remap) {
	auto replacement = remap.find(value);
	if (replacement != remap.end()) value = replacement->second;
}

void remap_operation(Operation& operation, std::unordered_map<std::string, std::string> const& remap) {
	remap_value(operation.line_id, remap);
	if (operation.left_id) remap_value(*operation.left_id, remap);
	if (operation.right_id) remap_value(*operation.right_id, remap);
}
}

std::string EncodeSubmitBatch(PendingBatch const& batch) {
	if (batch.batch_id.empty() || batch.batch_id.size() > 64 || batch.operations.empty())
		throw std::invalid_argument("collaboration batch is empty or invalid");
	json::Object object;
	object["batch_id"] = batch.batch_id;
	json::Array operations;
	for (auto const& operation : batch.operations) operations.emplace_back(encode_operation(operation));
	object["operations"] = std::move(operations);
	return write(json::UnknownElement(std::move(object)));
}

AppliedBatch DecodeAppliedBatch(std::string const& payload_json) {
	auto root = parse(payload_json);
	auto const& object = static_cast<json::Object const&>(root);
	AppliedBatch batch;
	batch.batch_id = static_cast<json::String const&>(required(object, "batch_id"));
	batch.actor_id = static_cast<json::String const&>(required(object, "actor_id"));
	for (auto const& item : static_cast<json::Array const&>(required(object, "operations"))) {
		auto const& encoded = static_cast<json::Object const&>(item);
		AppliedOperation operation;
		operation.operation = decode_operation(required(encoded, "operation"));
		if (auto line = optional(encoded, "line"); line && !is_null(*line)) {
			operation.line = decode_line(*line);
			operation.has_line = true;
			if (operation.line.id != operation.operation.line_id)
				throw std::invalid_argument("canonical line does not match its operation ID");
		}
		if (auto version = optional(encoded, "styles_version")) operation.styles_version = static_cast<json::Integer const&>(*version);
		if (auto version = optional(encoded, "script_info_version")) operation.script_info_version = static_cast<json::Integer const&>(*version);
		batch.operations.push_back(std::move(operation));
	}
	for (auto const& item : static_cast<json::Object const&>(required(object, "id_remap")))
		batch.id_remap.emplace(item.first, static_cast<json::String const&>(item.second));
	if (auto positions = optional(object, "positions"))
		for (auto const& item : static_cast<json::Object const&>(*positions))
			batch.positions.emplace(item.first, static_cast<json::String const&>(item.second));
	if (batch.batch_id.empty() || batch.actor_id.empty() || batch.operations.empty())
		throw std::invalid_argument("applied collaboration batch is incomplete");
	for (auto const& item : batch.id_remap)
		if (!IsValidLineId(item.first) || !IsValidLineId(item.second)) throw std::invalid_argument("invalid collaboration ID remap");
	return batch;
}

RejectedBatch DecodeRejectedBatch(std::string const& payload_json) {
	auto root = parse(payload_json);
	auto const& object = static_cast<json::Object const&>(root);
	RejectedBatch rejection;
	rejection.batch_id = static_cast<json::String const&>(required(object, "batch_id"));
	rejection.code = static_cast<json::String const&>(required(object, "code"));
	rejection.message = static_cast<json::String const&>(required(object, "message"));
	if (auto line = optional(object, "line_id")) rejection.line_id = static_cast<json::String const&>(*line);
	if (auto index = optional(object, "operation_index")) rejection.operation_index = static_cast<int>(static_cast<json::Integer const&>(*index));
	if (rejection.batch_id.empty() || rejection.code.empty()) throw std::invalid_argument("batch rejection is incomplete");
	return rejection;
}

std::string EncodeSnapshotRequest(std::int64_t after_revision) {
	if (after_revision < 0) throw std::invalid_argument("snapshot revision cannot be negative");
	json::Object object;
	object["after_revision"] = after_revision;
	return write(json::UnknownElement(std::move(object)));
}

Snapshot DecodeSnapshotState(std::string const& payload_json, std::int64_t& revision) {
	auto root = parse(payload_json);
	auto const& object = static_cast<json::Object const&>(root);
	revision = static_cast<json::Integer const&>(required(object, "revision"));
	if (revision < 0) throw std::invalid_argument("snapshot revision cannot be negative");
	auto snapshot = decode_snapshot(required(object, "snapshot"));
	if (!validate_snapshot(snapshot)) throw std::invalid_argument("invalid authoritative collaboration snapshot");
	return snapshot;
}

std::string EncodeOfflineJournal(OfflineJournal const& journal) {
	if (journal.base_revision < 0 || !validate_snapshot(journal.baseline) || !validate_snapshot(journal.local))
		throw std::invalid_argument("invalid offline collaboration journal");
	json::Object object;
	object["format_version"] = 1;
	object["base_revision"] = journal.base_revision;
	object["baseline"] = encode_snapshot(journal.baseline);
	object["local"] = encode_snapshot(journal.local);
	json::Array pending;
	for (auto const& batch : journal.pending) {
		if (batch.batch_id.empty() || batch.operations.empty()) throw std::invalid_argument("invalid pending offline batch");
		json::Object encoded;
		encoded["batch_id"] = batch.batch_id;
		json::Array operations;
		for (auto const& operation : batch.operations) operations.emplace_back(encode_operation(operation));
		encoded["operations"] = std::move(operations);
		pending.emplace_back(std::move(encoded));
	}
	object["pending"] = std::move(pending);
	return write(json::UnknownElement(std::move(object)));
}

OfflineJournal DecodeOfflineJournal(std::string const& input) {
	auto root = parse(input);
	auto const& object = static_cast<json::Object const&>(root);
	if (static_cast<json::Integer const&>(required(object, "format_version")) != 1)
		throw std::invalid_argument("unsupported offline collaboration journal");
	OfflineJournal journal;
	journal.base_revision = static_cast<json::Integer const&>(required(object, "base_revision"));
	journal.baseline = decode_snapshot(required(object, "baseline"));
	journal.local = decode_snapshot(required(object, "local"));
	for (auto const& item : static_cast<json::Array const&>(required(object, "pending"))) {
		auto const& encoded = static_cast<json::Object const&>(item);
		PendingBatch batch;
		batch.batch_id = static_cast<json::String const&>(required(encoded, "batch_id"));
		for (auto const& operation : static_cast<json::Array const&>(required(encoded, "operations")))
			batch.operations.push_back(decode_operation(operation));
		if (batch.batch_id.empty() || batch.operations.empty()) throw std::invalid_argument("invalid pending offline batch");
		journal.pending.push_back(std::move(batch));
	}
	if (journal.base_revision < 0 || !validate_snapshot(journal.baseline) || !validate_snapshot(journal.local))
		throw std::invalid_argument("invalid offline collaboration journal");
	return journal;
}

void SyncState::Initialize(Snapshot snapshot, std::int64_t room_revision) {
	if (room_revision < 0 || !validate_snapshot(snapshot)) throw std::invalid_argument("invalid collaboration synchronization baseline");
	confirmed = DocumentState{};
	confirmed.snapshot = std::move(snapshot);
	projected = confirmed;
	pending.clear();
	revision = room_revision;
	initialized = true;
}

bool SyncState::ResetConfirmed(Snapshot snapshot, std::int64_t room_revision, bool preserve_pending) {
	if (!initialized || !preserve_pending) {
		Initialize(std::move(snapshot), room_revision);
		return true;
	}
	if (room_revision < 0 || !validate_snapshot(snapshot)) throw std::invalid_argument("invalid collaboration synchronization baseline");
	DocumentState previous_confirmed = confirmed;
	DocumentState previous_projected = projected;
	auto previous_revision = revision;
	confirmed = DocumentState{};
	confirmed.snapshot = std::move(snapshot);
	revision = room_revision;
	if (rebuild_projected()) return true;
	confirmed = std::move(previous_confirmed);
	projected = std::move(previous_projected);
	revision = previous_revision;
	return false;
}

void SyncState::RememberTombstonesFrom(Snapshot const& baseline) {
	if (!initialized) throw std::logic_error("collaboration synchronization is not initialized");
	for (auto const& line : baseline.lines) {
		if (find_line(confirmed.snapshot.lines, line.id) == confirmed.snapshot.lines.size()) {
			confirmed.tombstones.emplace(line.id, line);
			projected.tombstones.emplace(line.id, line);
		}
	}
}

bool SyncState::rebuild_projected() {
	DocumentState rebuilt = confirmed;
	for (auto const& batch : pending)
		if (!ApplyOperations(rebuilt, batch.operations)) return false;
	projected = std::move(rebuilt);
	return true;
}

PendingBatch SyncState::QueueLocalSnapshot(std::string batch_id, Snapshot const& local_snapshot) {
	if (!initialized) throw std::logic_error("collaboration synchronization is not initialized");
	if (batch_id.empty() || batch_id.size() > 64) throw std::invalid_argument("invalid collaboration batch ID");
	if (std::any_of(pending.begin(), pending.end(), [&](PendingBatch const& batch) { return batch.batch_id == batch_id; }))
		throw std::invalid_argument("duplicate pending collaboration batch ID");
	PendingBatch batch{std::move(batch_id), DiffSnapshots(projected, local_snapshot)};
	if (batch.operations.empty()) return batch;
	DocumentState updated = projected;
	if (!ApplyOperations(updated, batch.operations)) throw std::logic_error("local collaboration diff could not be projected");
	projected = std::move(updated);
	pending.push_back(batch);
	return batch;
}

SyncApplyResult SyncState::ApplyBatch(AppliedBatch const& batch, std::int64_t room_revision, std::string const& own_member_id) {
	if (!initialized) throw std::logic_error("collaboration synchronization is not initialized");
	auto pending_batch = std::find_if(pending.begin(), pending.end(), [&](PendingBatch const& item) { return item.batch_id == batch.batch_id; });
	bool own = batch.actor_id == own_member_id && pending_batch != pending.end();
	if (room_revision <= revision) {
		if (!own) return {SyncApplyStatus::duplicate, false, false};
		pending.erase(pending_batch);
		for (auto& remaining : pending)
			for (auto& operation : remaining.operations) remap_operation(operation, batch.id_remap);
		bool rebuilt = rebuild_projected();
		return {rebuilt ? SyncApplyStatus::duplicate : SyncApplyStatus::pending_conflict, rebuilt && !batch.id_remap.empty(), true};
	}
	if (room_revision != revision + 1) return {SyncApplyStatus::revision_gap, false, false};

	DocumentState updated = confirmed;
	for (auto const& operation : batch.operations)
		if (!apply_canonical(updated, operation)) throw std::invalid_argument("canonical collaboration batch does not apply to confirmed state");
	for (auto const& position : batch.positions) {
		auto index = find_line(updated.snapshot.lines, position.first);
		if (index == updated.snapshot.lines.size() || !IsValidPosition(position.second))
			throw std::invalid_argument("canonical reindex map references an invalid line");
		updated.snapshot.lines[index].position = position.second;
	}
	std::sort(updated.snapshot.lines.begin(), updated.snapshot.lines.end(), [](Line const& left, Line const& right) { return left.position < right.position; });
	if (!validate_snapshot(updated.snapshot)) throw std::invalid_argument("canonical collaboration batch produced an invalid snapshot");
	confirmed = std::move(updated);
	revision = room_revision;
	if (own) pending.erase(pending_batch);
	if (own && !batch.id_remap.empty())
		for (auto& remaining : pending)
			for (auto& operation : remaining.operations) remap_operation(operation, batch.id_remap);
	bool rebuilt = rebuild_projected();
	bool visible_change = rebuilt && (!own || !batch.id_remap.empty() || !batch.positions.empty());
	return {rebuilt ? SyncApplyStatus::applied : SyncApplyStatus::pending_conflict, visible_change, own};
}

std::vector<PendingBatch> SyncState::RejectBatch(RejectedBatch const& rejection) {
	if (!initialized) throw std::logic_error("collaboration synchronization is not initialized");
	auto rejected = std::find_if(pending.begin(), pending.end(), [&](PendingBatch const& item) { return item.batch_id == rejection.batch_id; });
	if (rejected == pending.end()) return {};
	std::vector<PendingBatch> removed(rejected, pending.end());
	pending.erase(rejected, pending.end());
	if (!rebuild_projected()) throw std::logic_error("remaining pending collaboration batches cannot be projected");
	return removed;
}

} }
