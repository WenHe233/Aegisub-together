// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <libaegisub/collaboration_protocol.h>

#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <zlib.h>

#include <array>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace agi { namespace collab {
namespace {
json::UnknownElement parse_json(std::string const& encoded) {
	std::istringstream stream(encoded);
	json::UnknownElement value;
	json::Reader::Read(value, stream);
	return value;
}

std::string write_json(json::UnknownElement const& value) {
	std::ostringstream stream;
	JsonWriter::Write(value, stream);
	return stream.str();
}

json::UnknownElement const& required(json::Object const& object, char const* name) {
	auto value = object.find(name);
	if (value == object.end()) throw std::invalid_argument(std::string("missing envelope field: ") + name);
	return value->second;
}

std::vector<std::uint8_t> deflate(std::string const& input) {
	if (input.size() > MaximumEnvelopeSize) throw std::length_error("collaboration envelope exceeds 64 MiB");
	uLongf output_size = compressBound(static_cast<uLong>(input.size()));
	std::vector<std::uint8_t> output(output_size + 1);
	output[0] = 0x01;
	auto result = compress2(output.data() + 1, &output_size,
			reinterpret_cast<Bytef const*>(input.data()), static_cast<uLong>(input.size()), Z_DEFAULT_COMPRESSION);
	if (result != Z_OK) throw std::runtime_error("could not compress collaboration envelope");
	output.resize(static_cast<std::size_t>(output_size) + 1);
	return output;
}

std::string inflate(std::vector<std::uint8_t> const& input) {
	if (input.empty() || input[0] != 0x01) throw std::invalid_argument("unknown collaboration binary frame");
	if (input.size() > MaximumEnvelopeSize) throw std::length_error("compressed collaboration frame exceeds 64 MiB");
	z_stream stream{};
	stream.next_in = const_cast<Bytef*>(reinterpret_cast<Bytef const*>(input.data() + 1));
	stream.avail_in = static_cast<uInt>(input.size() - 1);
	if (inflateInit(&stream) != Z_OK) throw std::runtime_error("could not initialize collaboration decompressor");
	struct EndInflate {
		z_stream* stream;
		~EndInflate() { inflateEnd(stream); }
	} cleanup{&stream};

	std::string output;
	std::array<char, 32 * 1024> buffer{};
	for (;;) {
		stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
		stream.avail_out = static_cast<uInt>(buffer.size());
		auto result = ::inflate(&stream, Z_NO_FLUSH);
		auto produced = buffer.size() - stream.avail_out;
		if (produced > MaximumEnvelopeSize - output.size()) throw std::length_error("decompressed collaboration envelope exceeds 64 MiB");
		output.append(buffer.data(), produced);
		if (result == Z_STREAM_END) {
			if (stream.avail_in != 0) throw std::invalid_argument("collaboration frame has trailing compressed data");
			break;
		}
		if (result != Z_OK || produced == 0) throw std::invalid_argument("invalid compressed collaboration frame");
	}
	return output;
}
}

std::string EncodeEnvelope(WireEnvelope const& envelope) {
	if (envelope.protocol_version != ProtocolVersion || !IsKnownMessageType(envelope.type) || envelope.request_id.empty() || envelope.room_revision < 0)
		throw std::invalid_argument("collaboration envelope metadata is invalid");
	auto payload = parse_json(envelope.payload_json);
	json::Object object;
	object["protocol_version"] = static_cast<std::int64_t>(envelope.protocol_version);
	object["type"] = envelope.type;
	object["request_id"] = envelope.request_id;
	object["room_revision"] = envelope.room_revision;
	object["payload"] = std::move(payload);
	json::UnknownElement root(std::move(object));
	auto encoded = write_json(root);
	if (encoded.size() > MaximumEnvelopeSize) throw std::length_error("collaboration envelope exceeds 64 MiB");
	return encoded;
}

WireEnvelope DecodeEnvelope(std::string const& encoded) {
	if (encoded.size() > MaximumEnvelopeSize) throw std::length_error("collaboration envelope exceeds 64 MiB");
	auto root = parse_json(encoded);
	auto const& object = static_cast<json::Object const&>(root);
	WireEnvelope envelope;
	envelope.protocol_version = static_cast<int>(static_cast<json::Integer const&>(required(object, "protocol_version")));
	envelope.type = static_cast<json::String const&>(required(object, "type"));
	envelope.request_id = static_cast<json::String const&>(required(object, "request_id"));
	envelope.room_revision = static_cast<json::Integer const&>(required(object, "room_revision"));
	envelope.payload_json = write_json(required(object, "payload"));
	if (envelope.protocol_version != ProtocolVersion) throw std::invalid_argument("unsupported collaboration protocol version");
	if (!IsKnownMessageType(envelope.type) || envelope.request_id.empty() || envelope.room_revision < 0)
		throw std::invalid_argument("collaboration envelope metadata is invalid");
	return envelope;
}

WireFrame EncodeFrame(WireEnvelope const& envelope, std::size_t compression_threshold) {
	auto encoded = EncodeEnvelope(envelope);
	if (encoded.size() <= compression_threshold)
		return {false, std::vector<std::uint8_t>(encoded.begin(), encoded.end())};
	return {true, deflate(encoded)};
}

WireEnvelope DecodeFrame(bool binary, std::vector<std::uint8_t> const& data) {
	if (data.size() > MaximumEnvelopeSize) throw std::length_error("collaboration frame exceeds 64 MiB");
	std::string encoded = binary ? inflate(data) : std::string(data.begin(), data.end());
	return DecodeEnvelope(encoded);
}

bool IsKnownMessageType(std::string const& type) {
	static constexpr char const* types[] = {
		"access_auth", "access_ok", "create_room", "join_room", "room_joined",
		"submit_batch", "batch_applied", "batch_rejected", "snapshot_request",
		"lock_request", "lock_release", "lock_state", "presence", "heartbeat",
		"maintenance_request", "maintenance_release", "maintenance_state",
		"maintenance_cancel_request", "maintenance_cancel_force", "comment_create",
		"comment_set_state", "comment_changed", "snapshot_state", "audit_request",
		"audit_page", "reindex", "error",
	};
	return std::find(std::begin(types), std::end(types), type) != std::end(types);
}

} }
