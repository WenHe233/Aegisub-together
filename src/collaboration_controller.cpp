// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include "collaboration_controller.h"

#include "ass_file.h"
#include "ass_dialogue.h"
#include "collaboration_model.h"
#include "collaboration_transport.h"
#include "command/command.h"
#include "compat.h"
#include "frame_main.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "selection_controller.h"
#include "subs_controller.h"

#include <libaegisub/collaboration_room.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace agi { namespace collab {
namespace {
enum class RoomMode { create, join };

struct ConnectionInput {
	RoomMode mode = RoomMode::join;
	std::string server_url;
	std::string access_password;
	std::string room_name;
	std::string room_password;
	std::string nickname;
	bool remember_passwords = false;
	bool lock_enabled = true;
};

class ConnectDialog final : public wxDialog {
	RoomMode mode;
	wxTextCtrl* server;
	wxTextCtrl* access_password;
	wxTextCtrl* room;
	wxTextCtrl* room_password;
	wxTextCtrl* nickname;
	wxCheckBox* remember;
	wxCheckBox* locks;

	void add_row(wxFlexGridSizer* grid, wxString const& label, wxWindow* control) {
		grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
		grid->Add(control, 1, wxEXPAND);
	}

	void validate(wxCommandEvent&) {
		auto server_value = from_wx(server->GetValue());
		auto room_value = from_wx(room->GetValue());
		auto nickname_value = from_wx(nickname->GetValue());
		auto password_value = from_wx(room_password->GetValue());
		if (server_value.compare(0, 6, "wss://") != 0) {
			wxMessageBox(_("The collaboration server address must begin with wss://."), _("Invalid server"), wxOK | wxICON_ERROR, this);
			return;
		}
		if (room_value.empty() || room->GetValue().length() > 64 || nickname_value.empty() || nickname->GetValue().length() > 32) {
			wxMessageBox(_("Room names must be 1-64 characters and nicknames 1-32 characters."), _("Invalid room details"), wxOK | wxICON_ERROR, this);
			return;
		}
		if (password_value.size() < 8 || password_value.size() > 128) {
			wxMessageBox(_("The room password must be 8-128 UTF-8 bytes."), _("Invalid room password"), wxOK | wxICON_ERROR, this);
			return;
		}
		EndModal(wxID_OK);
	}

public:
	ConnectDialog(wxWindow* parent, RoomMode mode, std::string const& default_server,
		std::string const& default_room, std::string const& default_nickname,
		std::string const& saved_access_password, std::string const& saved_room_password)
	: wxDialog(parent, wxID_ANY, mode == RoomMode::create ? _("Create collaboration room") : _("Join collaboration room"),
		wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, mode(mode)
	, server(new wxTextCtrl(this, wxID_ANY, to_wx(default_server)))
	, access_password(new wxTextCtrl(this, wxID_ANY, to_wx(saved_access_password), wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD))
	, room(new wxTextCtrl(this, wxID_ANY, to_wx(default_room)))
	, room_password(new wxTextCtrl(this, wxID_ANY, to_wx(saved_room_password), wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD))
	, nickname(new wxTextCtrl(this, wxID_ANY, to_wx(default_nickname)))
	, remember(new wxCheckBox(this, wxID_ANY, _("Remember passwords in Windows Credential Manager")))
	, locks(new wxCheckBox(this, wxID_ANY, _("Require line locks")))
	{
		remember->SetValue(!saved_access_password.empty() || !saved_room_password.empty());
		locks->SetValue(true);
		auto grid = new wxFlexGridSizer(2, 8, 10);
		grid->AddGrowableCol(1, 1);
		add_row(grid, _("Server:"), server);
		add_row(grid, _("Server access password:"), access_password);
		add_row(grid, _("Room:"), room);
		add_row(grid, _("Room password:"), room_password);
		add_row(grid, _("Nickname:"), nickname);
		auto content = new wxBoxSizer(wxVERTICAL);
		content->Add(grid, 1, wxEXPAND | wxALL, 12);
		if (mode == RoomMode::create) content->Add(locks, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
		content->Add(remember, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
		content->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
		SetSizerAndFit(content);
		SetMinSize(wxSize(520, GetSize().GetHeight()));
		Bind(wxEVT_BUTTON, &ConnectDialog::validate, this, wxID_OK);
	}

	ConnectionInput Input() const {
		return {
			mode, from_wx(server->GetValue()), from_wx(access_password->GetValue()), from_wx(room->GetValue()),
			from_wx(room_password->GetValue()), from_wx(nickname->GetValue()), remember->GetValue(), locks->GetValue()
		};
	}
};

std::uint64_t random_prefix() {
	std::random_device random;
	std::uint64_t value = 0;
	for (int index = 0; index < 4; ++index) value = (value << 16) ^ (static_cast<std::uint64_t>(random()) & 0xffff);
	return value & ((std::uint64_t{1} << 50) - 1);
}

std::unique_ptr<IdAllocator> load_allocator() {
	auto prefix = OPT_GET("Collaboration/Client Prefix")->GetString();
	std::uint64_t next = 1;
	try {
		next = std::stoull(OPT_GET("Collaboration/Next ID")->GetString());
		return std::unique_ptr<IdAllocator>(new IdAllocator(prefix, next));
	}
	catch (...) {
		prefix = EncodeClientPrefix(random_prefix());
		OPT_SET("Collaboration/Client Prefix")->SetString(prefix);
		OPT_SET("Collaboration/Next ID")->SetString("1");
		return std::unique_ptr<IdAllocator>(new IdAllocator(prefix, 1));
	}
}

std::string read_credential(std::string const& target) {
	try {
		auto value = ReadCredential(target);
		return value ? *value : std::string{};
	}
	catch (...) {
		return {};
	}
}
}

class CollaborationController::Impl final : public wxEvtHandler {
	enum class Phase { idle, connecting, waiting_access, waiting_room, joined };

	Context* context;
	CollaborationTransport transport;
	wxTimer timer;
	std::unique_ptr<IdAllocator> allocator;
	ConnectionInput input;
	Snapshot create_snapshot;
	RoomJoined room;
	Phase phase = Phase::idle;
	std::int64_t revision = 0;
	std::uint64_t next_request = 1;
	std::chrono::steady_clock::time_point last_heartbeat;
	bool load_snapshot_on_join = false;

	std::string request_id(char const* prefix) {
		return std::string(prefix) + "-" + std::to_string(next_request++);
	}

	bool send(std::string type, std::string payload) {
		WireEnvelope envelope;
		envelope.type = std::move(type);
		envelope.request_id = request_id("client");
		envelope.room_revision = revision;
		envelope.payload_json = std::move(payload);
		if (transport.Send(std::move(envelope))) return true;
		context->frame->StatusTimeout(_("Collaboration send queue is unavailable."));
		return false;
	}

	void send_access_auth() {
		if (send("access_auth", EncodeAccessAuth(input.access_password))) phase = Phase::waiting_access;
	}

	void send_room_request() {
		if (input.mode == RoomMode::create) {
			CreateRoomRequest request{input.room_name, input.room_password, input.nickname, input.lock_enabled, create_snapshot};
			if (send("create_room", EncodeCreateRoom(request))) phase = Phase::waiting_room;
		}
		else {
			JoinRoomRequest request{input.room_name, input.room_password, input.nickname, room.resume_token};
			if (send("join_room", EncodeJoinRoom(request))) phase = Phase::waiting_room;
		}
	}

	void persist_credentials() {
		auto access_target = CredentialTarget(input.server_url, "", "access-password");
		auto room_target = CredentialTarget(input.server_url, input.room_name, "room-password");
		try {
			if (input.remember_passwords) {
				StoreCredential(access_target, input.access_password);
				StoreCredential(room_target, input.room_password);
			}
			else {
				DeleteCredential(access_target);
				DeleteCredential(room_target);
			}
		}
		catch (std::exception const& error) {
			wxMessageBox(to_wx(error.what()), _("Could not update saved credentials"), wxOK | wxICON_WARNING, context->parent);
		}
	}

	void forget_credentials(ConnectionInput const& selected) {
		try {
			DeleteCredential(CredentialTarget(selected.server_url, "", "access-password"));
			DeleteCredential(CredentialTarget(selected.server_url, selected.room_name, "room-password"));
		}
		catch (std::exception const& error) {
			wxMessageBox(to_wx(error.what()), _("Could not remove saved credentials"), wxOK | wxICON_WARNING, context->parent);
		}
	}

	void apply_joined_room(WireEnvelope const& envelope) {
		auto joined = DecodeRoomJoined(envelope.payload_json);
		if (load_snapshot_on_join) {
			ass::LoadSnapshot(*context->ass, joined.snapshot);
			context->subsController->AdoptCollaborationSnapshot();
			if (!context->ass->Events.empty()) {
				auto* first = &context->ass->Events.front();
				context->selectionController->SetSelectionAndActive({first}, first);
			}
		}
		room = std::move(joined);
		input.mode = RoomMode::join;
		load_snapshot_on_join = false;
		phase = Phase::joined;
		last_heartbeat = std::chrono::steady_clock::now();
		persist_credentials();
		context->frame->StatusTimeout(fmt_tl("Connected to collaboration room %s as %s", input.room_name, input.nickname));
	}

	void fail_protocol(std::string const& message) {
		wxMessageBox(to_wx(message), _("Collaboration connection failed"), wxOK | wxICON_ERROR, context->parent);
		disconnect();
	}

	void handle_message(WireEnvelope const& envelope) {
		revision = (std::max)(revision, envelope.room_revision);
		try {
			if (envelope.type == "access_ok" && phase == Phase::waiting_access) send_room_request();
			else if (envelope.type == "room_joined" && phase == Phase::waiting_room) apply_joined_room(envelope);
			else if (envelope.type == "error") {
				auto error = DecodeProtocolError(envelope.payload_json);
				fail_protocol(error.message.empty() ? error.code : error.message);
			}
		}
		catch (std::exception const& error) {
			fail_protocol(error.what());
		}
	}

	void on_timer(wxTimerEvent&) {
		while (auto event = transport.PollEvent()) {
			if (event->type == TransportEventType::message) handle_message(event->message);
			else if (event->type == TransportEventType::error) context->frame->StatusTimeout(to_wx(event->detail));
			else if (event->state == TransportState::connected) send_access_auth();
			else if (event->state == TransportState::connecting) {
				if (phase == Phase::joined) load_snapshot_on_join = true;
				phase = Phase::connecting;
			}
			else if (event->state == TransportState::retry_wait) context->frame->StatusTimeout(_("Collaboration connection lost; retrying..."));
		}
		if (phase == Phase::joined && std::chrono::steady_clock::now() - last_heartbeat >= std::chrono::seconds(10)) {
			send("heartbeat", "{}");
			last_heartbeat = std::chrono::steady_clock::now();
		}
	}

	bool prompt_to_save_before_join() {
		if (!context->subsController->IsModified()) return true;
		wxMessageDialog dialog(context->parent,
			_("Joining a room replaces the core subtitle data in memory. Save the current document before continuing?"),
			_("Save before joining"), wxYES_NO | wxICON_WARNING);
		dialog.SetYesNoLabels(_("Save"), _("Cancel"));
		if (dialog.ShowModal() != wxID_YES) return false;
		cmd::call("subtitle/save", context);
		return !context->subsController->IsModified();
	}

	void show_dialog(RoomMode mode) {
		if (phase != Phase::idle) return;
		auto server = OPT_GET("Collaboration/Server")->GetString();
		auto room_name = OPT_GET("Collaboration/Room")->GetString();
		auto nickname = OPT_GET("Collaboration/Nickname")->GetString();
		auto access_password = server.empty() ? std::string{} : read_credential(CredentialTarget(server, "", "access-password"));
		auto room_password = server.empty() || room_name.empty() ? std::string{} : read_credential(CredentialTarget(server, room_name, "room-password"));
		ConnectDialog dialog(context->parent, mode, server, room_name, nickname, access_password, room_password);
		if (dialog.ShowModal() != wxID_OK) return;
		auto selected = dialog.Input();
		if (mode == RoomMode::join && !prompt_to_save_before_join()) return;

		OPT_SET("Collaboration/Server")->SetString(selected.server_url);
		OPT_SET("Collaboration/Room")->SetString(selected.room_name);
		OPT_SET("Collaboration/Nickname")->SetString(selected.nickname);
		if (mode == RoomMode::create) {
			SanitizeContext sanitize;
			sanitize.accept_unknown_ids = true;
			auto result = ass::SanitizeFileMetadata(*context->ass, *allocator, sanitize);
			OPT_SET("Collaboration/Next ID")->SetString(std::to_string(allocator->NextCounter()));
			if (result.changed()) context->ass->Commit(_("initialize collaboration metadata"), AssFile::COMMIT_EXTRADATA);
			create_snapshot = ass::CaptureSnapshot(*context->ass, {});
		}
		input = std::move(selected);
		if (!input.remember_passwords) forget_credentials(input);
		load_snapshot_on_join = mode == RoomMode::join;
		room = RoomJoined{};
		revision = 0;
		phase = Phase::connecting;
		transport.Start({input.server_url});
		timer.Start(50);
		context->frame->StatusTimeout(_("Connecting to collaboration server..."));
	}

public:
	explicit Impl(Context* context)
	: context(context)
	, timer(this)
	, allocator(load_allocator())
	{
		Bind(wxEVT_TIMER, &Impl::on_timer, this);
	}

	~Impl() { disconnect(); }

	void create() { show_dialog(RoomMode::create); }
	void join() { show_dialog(RoomMode::join); }

	void disconnect() {
		timer.Stop();
		transport.Stop();
		input.access_password.assign(input.access_password.size(), '\0');
		input.room_password.assign(input.room_password.size(), '\0');
		input = ConnectionInput{};
		create_snapshot = Snapshot{};
		room = RoomJoined{};
		phase = Phase::idle;
		revision = 0;
	}

	bool running() const { return phase != Phase::idle; }
	bool joined() const { return phase == Phase::joined; }
};

CollaborationController::CollaborationController(Context* context) : impl(new Impl(context)) { }
CollaborationController::~CollaborationController() = default;
void CollaborationController::ShowCreateRoomDialog() { impl->create(); }
void CollaborationController::ShowJoinRoomDialog() { impl->join(); }
void CollaborationController::Disconnect() { impl->disconnect(); }
bool CollaborationController::IsRunning() const { return impl->running(); }
bool CollaborationController::IsJoined() const { return impl->joined(); }

} }
