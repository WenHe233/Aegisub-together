// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <libaegisub/collaboration_room.h>

#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <sstream>
#include <stdexcept>

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
	object["comments"] = json::Array{};
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
	static_cast<void>(static_cast<json::Array const&>(required(object, "comments")));
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

} }
