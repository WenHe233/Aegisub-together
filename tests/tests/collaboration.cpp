// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <main.h>

#include <libaegisub/collaboration.h>

#include <algorithm>

using namespace agi::collab;

namespace {
Line make_line(std::string id, std::string text) {
	Line line;
	line.id = std::move(id);
	line.fields.text = std::move(text);
	return line;
}

std::vector<std::string> ids(Snapshot const& snapshot) {
	std::vector<std::string> result;
	for (auto const& line : snapshot.lines) result.push_back(line.id);
	return result;
}
}

TEST(collaboration_id, allocator_uses_50_bit_crockford_prefix) {
	EXPECT_EQ("0000000000", EncodeClientPrefix(0));
	EXPECT_EQ("ZZZZZZZZZZ", EncodeClientPrefix((std::uint64_t{1} << 50) - 1));
	EXPECT_THROW(EncodeClientPrefix(std::uint64_t{1} << 50), std::out_of_range);
	EXPECT_TRUE(IsValidLineId("9K3MT7Q2CD-1"));
	EXPECT_FALSE(IsValidLineId("9K3MT7Q2CD-01"));
	EXPECT_FALSE(IsValidLineId("9K3MT7Q2CI-1"));
	IdAllocator allocator("9K3MT7Q2CD", 128);
	EXPECT_EQ("9K3MT7Q2CD-128", allocator.Mint());
	EXPECT_EQ("9K3MT7Q2CD-129", allocator.Mint());
	EXPECT_EQ(130u, allocator.NextCounter());
}

TEST(collaboration_metadata, sanitizes_missing_duplicate_resurrected_and_foreign_ids) {
	IdAllocator allocator("9K3MT7Q2CD", 100);
	SanitizeContext context;
	context.known_live_ids.insert("ABCDEFGHJK-1");
	context.tombstoned_ids.insert("ZZZZZZZZZZ-9");
	std::vector<MetadataLine> lines{
		{"ABCDEFGHJK-1", "bad"},
		{"ABCDEFGHJK-1", "bad"},
		{"", ""},
		{"ZZZZZZZZZZ-9", ""},
		{"0000000000-7", ""},
		{"9K3MT7Q2CD-50", ""},
	};
	auto result = SanitizeMetadata(lines, allocator, context);
	EXPECT_EQ((std::vector<std::size_t>{1, 2, 4, 5}), result.reminted_lines);
	EXPECT_EQ((std::vector<std::string>{"ZZZZZZZZZZ-9"}), result.resurrected_ids);
	EXPECT_EQ("ABCDEFGHJK-1", lines[0].id);
	EXPECT_EQ("9K3MT7Q2CD-100", lines[1].id);
	EXPECT_EQ("9K3MT7Q2CD-101", lines[2].id);
	EXPECT_EQ("ZZZZZZZZZZ-9", lines[3].id);
	EXPECT_EQ("9K3MT7Q2CD-102", lines[4].id);
	EXPECT_EQ("9K3MT7Q2CD-103", lines[5].id);
	EXPECT_TRUE(result.positions_reindexed);
	for (std::size_t index = 0; index < lines.size(); ++index) {
		EXPECT_TRUE(IsValidPosition(lines[index].position));
		if (index) EXPECT_LT(lines[index - 1].position, lines[index].position);
	}
}

TEST(collaboration_metadata, initial_import_can_keep_unknown_foreign_ids) {
	IdAllocator allocator("9K3MT7Q2CD", 1);
	SanitizeContext context;
	context.accept_unknown_ids = true;
	std::vector<MetadataLine> lines{{"ABCDEFGHJK-22", "Uzzzzzzzzzzzzzzz"}};
	auto result = SanitizeMetadata(lines, allocator, context);
	EXPECT_TRUE(result.reminted_lines.empty());
	EXPECT_FALSE(result.positions_reindexed);
}

TEST(collaboration_order, canonical_single_line_rank_matches_server) {
	std::vector<Line> lines{make_line("9K3MT7Q2CD-1", "one")};
	ReindexPositions(lines);
	EXPECT_EQ("Uzzzzzzzzzzzzzzz", lines[0].position);
}

TEST(collaboration_diff, operations_reproduce_order_fields_and_sections) {
	DocumentState confirmed;
	confirmed.snapshot.lines = {
		make_line("9K3MT7Q2CD-1", "a"),
		make_line("9K3MT7Q2CD-2", "b"),
		make_line("9K3MT7Q2CD-3", "c"),
	};
	ReindexPositions(confirmed.snapshot.lines);
	confirmed.snapshot.styles = {"Style: Default,Arial,48"};
	confirmed.snapshot.script_info = {{"ScriptType", "v4.00+"}};
	Snapshot projected = confirmed.snapshot;
	projected.lines = {
		confirmed.snapshot.lines[2],
		confirmed.snapshot.lines[0],
		make_line("9K3MT7Q2CD-4", "d"),
	};
	projected.lines[0].fields.text = "changed c";
	projected.styles = {"Style: Default,Arial,50"};
	projected.script_info.push_back({"PlayResX", "1920"});

	auto operations = DiffSnapshots(confirmed, projected);
	ASSERT_FALSE(operations.empty());
	DocumentState applied = confirmed;
	std::string error;
	ASSERT_TRUE(ApplyOperations(applied, operations, &error)) << error;
	EXPECT_EQ((std::vector<std::string>{"9K3MT7Q2CD-3", "9K3MT7Q2CD-1", "9K3MT7Q2CD-4"}), ids(applied.snapshot));
	EXPECT_EQ("changed c", applied.snapshot.lines[0].fields.text);
	EXPECT_EQ(projected.styles, applied.snapshot.styles);
	EXPECT_EQ(projected.script_info, applied.snapshot.script_info);
	EXPECT_EQ(2, applied.snapshot.styles_version);
	EXPECT_EQ(2, applied.snapshot.script_info_version);
	EXPECT_TRUE(applied.tombstones.count("9K3MT7Q2CD-2"));
}

