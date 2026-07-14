// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <main.h>

#include <libaegisub/cajun/reader.h>
#include <libaegisub/collaboration_sync.h>

#include <sstream>

using namespace agi::collab;

namespace {
Snapshot baseline() {
	Snapshot snapshot;
	Line first;
	first.id = "9K3MT7Q2CD-1";
	first.position = "KfKfKfKfKfKfKfKf";
	first.fields.text = "one";
	Line second;
	second.id = "9K3MT7Q2CD-2";
	second.position = "fKfKfKfKfKfKfKfK";
	second.fields.text = "two";
	snapshot.lines = {first, second};
	snapshot.styles = {"Style: Default,Arial,48"};
	snapshot.script_info = {{"ScriptType", "v4.00+"}};
	return snapshot;
}

AppliedBatch canonical_modify(std::string batch_id, std::string actor, std::string text, std::int64_t base_version = 1) {
	AppliedBatch batch;
	batch.batch_id = std::move(batch_id);
	batch.actor_id = std::move(actor);
	AppliedOperation applied;
	applied.operation.kind = OperationKind::Modify;
	applied.operation.line_id = "9K3MT7Q2CD-1";
	applied.operation.base_version = base_version;
	applied.operation.fields.text = text;
	applied.has_line = true;
	applied.line = baseline().lines[0];
	applied.line.version = base_version + 1;
	applied.line.fields.text = std::move(text);
	batch.operations.push_back(std::move(applied));
	return batch;
}
}

TEST(collaboration_sync, local_batches_advance_projected_only_until_confirmed) {
	SyncState state;
	state.Initialize(baseline(), 4);
	auto local = baseline();
	local.lines[0].fields.text = "local";
	auto pending = state.QueueLocalSnapshot("local-b1", local);
	ASSERT_EQ(1u, pending.operations.size());
	EXPECT_EQ("one", state.Confirmed().snapshot.lines[0].fields.text);
	EXPECT_EQ("local", state.Projected().snapshot.lines[0].fields.text);

	auto result = state.ApplyBatch(canonical_modify("local-b1", "self", "local"), 5, "self");
	EXPECT_EQ(SyncApplyStatus::applied, result.status);
	EXPECT_TRUE(result.own_batch_confirmed);
	EXPECT_TRUE(state.Pending().empty());
	EXPECT_EQ("local", state.Confirmed().snapshot.lines[0].fields.text);
}

TEST(collaboration_sync, remote_batch_is_replayed_below_unrelated_pending_change) {
	SyncState state;
	state.Initialize(baseline(), 0);
	auto local = baseline();
	local.lines[1].fields.text = "local two";
	state.QueueLocalSnapshot("local-b1", local);

	auto remote = canonical_modify("remote-b1", "other", "remote one");
	auto result = state.ApplyBatch(remote, 1, "self");
	EXPECT_EQ(SyncApplyStatus::applied, result.status);
	EXPECT_EQ("remote one", state.Projected().snapshot.lines[0].fields.text);
	EXPECT_EQ("local two", state.Projected().snapshot.lines[1].fields.text);
	EXPECT_EQ("two", state.Confirmed().snapshot.lines[1].fields.text);
}

TEST(collaboration_sync, duplicate_and_revision_gap_messages_are_safe) {
	SyncState state;
	state.Initialize(baseline(), 8);
	auto duplicate = canonical_modify("old", "other", "ignored");
	EXPECT_EQ(SyncApplyStatus::duplicate, state.ApplyBatch(duplicate, 8, "self").status);
	EXPECT_EQ("one", state.Confirmed().snapshot.lines[0].fields.text);
	auto gap = canonical_modify("future", "other", "future");
	EXPECT_EQ(SyncApplyStatus::revision_gap, state.ApplyBatch(gap, 10, "self").status);
	EXPECT_EQ(8, state.Revision());
}

