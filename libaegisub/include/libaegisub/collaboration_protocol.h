// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agi { namespace collab {

constexpr int ProtocolVersion = 1;
constexpr std::size_t CompressionThreshold = 32 * 1024;
constexpr std::size_t MaximumEnvelopeSize = 64 * 1024 * 1024;

struct WireEnvelope {
	int protocol_version = ProtocolVersion;
	std::string type;
	std::string request_id;
	std::int64_t room_revision = 0;
	std::string payload_json = "{}";
};

struct WireFrame {
	bool binary = false;
	std::vector<std::uint8_t> data;
};

std::string EncodeEnvelope(WireEnvelope const& envelope);
WireEnvelope DecodeEnvelope(std::string const& encoded);
WireFrame EncodeFrame(WireEnvelope const& envelope, std::size_t compression_threshold = CompressionThreshold);
WireEnvelope DecodeFrame(bool binary, std::vector<std::uint8_t> const& data);
bool IsKnownMessageType(std::string const& type);

} }
