// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <main.h>

#include "../../src/collaboration_transport.h"
#include <libaegisub/collaboration_room.h>

#include <cstdlib>
#include <thread>

using namespace agi::collab;
using namespace std::chrono_literals;

TEST(collaboration_transport, reconnect_delay_is_bounded_exponential) {
	EXPECT_EQ(500ms, ReconnectDelay(0, 500ms, 30s));
	EXPECT_EQ(1s, ReconnectDelay(1, 500ms, 30s));
	EXPECT_EQ(16s, ReconnectDelay(5, 500ms, 30s));
	EXPECT_EQ(30s, ReconnectDelay(6, 500ms, 30s));
	EXPECT_EQ(30s, ReconnectDelay(1000, 500ms, 30s));
	EXPECT_THROW(ReconnectDelay(0, 0ms, 30s), std::invalid_argument);
}

TEST(collaboration_transport, formats_structured_failures_without_connection_secrets) {
	TransportEvent::Failure failure;
	failure.stage = "receive";
	failure.operation = "WinHttpWebSocketReceive";
	failure.native_error = 12030;
	failure.native_message = "The connection was terminated abnormally";
	failure.close_status = 1001;
	failure.close_reason = "heartbeat timeout";
	auto formatted = FormatTransportFailure(failure);
	EXPECT_NE(std::string::npos, formatted.find("Stage: receive"));
	EXPECT_NE(std::string::npos, formatted.find("Windows error: 12030"));
	EXPECT_NE(std::string::npos, formatted.find("WebSocket close status: 1001"));
	EXPECT_EQ(std::string::npos, formatted.find("password"));
	EXPECT_EQ(std::string::npos, formatted.find("?token="));
}

TEST(collaboration_transport, credential_targets_do_not_have_delimiter_collisions) {
	auto first = CredentialTarget("wss://example.test/a:b", "c", "room-password");
	auto second = CredentialTarget("wss://example.test/a", "b:c", "room-password");
	EXPECT_NE(first, second);
	EXPECT_EQ(first, CredentialTarget("wss://example.test/a:b", "c", "room-password"));
}

TEST(collaboration_transport, parses_secure_and_insecure_server_urls) {
	auto secure = ParseCollaborationServerUrl("WSS://Example.TEST");
	EXPECT_TRUE(secure.secure);
	EXPECT_EQ("example.test", secure.host);
	EXPECT_EQ(443, secure.port);
	EXPECT_EQ("/v1/ws", secure.path);
	EXPECT_EQ("wss://example.test/v1/ws", secure.canonical_url);

	auto insecure = ParseCollaborationServerUrl("ws://Example.TEST:8080/custom/socket?token=test");
	EXPECT_FALSE(insecure.secure);
	EXPECT_EQ("example.test", insecure.host);
	EXPECT_EQ(8080, insecure.port);
	EXPECT_EQ("/custom/socket?token=test", insecure.path);
	EXPECT_EQ("ws://example.test:8080/custom/socket?token=test", insecure.canonical_url);

	auto query_only = ParseCollaborationServerUrl("ws://example.test/?transport=websocket");
	EXPECT_EQ("/v1/ws?transport=websocket", query_only.path);
}

TEST(collaboration_transport, rejects_invalid_or_unsafe_server_urls) {
	EXPECT_THROW(ParseCollaborationServerUrl("http://example.test"), std::invalid_argument);
	EXPECT_THROW(ParseCollaborationServerUrl("https://example.test"), std::invalid_argument);
	EXPECT_THROW(ParseCollaborationServerUrl("ws://"), std::invalid_argument);
	EXPECT_THROW(ParseCollaborationServerUrl("ws://user:password@example.test"), std::invalid_argument);
	EXPECT_THROW(ParseCollaborationServerUrl("wss://example.test/v1/ws#fragment"), std::invalid_argument);
}

TEST(collaboration_transport, remembers_insecure_server_confirmation_by_canonical_url) {
	std::vector<std::string> confirmed{""};
	EXPECT_FALSE(RequiresInsecureServerConfirmation("wss://example.test", confirmed));
	EXPECT_TRUE(RequiresInsecureServerConfirmation("ws://Example.TEST/", confirmed));
	RememberInsecureServerConfirmation("ws://Example.TEST/", confirmed);
	EXPECT_FALSE(RequiresInsecureServerConfirmation("ws://example.test/v1/ws", confirmed));
	EXPECT_TRUE(RequiresInsecureServerConfirmation("ws://example.test:8080", confirmed));
	EXPECT_EQ(1U, confirmed.size());
	RememberInsecureServerConfirmation("ws://example.test/v1/ws", confirmed);
	EXPECT_EQ(1U, confirmed.size());
}

