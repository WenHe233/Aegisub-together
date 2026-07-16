// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <libaegisub/collaboration_room.h>

#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <sstream>
#include <stdexcept>
#include <algorithm>

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
	if (value == object.end()) throw std::invalid_argument(std::string("missing room payload field: ") + name);
	return value->second;
}

std::optional<std::string> optional_nullable_string(json::Object const& object, char const* name) {
	auto value = object.find(name);
	if (value == object.end()) return std::nullopt;
	try {
		static_cast<void>(static_cast<json::Null const&>(value->second));
		return std::nullopt;
	}
	catch (json::Exception const&) {
		return static_cast<json::String const&>(value->second);
	}
}

json::Object encode_comment(Comment const& comment) {
	json::Object object;
	object["comment_id"] = comment.comment_id;
	object["line_id"] = comment.line_id;
	object["author_id"] = comment.author_id;
	object["author_name"] = comment.author_name;
	object["body"] = comment.body;
	if (comment.suggested_text) object["suggested_text"] = *comment.suggested_text;
	object["base_line_version"] = comment.base_line_version;
	object["state"] = comment.state;
	object["created_at"] = comment.created_at;
	if (comment.resolved_by) object["resolved_by"] = *comment.resolved_by;
	return object;
}

Comment decode_comment(json::UnknownElement const& value) {
	auto const& object = static_cast<json::Object const&>(value);
	Comment comment;
	comment.comment_id = static_cast<json::String const&>(required(object, "comment_id"));
	comment.line_id = static_cast<json::String const&>(required(object, "line_id"));
	comment.author_id = static_cast<json::String const&>(required(object, "author_id"));
	comment.author_name = static_cast<json::String const&>(required(object, "author_name"));
	comment.body = static_cast<json::String const&>(required(object, "body"));
	comment.suggested_text = optional_nullable_string(object, "suggested_text");
	comment.base_line_version = static_cast<json::Integer const&>(required(object, "base_line_version"));
	comment.state = static_cast<json::String const&>(required(object, "state"));
	comment.created_at = static_cast<json::String const&>(required(object, "created_at"));
	comment.resolved_by = optional_nullable_string(object, "resolved_by");
	if (comment.comment_id.empty() || !IsValidLineId(comment.line_id) || comment.author_id.empty() || comment.body.empty() ||
		comment.base_line_version < 1 || (comment.state != "open" && comment.state != "accepted" && comment.state != "rejected" && comment.state != "resolved"))
		throw std::invalid_argument("invalid collaboration comment");
	return comment;
}

