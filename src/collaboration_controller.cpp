// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include "collaboration_controller.h"

#include "ass_file.h"
#include "ass_dialogue.h"
#include "base_grid.h"
#include "collaboration_model.h"
#include "collaboration_transport.h"
#include "command/command.h"
#include "compat.h"
#include "frame_main.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "selection_controller.h"
#include "subs_edit_box.h"
#include "subs_controller.h"

#include <libaegisub/collaboration_room.h>
#include <libaegisub/collaboration_sync.h>
#include <libaegisub/signal.h>

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
#include <deque>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

class FlagGuard final {
	bool& flag;
public:
	explicit FlagGuard(bool& flag) : flag(flag) { flag = true; }
	~FlagGuard() { flag = false; }
};
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
	SyncState sync;
	Phase phase = Phase::idle;
	std::int64_t revision = 0;
	std::uint64_t next_request = 1;
	std::chrono::steady_clock::time_point last_heartbeat;
	bool load_snapshot_on_join = false;
	bool applying_snapshot = false;
	std::string active_line_id;
	std::unordered_map<std::string, std::string> lock_holders;
	std::vector<PresenceMember> presence;
	MaintenanceStateMessage maintenance;
	struct HistoryEntry { Snapshot undo_target; Snapshot redo_target; Snapshot expected; };
	struct HistoryAction { std::string batch_id; bool undo = true; };
	std::vector<HistoryEntry> undo_history;
	std::vector<HistoryEntry> redo_history;
	std::optional<HistoryAction> history_action;
	std::vector<PendingBatch> rejected_batches;
	std::deque<WireEnvelope> deferred_persistent_messages;
	unsigned mutation_depth = 0;
	agi::signal::Connection commit_connection;
	agi::signal::Connection active_line_connection;
	agi::signal::Connection selection_connection;

	void update_editability() {
		if (!context->subsEditBox) return;
		bool owns_maintenance = phase == Phase::joined && maintenance.active && maintenance.holder_id && *maintenance.holder_id == room.member_id;
		bool editable = phase == Phase::idle || owns_maintenance || (phase == Phase::joined && !maintenance.active && !room.lock_enabled);
		if (!editable && phase == Phase::joined && !maintenance.active && context->selectionController->GetSelectedSet().size() == 1 && !active_line_id.empty()) {
			auto holder = lock_holders.find(active_line_id);
			editable = holder != lock_holders.end() && holder->second == room.member_id;
		}
		context->subsEditBox->SetCollaborationEditable(editable);
	}

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

	std::string mint_batch_id() {
		auto id = allocator->Mint() + "-batch";
		OPT_SET("Collaboration/Next ID")->SetString(std::to_string(allocator->NextCounter()));
		return id;
	}

	void request_snapshot() {
		if (sync.IsInitialized()) send("snapshot_request", EncodeSnapshotRequest(sync.Revision()));
	}

	void apply_projected_snapshot(std::unordered_map<std::string, std::string> const& remap = {}) {
		std::string active = active_line_id;
		if (auto replacement = remap.find(active); replacement != remap.end()) active = replacement->second;
		std::unordered_set<std::string> selected;
		for (auto const* line : context->selectionController->GetSelectedSet()) {
			auto id = ass::GetMetadataValue(*context->ass, line->ExtradataIds.get(), IdExtradataKey);
			if (auto replacement = remap.find(id); replacement != remap.end()) id = replacement->second;
			if (!id.empty()) selected.insert(std::move(id));
		}

		FlagGuard applying(applying_snapshot);
		ass::LoadSnapshot(*context->ass, sync.Projected().snapshot);
		context->ass->Commit(_("apply collaboration update"), AssFile::COMMIT_NEW);

		Selection selection;
		AssDialogue* active_line = nullptr;
		for (auto& line : context->ass->Events) {
			auto id = ass::GetMetadataValue(*context->ass, line.ExtradataIds.get(), IdExtradataKey);
			if (selected.count(id)) selection.insert(&line);
			if (id == active) active_line = &line;
		}
		if (!active_line && !selection.empty()) active_line = *selection.begin();
		if (!active_line && !context->ass->Events.empty()) active_line = &context->ass->Events.front();
		if (selection.empty() && active_line) selection.insert(active_line);
		context->selectionController->SetSelectionAndActive(std::move(selection), active_line);
	}

	void on_local_commit(int, AssDialogue const*) {
		if (phase != Phase::joined || applying_snapshot || !sync.IsInitialized()) return;
		try {
			SanitizeContext sanitize;
			for (auto const& line : sync.Projected().snapshot.lines) sanitize.known_live_ids.insert(line.id);
			for (auto const& tombstone : sync.Projected().tombstones) sanitize.tombstoned_ids.insert(tombstone.first);
			auto cleaned = ass::SanitizeFileMetadata(*context->ass, *allocator, sanitize);
			if (cleaned.changed()) OPT_SET("Collaboration/Next ID")->SetString(std::to_string(allocator->NextCounter()));
			std::unordered_map<std::string, std::int64_t> versions;
			for (auto const& line : sync.Projected().snapshot.lines) versions.emplace(line.id, line.version);
			auto local = ass::CaptureSnapshot(*context->ass, versions,
				sync.Projected().snapshot.styles_version, sync.Projected().snapshot.script_info_version);
			auto batch = sync.QueueLocalSnapshot(mint_batch_id(), local);
			if (!batch.operations.empty()) send("submit_batch", EncodeSubmitBatch(batch));
		}
		catch (std::exception const& error) {
			context->frame->StatusTimeout(to_wx(std::string("Collaboration change could not be queued: ") + error.what()));
			request_snapshot();
		}
	}

	void on_active_line_changed(AssDialogue* line) {
		auto previous = active_line_id;
		auto next = line ? ass::GetMetadataValue(*context->ass, line->ExtradataIds.get(), IdExtradataKey) : std::string{};
		if (previous == next) return;
		active_line_id = std::move(next);
		update_editability();
		if (phase != Phase::joined) return;
		if (!previous.empty()) send("lock_release", EncodeLineReference(previous));
		if (!active_line_id.empty()) send("lock_request", EncodeLineReference(active_line_id));
	}

	void on_selection_changed() { update_editability(); }

	void handle_lock_state(WireEnvelope const& envelope) {
		auto state = DecodeLockState(envelope.payload_json);
		if (state.holder_id) lock_holders[state.line_id] = *state.holder_id;
		else lock_holders.erase(state.line_id);
		update_editability();
		if (context->subsGrid) context->subsGrid->Refresh(false);
		if (state.line_id == active_line_id && state.holder_id && *state.holder_id != room.member_id && state.holder_name)
			context->frame->StatusTimeout(fmt_tl("Line is read-only: locked by %s", *state.holder_name));
		if (room.lock_enabled && state.line_id == active_line_id && !state.holder_id && phase == Phase::joined)
			send("lock_request", EncodeLineReference(active_line_id));
	}

	void handle_presence(WireEnvelope const& envelope) {
		presence = DecodePresence(envelope.payload_json);
		if (context->subsGrid) context->subsGrid->Refresh(false);
	}

	void handle_maintenance_state(WireEnvelope const& envelope) {
		maintenance = DecodeMaintenanceState(envelope.payload_json);
		lock_holders.clear();
		update_editability();
		if (!maintenance.active) {
			context->frame->SetCollaborationBanner({});
			if (!active_line_id.empty()) send("lock_request", EncodeLineReference(active_line_id));
			return;
		}
		bool owned = maintenance.holder_id && *maintenance.holder_id == room.member_id;
		wxString message = owned
			? _("Collaboration maintenance mode is active. Other members are frozen.")
			: fmt_tl("Room is frozen for maintenance by %s.", maintenance.holder_name ? *maintenance.holder_name : std::string("another member"));
		if (owned && maintenance.cancel_requested_name)
			message += fmt_tl(" %s requested cancellation.", *maintenance.cancel_requested_name);
		context->frame->SetCollaborationBanner(message);
	}

	void handle_applied_batch(WireEnvelope const& envelope) {
		auto batch = DecodeAppliedBatch(envelope.payload_json);
		auto before = sync.Confirmed().snapshot;
		if (batch.actor_id == room.member_id && room.lock_enabled) {
			for (auto const& operation : batch.operations) {
				if (operation.operation.kind == OperationKind::Insert || operation.operation.kind == OperationKind::Restore)
					lock_holders[operation.operation.line_id] = room.member_id;
				else if (operation.operation.kind == OperationKind::Delete)
					lock_holders.erase(operation.operation.line_id);
			}
		}
		auto result = sync.ApplyBatch(batch, envelope.room_revision, room.member_id);
		if (result.own_batch_confirmed) {
			auto after = sync.Confirmed().snapshot;
			if (history_action && history_action->batch_id == batch.batch_id) {
				if (history_action->undo && !undo_history.empty()) {
					auto entry = std::move(undo_history.back());
					undo_history.pop_back();
					entry.expected = after;
					redo_history.push_back(std::move(entry));
				}
				else if (!history_action->undo && !redo_history.empty()) {
					auto entry = std::move(redo_history.back());
					redo_history.pop_back();
					entry.expected = after;
					undo_history.push_back(std::move(entry));
				}
				history_action.reset();
			}
			else {
				undo_history.push_back({std::move(before), after, after});
				if (undo_history.size() > 100) undo_history.erase(undo_history.begin());
				redo_history.clear();
			}
		}
		revision = sync.Revision();
		if (result.status == SyncApplyStatus::revision_gap) {
			context->frame->StatusTimeout(_("Collaboration revision gap detected; refreshing room snapshot..."));
			request_snapshot();
			return;
		}
		if (result.status == SyncApplyStatus::pending_conflict) {
			context->frame->StatusTimeout(_("A pending collaboration change conflicts with a remote update."));
			request_snapshot();
			return;
		}
		if (result.document_changed) apply_projected_snapshot(batch.id_remap);
		update_editability();
	}

	void handle_rejected_batch(WireEnvelope const& envelope) {
		auto rejection = DecodeRejectedBatch(envelope.payload_json);
		try {
			auto recovery = context->subsController->SaveCollaborationRecovery();
			context->frame->StatusTimeout(to_wx("Rejected collaboration changes saved to " + recovery.string()));
		}
		catch (std::exception const& error) {
			wxMessageBox(to_wx(std::string("Could not save rejected collaboration changes: ") + error.what()),
				_("Collaboration recovery failed"), wxOK | wxICON_ERROR, context->parent);
		}
		auto removed = sync.RejectBatch(rejection);
		if (history_action && history_action->batch_id == rejection.batch_id) history_action.reset();
		rejected_batches.insert(rejected_batches.end(), removed.begin(), removed.end());
		revision = (std::max)(revision, envelope.room_revision);
		context->frame->StatusTimeout(to_wx("Collaboration batch rejected: " + rejection.message));
		request_snapshot();
	}

	void handle_snapshot_state(WireEnvelope const& envelope) {
		std::int64_t snapshot_revision = 0;
		auto snapshot = DecodeSnapshotState(envelope.payload_json, snapshot_revision);
		if (!sync.ResetConfirmed(std::move(snapshot), snapshot_revision, true))
			throw std::runtime_error("pending collaboration changes cannot be replayed over the refreshed snapshot");
		revision = sync.Revision();
		apply_projected_snapshot();
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
		bool reconnecting = sync.IsInitialized();
		if (reconnecting) {
			if (!sync.ResetConfirmed(joined.snapshot, envelope.room_revision, true))
				throw std::runtime_error("pending collaboration changes cannot be replayed after reconnecting");
		}
		else sync.Initialize(joined.snapshot, envelope.room_revision);
		revision = sync.Revision();
		if (load_snapshot_on_join) {
			if (reconnecting) apply_projected_snapshot();
			else {
				FlagGuard applying(applying_snapshot);
				ass::LoadSnapshot(*context->ass, joined.snapshot);
				context->subsController->AdoptCollaborationSnapshot();
				if (!context->ass->Events.empty()) {
					auto* first = &context->ass->Events.front();
					context->selectionController->SetSelectionAndActive({first}, first);
				}
			}
		}
		room = std::move(joined);
		input.mode = RoomMode::join;
		load_snapshot_on_join = false;
		phase = Phase::joined;
		if (auto* active = context->selectionController->GetActiveLine()) {
			active_line_id = ass::GetMetadataValue(*context->ass, active->ExtradataIds.get(), IdExtradataKey);
		}
		update_editability();
		if (!active_line_id.empty()) send("lock_request", EncodeLineReference(active_line_id));
		last_heartbeat = std::chrono::steady_clock::now();
		persist_credentials();
		context->frame->StatusTimeout(fmt_tl("Connected to collaboration room %s as %s", input.room_name, input.nickname));
	}

	void fail_protocol(std::string const& message) {
		wxMessageBox(to_wx(message), _("Collaboration connection failed"), wxOK | wxICON_ERROR, context->parent);
		disconnect();
	}

	void handle_message(WireEnvelope const& envelope) {
		if (mutation_depth && (envelope.type == "batch_applied" || envelope.type == "batch_rejected" || envelope.type == "snapshot_state")) {
			deferred_persistent_messages.push_back(envelope);
			return;
		}
		try {
			if (envelope.type == "access_ok" && phase == Phase::waiting_access) send_room_request();
			else if (envelope.type == "room_joined" && phase == Phase::waiting_room) apply_joined_room(envelope);
			else if (envelope.type == "batch_applied" && phase == Phase::joined) handle_applied_batch(envelope);
			else if (envelope.type == "batch_rejected" && phase == Phase::joined) handle_rejected_batch(envelope);
			else if (envelope.type == "snapshot_state" && phase == Phase::joined) handle_snapshot_state(envelope);
			else if (envelope.type == "lock_state" && phase == Phase::joined) handle_lock_state(envelope);
			else if (envelope.type == "presence" && phase == Phase::joined) handle_presence(envelope);
			else if (envelope.type == "maintenance_state" && phase == Phase::joined) handle_maintenance_state(envelope);
			else if (envelope.type == "error") {
				auto error = DecodeProtocolError(envelope.payload_json);
				auto message = error.message.empty() ? error.code : error.message;
				if (phase == Phase::joined) context->frame->StatusTimeout(to_wx("Collaboration request failed: " + message));
				else fail_protocol(message);
			}
			else revision = (std::max)(revision, envelope.room_revision);
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
				update_editability();
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
		update_editability();
		transport.Start({input.server_url});
		timer.Start(50);
		context->frame->StatusTimeout(_("Connecting to collaboration server..."));
	}

public:
	explicit Impl(Context* context)
	: context(context)
	, timer(this)
	, allocator(load_allocator())
	, commit_connection(context->ass->AddCommitListener(&Impl::on_local_commit, this))
	, active_line_connection(context->selectionController->AddActiveLineListener(&Impl::on_active_line_changed, this))
	, selection_connection(context->selectionController->AddSelectionListener(&Impl::on_selection_changed, this))
	{
		Bind(wxEVT_TIMER, &Impl::on_timer, this);
	}

	~Impl() { disconnect(); }

	void create() { show_dialog(RoomMode::create); }
	void join() { show_dialog(RoomMode::join); }
	void begin_mutation() { ++mutation_depth; }
	void end_mutation() {
		if (!mutation_depth || --mutation_depth) return;
		while (!deferred_persistent_messages.empty() && phase == Phase::joined) {
			auto message = std::move(deferred_persistent_messages.front());
			deferred_persistent_messages.pop_front();
			handle_message(message);
		}
	}
	bool can_run_command(std::string const& name) const {
		if (phase != Phase::joined) return true;
		if (maintenance.active) return maintenance.holder_id && *maintenance.holder_id == room.member_id;
		if (!room.lock_enabled) return true;
		auto starts = [&](char const* prefix) { return name.compare(0, std::char_traits<char>::length(prefix), prefix) == 0; };
		if (name == "edit/line/copy" || name == "grid/line/next" || name == "grid/line/prev" ||
			name == "time/next" || name == "time/prev" || starts("grid/fold/") || starts("grid/tag")) return true;
		if (name == "edit/undo") return can_collaborative_undo();
		if (name == "edit/redo") return can_collaborative_redo();
		if (name == "edit/find_replace" ||
			starts("automation/") || name == "time/shift" || name == "time/continuous/start" ||
			name == "time/continuous/end" || name == "tool/resampleres" || name == "tool/time/postprocess" ||
			name == "tool/time/kanji") return false;
		bool mutation = starts("edit/") || starts("time/") || starts("visual/") || starts("subtitle/insert/") ||
			starts("grid/move/") || starts("grid/sort/") || name == "grid/swap" || name == "grid/line/next/create" ||
			name == "tool/styling_assistant/commit" || name == "tool/translation_assistant/commit" ||
			name == "tool/translation_assistant/insert_original";
		if (!mutation) return true;
		auto const& selection = context->selectionController->GetSelectedSet();
		if (selection.size() != 1) return false;
		auto* line = *selection.begin();
		auto id = ass::GetMetadataValue(*context->ass, line->ExtradataIds.get(), IdExtradataKey);
		auto holder = lock_holders.find(id);
		return holder != lock_holders.end() && holder->second == room.member_id;
	}
	int line_lock_state(AssDialogue const* line) const {
		if (phase != Phase::joined || !line) return 0;
		auto id = ass::GetMetadataValue(*context->ass, line->ExtradataIds.get(), IdExtradataKey);
		if (room.lock_enabled) {
			auto holder = lock_holders.find(id);
			if (holder == lock_holders.end()) return 0;
			return holder->second == room.member_id ? 1 : 2;
		}
		for (auto const& member : presence)
			if (member.line_id && *member.line_id == id) return member.member_id == room.member_id ? 1 : 2;
		return 0;
	}
	void request_maintenance() { if (phase == Phase::joined) send("maintenance_request", "{}"); }
	void release_maintenance() { if (phase == Phase::joined) send("maintenance_release", "{}"); }
	void request_maintenance_cancel() { if (phase == Phase::joined) send("maintenance_cancel_request", "{}"); }
	void force_maintenance_cancel() { if (phase == Phase::joined) send("maintenance_cancel_force", "{}"); }
	bool maintenance_active() const { return phase == Phase::joined && maintenance.active; }
	bool maintenance_owned() const { return maintenance_active() && maintenance.holder_id && *maintenance.holder_id == room.member_id; }
	bool can_collaborative_undo() const { return phase == Phase::joined && !history_action && sync.Pending().empty() && !undo_history.empty(); }
	bool can_collaborative_redo() const { return phase == Phase::joined && !history_action && sync.Pending().empty() && !redo_history.empty(); }
	void apply_history(bool undo) {
		auto& source = undo ? undo_history : redo_history;
		if (source.empty() || history_action || !sync.Pending().empty()) return;
		auto const& entry = source.back();
		auto const& target = undo ? entry.undo_target : entry.redo_target;
		std::string error;
		auto operations = BuildSelectiveTransition(sync.Confirmed(), entry.expected, target, &error);
		if (operations.empty()) {
			context->frame->StatusTimeout(to_wx("Collaborative undo/redo refused: " + (error.empty() ? std::string("nothing can be changed safely") : error)));
			return;
		}
		if (!maintenance_owned() && room.lock_enabled) {
			for (auto const& operation : operations) {
				if (operation.kind == OperationKind::Restore || operation.kind == OperationKind::Insert ||
					operation.kind == OperationKind::ReplaceStyles || operation.kind == OperationKind::ReplaceScriptInfo) continue;
				auto holder = lock_holders.find(operation.line_id);
				if (holder == lock_holders.end() || holder->second != room.member_id) {
					context->frame->StatusTimeout(_("Collaborative undo/redo refused because a target line is not locked by you."));
					return;
				}
			}
		}
		DocumentState target_state = sync.Confirmed();
		if (!ApplyOperations(target_state, operations, &error)) {
			context->frame->StatusTimeout(to_wx("Collaborative undo/redo refused: " + error));
			return;
		}
		auto batch = sync.QueueLocalSnapshot(mint_batch_id(), target_state.snapshot);
		history_action = HistoryAction{batch.batch_id, undo};
		send("submit_batch", EncodeSubmitBatch(batch));
		apply_projected_snapshot();
	}

	void disconnect() {
		timer.Stop();
		transport.Stop();
		input.access_password.assign(input.access_password.size(), '\0');
		input.room_password.assign(input.room_password.size(), '\0');
		input = ConnectionInput{};
		create_snapshot = Snapshot{};
		room = RoomJoined{};
		sync = SyncState{};
		rejected_batches.clear();
		lock_holders.clear();
		presence.clear();
		maintenance = MaintenanceStateMessage{};
		undo_history.clear();
		redo_history.clear();
		history_action.reset();
		deferred_persistent_messages.clear();
		mutation_depth = 0;
		active_line_id.clear();
		phase = Phase::idle;
		revision = 0;
		update_editability();
		context->frame->SetCollaborationBanner({});
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
void CollaborationController::BeginMutationGuard() { impl->begin_mutation(); }
void CollaborationController::EndMutationGuard() { impl->end_mutation(); }
bool CollaborationController::CanRunCommand(std::string const& command_name) const { return impl->can_run_command(command_name); }
int CollaborationController::LineLockState(AssDialogue const* line) const { return impl->line_lock_state(line); }
void CollaborationController::RequestMaintenance() { impl->request_maintenance(); }
void CollaborationController::ReleaseMaintenance() { impl->release_maintenance(); }
void CollaborationController::RequestMaintenanceCancel() { impl->request_maintenance_cancel(); }
void CollaborationController::ForceMaintenanceCancel() { impl->force_maintenance_cancel(); }
bool CollaborationController::MaintenanceActive() const { return impl->maintenance_active(); }
bool CollaborationController::MaintenanceOwned() const { return impl->maintenance_owned(); }
bool CollaborationController::CanCollaborativeUndo() const { return impl->can_collaborative_undo(); }
bool CollaborationController::CanCollaborativeRedo() const { return impl->can_collaborative_redo(); }
void CollaborationController::CollaborativeUndo() { impl->apply_history(true); }
void CollaborationController::CollaborativeRedo() { impl->apply_history(false); }

} }