TEST(collaboration_transport, DISABLED_live_websocket_smoke) {
	auto* url = std::getenv("AEGISUB_COLLAB_SMOKE_URL");
	auto* access_password = std::getenv("AEGISUB_COLLAB_SMOKE_ACCESS_PASSWORD");
	auto* room_password = std::getenv("AEGISUB_COLLAB_SMOKE_ROOM_PASSWORD");
	if (!url || !access_password || !room_password) return;

	auto wait_for = [&](CollaborationTransport& transport, std::string const& type, bool state_event = false) -> std::optional<TransportEvent> {
		auto deadline = std::chrono::steady_clock::now() + 20s;
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto event = transport.PollEvent()) {
				if (event->type == TransportEventType::error) {
					ADD_FAILURE() << event->detail;
					return std::nullopt;
				}
				if (state_event && event->type == TransportEventType::state && event->state == TransportState::connected) return event;
				if (!state_event && event->type == TransportEventType::message && event->message.type == type) return event;
			}
			std::this_thread::sleep_for(10ms);
		}
		ADD_FAILURE() << "timed out waiting for " << type;
		return std::nullopt;
	};
	auto send = [&](CollaborationTransport& transport, std::string type, std::string payload, std::string request_id) {
		WireEnvelope envelope;
		envelope.type = std::move(type);
		envelope.request_id = std::move(request_id);
		envelope.payload_json = std::move(payload);
		return transport.Send(std::move(envelope));
	};

	CollaborationTransport creator;
	creator.Start({url});
	ASSERT_TRUE(wait_for(creator, "connected", true));
	ASSERT_TRUE(send(creator, "access_auth", EncodeAccessAuth(access_password), "smoke-auth"));
	ASSERT_TRUE(wait_for(creator, "access_ok"));
	CreateRoomRequest create;
	create.room_name = "native-smoke-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	create.room_password = room_password;
	create.nickname = "native-smoke";
	create.lock_enabled = false;
	std::uint32_t random = 0x91e10da5U;
	std::string incompressible_style;
	incompressible_style.reserve(48 * 1024);
	for (std::size_t index = 0; index < 48 * 1024; ++index) {
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		incompressible_style.push_back(static_cast<char>('!' + random % 90));
	}
	create.snapshot.styles.emplace_back(std::move(incompressible_style));
	ASSERT_TRUE(send(creator, "create_room", EncodeCreateRoom(create), "smoke-create"));
	ASSERT_TRUE(wait_for(creator, "room_joined"));

	CollaborationTransport joiner;
	joiner.Start({url});
	ASSERT_TRUE(wait_for(joiner, "connected", true));
	ASSERT_TRUE(send(joiner, "access_auth", EncodeAccessAuth(access_password), "smoke-join-auth"));
	ASSERT_TRUE(wait_for(joiner, "access_ok"));
	JoinRoomRequest join{create.room_name, create.room_password, "native-joiner", {}};
	ASSERT_TRUE(send(joiner, "join_room", EncodeJoinRoom(join), "smoke-join"));
	ASSERT_TRUE(wait_for(joiner, "room_joined"));
	for (int index = 0; index < 3; ++index) {
		ASSERT_TRUE(send(creator, "heartbeat", "{}", "smoke-heartbeat-" + std::to_string(index)));
		ASSERT_TRUE(wait_for(creator, "heartbeat"));
		ASSERT_TRUE(send(joiner, "heartbeat", "{}", "smoke-join-heartbeat-" + std::to_string(index)));
		ASSERT_TRUE(wait_for(joiner, "heartbeat"));
	}
	auto leave = [](std::string request_id) {
		WireEnvelope envelope;
		envelope.type = "leave_room";
		envelope.request_id = std::move(request_id);
		envelope.payload_json = "{}";
		return envelope;
	};
	auto stop_started = std::chrono::steady_clock::now();
	joiner.Stop(leave("smoke-join-leave"));
	ASSERT_TRUE(wait_for(creator, "presence"));
	creator.Stop(leave("smoke-create-leave"));
	EXPECT_LT(std::chrono::steady_clock::now() - stop_started, 2s);
}
