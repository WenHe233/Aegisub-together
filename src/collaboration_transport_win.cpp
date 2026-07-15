// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include "collaboration_transport.h"

#include <windows.h>
#include <wincred.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace agi { namespace collab {
namespace {
constexpr std::size_t MaximumQueuedMessages = 1024;
constexpr std::size_t MaximumQueuedEvents = 4096;
constexpr wchar_t UserAgent[] = L"Aegisub-Together/1";

std::wstring widen(std::string const& value) {
	if (value.empty()) return {};
	auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (!size) throw std::invalid_argument("invalid UTF-8 string");
	std::wstring output(static_cast<std::size_t>(size), L'\0');
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size))
		throw std::runtime_error("could not convert string to UTF-16");
	return output;
}

std::string narrow(std::wstring const& value) {
	if (value.empty()) return {};
	auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (!size) throw std::invalid_argument("invalid UTF-16 string");
	std::string output(static_cast<std::size_t>(size), '\0');
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr))
		throw std::runtime_error("could not convert string to UTF-8");
	return output;
}

std::string windows_error_message(DWORD code) {
	wchar_t* buffer = nullptr;
	auto size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
	if (!size || !buffer) return "unknown Windows error";
	std::wstring message(buffer, size);
	LocalFree(buffer);
	while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ' || message.back() == L'.'))
		message.pop_back();
	try { return narrow(message); }
	catch (...) { return "Windows error text is not valid UTF-8"; }
}

class TransportException final : public std::runtime_error {
public:
	TransportEvent::Failure failure;

	TransportException(std::string stage, std::string operation, DWORD code)
	: std::runtime_error(operation + " failed") {
		failure.stage = std::move(stage);
		failure.operation = std::move(operation);
		failure.native_error = code;
		failure.native_message = windows_error_message(code);
	}
};

TransportException winhttp_error(char const* operation, DWORD code = GetLastError(), char const* stage = "transport") {
	return TransportException(stage, operation, code);
}

class InternetHandle {
	HINTERNET value = nullptr;
public:
	InternetHandle() = default;
	explicit InternetHandle(HINTERNET value) : value(value) { }
	~InternetHandle() { if (value) WinHttpCloseHandle(value); }
	InternetHandle(InternetHandle const&) = delete;
	InternetHandle& operator=(InternetHandle const&) = delete;
	InternetHandle(InternetHandle&& other) noexcept : value(other.value) { other.value = nullptr; }
	InternetHandle& operator=(InternetHandle&& other) noexcept {
		if (this != &other) {
			if (value) WinHttpCloseHandle(value);
			value = other.value;
			other.value = nullptr;
		}
		return *this;
	}
	HINTERNET get() const { return value; }
};

struct WebSocketConnection {
	InternetHandle session;
	InternetHandle connection;
	InternetHandle socket;
};