TEST(collaboration_sync, own_id_remap_updates_dependent_pending_batches) {
	SyncState state;
	state.Initialize(baseline(), 0);
	auto with_insert = baseline();
	Line inserted;
	inserted.id = "9K3MT7Q2CD-3";
	inserted.position = "V000000000000000";
	inserted.fields.text = "inserted";
	with_insert.lines.insert(with_insert.lines.begin() + 1, inserted);
	state.QueueLocalSnapshot("insert", with_insert);
	auto changed_insert = with_insert;
	changed_insert.lines[1].fields.text = "changed";
	state.QueueLocalSnapshot("modify", changed_insert);

	AppliedBatch accepted;
	accepted.batch_id = "insert";
	accepted.actor_id = "self";
	accepted.id_remap["9K3MT7Q2CD-3"] = "0000000000-1";
	AppliedOperation operation;
	operation.operation.kind = OperationKind::Insert;
	operation.operation.line_id = "0000000000-1";
	operation.operation.left_id = "9K3MT7Q2CD-1";
	operation.operation.right_id = "9K3MT7Q2CD-2";
	operation.operation.data = inserted.fields;
	operation.has_line = true;
	operation.line = inserted;
	operation.line.id = "0000000000-1";
	accepted.operations.push_back(operation);
	auto result = state.ApplyBatch(accepted, 1, "self");
	EXPECT_EQ(SyncApplyStatus::applied, result.status);
	ASSERT_EQ(1u, state.Pending().size());
	EXPECT_EQ("0000000000-1", state.Pending()[0].operations[0].line_id);
	EXPECT_EQ("changed", state.Projected().snapshot.lines[1].fields.text);
}

TEST(collaboration_sync, rejection_removes_batch_and_every_dependent_batch) {
	SyncState state;
	state.Initialize(baseline(), 0);
	auto first = baseline();
	first.lines[0].fields.text = "first";
	state.QueueLocalSnapshot("one", first);
	auto second = first;
	second.lines[1].fields.text = "second";
	state.QueueLocalSnapshot("two", second);
	auto removed = state.RejectBatch({"one", "line_version_conflict", "stale", "9K3MT7Q2CD-1", 0});
	EXPECT_EQ(2u, removed.size());
	EXPECT_TRUE(state.Pending().empty());
	EXPECT_EQ("one", state.Projected().snapshot.lines[0].fields.text);
}

TEST(collaboration_sync, protocol_codec_matches_batch_shapes) {
	SyncState state;
	state.Initialize(baseline(), 0);
	auto local = baseline();
	local.lines[0].fields.text = "hello";
	auto batch = state.QueueLocalSnapshot("batch-1", local);
	auto encoded = EncodeSubmitBatch(batch);
	std::istringstream stream(encoded);
	json::UnknownElement root;
	json::Reader::Read(root, stream);
	EXPECT_EQ("batch-1", static_cast<json::String const&>(static_cast<json::Object const&>(root).at("batch_id")));

	auto applied = DecodeAppliedBatch(R"({"batch_id":"batch-1","actor_id":"self","operations":[{"operation":{"op":"modify","line_id":"9K3MT7Q2CD-1","base_version":1,"fields":{"text":"hello"}},"line":{"line_id":"9K3MT7Q2CD-1","pos_key":"KfKfKfKfKfKfKfKf","version":2,"fields":{"comment":false,"layer":0,"start_ms":0,"end_ms":5000,"style":"Default","actor":"","effect":"","margins":[0,0,0],"text":"hello"}}}],"id_remap":{}})");
	EXPECT_EQ("batch-1", applied.batch_id);
	EXPECT_EQ("hello", applied.operations[0].line.fields.text);
}

TEST(collaboration_sync, offline_journal_round_trips_baseline_local_and_pending_batches) {
	SyncState state;
	state.Initialize(baseline(), 7);
	auto local = baseline();
	local.lines[0].fields.text = "offline";
	auto batch = state.QueueLocalSnapshot("offline-1", local);
	OfflineJournal journal{7, baseline(), local, {batch}};
	auto decoded = DecodeOfflineJournal(EncodeOfflineJournal(journal));
	EXPECT_EQ(7, decoded.base_revision);
	EXPECT_EQ("one", decoded.baseline.lines[0].fields.text);
	EXPECT_EQ("offline", decoded.local.lines[0].fields.text);
	ASSERT_EQ(1u, decoded.pending.size());
	EXPECT_EQ("offline-1", decoded.pending[0].batch_id);
	ASSERT_EQ(1u, decoded.pending[0].operations.size());
	EXPECT_EQ(OperationKind::Modify, decoded.pending[0].operations[0].kind);
}

TEST(collaboration_sync, offline_baseline_marks_server_deletions_as_restorable) {
	SyncState state;
	auto server = baseline();
	server.lines.erase(server.lines.begin());
	ReindexPositions(server.lines);
	state.Initialize(server, 8);
	state.RememberTombstonesFrom(baseline());
	auto target = server;
	target.lines.insert(target.lines.begin(), baseline().lines[0]);
	ReindexPositions(target.lines);
	auto batch = state.QueueLocalSnapshot("restore-offline", target);
	ASSERT_FALSE(batch.operations.empty());
	EXPECT_EQ(OperationKind::Restore, batch.operations[0].kind);
}