json::Object encode_fields(LineFields const& fields) {
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

json::Object encode_snapshot(Snapshot const& snapshot) {
	json::Object object;
	json::Array lines;
	for (auto const& line : snapshot.lines) {
		json::Object encoded;
		encoded["line_id"] = line.id;
		encoded["pos_key"] = line.position;
		encoded["version"] = line.version;
		encoded["fields"] = encode_fields(line.fields);
		lines.emplace_back(std::move(encoded));
	}
	object["lines"] = std::move(lines);
	json::Array styles;
	for (auto const& style : snapshot.styles) styles.emplace_back(style);
	object["styles"] = std::move(styles);
	object["styles_version"] = snapshot.styles_version;
	json::Array script_info;
	for (auto const& entry : snapshot.script_info) {
		json::Object encoded;
		encoded["key"] = entry.key;
		encoded["value"] = entry.value;
		script_info.emplace_back(std::move(encoded));
	}
	object["script_info"] = std::move(script_info);
	object["script_info_version"] = snapshot.script_info_version;
	json::Array comments;
	for (auto const& comment : snapshot.comments) comments.emplace_back(encode_comment(comment));
	object["comments"] = std::move(comments);
	return object;
}

LineFields decode_fields(json::UnknownElement const& value) {
	auto const& object = static_cast<json::Object const&>(value);
	LineFields fields;
	fields.comment = static_cast<json::Boolean const&>(required(object, "comment"));
	fields.layer = static_cast<int>(static_cast<json::Integer const&>(required(object, "layer")));
	fields.start_ms = static_cast<json::Integer const&>(required(object, "start_ms"));
	fields.end_ms = static_cast<json::Integer const&>(required(object, "end_ms"));
	fields.style = static_cast<json::String const&>(required(object, "style"));
	fields.actor = static_cast<json::String const&>(required(object, "actor"));
	fields.effect = static_cast<json::String const&>(required(object, "effect"));
	auto const& margins = static_cast<json::Array const&>(required(object, "margins"));
	if (margins.size() != fields.margins.size()) throw std::invalid_argument("collaboration line must have three margins");
	for (std::size_t index = 0; index < margins.size(); ++index)
		fields.margins[index] = static_cast<int>(static_cast<json::Integer const&>(margins[index]));
	fields.text = static_cast<json::String const&>(required(object, "text"));
	if (!IsValidLineFields(fields)) throw std::invalid_argument("invalid collaboration line fields");
	return fields;
}

Snapshot decode_snapshot(json::UnknownElement const& value) {
	auto const& object = static_cast<json::Object const&>(value);
	Snapshot snapshot;
	for (auto const& encoded : static_cast<json::Array const&>(required(object, "lines"))) {
		auto const& line_object = static_cast<json::Object const&>(encoded);
		Line line;
		line.id = static_cast<json::String const&>(required(line_object, "line_id"));
		line.position = static_cast<json::String const&>(required(line_object, "pos_key"));
		line.version = static_cast<json::Integer const&>(required(line_object, "version"));
		line.fields = decode_fields(required(line_object, "fields"));
		snapshot.lines.emplace_back(std::move(line));
	}
	for (auto const& style : static_cast<json::Array const&>(required(object, "styles")))
		snapshot.styles.emplace_back(static_cast<json::String const&>(style));
	snapshot.styles_version = static_cast<json::Integer const&>(required(object, "styles_version"));
	for (auto const& encoded : static_cast<json::Array const&>(required(object, "script_info"))) {
		auto const& entry = static_cast<json::Object const&>(encoded);
		snapshot.script_info.push_back({
			static_cast<json::String const&>(required(entry, "key")),
			static_cast<json::String const&>(required(entry, "value"))
		});
	}
	snapshot.script_info_version = static_cast<json::Integer const&>(required(object, "script_info_version"));
	for (auto const& comment : static_cast<json::Array const&>(required(object, "comments")))
		snapshot.comments.push_back(decode_comment(comment));
	return snapshot;
}

std::string encode_object(json::Object object) {
	json::UnknownElement root(std::move(object));
	return write(root);
}

}

std::string EncodeAccessAuth(std::string const& password) {
	json::Object object;
	object["password"] = password;
	return encode_object(std::move(object));
}

std::string EncodeCreateRoom(CreateRoomRequest const& request) {
	json::Object object;
	object["room_name"] = request.room_name;
	object["room_password"] = request.room_password;
	object["nickname"] = request.nickname;
	object["lock_enabled"] = request.lock_enabled;
	object["snapshot"] = encode_snapshot(request.snapshot);
	return encode_object(std::move(object));
}

std::string EncodeJoinRoom(JoinRoomRequest const& request) {
	json::Object object;
	object["room_name"] = request.room_name;
	object["room_password"] = request.room_password;
	object["nickname"] = request.nickname;
	if (!request.resume_token.empty()) object["resume_token"] = request.resume_token;
	return encode_object(std::move(object));
}

RoomJoined DecodeRoomJoined(std::string const& payload_json) {
	auto root = parse(payload_json);
	auto const& object = static_cast<json::Object const&>(root);
	RoomJoined joined;
	joined.room_id = static_cast<json::String const&>(required(object, "room_id"));
	joined.member_id = static_cast<json::String const&>(required(object, "member_id"));
	joined.resume_token = static_cast<json::String const&>(required(object, "resume_token"));
	joined.lock_enabled = static_cast<json::Boolean const&>(required(object, "lock_enabled"));
	joined.snapshot = decode_snapshot(required(object, "snapshot"));
	auto lock_sets = object.find("lock_sets");
	if (lock_sets != object.end()) {
		for (auto const& encoded : static_cast<json::Array const&>(lock_sets->second))
			joined.lock_sets.push_back(DecodeLockSetState(write(encoded)));
	}
	auto presence = object.find("presence");
	if (presence != object.end()) joined.presence = DecodePresence(write(presence->second));
	if (joined.room_id.empty() || joined.member_id.empty() || joined.resume_token.empty())
		throw std::invalid_argument("room join identity is incomplete");
	return joined;
}