TEST(collaboration_diff, reproduces_every_six_line_permutation) {
	DocumentState initial;
	for (int index = 1; index <= 6; ++index)
		initial.snapshot.lines.push_back(make_line("9K3MT7Q2CD-" + std::to_string(index), std::to_string(index)));
	ReindexPositions(initial.snapshot.lines);
	initial.snapshot.styles = {"Style: Default,Arial,48"};
	std::vector<int> permutation{0, 1, 2, 3, 4, 5};
	do {
		Snapshot projected = initial.snapshot;
		for (std::size_t index = 0; index < permutation.size(); ++index)
			projected.lines[index] = initial.snapshot.lines[permutation[index]];
		auto applied = initial;
		auto operations = DiffSnapshots(initial, projected);
		ASSERT_TRUE(ApplyOperations(applied, operations));
		EXPECT_EQ(ids(projected), ids(applied.snapshot));
	} while (std::next_permutation(permutation.begin(), permutation.end()));
}

TEST(collaboration_diff, resurrects_known_tombstone) {
	DocumentState confirmed;
	confirmed.snapshot.styles = {"Style: Default,Arial,48"};
	confirmed.snapshot.lines = {make_line("9K3MT7Q2CD-1", "live")};
	ReindexPositions(confirmed.snapshot.lines);
	Line tombstone = make_line("9K3MT7Q2CD-2", "restored");
	tombstone.version = 4;
	std::vector<Line> all{confirmed.snapshot.lines[0], tombstone};
	ReindexPositions(all);
	confirmed.snapshot.lines[0].position = all[0].position;
	tombstone.position = all[1].position;
	confirmed.tombstones[tombstone.id] = tombstone;
	Snapshot projected = confirmed.snapshot;
	projected.lines.push_back(tombstone);

	auto operations = DiffSnapshots(confirmed, projected);
	ASSERT_EQ(1u, operations.size());
	EXPECT_EQ(OperationKind::Restore, operations[0].kind);
	ASSERT_TRUE(ApplyOperations(confirmed, operations));
	EXPECT_EQ(5, confirmed.snapshot.lines[1].version);
	EXPECT_FALSE(confirmed.tombstones.count(tombstone.id));
}

TEST(collaboration_apply, rejects_entire_operation_batch_atomically) {
	DocumentState state;
	state.snapshot.lines = {make_line("9K3MT7Q2CD-1", "before")};
	ReindexPositions(state.snapshot.lines);
	Operation modify;
	modify.kind = OperationKind::Modify;
	modify.line_id = "9K3MT7Q2CD-1";
	modify.base_version = 1;
	modify.fields.text = "after";
	Operation stale_delete;
	stale_delete.kind = OperationKind::Delete;
	stale_delete.line_id = "9K3MT7Q2CD-1";
	stale_delete.base_version = 1;
	std::string error;
	EXPECT_FALSE(ApplyOperations(state, {modify, stale_delete}, &error));
	EXPECT_EQ("before", state.snapshot.lines[0].fields.text);
	EXPECT_EQ(1, state.snapshot.lines[0].version);
}

TEST(collaboration_order, ten_thousand_dense_insertions_remain_strictly_ordered) {
	DocumentState state;
	state.snapshot.lines = {make_line("9K3MT7Q2CD-1", "left"), make_line("9K3MT7Q2CD-2", "right")};
	ReindexPositions(state.snapshot.lines);
	IdAllocator allocator("9K3MT7Q2CD", 3);
	for (int index = 0; index < 10000; ++index) {
		Operation insert;
		insert.kind = OperationKind::Insert;
		insert.line_id = allocator.Mint();
		insert.left_id = "9K3MT7Q2CD-1";
		insert.right_id = "9K3MT7Q2CD-2";
		insert.data.text = std::to_string(index);
		ASSERT_TRUE(ApplyOperation(state, insert));
	}
	for (std::size_t index = 1; index < state.snapshot.lines.size(); ++index) {
		EXPECT_LT(state.snapshot.lines[index - 1].position, state.snapshot.lines[index].position);
		EXPECT_TRUE(IsValidPosition(state.snapshot.lines[index].position));
	}
}
