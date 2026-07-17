// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <memory>
#include <string>

class AssDialogue;
struct SearchReplaceSettings;

namespace agi {
struct Context;
namespace collab {

enum class LineCollaborationKind {
	none,
	owned_lock,
	remote_lock,
	remote_presence
};

struct LineCollaborationState {
	LineCollaborationKind kind = LineCollaborationKind::none;
	std::string holder_name;
};

class CollaborationController final {
	class Impl;
	std::unique_ptr<Impl> impl;
public:
	explicit CollaborationController(Context* context);
	~CollaborationController();
	CollaborationController(CollaborationController const&) = delete;
	CollaborationController& operator=(CollaborationController const&) = delete;

	void ShowCreateRoomDialog();
	void ShowJoinRoomDialog();
	void Disconnect();
	bool IsRunning() const;
	bool IsJoined() const;
	void BeginMutationGuard();
	void EndMutationGuard();
	bool CanRunCommand(std::string const& command_name) const;
	bool CanModifySelectedRows() const;
	void BeginSelectionGesture();
	void EndSelectionGesture();
	bool RequestGlobalReplace(SearchReplaceSettings const& settings);
	LineCollaborationState LineState(AssDialogue const* line) const;
	void RequestMaintenance();
	void ReleaseMaintenance();
	void RequestMaintenanceCancel();
	void ForceMaintenanceCancel();
	bool MaintenanceActive() const;
	bool MaintenanceOwned() const;
	bool CanCollaborativeUndo() const;
	bool CanCollaborativeRedo() const;
	void CollaborativeUndo();
	void CollaborativeRedo();
	int LineCommentCount(AssDialogue const* line) const;
	void ShowLineComments();
};

}
}