WebSocketConnection connect_websocket(std::string const& server_url) {
	auto parsed = ParseCollaborationServerUrl(server_url);
	auto host = widen(parsed.host);
	auto path = widen(parsed.path);

	WebSocketConnection output;
	output.session = InternetHandle(WinHttpOpen(UserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!output.session.get()) throw winhttp_error("WinHttpOpen", GetLastError(), "session");
	if (!WinHttpSetTimeouts(output.session.get(), 5000, 10000, 10000, 30000))
		throw winhttp_error("WinHttpSetTimeouts", GetLastError(), "session");
	output.connection = InternetHandle(WinHttpConnect(output.session.get(), host.c_str(), parsed.port, 0));
	if (!output.connection.get()) throw winhttp_error("WinHttpConnect", GetLastError(), "connect");
	InternetHandle request(WinHttpOpenRequest(output.connection.get(), L"GET", path.c_str(), nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed.secure ? WINHTTP_FLAG_SECURE : 0));
	if (!request.get()) throw winhttp_error("WinHttpOpenRequest", GetLastError(), "upgrade");
	if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
		throw winhttp_error("WinHttpSetOption(WEB_SOCKET)", GetLastError(), "upgrade");
	if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
		throw winhttp_error("WinHttpSendRequest", GetLastError(), "upgrade");
	if (!WinHttpReceiveResponse(request.get(), nullptr)) throw winhttp_error("WinHttpReceiveResponse", GetLastError(), "upgrade");
	DWORD status = 0;
	DWORD status_size = sizeof(status);
	if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
		throw winhttp_error("WinHttpQueryHeaders", GetLastError(), "upgrade");
	if (status != 101) throw std::runtime_error("collaboration WebSocket upgrade was rejected with HTTP status " + std::to_string(status));
	output.socket = InternetHandle(WinHttpWebSocketCompleteUpgrade(request.get(), 0));
	if (!output.socket.get()) throw winhttp_error("WinHttpWebSocketCompleteUpgrade", GetLastError(), "upgrade");
	return output;
}

void close_websocket(HINTERNET socket) {
	if (socket) WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
}

TransportEvent::Failure failure_from_exception(std::exception const& error, std::string stage, std::string operation) {
	if (auto const* transport = dynamic_cast<TransportException const*>(&error)) return transport->failure;
	TransportEvent::Failure failure;
	failure.stage = std::move(stage);
	failure.operation = std::move(operation);
	failure.native_message = error.what();
	return failure;
}

std::wstring credential_name(std::string const& target) {
	if (target.empty()) throw std::invalid_argument("credential target is empty");
	auto name = widen(target);
	if (name.size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH) throw std::length_error("credential target is too large");
	return name;
}
}

std::string FormatTransportFailure(TransportEvent::Failure const& failure) {
	std::ostringstream output;
	output << "Collaboration transport failure";
	if (!failure.stage.empty()) output << "\nStage: " << failure.stage;
	if (!failure.operation.empty()) output << "\nOperation: " << failure.operation;
	if (failure.native_error) {
		output << "\nWindows error: " << failure.native_error;
		if (!failure.native_message.empty()) output << " (" << failure.native_message << ")";
	}
	else if (!failure.native_message.empty()) output << "\nMessage: " << failure.native_message;
	if (failure.close_status) output << "\nWebSocket close status: " << failure.close_status;
	if (!failure.close_reason.empty()) output << "\nWebSocket close reason: " << failure.close_reason;
	return output.str();
}

