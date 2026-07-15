// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <main.h>

#include <libaegisub/cajun/reader.h>
#include <libaegisub/collaboration_protocol.h>

#include <sstream>

using namespace agi::collab;

TEST(collaboration_protocol, encodes_and_decodes_text_envelopes) {
	WireEnvelope input{1, "heartbeat", "request-1", 42, R"({"future_field":true})"};
	auto frame = EncodeFrame(input);
	EXPECT_FALSE(frame.binary);
	auto output = DecodeFrame(frame.binary, frame.data);
	EXPECT_EQ(input.protocol_version, output.protocol_version);
	EXPECT_EQ(input.type, output.type);
	EXPECT_EQ(input.request_id, output.request_id);
	EXPECT_EQ(input.room_revision, output.room_revision);
	std::istringstream payload_stream(output.payload_json);
	json::UnknownElement payload;
	EXPECT_NO_THROW(json::Reader::Read(payload, payload_stream));
	EXPECT_TRUE(static_cast<json::Boolean>(static_cast<json::Object const&>(payload).at("future_field")));
}

TEST(collaboration_protocol, compresses_large_envelopes_and_round_trips_utf8) {
	auto text = std::string(40 * 1024, 'x') + "你好";
	WireEnvelope input{1, "snapshot_state", "request-2", 9, "{\"text\":\"" + text + "\"}"};
	auto frame = EncodeFrame(input);
	EXPECT_TRUE(frame.binary);
	ASSERT_FALSE(frame.data.empty());
	EXPECT_EQ(0x01, frame.data[0]);
	auto output = DecodeFrame(true, frame.data);
	EXPECT_EQ("snapshot_state", output.type);
	EXPECT_NE(std::string::npos, output.payload_json.find("你好"));
}

TEST(collaboration_protocol, decodes_json_unicode_escapes_from_go) {
	auto prefix = std::string(R"({"protocol_version":1,"type":"heartbeat","request_id":"unicode","room_revision":0,"payload":{"text":")");
	auto envelope = DecodeEnvelope(prefix + "\\u003c\\u4f60\\u597d \\ud83d\\ude00" + R"("}})");
	EXPECT_NE(std::string::npos, envelope.payload_json.find("<你好 😀"));
	EXPECT_THROW(DecodeEnvelope(prefix + "\\ud83dX" + R"("}})"),
		json::Reader::ScanException);
}

TEST(collaboration_protocol, rejects_incompatible_or_malformed_frames) {
	EXPECT_THROW(DecodeEnvelope(R"({"protocol_version":2,"type":"heartbeat","request_id":"r","room_revision":0,"payload":{}})"), std::invalid_argument);
	EXPECT_THROW(DecodeEnvelope(R"({"protocol_version":1,"type":"future_required_message","request_id":"r","room_revision":0,"payload":{}})"), std::invalid_argument);
	EXPECT_THROW(DecodeFrame(true, {0x02, 0x00}), std::invalid_argument);
	EXPECT_THROW(DecodeEnvelope(R"({"protocol_version":1,"type":"heartbeat","request_id":"","room_revision":0,"payload":{}})"), std::invalid_argument);
	WireEnvelope compressed{1, "heartbeat", "r", 0, "{}"};
	auto frame = EncodeFrame(compressed, 0);
	frame.data.push_back(0);
	EXPECT_THROW(DecodeFrame(true, frame.data), std::invalid_argument);
}