ProtocolError DecodeProtocolError(std::string const& payload_json) {
	auto root = parse(payload_json);
	auto const& object = static_cast<json::Object const&>(root);
	ProtocolError error;
	error.code = static_cast<json::String const&>(required(object, "code"));
	error.message = static_cast<json::String const&>(required(object, "message"));
	error.retryable = static_cast<json::Boolean const&>(required(object, "retryable"));
	return error;
}

std::string EncodeLineReference(std::string const& line_id) {
	if (!IsValidLineId(line_id)) throw std::invalid_argument("invalid collaboration line reference");
	json::Object object;
	object["line_id"] = line_id;
	return encode_object(std::move(object));
}

LockStateMessage DecodeLockState(std::string const& payload_json) {
	auto root = parse(payload_json);
	auto const& object = static_cast<json::Object const&>(root);
	LockStateMessage state;
	state.line_id = static_cast<json::String const&>(required(object, "line_id"));
	state.requester_id = static_cast<json::String const&>(required(object, "requester_id"));
	state.granted = static_cast<json::Boolean const&>(required(object, "granted"));
	state.holder_id = optional_nullable_string(object, "holder_id");
	state.holder_name = optional_nullable_string(object, "holder_name");
	state.expires_in_ms = static_cast<json::Integer const&>(required(object, "expires_in_ms"));
	if (!IsValidLineId(state.line_id) || state.requester_id.empty() || state.expires_in_ms < 0)
		throw std::invalid_argument("invalid collaboration lock state");
	return state;
}

std::string EncodeLockSetRequest(std::vector<std::string> line_ids,
	std::optional<std::string> const& active_line_id, std::int64_t generation) {
	if (generation < 0 || line_ids.size() > MaximumLockSetSize)
		throw std::invalid_argument("invalid collaboration lock set request");
	std::sort(line_ids.begin(), line_ids.end());
	if (std::adjacent_find(line_ids.begin(), line_ids.end()) != line_ids.end())
		throw std::invalid_argument("duplicate collaboration lock set line");
	for (auto const& line_id : line_ids)
		if (!IsValidLineId(line_id)) throw std::invalid_argument("invalid collaboration lock set line");
	if (active_line_id && !IsValidLineId(*active_line_id))
		throw std::invalid_argument("invalid collaboration active line");
	json::Object object;
	json::Array lines;
	for (auto const& line_id : line_ids) lines.emplace_back(line_id);
	object["line_ids"] = std::move(lines);
	if (active_line_id) object["active_line_id"] = *active_line_id;
	else object["active_line_id"] = json::Null{};
	object["generation"] = generation;
	return encode_object(std::move(object));
}

LockSetStateMessage DecodeLockSetState(std::string const& payload_json) {
	auto root = parse(payload_json);
	auto const& object = static_cast<json::Object const&>(root);
	LockSetStateMessage state;
	state.member_id = static_cast<json::String const&>(required(object, "member_id"));
	state.member_name = static_cast<json::String const&>(required(object, "member_name"));
	state.granted = static_cast<json::Boolean const&>(required(object, "granted"));
	state.generation = static_cast<json::Integer const&>(required(object, "generation"));
	for (auto const& encoded : static_cast<json::Array const&>(required(object, "line_ids"))) {
		auto line_id = static_cast<json::String const&>(encoded);
		if (!IsValidLineId(line_id)) throw std::invalid_argument("invalid collaboration lock set line");
		state.line_ids.push_back(std::move(line_id));
	}
	for (auto const& encoded : static_cast<json::Array const&>(required(object, "conflicts"))) {
		auto const& conflict_object = static_cast<json::Object const&>(encoded);
		LockConflictMessage conflict;
		conflict.line_id = static_cast<json::String const&>(required(conflict_object, "line_id"));
		conflict.holder_id = static_cast<json::String const&>(required(conflict_object, "holder_id"));
		conflict.holder_name = static_cast<json::String const&>(required(conflict_object, "holder_name"));
		conflict.expires_in_ms = static_cast<json::Integer const&>(required(conflict_object, "expires_in_ms"));
		if (!IsValidLineId(conflict.line_id) || conflict.holder_id.empty() || conflict.holder_name.empty() || conflict.expires_in_ms < 0)
			throw std::invalid_argument("invalid collaboration lock conflict");
		state.conflicts.push_back(std::move(conflict));
	}
	if (state.member_id.empty() || state.member_name.empty() || state.generation < 0 ||
		state.line_ids.size() > MaximumLockSetSize || (state.granted && !state.conflicts.empty()) ||
		(!state.granted && !state.line_ids.empty()))
		throw std::invalid_argument("invalid collaboration lock set state");
	return state;
}

