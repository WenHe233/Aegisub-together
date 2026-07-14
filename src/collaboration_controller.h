// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <memory>

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
};

}
}