CollaborationServerUrl ParseCollaborationServerUrl(std::string const& server_url) {
	auto delimiter = server_url.find("://");
	if (delimiter == std::string::npos || server_url.find('#') != std::string::npos)
		throw std::invalid_argument("collaboration server URL must use ws:// or wss:// without a fragment");
	auto scheme = server_url.substr(0, delimiter);
	std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	bool secure;
	if (scheme == "wss") secure = true;
	else if (scheme == "ws") secure = false;
	else throw std::invalid_argument("collaboration server URL must use ws:// or wss://");
	if (delimiter + 3 == server_url.size()) throw std::invalid_argument("collaboration server URL has no host");

	auto normalized = widen(std::string(secure ? "https://" : "http://") + server_url.substr(delimiter + 3));
	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	components.dwHostNameLength = static_cast<DWORD>(-1);
	components.dwUrlPathLength = static_cast<DWORD>(-1);
	components.dwExtraInfoLength = static_cast<DWORD>(-1);
	components.dwUserNameLength = static_cast<DWORD>(-1);
	components.dwPasswordLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(normalized.c_str(), static_cast<DWORD>(normalized.size()), 0, &components))
		throw winhttp_error("WinHttpCrackUrl");
	auto expected_scheme = secure ? INTERNET_SCHEME_HTTPS : INTERNET_SCHEME_HTTP;
	if (components.nScheme != expected_scheme || !components.lpszHostName || components.dwHostNameLength == 0 ||
		components.dwUserNameLength || components.dwPasswordLength)
		throw std::invalid_argument("collaboration server URL is invalid");
	std::wstring host(components.lpszHostName, components.dwHostNameLength);
	std::transform(host.begin(), host.end(), host.begin(), [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
	std::wstring path;
	if (components.lpszUrlPath && components.dwUrlPathLength)
		path.assign(components.lpszUrlPath, components.dwUrlPathLength);
	if (path.empty() || path == L"/") path = L"/v1/ws";
	if (components.lpszExtraInfo && components.dwExtraInfoLength)
		path.append(components.lpszExtraInfo, components.dwExtraInfoLength);

	CollaborationServerUrl output;
	output.secure = secure;
	output.host = narrow(host);
	output.path = narrow(path);
	output.port = components.nPort;
	std::string canonical_host = output.host.find(':') == std::string::npos ? output.host : "[" + output.host + "]";
	auto default_port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
	output.canonical_url = scheme + "://" + canonical_host;
	if (output.port != default_port) output.canonical_url += ":" + std::to_string(output.port);
	output.canonical_url += output.path;
	return output;
}

bool RequiresInsecureServerConfirmation(std::string const& server_url, std::vector<std::string> const& confirmed_servers) {
	auto parsed = ParseCollaborationServerUrl(server_url);
	return !parsed.secure && std::find(confirmed_servers.begin(), confirmed_servers.end(), parsed.canonical_url) == confirmed_servers.end();
}

void RememberInsecureServerConfirmation(std::string const& server_url, std::vector<std::string>& confirmed_servers) {
	auto parsed = ParseCollaborationServerUrl(server_url);
	if (parsed.secure) return;
	confirmed_servers.erase(std::remove(confirmed_servers.begin(), confirmed_servers.end(), std::string{}), confirmed_servers.end());
	if (std::find(confirmed_servers.begin(), confirmed_servers.end(), parsed.canonical_url) == confirmed_servers.end())
		confirmed_servers.emplace_back(std::move(parsed.canonical_url));
}

class CollaborationTransport::Impl {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<WireEnvelope> outgoing;
	std::deque<TransportEvent> events;
	std::thread worker;
	bool stopping = false;
	bool running = false;
	bool receiver_done = false;
	std::optional<TransportEvent::Failure> receiver_error;
	TransportConfig config;

	void push(TransportEvent event) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (events.size() >= MaximumQueuedEvents) {
				events.clear();
				TransportEvent overflow;
				overflow.type = TransportEventType::error;
				overflow.detail = "collaboration event queue overflow";
				events.emplace_back(std::move(overflow));
				stopping = true;
			}
			else {
				events.emplace_back(std::move(event));
			}
		}
		wake.notify_all();
	}

	void push_state(TransportState state, std::string detail = {}) {
		TransportEvent event;
		event.type = TransportEventType::state;
		event.state = state;
		event.detail = std::move(detail);
		push(std::move(event));
	}

	void push_error(TransportEvent::Failure failure) {
		TransportEvent event;
		event.type = TransportEventType::error;
		event.failure = std::move(failure);
		event.detail = FormatTransportFailure(event.failure);
		push(std::move(event));
	}

	void push_error(std::exception const& error, std::string stage, std::string operation) {
		push_error(failure_from_exception(error, std::move(stage), std::move(operation)));
	}

	void receive_loop(HINTERNET socket) {
		try {
			std::vector<std::uint8_t> message;
			bool binary = false;
			bool fragmented = false;
			std::vector<std::uint8_t> buffer(16 * 1024);
			for (;;) {
				DWORD received = 0;
				WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
				auto result = WinHttpWebSocketReceive(socket, buffer.data(), static_cast<DWORD>(buffer.size()), &received, &type);
				if (result != ERROR_SUCCESS) throw winhttp_error("WinHttpWebSocketReceive", result, "receive");
				if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
					USHORT status = 0;
					DWORD reason_size = 0;
					std::vector<std::uint8_t> reason(123);
					auto close_result = WinHttpWebSocketQueryCloseStatus(socket, &status, reason.data(),
						static_cast<DWORD>(reason.size()), &reason_size);
					if (close_result != ERROR_SUCCESS) throw winhttp_error("WinHttpWebSocketQueryCloseStatus", close_result, "close");
					TransportEvent::Failure failure;
					failure.stage = "close";
					failure.operation = "server closed WebSocket";
					failure.close_status = status;
					failure.close_reason.assign(reason.begin(), reason.begin() + (std::min)(reason_size, static_cast<DWORD>(reason.size())));
					{
						std::lock_guard<std::mutex> lock(mutex);
						receiver_error = std::move(failure);
					}
					break;
				}
				bool part_binary = type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
				bool final = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
				if (fragmented && binary != part_binary) throw std::invalid_argument("WebSocket message changed type between fragments");
				if (!fragmented) binary = part_binary;
				if (received > MaximumEnvelopeSize - message.size()) throw std::length_error("collaboration WebSocket frame exceeds 64 MiB");
				message.insert(message.end(), buffer.begin(), buffer.begin() + received);
				fragmented = !final;
				if (!final) continue;
				TransportEvent event;
				event.type = TransportEventType::message;
				event.state = TransportState::connected;
				event.message = DecodeFrame(binary, message);
				push(std::move(event));
				message.clear();
				fragmented = false;
			}
		}
		catch (std::exception const& error) {
			std::lock_guard<std::mutex> lock(mutex);
			receiver_error = failure_from_exception(error, "receive", "receive WebSocket message");
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			receiver_done = true;
		}
		wake.notify_all();
	}

	bool wait_retry(std::chrono::milliseconds delay) {
		std::unique_lock<std::mutex> lock(mutex);
		return !wake.wait_for(lock, delay, [&] { return stopping; });
	}

	void run() {
		unsigned attempt = 0;
		for (;;) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (stopping) break;
			}
			push_state(TransportState::connecting);
			try {
				auto connection = connect_websocket(config.server_url);
				attempt = 0;
				{
					std::lock_guard<std::mutex> lock(mutex);
					receiver_done = false;
					receiver_error.reset();
				}
				push_state(TransportState::connected);
				std::thread receiver([this, socket = connection.socket.get()] { receive_loop(socket); });
				bool connection_failed = false;
				for (;;) {
					WireEnvelope envelope;
					bool have_message = false;
					{
						std::unique_lock<std::mutex> lock(mutex);
						wake.wait(lock, [&] { return stopping || receiver_done || !outgoing.empty(); });
						if (stopping || receiver_done) break;
						envelope = std::move(outgoing.front());
						outgoing.pop_front();
						have_message = true;
					}
					if (have_message) {
						WireFrame frame;
						try {
							frame = EncodeFrame(envelope);
						}
						catch (std::exception const& error) {
							push_error(error, "encode", "encode protocol message");
							continue;
						}
						auto type = frame.binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
						auto result = WinHttpWebSocketSend(connection.socket.get(), type, frame.data.data(), static_cast<DWORD>(frame.data.size()));
						if (result != ERROR_SUCCESS) {
							push_error(winhttp_error("WinHttpWebSocketSend", result, "send"), "send", "WinHttpWebSocketSend");
							connection_failed = true;
							break;
						}
					}
				}
				close_websocket(connection.socket.get());
				receiver.join();
				{
					std::lock_guard<std::mutex> lock(mutex);
					if (receiver_error) {
						push_error_unlocked(*receiver_error);
						connection_failed = true;
					}
					if (stopping) break;
				}
				push_state(TransportState::retry_wait, connection_failed ? "connection lost" : "server closed connection");
			}
			catch (std::exception const& error) {
				push_error(error, "connect", "open WebSocket connection");
				{
					std::lock_guard<std::mutex> lock(mutex);
					if (stopping) break;
				}
				push_state(TransportState::retry_wait, "connection failed");
			}
			auto delay = ReconnectDelay(attempt++, config.reconnect_initial, config.reconnect_max);
			if (!wait_retry(delay)) break;
		}
		push_state(TransportState::stopped);
		std::lock_guard<std::mutex> lock(mutex);
		running = false;
	}

	void push_error_unlocked(TransportEvent::Failure const& failure) {
		TransportEvent event;
		event.type = TransportEventType::error;
		event.failure = failure;
		event.detail = FormatTransportFailure(event.failure);
		events.emplace_back(std::move(event));
	}