std::vector<PresenceMember> DecodePresence(std::string const& payload_json) {
	auto root = parse(payload_json);
	auto const& object = static_cast<json::Object const&>(root);
	std::vector<PresenceMember> members;
	for (auto const& item : static_cast<json::Array const&>(required(object, "members"))) {
		auto const& encoded = static_cast<json::Object const&>(item);
		PresenceMember member;
		member.member_id = static_cast<json::String const&>(required(encoded, "member_id"));
		member.nickname = static_cast<json::String const&>(required(encoded, "nickname"));
		member.line_id = optional_nullable_string(encoded, "line_id");
		if (member.member_id.empty() || member.nickname.empty() || (member.line_id && !IsValidLineId(*member.line_id)))
			throw std::invalid_argument("invalid collaboration presence member");
		members.push_back(std::move(member));
	}
	return members;
}

MaintenanceStateMessage DecodeMaintenanceState(std::string const& payload_json) {
	auto root = parse(payload_json);
	auto const& object = static_cast<json::Object const&>(root);
	MaintenanceStateMessage state;
	state.active = static_cast<json::Boolean const&>(required(object, "active"));
	state.holder_id = optional_nullable_string(object, "holder_id");
	state.holder_name = optional_nullable_string(object, "holder_name");
	state.idle_expires_at = optional_nullable_string(object, "idle_expires_at");
	state.hard_expires_at = optional_nullable_string(object, "hard_expires_at");
	state.cancel_requested_by = optional_nullable_string(object, "cancel_requested_by");
	state.cancel_requested_name = optional_nullable_string(object, "cancel_requested_name");
	state.cancel_force_at = optional_nullable_string(object, "cancel_force_at");
	if (state.active && (!state.holder_id || !state.holder_name)) throw std::invalid_argument("active maintenance state has no holder");
	return state;
}

std::string EncodeCommentCreate(std::string const& line_id, std::int64_t base_line_version,
	std::string const& body, std::optional<std::string> const& suggested_text) {
	if (!IsValidLineId(line_id) || base_line_version < 1 || body.empty()) throw std::invalid_argument("invalid collaboration comment");
	json::Object object;
	object["line_id"] = line_id;
	object["base_line_version"] = base_line_version;
	object["body"] = body;
	if (suggested_text) object["suggested_text"] = *suggested_text;
	else object["suggested_text"] = json::Null{};
	return encode_object(std::move(object));
}

std::string EncodeCommentSetState(std::string const& comment_id, std::string const& state) {
	if (comment_id.empty() || (state != "accepted" && state != "rejected" && state != "resolved"))
		throw std::invalid_argument("invalid collaboration comment state");
	json::Object object;
	object["comment_id"] = comment_id;
	object["state"] = state;
	return encode_object(std::move(object));
}

CommentChangedMessage DecodeCommentChanged(std::string const& payload_json) {
	auto root = parse(payload_json);
	auto const& object = static_cast<json::Object const&>(root);
	CommentChangedMessage message;
	message.comment = decode_comment(required(object, "comment"));
	message.actor_id = static_cast<json::String const&>(required(object, "actor_id"));
	auto const& encoded_line = required(object, "line");
	try { static_cast<void>(static_cast<json::Null const&>(encoded_line)); }
	catch (json::Exception const&) {
		auto const& line_object = static_cast<json::Object const&>(encoded_line);
		Line line;
		line.id = static_cast<json::String const&>(required(line_object, "line_id"));
		line.position = static_cast<json::String const&>(required(line_object, "pos_key"));
		line.version = static_cast<json::Integer const&>(required(line_object, "version"));
		line.fields = decode_fields(required(line_object, "fields"));
		if (!IsValidLineId(line.id) || !IsValidPosition(line.position) || line.version < 1)
			throw std::invalid_argument("invalid collaboration comment line");
		message.line = std::move(line);
	}
	if (message.actor_id.empty() || (message.line && message.line->id != message.comment.line_id))
		throw std::invalid_argument("invalid collaboration comment change");
	return message;
}

} }
