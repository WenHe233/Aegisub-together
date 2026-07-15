// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <main.h>

#include "../../src/collaboration_transport.h"

using namespace agi::collab;

TEST(collaboration_connection_policy, does_not_retry_incomplete_access_or_join_handshakes) {
	auto access = EvaluateConnectionLoss(false, false);
	EXPECT_FALSE(access.retry);
	EXPECT_FALSE(access.enable_offline_journal);
	EXPECT_FALSE(access.create_may_have_completed);
}

TEST(collaboration_connection_policy, warns_that_a_submitted_create_may_have_completed) {
	auto create = EvaluateConnectionLoss(false, true);
	EXPECT_FALSE(create.retry);
	EXPECT_FALSE(create.enable_offline_journal);
	EXPECT_TRUE(create.create_may_have_completed);
}

TEST(collaboration_connection_policy, reconnects_only_after_room_joined) {
	auto joined = EvaluateConnectionLoss(true, false);
	EXPECT_TRUE(joined.retry);
	EXPECT_TRUE(joined.enable_offline_journal);
	EXPECT_FALSE(joined.create_may_have_completed);
}

TEST(collaboration_connection_policy, submitted_create_is_irrelevant_after_room_joined) {
	auto joined = EvaluateConnectionLoss(true, true);
	EXPECT_TRUE(joined.retry);
	EXPECT_TRUE(joined.enable_offline_journal);
	EXPECT_FALSE(joined.create_may_have_completed);
}