public:
	~Impl() { stop(); }

	void start(TransportConfig input) {
		if (input.server_url.empty() || input.reconnect_initial.count() <= 0 || input.reconnect_max < input.reconnect_initial)
			throw std::invalid_argument("collaboration transport configuration is invalid");
		std::lock_guard<std::mutex> lock(mutex);
		if (running) throw std::logic_error("collaboration transport is already running");
		config = std::move(input);
		stopping = false;
		running = true;
		worker = std::thread([this] { run(); });
	}

	void stop() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!running && !worker.joinable()) return;
			stopping = true;
		}
		wake.notify_all();
		if (worker.joinable()) worker.join();
	}

	bool send(WireEnvelope envelope) {
		std::lock_guard<std::mutex> lock(mutex);
		if (!running || stopping || outgoing.size() >= MaximumQueuedMessages) return false;
		outgoing.emplace_back(std::move(envelope));
		wake.notify_all();
		return true;
	}

	std::optional<TransportEvent> poll() {
		std::lock_guard<std::mutex> lock(mutex);
		if (events.empty()) return std::nullopt;
		auto event = std::move(events.front());
		events.pop_front();
		return event;
	}
};

CollaborationTransport::CollaborationTransport() : impl(new Impl) { }
CollaborationTransport::~CollaborationTransport() = default;
void CollaborationTransport::Start(TransportConfig config) { impl->start(std::move(config)); }
void CollaborationTransport::Stop() { impl->stop(); }
bool CollaborationTransport::Send(WireEnvelope envelope) { return impl->send(std::move(envelope)); }
std::optional<TransportEvent> CollaborationTransport::PollEvent() { return impl->poll(); }

