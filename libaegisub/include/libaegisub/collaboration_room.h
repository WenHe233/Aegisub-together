// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <libaegisub/collaboration.h>

#include <string>
#include <optional>
#include <vector>

namespace agi { namespace collab {

struct CreateRoomRequest {
	std::string room_name;
	std::string room_password;
	std::string nickname;
	bool lock_enabled = true;
	Snapshot snapshot;
};

struct JoinRoomRequest {
	std::string room_name;
	std::string room_password;
	std::string nickname;
	std::string resume_token;
};

struct RoomJoined {
	std::string room_id;
	std::string member_id;
	std::string resume_token;
	bool lock_enabled = true;
	Snapshot snapshot;
};

struct ProtocolError {
	std::string code;
	std::string message;
	bool retryable = false;
};

struct LockStateMessage {
	std::string line_id;
	std::string requester_id;
	bool granted = false;
	std::optional<std::string> holder_id;
	std::optional<std::string> holder_name;
	std::int64_t expires_in_ms = 0;
};

struct PresenceMember {
	std::string member_id;
	std::string nickname;
	std::optional<std::string> line_id;
};

struct MaintenanceStateMessage {
	bool active = false;
	std::optional<std::string> holder_id;
	std::optional<std::string> holder_name;
	std::optional<std::string> idle_expires_at;
	std::optional<std::string> hard_expires_at;
	std::optional<std::string> cancel_requested_by;
	std::optional<std::string> cancel_requested_name;
	std::optional<std::string> cancel_force_at;
};

struct CommentChangedMessage {
	Comment comment;
	std::optional<Line> line;
	std::string actor_id;
};

std::string EncodeAccessAuth(std::string const& password);
std::string EncodeCreateRoom(CreateRoomRequest const& request);
std::string EncodeJoinRoom(JoinRoomRequest const& request);
RoomJoined DecodeRoomJoined(std::string const& payload_json);
ProtocolError DecodeProtocolError(std::string const& payload_json);
std::string EncodeLineReference(std::string const& line_id);
LockStateMessage DecodeLockState(std::string const& payload_json);
std::vector<PresenceMember> DecodePresence(std::string const& payload_json);
MaintenanceStateMessage DecodeMaintenanceState(std::string const& payload_json);
std::string EncodeCommentCreate(std::string const& line_id, std::int64_t base_line_version,
	std::string const& body, std::optional<std::string> const& suggested_text);
std::string EncodeCommentSetState(std::string const& comment_id, std::string const& state);
CommentChangedMessage DecodeCommentChanged(std::string const& payload_json);

} }
