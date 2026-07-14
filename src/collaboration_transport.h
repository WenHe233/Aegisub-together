// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <libaegisub/collaboration_protocol.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agi { namespace collab {

enum class TransportState {
	stopped,
	connecting,
	connected,
	retry_wait,
};

enum class TransportEventType {
	state,
	message,
	error,
};

struct TransportEvent {
	TransportEventType type = TransportEventType::state;
	TransportState state = TransportState::stopped;
	WireEnvelope message;
	std::string detail;
};

struct TransportConfig {
	std::string server_url;
	std::chrono::milliseconds reconnect_initial{500};
	std::chrono::milliseconds reconnect_max{30000};
};

struct CollaborationServerUrl {
	bool secure = true;
	std::string canonical_url;
	std::string host;
	std::string path;
	std::uint16_t port = 0;
};

class CollaborationTransport final {
	class Impl;
	std::unique_ptr<Impl> impl;

public:
	CollaborationTransport();
	~CollaborationTransport();
	CollaborationTransport(CollaborationTransport const&) = delete;
	CollaborationTransport& operator=(CollaborationTransport const&) = delete;

	void Start(TransportConfig config);
	void Stop();
	bool Send(WireEnvelope envelope);
	std::optional<TransportEvent> PollEvent();
};

std::chrono::milliseconds ReconnectDelay(unsigned attempt, std::chrono::milliseconds initial, std::chrono::milliseconds maximum);

CollaborationServerUrl ParseCollaborationServerUrl(std::string const& server_url);
bool RequiresInsecureServerConfirmation(std::string const& server_url, std::vector<std::string> const& confirmed_servers);
void RememberInsecureServerConfirmation(std::string const& server_url, std::vector<std::string>& confirmed_servers);

std::string CredentialTarget(std::string const& server_url, std::string const& room_name, std::string const& secret_name);
void StoreCredential(std::string const& target, std::string const& secret);
std::optional<std::string> ReadCredential(std::string const& target);
void DeleteCredential(std::string const& target);

} }
