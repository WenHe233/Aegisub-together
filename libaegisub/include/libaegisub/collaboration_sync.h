// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <libaegisub/collaboration.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace agi { namespace collab {

struct PendingBatch {
	std::string batch_id;
	std::vector<Operation> operations;
};

struct AppliedOperation {
	Operation operation;
	bool has_line = false;
	Line line;
	std::int64_t styles_version = 0;
	std::int64_t script_info_version = 0;
};

struct AppliedBatch {
	std::string batch_id;
	std::string actor_id;
	std::vector<AppliedOperation> operations;
	std::unordered_map<std::string, std::string> id_remap;
	std::unordered_map<std::string, std::string> positions;
};

struct RejectedBatch {
	std::string batch_id;
	std::string code;
	std::string message;
	std::string line_id;
	int operation_index = -1;
};

enum class SyncApplyStatus {
	applied,
	duplicate,
	revision_gap,
	pending_conflict,
};

struct SyncApplyResult {
	SyncApplyStatus status = SyncApplyStatus::applied;
	bool document_changed = false;
	bool own_batch_confirmed = false;
};

std::string EncodeSubmitBatch(PendingBatch const& batch);
AppliedBatch DecodeAppliedBatch(std::string const& payload_json);
RejectedBatch DecodeRejectedBatch(std::string const& payload_json);
std::string EncodeSnapshotRequest(std::int64_t after_revision);
Snapshot DecodeSnapshotState(std::string const& payload_json, std::int64_t& revision);

class SyncState final {
	DocumentState confirmed;
	DocumentState projected;
	std::vector<PendingBatch> pending;
	std::int64_t revision = 0;
	bool initialized = false;

	bool rebuild_projected();

public:
	void Initialize(Snapshot snapshot, std::int64_t room_revision);
	bool ResetConfirmed(Snapshot snapshot, std::int64_t room_revision, bool preserve_pending);
	bool IsInitialized() const { return initialized; }
	std::int64_t Revision() const { return revision; }
	DocumentState const& Confirmed() const { return confirmed; }
	DocumentState const& Projected() const { return projected; }
	std::vector<PendingBatch> const& Pending() const { return pending; }

	PendingBatch QueueLocalSnapshot(std::string batch_id, Snapshot const& local_snapshot);
	SyncApplyResult ApplyBatch(AppliedBatch const& batch, std::int64_t room_revision, std::string const& own_member_id);
	std::vector<PendingBatch> RejectBatch(RejectedBatch const& rejection);
};

} }
