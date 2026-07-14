// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <memory>
#include <string>

class AssDialogue;

namespace agi {
struct Context;
namespace collab {

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
	/// 0 = none, 1 = owned by this client, 2 = held/present remotely.
	int LineLockState(AssDialogue const* line) const;
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
