// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include "collaboration_transport.h"

#include <windows.h>
#include <wincred.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
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
	struct Generation;

	std::mutex mutex;
	std::condition_variable wake;
	std::deque<WireEnvelope> outgoing;
	std::deque<TransportEvent> events;
	std::thread worker;
	bool stopping = false;
	bool running = false;
	TransportConfig config;
	std::atomic<std::uint64_t> active_generation{0};
	std::uint64_t next_generation = 1;

	void push(TransportEvent event) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (events.size() >= MaximumQueuedEvents) {
				events.clear();
				TransportEvent overflow;
				overflow.type = TransportEventType::error;
				overflow.failure.stage = "queue";
				overflow.failure.operation = "queue transport event";
				overflow.failure.native_message = "collaboration event queue overflow";
				overflow.detail = FormatTransportFailure(overflow.failure);
				events.emplace_back(std::move(overflow));
				stopping = true;
			}
			else events.emplace_back(std::move(event));
		}
		wake.notify_all();
	}

	void push_generation(std::uint64_t generation, TransportEvent event) {
		if (active_generation.load(std::memory_order_acquire) != generation) return;
		push(std::move(event));
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

	struct Generation {
		Impl* owner;
		std::uint64_t id;
		std::mutex mutex;
		std::condition_variable changed;
		HINTERNET session = nullptr;
		HINTERNET connection = nullptr;
		HINTERNET request = nullptr;
		HINTERNET socket = nullptr;
		bool request_closing = false;
		bool request_has_context = false;
		bool socket_closing = false;
		bool handshake_complete = false;
		bool opened = false;
		bool ended = false;
		bool receive_inflight = false;
		bool send_inflight = false;
		bool shutdown_inflight = false;
		bool shutdown_sent = false;
		bool close_inflight = false;
		bool stop_requested = false;
		bool peer_closed = false;
		std::optional<TransportEvent::Failure> failure;
		std::vector<std::uint8_t> receive_buffer = std::vector<std::uint8_t>(16 * 1024);
		std::vector<std::uint8_t> message;
		bool message_binary = false;
		bool fragmented = false;
		WireFrame sending;

		explicit Generation(Impl* owner, std::uint64_t id) : owner(owner), id(id) { }

		static char const* websocket_operation(WINHTTP_WEB_SOCKET_OPERATION operation) {
			switch (operation) {
				case WINHTTP_WEB_SOCKET_SEND_OPERATION: return "WinHttpWebSocketSend";
				case WINHTTP_WEB_SOCKET_RECEIVE_OPERATION: return "WinHttpWebSocketReceive";
				case WINHTTP_WEB_SOCKET_CLOSE_OPERATION: return "WinHttpWebSocketClose";
				case WINHTTP_WEB_SOCKET_SHUTDOWN_OPERATION: return "WinHttpWebSocketShutdown";
			}
			return "WinHTTP WebSocket operation";
		}

		void notify() {
			changed.notify_all();
			owner->wake.notify_all();
		}

		void set_failure(TransportEvent::Failure input) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (!failure) failure = std::move(input);
			}
			notify();
		}

		void set_windows_failure(char const* stage, char const* operation, DWORD code) {
			set_failure(winhttp_error(operation, code, stage).failure);
		}

		static void CALLBACK callback(HINTERNET handle, DWORD_PTR context, DWORD status, void* information, DWORD information_size) {
			if (!context) return;
			auto* self = reinterpret_cast<Generation*>(context);
			self->on_callback(handle, status, information, information_size);
		}

		void on_callback(HINTERNET handle, DWORD status, void* information, DWORD information_size) {
			if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE) {
				if (!WinHttpReceiveResponse(handle, nullptr) && GetLastError() != ERROR_IO_PENDING)
					set_windows_failure("upgrade", "WinHttpReceiveResponse", GetLastError());
				return;
			}
			if (status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) {
				on_headers_available(handle);
				return;
			}
			if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
				if (information_size == sizeof(WINHTTP_WEB_SOCKET_STATUS))
					on_read_complete(*static_cast<WINHTTP_WEB_SOCKET_STATUS*>(information));
				else set_windows_failure("receive", "WinHttpWebSocketReceive callback", ERROR_INVALID_DATA);
				return;
			}
			if (status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					send_inflight = false;
					sending = WireFrame{};
				}
				notify();
				maybe_finish_close();
				return;
			}
			if (status == WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE) {
				bool finish_stop = false;
				{
					std::lock_guard<std::mutex> lock(mutex);
					shutdown_inflight = false;
					finish_stop = stop_requested && !peer_closed;
				}
				notify();
				if (finish_stop) close_socket_handle();
				else maybe_finish_close();
				return;
			}
			if (status == WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					close_inflight = false;
				}
				close_socket_handle();
				return;
			}
			if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
				on_request_error(handle, information, information_size);
				return;
			}
			if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					if (handle == request) {
						request = nullptr;
						request_closing = false;
						request_has_context = false;
					}
					if (handle == socket) {
						socket = nullptr;
						socket_closing = false;
						receive_inflight = false;
						send_inflight = false;
						shutdown_inflight = false;
						close_inflight = false;
						ended = true;
					}
				}
				notify();
			}
		}

		void on_headers_available(HINTERNET handle) {
			DWORD status = 0;
			DWORD status_size = sizeof(status);
			if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
				set_windows_failure("upgrade", "WinHttpQueryHeaders", GetLastError());
				return;
			}
			if (status != 101) {
				TransportEvent::Failure rejected;
				rejected.stage = "upgrade";
				rejected.operation = "WebSocket HTTP upgrade";
				rejected.native_message = "server returned HTTP status " + std::to_string(status);
				set_failure(std::move(rejected));
				return;
			}
			auto upgraded = WinHttpWebSocketCompleteUpgrade(handle, reinterpret_cast<DWORD_PTR>(this));
			if (!upgraded) {
				set_windows_failure("upgrade", "WinHttpWebSocketCompleteUpgrade", GetLastError());
				return;
			}
			{
				std::lock_guard<std::mutex> lock(mutex);
				socket = upgraded;
				opened = true;
				handshake_complete = true;
			}
			close_request_handle();
			notify();
		}

		void on_request_error(HINTERNET handle, void* information, DWORD information_size) {
			DWORD code = ERROR_WINHTTP_INTERNAL_ERROR;
			std::string stage = "upgrade";
			std::string operation = "asynchronous WinHTTP request";
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (handle == socket && information_size >= sizeof(WINHTTP_WEB_SOCKET_ASYNC_RESULT)) {
					auto const& result = *static_cast<WINHTTP_WEB_SOCKET_ASYNC_RESULT*>(information);
					code = result.AsyncResult.dwError;
					operation = websocket_operation(result.Operation);
					stage = result.Operation == WINHTTP_WEB_SOCKET_RECEIVE_OPERATION ? "receive" :
						result.Operation == WINHTTP_WEB_SOCKET_SEND_OPERATION ? "send" : "close";
					if (result.Operation == WINHTTP_WEB_SOCKET_RECEIVE_OPERATION) receive_inflight = false;
					else if (result.Operation == WINHTTP_WEB_SOCKET_SEND_OPERATION) send_inflight = false;
					else if (result.Operation == WINHTTP_WEB_SOCKET_SHUTDOWN_OPERATION) shutdown_inflight = false;
					else if (result.Operation == WINHTTP_WEB_SOCKET_CLOSE_OPERATION) close_inflight = false;
				}
				else if (information_size >= sizeof(WINHTTP_ASYNC_RESULT)) {
					auto const& result = *static_cast<WINHTTP_ASYNC_RESULT*>(information);
					code = result.dwError;
				}
			}
			bool expected_cancellation = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				expected_cancellation = stop_requested &&
					(code == ERROR_WINHTTP_OPERATION_CANCELLED || code == ERROR_OPERATION_ABORTED);
			}
			if (!expected_cancellation) set_windows_failure(stage.c_str(), operation.c_str(), code);
			if (handle == socket) close_socket_handle();
			else close_request_handle();
		}

		void on_read_complete(WINHTTP_WEB_SOCKET_STATUS const& status) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				receive_inflight = false;
			}
			if (status.eBufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
				TransportEvent::Failure closed;
				closed.stage = "close";
				closed.operation = "server closed WebSocket";
				USHORT code = 0;
				DWORD reason_size = 0;
				std::vector<std::uint8_t> reason(WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH);
				auto query = WinHttpWebSocketQueryCloseStatus(socket, &code, reason.data(), static_cast<DWORD>(reason.size()), &reason_size);
				if (query == ERROR_SUCCESS) {
					closed.close_status = code;
					closed.close_reason.assign(reason.begin(), reason.begin() + (std::min)(reason_size, static_cast<DWORD>(reason.size())));
				}
				else {
					closed.native_error = query;
					closed.native_message = windows_error_message(query);
				}
				{
					std::lock_guard<std::mutex> lock(mutex);
					peer_closed = true;
					if (!stop_requested && !failure) failure = closed;
				}
				notify();
				maybe_finish_close();
				return;
			}

			try {
				bool part_binary = status.eBufferType == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
					status.eBufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
				bool final = status.eBufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
					status.eBufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
				if (fragmented && message_binary != part_binary) throw std::invalid_argument("WebSocket message changed type between fragments");
				if (!fragmented) message_binary = part_binary;
				if (status.dwBytesTransferred > MaximumEnvelopeSize - message.size())
					throw std::length_error("collaboration WebSocket frame exceeds 64 MiB");
				message.insert(message.end(), receive_buffer.begin(), receive_buffer.begin() + status.dwBytesTransferred);
				fragmented = !final;
				if (final) {
					TransportEvent event;
					event.type = TransportEventType::message;
					event.state = TransportState::connected;
					event.message = DecodeFrame(message_binary, message);
					owner->push_generation(id, std::move(event));
					message.clear();
					fragmented = false;
				}
			}
			catch (std::exception const& error) {
				set_failure(failure_from_exception(error, "decode", "decode WebSocket message"));
				close_socket_handle();
				return;
			}
			start_receive();
		}

		bool start(std::string const& server_url) {
			try {
				auto parsed = ParseCollaborationServerUrl(server_url);
				auto host = widen(parsed.host);
				auto path = widen(parsed.path);
				session = WinHttpOpen(UserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
				if (!session) throw winhttp_error("WinHttpOpen", GetLastError(), "session");
				auto previous = WinHttpSetStatusCallback(session, callback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
				if (previous == WINHTTP_INVALID_STATUS_CALLBACK) throw winhttp_error("WinHttpSetStatusCallback", GetLastError(), "session");
				if (!WinHttpSetTimeouts(session, 5000, 10000, 10000, 30000))
					throw winhttp_error("WinHttpSetTimeouts", GetLastError(), "session");
				connection = WinHttpConnect(session, host.c_str(), parsed.port, 0);
				if (!connection) throw winhttp_error("WinHttpConnect", GetLastError(), "connect");
				request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
					WINHTTP_DEFAULT_ACCEPT_TYPES, parsed.secure ? WINHTTP_FLAG_SECURE : 0);
				if (!request) throw winhttp_error("WinHttpOpenRequest", GetLastError(), "upgrade");
				DWORD_PTR callback_context = reinterpret_cast<DWORD_PTR>(this);
				if (!WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &callback_context, sizeof(callback_context)))
					throw winhttp_error("WinHttpSetOption(CONTEXT_VALUE)", GetLastError(), "upgrade");
				request_has_context = true;
				if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
					throw winhttp_error("WinHttpSetOption(WEB_SOCKET)", GetLastError(), "upgrade");
				if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
					reinterpret_cast<DWORD_PTR>(this)) && GetLastError() != ERROR_IO_PENDING)
					throw winhttp_error("WinHttpSendRequest", GetLastError(), "upgrade");
				return true;
			}
			catch (std::exception const& error) {
				set_failure(failure_from_exception(error, "connect", "open WebSocket connection"));
				close_request_handle();
				return false;
			}
		}

		void start_receive() {
			HINTERNET current = nullptr;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (!opened || ended || receive_inflight || peer_closed || failure) return;
				receive_inflight = true;
				current = socket;
			}
			DWORD ignored_bytes = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE ignored_type{};
			auto result = WinHttpWebSocketReceive(current, receive_buffer.data(), static_cast<DWORD>(receive_buffer.size()),
				&ignored_bytes, &ignored_type);
			if (result != ERROR_SUCCESS) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					receive_inflight = false;
				}
				set_windows_failure("receive", "WinHttpWebSocketReceive", result);
				close_socket_handle();
			}
		}

		bool can_send() {
			std::lock_guard<std::mutex> lock(mutex);
			return opened && !ended && !send_inflight && !shutdown_inflight && !close_inflight && !stop_requested && !failure;
		}

		void send(WireFrame frame) {
			HINTERNET current = nullptr;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (!opened || ended || send_inflight || stop_requested || failure) return;
				sending = std::move(frame);
				send_inflight = true;
				current = socket;
				type = sending.binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
			}
			auto result = WinHttpWebSocketSend(current, type, sending.data.data(), static_cast<DWORD>(sending.data.size()));
			if (result != ERROR_SUCCESS) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					send_inflight = false;
				}
				set_windows_failure("send", "WinHttpWebSocketSend", result);
				close_socket_handle();
			}
		}

		void request_stop() {
			{
				std::lock_guard<std::mutex> lock(mutex);
				stop_requested = true;
			}
			maybe_finish_close();
			close_request_handle();
		}

		void maybe_finish_close() {
			HINTERNET current = nullptr;
			bool shutdown = false;
			bool close = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (!socket || socket_closing || ended) return;
				if (peer_closed && !receive_inflight && !send_inflight && !shutdown_inflight && !close_inflight) {
					close_inflight = true;
					close = true;
					current = socket;
				}
				else if (!failure && stop_requested && !shutdown_sent && !send_inflight && !shutdown_inflight && !close_inflight) {
					shutdown_inflight = true;
					shutdown_sent = true;
					shutdown = true;
					current = socket;
				}
			}
			if (shutdown) {
				auto result = WinHttpWebSocketShutdown(current, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
				if (result != ERROR_SUCCESS) {
					{
						std::lock_guard<std::mutex> lock(mutex);
						shutdown_inflight = false;
					}
					set_windows_failure("close", "WinHttpWebSocketShutdown", result);
					close_socket_handle();
				}
			}
			else if (close) {
				auto result = WinHttpWebSocketClose(current, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
				if (result != ERROR_SUCCESS) {
					{
						std::lock_guard<std::mutex> lock(mutex);
						close_inflight = false;
					}
					set_windows_failure("close", "WinHttpWebSocketClose", result);
					close_socket_handle();
				}
			}
		}

		void close_request_handle() {
			HINTERNET current = nullptr;
			bool wait_for_callback = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (!request || request_closing) return;
				current = request;
				wait_for_callback = request_has_context;
				request_closing = wait_for_callback;
				if (!wait_for_callback) request = nullptr;
			}
			if (!WinHttpCloseHandle(current)) {
				auto error = GetLastError();
				{
					std::lock_guard<std::mutex> lock(mutex);
					request = nullptr;
					request_closing = false;
					request_has_context = false;
				}
				set_windows_failure("close", "WinHttpCloseHandle(request)", error);
			}
		}

		void close_socket_handle() {
			HINTERNET current = nullptr;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (!socket || socket_closing) return;
				socket_closing = true;
				current = socket;
			}
			if (!WinHttpCloseHandle(current)) {
				auto error = GetLastError();
				{
					std::lock_guard<std::mutex> lock(mutex);
					socket = nullptr;
					socket_closing = false;
					ended = true;
				}
				set_windows_failure("close", "WinHttpCloseHandle(WebSocket)", error);
			}
		}

		bool wait_open_or_end() {
			std::unique_lock<std::mutex> lock(mutex);
			while (!opened && !failure && !ended && !owner->is_stopping())
				changed.wait_for(lock, std::chrono::milliseconds(50));
			return opened && !failure;
		}

		bool is_ended() {
			std::lock_guard<std::mutex> lock(mutex);
			return ended;
		}

		std::optional<TransportEvent::Failure> get_failure() {
			std::lock_guard<std::mutex> lock(mutex);
			return failure;
		}

		void force_cancel() {
			close_socket_handle();
			close_request_handle();
		}

		void cleanup() {
			force_cancel();
			{
				std::unique_lock<std::mutex> lock(mutex);
				changed.wait(lock, [&] { return !request_closing && !socket_closing; });
			}
			if (connection) {
				WinHttpCloseHandle(connection);
				connection = nullptr;
			}
			if (session) {
				WinHttpSetStatusCallback(session, nullptr, 0, 0);
				WinHttpCloseHandle(session);
				session = nullptr;
			}
		}
	};

	bool is_stopping() {
		std::lock_guard<std::mutex> lock(mutex);
		return stopping;
	}

	bool wait_retry(std::chrono::milliseconds delay) {
		std::unique_lock<std::mutex> lock(mutex);
		return !wake.wait_for(lock, delay, [&] { return stopping; });
	}

	void run() {
		unsigned attempt = 0;
		for (;;) {
			if (is_stopping()) break;
			push_state(TransportState::connecting);
			auto generation_id = next_generation++;
			active_generation.store(generation_id, std::memory_order_release);
			Generation generation(this, generation_id);
			generation.start(config.server_url);
			bool opened = generation.wait_open_or_end();
			if (is_stopping()) generation.request_stop();
			if (opened && !is_stopping()) {
				attempt = 0;
				push_state(TransportState::connected);
				generation.start_receive();
			}

			auto stop_deadline = std::chrono::steady_clock::time_point::max();
			while (opened && !generation.is_ended()) {
				if (is_stopping()) {
					generation.request_stop();
					if (stop_deadline == std::chrono::steady_clock::time_point::max())
						stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
					if (std::chrono::steady_clock::now() >= stop_deadline) generation.force_cancel();
				}
				else if (generation.can_send()) {
					WireEnvelope envelope;
					bool have_message = false;
					{
						std::lock_guard<std::mutex> lock(mutex);
						if (!outgoing.empty()) {
							envelope = std::move(outgoing.front());
							outgoing.pop_front();
							have_message = true;
						}
					}
					if (have_message) {
						try { generation.send(EncodeFrame(envelope)); }
						catch (std::exception const& error) { push_error(failure_from_exception(error, "encode", "encode protocol message")); }
					}
				}
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait_for(lock, std::chrono::milliseconds(50));
			}

			generation.cleanup();
			active_generation.store(0, std::memory_order_release);
			if (auto failure = generation.get_failure()) push_error(std::move(*failure));
			if (is_stopping()) break;
			push_state(TransportState::retry_wait, opened ? "connection lost" : "connection failed");
			auto delay = ReconnectDelay(attempt++, config.reconnect_initial, config.reconnect_max);
			if (!wait_retry(delay)) break;
		}
		active_generation.store(0, std::memory_order_release);
		push_state(TransportState::stopped);
		std::lock_guard<std::mutex> lock(mutex);
		running = false;
	}

public:
	~Impl() { stop(); }

	void start(TransportConfig input) {
		if (input.server_url.empty() || input.reconnect_initial.count() <= 0 || input.reconnect_max < input.reconnect_initial)
			throw std::invalid_argument("collaboration transport configuration is invalid");
		std::lock_guard<std::mutex> lock(mutex);
		if (running) throw std::logic_error("collaboration transport is already running");
		config = std::move(input);
		outgoing.clear();
		events.clear();
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

ConnectionLossPolicy EvaluateConnectionLoss(bool joined_once, bool create_request_sent) {
	ConnectionLossPolicy policy;
	policy.retry = joined_once;
	policy.enable_offline_journal = joined_once;
	policy.create_may_have_completed = !joined_once && create_request_sent;
	return policy;
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
