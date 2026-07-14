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

TEST(collaboration_transport, credential_targets_do_not_have_delimiter_collisions) {
	auto first = CredentialTarget("wss://example.test/a:b", "c", "room-password");
	auto second = CredentialTarget("wss://example.test/a", "b:c", "room-password");
	EXPECT_NE(first, second);
	EXPECT_EQ(first, CredentialTarget("wss://example.test/a:b", "c", "room-password"));
}
