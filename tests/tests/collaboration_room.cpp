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

TEST(collaboration_room, encodes_and_decodes_atomic_lock_sets) {
	auto encoded = EncodeLockSetRequest({"9K3MT7Q2CD-2", "9K3MT7Q2CD-1"}, std::string("9K3MT7Q2CD-2"), 17);
	EXPECT_NE(std::string::npos, encoded.find("\"generation\" : 17"));
	auto state = DecodeLockSetState(R"({"member_id":"member-1","member_name":"translator","granted":true,"line_ids":["9K3MT7Q2CD-1","9K3MT7Q2CD-2"],"conflicts":[],"generation":17})");
	EXPECT_TRUE(state.granted);
	EXPECT_EQ(2u, state.line_ids.size());
	EXPECT_THROW(EncodeLockSetRequest({"9K3MT7Q2CD-1", "9K3MT7Q2CD-1"}, std::nullopt, 1), std::invalid_argument);
}

TEST(collaboration_room, complete_multi_line_lock_set_controls_editability) {
	std::unordered_map<std::string, std::string> holders{
		{"9K3MT7Q2CD-1", "member-1"}, {"9K3MT7Q2CD-2", "member-1"}, {"9K3MT7Q2CD-3", "member-2"}
	};
	EXPECT_TRUE(OwnsCompleteLockSet({"9K3MT7Q2CD-1", "9K3MT7Q2CD-2"}, holders, "member-1", false));
	EXPECT_FALSE(OwnsCompleteLockSet({"9K3MT7Q2CD-1", "9K3MT7Q2CD-3"}, holders, "member-1", false));
	EXPECT_FALSE(OwnsCompleteLockSet({"9K3MT7Q2CD-1"}, holders, "member-1", true));
	EXPECT_FALSE(OwnsCompleteLockSet({}, holders, "member-1", false));
}

TEST(collaboration_room, decodes_maintenance_ownership_and_cancel_window) {
	auto state = DecodeMaintenanceState(R"({"active":true,"holder_id":"member-1","holder_name":"translator","started_at":"2026-07-14T00:00:00Z","idle_expires_at":"2026-07-14T00:10:00Z","hard_expires_at":"2026-07-14T01:00:00Z","cancel_requested_by":"member-2","cancel_requested_name":"proofreader","cancel_force_at":"2026-07-14T00:00:30Z"})");
	EXPECT_TRUE(state.active);
	EXPECT_EQ("member-1", *state.holder_id);
	EXPECT_EQ("member-2", *state.cancel_requested_by);
	EXPECT_TRUE(state.cancel_force_at);
}

TEST(collaboration_room, encodes_comment_requests_and_decodes_canonical_change) {
	auto create = EncodeCommentCreate("9K3MT7Q2CD-1", 3, "Please shorten this.", std::string("Short text"));
	EXPECT_NE(std::string::npos, create.find("Short text"));
	auto state = EncodeCommentSetState("comment-1", "accepted");
	EXPECT_NE(std::string::npos, state.find("accepted"));
	auto changed = DecodeCommentChanged(R"({"comment":{"comment_id":"comment-1","line_id":"9K3MT7Q2CD-1","author_id":"member-2","author_name":"reviewer","body":"Please shorten this.","suggested_text":"Short text","base_line_version":3,"state":"accepted","created_at":"2026-07-14T10:00:00Z","resolved_by":"member-1"},"line":{"line_id":"9K3MT7Q2CD-1","pos_key":"KfKfKfKfKfKfKfKf","version":4,"fields":{"comment":false,"layer":0,"start_ms":0,"end_ms":5000,"style":"Default","actor":"","effect":"","margins":[0,0,0],"text":"Short text"}},"actor_id":"member-1"})");
	EXPECT_EQ("comment-1", changed.comment.comment_id);
	EXPECT_EQ("accepted", changed.comment.state);
	ASSERT_TRUE(changed.line);
	EXPECT_EQ("Short text", changed.line->fields.text);
}
