// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <libaegisub/collaboration.h>

#include <string>

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

std::string EncodeAccessAuth(std::string const& password);
std::string EncodeCreateRoom(CreateRoomRequest const& request);
std::string EncodeJoinRoom(JoinRoomRequest const& request);
RoomJoined DecodeRoomJoined(std::string const& payload_json);
ProtocolError DecodeProtocolError(std::string const& payload_json);

} }
