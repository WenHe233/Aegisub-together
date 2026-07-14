// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <main.h>

#include <libaegisub/cajun/reader.h>
#include <libaegisub/collaboration_room.h>

#include <sstream>

using namespace agi::collab;

namespace {
Snapshot room_snapshot() {
	Snapshot snapshot;
	Line line;
	line.id = "9K3MT7Q2CD-1";
	line.position = "V";
	line.fields.text = "Hello";
	snapshot.lines.push_back(line);
	snapshot.styles = {"Style: Default,Arial,48"};
	snapshot.script_info = {{"ScriptType", "v4.00+"}};
	return snapshot;
}
}

TEST(collaboration_room, create_payload_matches_protocol_shape) {
	CreateRoomRequest request{"episode-01", "room password", "translator", true, room_snapshot()};
	auto encoded = EncodeCreateRoom(request);
	std::istringstream stream(encoded);
	json::UnknownElement root;
	json::Reader::Read(root, stream);
	auto const& object = static_cast<json::Object const&>(root);
	EXPECT_EQ("episode-01", static_cast<json::String const&>(object.at("room_name")));
	EXPECT_EQ(1u, static_cast<json::Array const&>(static_cast<json::Object const&>(object.at("snapshot")).at("lines")).size());
}

TEST(collaboration_room, decodes_server_room_snapshot_and_ignores_future_fields) {
	auto payload = R"({"room_id":"room-1","member_id":"member-1","resume_token":"resume-1","lock_enabled":true,"future":7,"snapshot":{"lines":[{"line_id":"9K3MT7Q2CD-1","pos_key":"V","version":1,"fields":{"comment":false,"layer":0,"start_ms":0,"end_ms":5000,"style":"Default","actor":"","effect":"","margins":[0,0,0],"text":"Hello"}}],"styles":["Style: Default,Arial,48"],"styles_version":1,"script_info":[{"key":"ScriptType","value":"v4.00+"}],"script_info_version":1,"comments":[]}})";
	auto joined = DecodeRoomJoined(payload);
	EXPECT_EQ("room-1", joined.room_id);
	EXPECT_EQ("resume-1", joined.resume_token);
	ASSERT_EQ(1u, joined.snapshot.lines.size());
	EXPECT_EQ("Hello", joined.snapshot.lines[0].fields.text);
}

TEST(collaboration_room, rejects_incomplete_snapshot) {
	EXPECT_THROW(DecodeRoomJoined(R"({"room_id":"room-1"})"), std::exception);
}

TEST(collaboration_room, encodes_line_references_and_decodes_lock_presence) {
	auto reference = EncodeLineReference("9K3MT7Q2CD-1");
	EXPECT_NE(std::string::npos, reference.find("9K3MT7Q2CD-1"));
	auto lock = DecodeLockState(R"({"line_id":"9K3MT7Q2CD-1","requester_id":"member-2","granted":false,"holder_id":"member-1","holder_name":"translator","expires_in_ms":59000})");
	ASSERT_TRUE(lock.holder_id);
	EXPECT_EQ("member-1", *lock.holder_id);
	EXPECT_EQ("translator", *lock.holder_name);
	auto presence = DecodePresence(R"({"members":[{"member_id":"member-1","nickname":"translator","line_id":"9K3MT7Q2CD-1","last_seen":"2026-07-14T00:00:00Z"},{"member_id":"member-2","nickname":"proofreader","line_id":null,"last_seen":"2026-07-14T00:00:00Z"}]})");
	ASSERT_EQ(2u, presence.size());
	EXPECT_EQ("9K3MT7Q2CD-1", *presence[0].line_id);
	EXPECT_FALSE(presence[1].line_id);
}
