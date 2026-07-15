// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <main.h>

#include "../../src/collaboration_transport.h"

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