std::chrono::milliseconds ReconnectDelay(unsigned attempt, std::chrono::milliseconds initial, std::chrono::milliseconds maximum) {
	if (initial.count() <= 0 || maximum < initial) throw std::invalid_argument("invalid reconnect delay bounds");
	auto result = initial;
	while (attempt-- && result < maximum) {
		if (result.count() > maximum.count() / 2) return maximum;
		result *= 2;
	}
	return (std::min)(result, maximum);
}

std::string CredentialTarget(std::string const& server_url, std::string const& room_name, std::string const& secret_name) {
	if (server_url.empty() || secret_name.empty()) throw std::invalid_argument("credential identity is incomplete");
	return "AegisubTogether:" + std::to_string(server_url.size()) + ":" + server_url
		+ ":" + std::to_string(room_name.size()) + ":" + room_name + ":" + secret_name;
}

void StoreCredential(std::string const& target, std::string const& secret) {
	if (secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) throw std::length_error("credential is too large");
	auto name = credential_name(target);
	CREDENTIALW credential{};
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = const_cast<wchar_t*>(name.c_str());
	credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
	credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	credential.UserName = const_cast<wchar_t*>(L"Aegisub Together");
	if (!CredWriteW(&credential, 0)) throw winhttp_error("CredWriteW");
}

std::optional<std::string> ReadCredential(std::string const& target) {
	auto name = credential_name(target);
	PCREDENTIALW credential = nullptr;
	if (!CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
		if (GetLastError() == ERROR_NOT_FOUND) return std::nullopt;
		throw winhttp_error("CredReadW");
	}
	struct FreeCredential { PCREDENTIALW value; ~FreeCredential() { CredFree(value); } } cleanup{credential};
	return std::string(reinterpret_cast<char const*>(credential->CredentialBlob), credential->CredentialBlobSize);
}

void DeleteCredential(std::string const& target) {
	auto name = credential_name(target);
	if (!CredDeleteW(name.c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND)
		throw winhttp_error("CredDeleteW");
}

} }
