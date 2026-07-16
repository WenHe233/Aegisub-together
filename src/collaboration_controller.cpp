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
#include "search_replace_engine.h"
#include "subs_edit_box.h"
#include "subs_controller.h"

#include <libaegisub/collaboration_room.h>
#include <libaegisub/collaboration_sync.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/signal.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iterator>
#include <random>
#include <sstream>
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

enum class CommentAction { none, create, accept, reject, resolve };

struct CommentDialogResult {
	CommentAction action = CommentAction::none;
	std::string comment_id;
	std::string body;
	std::optional<std::string> suggested_text;
};

class TransportFailureDialog final : public wxDialog {
	wxTextCtrl* diagnostics;

public:
	TransportFailureDialog(wxWindow* parent, std::string const& detail, bool create_may_have_completed)
	: wxDialog(parent, wxID_ANY, _("Collaboration connection failed"), wxDefaultPosition, wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
		auto* root = new wxBoxSizer(wxVERTICAL);
		auto message = create_may_have_completed
			? _("The connection failed before Aegisub received the room confirmation. The room may already have been created; try joining the same room instead of creating it again.")
			: _("The connection failed before Aegisub joined the room.");
		root->Add(new wxStaticText(this, wxID_ANY, message), wxSizerFlags().Expand().Border(wxALL, 10));
		diagnostics = new wxTextCtrl(this, wxID_ANY, to_wx(detail), wxDefaultPosition, wxSize(620, 180),
			wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
		root->Add(diagnostics, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT, 10));
		auto* buttons = new wxStdDialogButtonSizer;
		auto* copy = new wxButton(this, wxID_COPY, _("Copy diagnostics"));
		buttons->AddButton(copy);
		buttons->AddButton(new wxButton(this, wxID_OK));
		buttons->Realize();
		root->Add(buttons, wxSizerFlags().Expand().Border(wxALL, 10));
		SetSizerAndFit(root);
		SetMinSize(wxSize(560, 300));
		copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (wxTheClipboard->Open()) {
				wxTheClipboard->SetData(new wxTextDataObject(diagnostics->GetValue()));
				wxTheClipboard->Close();
			}
		});
	}
};

wxString comment_state_label(std::string const& state) {
	if (state == "open") return _("Open");
	if (state == "accepted") return _("Accepted");
	if (state == "rejected") return _("Rejected");
	if (state == "resolved") return _("Resolved");
	return to_wx(state);
}

class CommentsDialog final : public wxDialog {
	std::vector<Comment> comments;
	wxListBox* list;
	wxTextCtrl* detail;
	wxTextCtrl* body;
	wxCheckBox* suggest;
	wxTextCtrl* suggestion;
	wxButton* accept;
	wxButton* reject;
	wxButton* resolve;
	bool writable;
	bool can_accept;
	CommentDialogResult result;

	void update_selection(wxCommandEvent&) {
		auto selected = list->GetSelection();
		bool open = selected != wxNOT_FOUND && comments[static_cast<std::size_t>(selected)].state == "open";
		accept->Enable(writable && can_accept && open && comments[static_cast<std::size_t>(selected)].suggested_text.has_value());
		reject->Enable(writable && open);
		resolve->Enable(writable && open);
		if (selected == wxNOT_FOUND) { detail->Clear(); return; }
		auto const& comment = comments[static_cast<std::size_t>(selected)];
		wxString text = fmt_tl("Author: %s\nState: %s\nCreated: %s\nBased on line version: %d\n\n%s",
			comment.author_name, comment_state_label(comment.state), comment.created_at, comment.base_line_version, comment.body);
		if (comment.suggested_text) text += fmt_tl("\n\nSuggested text:\n%s", *comment.suggested_text);
		detail->SetValue(text);
	}

	void choose(CommentAction action) {
		auto selected = list->GetSelection();
		if (selected == wxNOT_FOUND) return;
		result.action = action;
		result.comment_id = comments[static_cast<std::size_t>(selected)].comment_id;
		EndModal(wxID_OK);
	}

public:
	CommentsDialog(wxWindow* parent, std::vector<Comment> comments, std::string const& current_text, bool writable, bool can_accept)
	: wxDialog(parent, wxID_ANY, _("Line comments and suggestions"), wxDefaultPosition, wxSize(680, 620), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, comments(std::move(comments))
	, list(new wxListBox(this, wxID_ANY))
	, detail(new wxTextCtrl(this, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY))
	, body(new wxTextCtrl(this, wxID_ANY, {}, wxDefaultPosition, wxSize(-1, 80), wxTE_MULTILINE))
	, suggest(new wxCheckBox(this, wxID_ANY, _("Include a suggested subtitle text")))
	, suggestion(new wxTextCtrl(this, wxID_ANY, to_wx(current_text), wxDefaultPosition, wxSize(-1, 80), wxTE_MULTILINE))
	, accept(new wxButton(this, wxID_ANY, _("Accept suggestion")))
	, reject(new wxButton(this, wxID_ANY, _("Reject")))
	, resolve(new wxButton(this, wxID_ANY, _("Resolve")))
	, writable(writable)
	, can_accept(can_accept)
	{
		for (auto const& comment : this->comments) {
			auto preview = to_wx(comment.body);
			if (preview.length() > 70) preview = preview.Left(67) + "...";
			list->Append(fmt_tl("[%s] %s: %s", comment_state_label(comment.state), comment.author_name, preview));
		}
		auto root = new wxBoxSizer(wxVERTICAL);
		root->Add(new wxStaticText(this, wxID_ANY, _("Comments on this line:")), 0, wxLEFT | wxRIGHT | wxTOP, 10);
		root->Add(list, 1, wxEXPAND | wxALL, 10);
		root->Add(detail, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
		auto state_buttons = new wxBoxSizer(wxHORIZONTAL);
		state_buttons->Add(accept, 0, wxRIGHT, 6);
		state_buttons->Add(reject, 0, wxRIGHT, 6);
		state_buttons->Add(resolve, 0);
		root->Add(state_buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
		root->Add(new wxStaticText(this, wxID_ANY, _("New comment:")), 0, wxLEFT | wxRIGHT, 10);
		root->Add(body, 0, wxEXPAND | wxALL, 10);
		root->Add(suggest, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
		root->Add(suggestion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
		auto bottom = new wxBoxSizer(wxHORIZONTAL);
		auto add = new wxButton(this, wxID_ADD, _("Add comment"));
		bottom->Add(add, 0, wxRIGHT, 8);
		bottom->AddStretchSpacer();
		bottom->Add(new wxButton(this, wxID_CANCEL, _("Close")), 0);
		root->Add(bottom, 0, wxEXPAND | wxALL, 10);
		SetSizer(root);
		body->Enable(writable);
		suggest->Enable(writable);
		suggestion->Enable(writable);
		add->Enable(writable);
		accept->Enable(false);
		reject->Enable(false);
		resolve->Enable(false);
		list->Bind(wxEVT_LISTBOX, &CommentsDialog::update_selection, this);
		add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (body->GetValue().empty()) { wxMessageBox(_("A comment message is required."), _("Cannot add comment"), wxOK | wxICON_WARNING, this); return; }
			result.action = CommentAction::create;
			result.body = from_wx(body->GetValue());
			if (suggest->GetValue()) result.suggested_text = from_wx(suggestion->GetValue());
			EndModal(wxID_OK);
		});
		accept->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { choose(CommentAction::accept); });
		reject->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { choose(CommentAction::reject); });
		resolve->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { choose(CommentAction::resolve); });
		if (!this->comments.empty()) {
			list->SetSelection(0);
			wxCommandEvent event;
			update_selection(event);
		}
	}

	CommentDialogResult const& Result() const { return result; }
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
		try {
			ParseCollaborationServerUrl(server_value);
		}
		catch (std::exception const&) {
			wxMessageBox(_("The collaboration server address must be a valid ws:// or wss:// URL without credentials or a fragment."), _("Invalid server"), wxOK | wxICON_ERROR, this);
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
	std::unordered_map<std::string, std::string> lock_holder_names;
	std::vector<std::string> desired_lock_ids;
	std::optional<std::string> desired_active_line_id;
	std::int64_t next_lock_generation = 1;
	std::int64_t latest_lock_generation = 0;
	std::chrono::steady_clock::time_point lock_debounce_at;
	bool lock_request_scheduled = false;
	bool lock_request_inflight = false;
	bool lock_selection_too_large = false;
	std::vector<PresenceMember> presence;
	MaintenanceStateMessage maintenance;
	bool offline_session = false;
	OfflineJournal offline_journal;
	agi::fs::path offline_journal_path;
	std::optional<Snapshot> reconciliation_target;
	std::string reconciliation_batch_id;
	std::optional<SearchReplaceSettings> pending_global_replace;
	std::string global_replace_batch_id;
	struct HistoryEntry { Snapshot undo_target; Snapshot redo_target; Snapshot expected; };
	struct HistoryAction { std::string batch_id; bool undo = true; };
	std::vector<HistoryEntry> undo_history;
	std::vector<HistoryEntry> redo_history;
	std::optional<HistoryAction> history_action;
	std::vector<PendingBatch> rejected_batches;
	std::deque<WireEnvelope> deferred_persistent_messages;
	unsigned mutation_depth = 0;
	bool transport_failure_reported = false;
	bool joined_once = false;
	bool create_request_sent = false;
	agi::signal::Connection commit_connection;
	agi::signal::Connection active_line_connection;
	agi::signal::Connection selection_connection;

	std::vector<std::string> selected_line_ids() const {
		std::vector<std::string> line_ids;
		line_ids.reserve(context->selectionController->GetSelectedSet().size());
		for (auto const* line : context->selectionController->GetSelectedSet()) {
			auto id = ass::GetMetadataValue(*context->ass, line->ExtradataIds.get(), IdExtradataKey);
			if (!id.empty()) line_ids.push_back(std::move(id));
		}
		std::sort(line_ids.begin(), line_ids.end());
		line_ids.erase(std::unique(line_ids.begin(), line_ids.end()), line_ids.end());
		return line_ids;
	}

	bool owns_selected_lock_set() const {
		auto line_ids = selected_line_ids();
		return OwnsCompleteLockSet(line_ids, lock_holders, room.member_id, lock_request_inflight || lock_request_scheduled);
	}

	void update_editability() {
		if (!context->subsEditBox) return;
		bool owns_maintenance = phase == Phase::joined && maintenance.active && maintenance.holder_id && *maintenance.holder_id == room.member_id;
		bool editable = phase == Phase::idle || (offline_session && phase != Phase::joined) || owns_maintenance ||
			(phase == Phase::joined && !maintenance.active && !room.lock_enabled);
		if (reconciliation_target) editable = false;
		if (!editable && phase == Phase::joined && !maintenance.active) editable = owns_selected_lock_set();
		context->subsEditBox->SetCollaborationEditable(editable);
	}

	void schedule_lock_set_request() {
		if (phase != Phase::joined) return;
		desired_lock_ids = selected_line_ids();
		desired_active_line_id = active_line_id.empty() ? std::nullopt : std::optional<std::string>(active_line_id);
		lock_selection_too_large = desired_lock_ids.size() > MaximumLockSetSize;
		if (lock_selection_too_large) {
			desired_lock_ids.clear();
			context->frame->StatusTimeout(_("More than 10,000 lines are selected. Use maintenance mode for this operation."));
		}
		lock_request_scheduled = true;
		lock_request_inflight = true;
		lock_debounce_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
		update_editability();
	}

	void send_scheduled_lock_set() {
		if (!lock_request_scheduled || phase != Phase::joined || std::chrono::steady_clock::now() < lock_debounce_at) return;
		lock_request_scheduled = false;
		latest_lock_generation = next_lock_generation++;
		if (!send("lock_set_request", EncodeLockSetRequest(desired_lock_ids, desired_active_line_id, latest_lock_generation)))
			lock_request_inflight = false;
		update_editability();
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
			if (send("create_room", EncodeCreateRoom(request))) {
				create_request_sent = true;
				phase = Phase::waiting_room;
			}
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

	agi::fs::path journal_path(ConnectionInput const& selected) const {
		std::uint64_t hash = 1469598103934665603ULL;
		for (unsigned char value : selected.server_url + "\n" + selected.room_name) {
			hash ^= value;
			hash *= 1099511628211ULL;
		}
		std::ostringstream name;
		name << "offline-" << std::hex << std::setw(16) << std::setfill('0') << hash << ".json";
		return context->path->Decode("?user/collaboration") / name.str();
	}

	void persist_offline_journal() {
		if (offline_journal_path.empty()) return;
		agi::fs::CreateDirectory(offline_journal_path.parent_path());
		agi::io::Save output(offline_journal_path);
		output.Get() << EncodeOfflineJournal(offline_journal);
	}

	void remove_offline_journal() {
		if (!offline_journal_path.empty() && agi::fs::FileExists(offline_journal_path)) agi::fs::Remove(offline_journal_path);
	}

	void capture_offline_document() {
		SanitizeContext sanitize;
		for (auto const& line : offline_journal.baseline.lines) sanitize.known_live_ids.insert(line.id);
		for (auto const& line : offline_journal.local.lines) sanitize.known_live_ids.insert(line.id);
		auto cleaned = ass::SanitizeFileMetadata(*context->ass, *allocator, sanitize);
		if (cleaned.changed()) OPT_SET("Collaboration/Next ID")->SetString(std::to_string(allocator->NextCounter()));
		std::unordered_map<std::string, std::int64_t> versions;
		for (auto const& line : offline_journal.baseline.lines) versions[line.id] = line.version;
		for (auto const& line : offline_journal.local.lines) versions[line.id] = line.version;
		offline_journal.local = ass::CaptureSnapshot(*context->ass, versions,
			offline_journal.baseline.styles_version, offline_journal.baseline.script_info_version);
		offline_journal.local.comments = offline_journal.baseline.comments;
		persist_offline_journal();
	}

	void enter_offline_mode() {
		if (!sync.IsInitialized()) return;
		if (!offline_session) {
			offline_session = true;
			offline_journal_path = journal_path(input);
			offline_journal.base_revision = sync.Revision();
			offline_journal.baseline = sync.Confirmed().snapshot;
			offline_journal.local = sync.Projected().snapshot;
			offline_journal.pending = sync.Pending();
			try { capture_offline_document(); }
			catch (std::exception const& error) {
				context->frame->StatusTimeout(fmt_tl("Could not persist offline collaboration state: %s", error.what()));
			}
		}
		lock_holders.clear();
		lock_holder_names.clear();
		lock_request_inflight = false;
		lock_request_scheduled = false;
		presence.clear();
		maintenance = MaintenanceStateMessage{};
		phase = Phase::connecting;
		context->frame->SetCollaborationBanner(_("Connection lost. Offline edits are being saved and will be reconciled after reconnecting."));
		update_editability();
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
		if (applying_snapshot) return;
		if (offline_session && phase != Phase::joined) {
			try { capture_offline_document(); }
			catch (std::exception const& error) {
				context->frame->StatusTimeout(fmt_tl("Offline collaboration edit could not be saved: %s", error.what()));
			}
			return;
		}
		if (phase != Phase::joined || !sync.IsInitialized() || reconciliation_target) return;
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
			if (!batch.operations.empty()) {
				if (pending_global_replace) global_replace_batch_id = batch.batch_id;
				send("submit_batch", EncodeSubmitBatch(batch));
			}
		}
		catch (std::exception const& error) {
			context->frame->StatusTimeout(fmt_tl("Collaboration change could not be queued: %s", error.what()));
			request_snapshot();
		}
	}

	void on_active_line_changed(AssDialogue* line) {
		auto next = line ? ass::GetMetadataValue(*context->ass, line->ExtradataIds.get(), IdExtradataKey) : std::string{};
		if (active_line_id == next) return;
		active_line_id = std::move(next);
		update_editability();
		show_active_line_status();
		schedule_lock_set_request();
	}

	void on_selection_changed() {
		if (auto* active = context->selectionController->GetActiveLine())
			active_line_id = ass::GetMetadataValue(*context->ass, active->ExtradataIds.get(), IdExtradataKey);
		else active_line_id.clear();
		update_editability();
		schedule_lock_set_request();
	}

	void handle_lock_set_state(WireEnvelope const& envelope) {
		auto state = DecodeLockSetState(envelope.payload_json);
		if (state.member_id == room.member_id && state.generation > 0 && state.generation < latest_lock_generation) return;
		for (auto holder = lock_holders.begin(); holder != lock_holders.end();) {
			if (holder->second == state.member_id) {
				lock_holder_names.erase(holder->first);
				holder = lock_holders.erase(holder);
			}
			else ++holder;
		}
		for (auto const& line_id : state.line_ids) {
			lock_holders[line_id] = state.member_id;
			lock_holder_names[line_id] = state.member_name;
		}
		if (state.member_id == room.member_id) {
			lock_request_inflight = false;
			if (!state.granted && !state.conflicts.empty()) {
				auto const& conflict = state.conflicts.front();
				context->frame->StatusTimeout(fmt_tl("Selected lines are read-only: %s is editing line %s", conflict.holder_name, conflict.line_id));
			}
		}
		update_editability();
		show_active_line_status();
		if (context->subsGrid) context->subsGrid->Refresh(false);
	}

	void handle_presence(WireEnvelope const& envelope) {
		presence = DecodePresence(envelope.payload_json);
		show_active_line_status();
		if (context->subsGrid) context->subsGrid->Refresh(false);
	}

	void handle_comment_change(WireEnvelope const& envelope) {
		auto changed = DecodeCommentChanged(envelope.payload_json);
		auto result = sync.ApplyCommentChange(changed.comment, changed.line, envelope.room_revision);
		revision = sync.Revision();
		if (result.status == SyncApplyStatus::revision_gap) {
			context->frame->StatusTimeout(_("Collaboration comment revision gap detected; refreshing room snapshot..."));
			request_snapshot();
			return;
		}
		if (result.status == SyncApplyStatus::pending_conflict) {
			context->frame->StatusTimeout(_("An accepted suggestion conflicts with a pending local change; refreshing room snapshot..."));
			request_snapshot();
			return;
		}
		if (result.document_changed) apply_projected_snapshot();
		if (context->subsGrid) context->subsGrid->Refresh(false);
	}

	void run_pending_global_replace() {
		if (!pending_global_replace || !maintenance.active || !maintenance.holder_id || *maintenance.holder_id != room.member_id) return;
		global_replace_batch_id.clear();
		context->search->Configure(*pending_global_replace);
		try {
			context->search->ReplaceAll();
		}
		catch (std::exception const& error) {
			pending_global_replace.reset();
			send("maintenance_release", "{}");
			wxMessageBox(to_wx(error.what()), _("Replace failed"), wxOK | wxICON_ERROR, context->parent);
			return;
		}
		if (global_replace_batch_id.empty()) {
			pending_global_replace.reset();
			send("maintenance_release", "{}");
		}
	}

	void handle_maintenance_state(WireEnvelope const& envelope) {
		maintenance = DecodeMaintenanceState(envelope.payload_json);
		lock_holders.clear();
		lock_holder_names.clear();
		lock_request_inflight = false;
		lock_request_scheduled = false;
		update_editability();
		if (pending_global_replace) {
			run_pending_global_replace();
			return;
		}
		if (reconciliation_target) {
			bool owned = maintenance.active && maintenance.holder_id && *maintenance.holder_id == room.member_id;
			if (owned && reconciliation_batch_id.empty()) {
				OfflineMergeResolution resolution;
				if (!resolve_offline_conflicts(sync.Confirmed().snapshot, resolution)) {
					send("maintenance_release", "{}");
					context->frame->StatusTimeout(_("Offline reconciliation was cancelled; the journal was preserved."));
					disconnect();
					return;
				}
				*reconciliation_target = MergeOfflineSnapshots(offline_journal.baseline, offline_journal.local,
					sync.Confirmed().snapshot, &resolution).merged;
				auto batch = sync.QueueLocalSnapshot(mint_batch_id(), *reconciliation_target);
				if (batch.operations.empty()) {
					reconciliation_target.reset();
					offline_session = false;
					remove_offline_journal();
					send("maintenance_release", "{}");
					apply_projected_snapshot();
				}
				else {
					reconciliation_batch_id = batch.batch_id;
					send("submit_batch", EncodeSubmitBatch(batch));
					apply_projected_snapshot();
				}
			}
			context->frame->SetCollaborationBanner(owned
				? _("Reconciling offline edits atomically; editing is temporarily frozen.")
				: _("Waiting for room maintenance mode to reconcile offline edits."));
			return;
		}
		if (!maintenance.active) {
			context->frame->SetCollaborationBanner({});
			return;
		}
		bool owned = maintenance.holder_id && *maintenance.holder_id == room.member_id;
		wxString message = owned
			? _("Collaboration maintenance mode is active. Other members are frozen.")
			: fmt_tl("Room is frozen for maintenance by %s.", maintenance.holder_name ? *maintenance.holder_name : from_wx(_("another member")));
		if (owned && maintenance.cancel_requested_name)
			message += fmt_tl(" %s requested cancellation.", *maintenance.cancel_requested_name);
		context->frame->SetCollaborationBanner(message);
	}

	void handle_applied_batch(WireEnvelope const& envelope) {
		auto batch = DecodeAppliedBatch(envelope.payload_json);
		bool reconciliation_ack = !reconciliation_batch_id.empty() && batch.batch_id == reconciliation_batch_id;
		bool global_replace_ack = !global_replace_batch_id.empty() && batch.batch_id == global_replace_batch_id;
		auto before = sync.Confirmed().snapshot;
		if (room.lock_enabled) {
			for (auto const& operation : batch.operations) {
				if (operation.operation.kind == OperationKind::Insert || operation.operation.kind == OperationKind::Restore) {
					lock_holders[operation.operation.line_id] = batch.actor_id;
					lock_holder_names[operation.operation.line_id] = batch.actor_id == room.member_id ? input.nickname : from_wx(_("another member"));
				}
				else if (operation.operation.kind == OperationKind::Delete) {
					lock_holders.erase(operation.operation.line_id);
					lock_holder_names.erase(operation.operation.line_id);
				}
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
		if (result.document_changed && (!reconciliation_target || !reconciliation_batch_id.empty()))
			apply_projected_snapshot(batch.id_remap);
		if (reconciliation_ack && result.own_batch_confirmed) {
			reconciliation_batch_id.clear();
			reconciliation_target.reset();
			offline_session = false;
			offline_journal = OfflineJournal{};
			remove_offline_journal();
			send("maintenance_release", "{}");
			context->frame->SetCollaborationBanner({});
			context->frame->StatusTimeout(_("Offline collaboration edits were reconciled successfully."));
		}
		if (global_replace_ack && result.own_batch_confirmed) {
			global_replace_batch_id.clear();
			pending_global_replace.reset();
			send("maintenance_release", "{}");
		}
		update_editability();
	}

	void handle_rejected_batch(WireEnvelope const& envelope) {
		auto rejection = DecodeRejectedBatch(envelope.payload_json);
		try {
			auto recovery = context->subsController->SaveCollaborationRecovery();
			context->frame->StatusTimeout(fmt_tl("Rejected collaboration changes saved to %s", recovery.string()));
		}
		catch (std::exception const& error) {
			wxMessageBox(fmt_tl("Could not save rejected collaboration changes: %s", error.what()),
				_("Collaboration recovery failed"), wxOK | wxICON_ERROR, context->parent);
		}
		auto removed = sync.RejectBatch(rejection);
		if (history_action && history_action->batch_id == rejection.batch_id) history_action.reset();
		if (reconciliation_batch_id == rejection.batch_id) {
			reconciliation_batch_id.clear();
			reconciliation_target.reset();
			send("maintenance_release", "{}");
		}
		if (global_replace_batch_id == rejection.batch_id) {
			global_replace_batch_id.clear();
			pending_global_replace.reset();
			send("maintenance_release", "{}");
		}
		rejected_batches.insert(rejected_batches.end(), removed.begin(), removed.end());
		revision = (std::max)(revision, envelope.room_revision);
		context->frame->StatusTimeout(fmt_tl("Collaboration batch rejected: %s", rejection.message));
		request_snapshot();
	}

	void handle_snapshot_state(WireEnvelope const& envelope) {
		std::int64_t snapshot_revision = 0;
		auto snapshot = DecodeSnapshotState(envelope.payload_json, snapshot_revision);
		if (offline_session) {
			prepare_offline_reconciliation(std::move(snapshot), snapshot_revision);
			return;
		}
		if (!sync.ResetConfirmed(std::move(snapshot), snapshot_revision, true))
			throw std::runtime_error("pending collaboration changes cannot be replayed over the refreshed snapshot");
		revision = sync.Revision();
		apply_projected_snapshot();
	}

	bool resolve_offline_conflicts(Snapshot const& server, OfflineMergeResolution& resolution) {
		auto detected = MergeOfflineSnapshots(offline_journal.baseline, offline_journal.local, server);
		for (auto const& conflict : detected.conflicts) {
			wxString subject;
			if (conflict.kind == OfflineConflictKind::Line) subject = fmt_tl("subtitle line %s", conflict.line_id);
			else if (conflict.kind == OfflineConflictKind::Styles) subject = _("the Styles section");
			else subject = _("the Script Info section");
			wxMessageDialog dialog(context->parent,
				fmt_tl("Both the server and your offline copy changed %s. Which version should be kept?", subject),
				_("Resolve offline collaboration conflict"), wxYES_NO | wxCANCEL | wxICON_WARNING);
			dialog.SetYesNoLabels(_("Keep local"), _("Use server"));
			auto choice = dialog.ShowModal();
			if (choice == wxID_CANCEL) return false;
			if (choice != wxID_YES) continue;
			if (conflict.kind == OfflineConflictKind::Line) resolution.local_lines.insert(conflict.line_id);
			else if (conflict.kind == OfflineConflictKind::Styles) resolution.local_styles = true;
			else resolution.local_script_info = true;
		}
		return true;
	}

	void prepare_offline_reconciliation(Snapshot server, std::int64_t server_revision) {
		auto detected = MergeOfflineSnapshots(offline_journal.baseline, offline_journal.local, server);
		sync.Initialize(std::move(server), server_revision);
		sync.RememberTombstonesFrom(offline_journal.baseline);
		revision = sync.Revision();
		auto operations = DiffSnapshots(sync.Confirmed(), detected.merged);
		if (operations.empty() && detected.conflicts.empty()) {
			offline_session = false;
			offline_journal = OfflineJournal{};
			remove_offline_journal();
			apply_projected_snapshot();
			context->frame->SetCollaborationBanner({});
			context->frame->StatusTimeout(_("Reconnected; no offline changes needed to be submitted."));
			return;
		}
		reconciliation_target = std::move(detected.merged);
		reconciliation_batch_id.clear();
		context->frame->SetCollaborationBanner(_("Waiting for room maintenance mode to reconcile offline edits."));
		send("maintenance_request", "{}");
		update_editability();
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
		if (offline_session) {
			room = joined;
			input.mode = RoomMode::join;
			load_snapshot_on_join = false;
			phase = Phase::joined;
			last_heartbeat = std::chrono::steady_clock::now();
			persist_credentials();
			prepare_offline_reconciliation(std::move(joined.snapshot), envelope.room_revision);
			return;
		}
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
		joined_once = true;
		if (auto* active = context->selectionController->GetActiveLine()) {
			active_line_id = ass::GetMetadataValue(*context->ass, active->ExtradataIds.get(), IdExtradataKey);
		}
		lock_holders.clear();
		lock_holder_names.clear();
		for (auto const& state : room.lock_sets) {
			for (auto const& line_id : state.line_ids) {
				lock_holders[line_id] = state.member_id;
				lock_holder_names[line_id] = state.member_name;
			}
		}
		presence = room.presence;
		update_editability();
		schedule_lock_set_request();
		last_heartbeat = std::chrono::steady_clock::now();
		persist_credentials();
		context->frame->StatusTimeout(fmt_tl("Connected to collaboration room %s as %s", input.room_name, input.nickname));
	}

	void fail_protocol(std::string const& message) {
		wxMessageBox(to_wx(message), _("Collaboration connection failed"), wxOK | wxICON_ERROR, context->parent);
		disconnect();
	}

	void handle_message(WireEnvelope const& envelope) {
		if (mutation_depth && (envelope.type == "batch_applied" || envelope.type == "batch_rejected" || envelope.type == "snapshot_state" || envelope.type == "comment_changed")) {
			deferred_persistent_messages.push_back(envelope);
			return;
		}
		try {
			if (envelope.type == "access_ok" && phase == Phase::waiting_access) send_room_request();
			else if (envelope.type == "room_joined" && phase == Phase::waiting_room) apply_joined_room(envelope);
			else if (envelope.type == "batch_applied" && phase == Phase::joined) handle_applied_batch(envelope);
			else if (envelope.type == "batch_rejected" && phase == Phase::joined) handle_rejected_batch(envelope);
			else if (envelope.type == "snapshot_state" && phase == Phase::joined) handle_snapshot_state(envelope);
			else if (envelope.type == "lock_set_state" && phase == Phase::joined) handle_lock_set_state(envelope);
			else if (envelope.type == "presence" && phase == Phase::joined) handle_presence(envelope);
			else if (envelope.type == "maintenance_state" && phase == Phase::joined) handle_maintenance_state(envelope);
			else if (envelope.type == "comment_changed" && phase == Phase::joined) handle_comment_change(envelope);
			else if (envelope.type == "error") {
				auto error = DecodeProtocolError(envelope.payload_json);
				auto message = error.message.empty() ? error.code : error.message;
				if (phase == Phase::joined) {
					if (pending_global_replace) {
						pending_global_replace.reset();
						global_replace_batch_id.clear();
						wxMessageBox(fmt_tl("Replace all could not enter maintenance mode: %s", message), _("Replace unavailable"), wxOK | wxICON_WARNING, context->parent);
					}
					else context->frame->StatusTimeout(fmt_tl("Collaboration request failed: %s", message));
				}
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
			else if (event->type == TransportEventType::error) {
				auto detail = event->detail.empty() ? FormatTransportFailure(event->failure) : event->detail;
				LOG_E("collaboration/transport") << detail;
				if (!joined_once && !transport_failure_reported) {
					transport_failure_reported = true;
					TransportFailureDialog dialog(context->parent, detail, create_request_sent);
					dialog.ShowModal();
				}
				else context->frame->StatusTimeout(to_wx(detail));
			}
			else if (event->state == TransportState::connected) send_access_auth();
			else if (event->state == TransportState::connecting) {
				if (phase == Phase::joined) load_snapshot_on_join = true;
				phase = Phase::connecting;
				update_editability();
			}
			else if (event->state == TransportState::retry_wait) {
				auto policy = EvaluateConnectionLoss(joined_once, create_request_sent);
				if (!policy.retry) {
					if (!transport_failure_reported) {
						TransportFailureDialog dialog(context->parent,
							event->detail.empty() ? from_wx(_("The WebSocket connection ended before room_joined was received.")) : event->detail,
							policy.create_may_have_completed);
						dialog.ShowModal();
					}
					disconnect();
					return;
				}
				if (policy.enable_offline_journal) enter_offline_mode();
				context->frame->StatusTimeout(_("Collaboration connection lost; retrying while offline edits remain enabled..."));
			}
		}
		if (phase == Phase::joined && std::chrono::steady_clock::now() - last_heartbeat >= std::chrono::seconds(10)) {
			send("heartbeat", "{}");
			last_heartbeat = std::chrono::steady_clock::now();
		}
		send_scheduled_lock_set();
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
		auto confirmed_insecure_servers = OPT_GET("Collaboration/Insecure Servers Confirmed")->GetListString();
		if (RequiresInsecureServerConfirmation(selected.server_url, confirmed_insecure_servers)) {
			auto result = wxMessageBox(
				_("This ws:// connection is not encrypted. Server passwords, room passwords, subtitles, and collaboration data can be read or changed in transit. Continue only on a trusted local network."),
				_("Unencrypted collaboration connection"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, context->parent);
			if (result != wxYES) return;
			RememberInsecureServerConfirmation(selected.server_url, confirmed_insecure_servers);
			OPT_SET("Collaboration/Insecure Servers Confirmed")->SetListString(std::move(confirmed_insecure_servers));
		}
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
		offline_journal_path = journal_path(input);
		if (mode == RoomMode::join && agi::fs::FileExists(offline_journal_path)) {
			try {
				auto stream = agi::io::Open(offline_journal_path);
				std::string encoded{std::istreambuf_iterator<char>(*stream), std::istreambuf_iterator<char>()};
				auto saved = DecodeOfflineJournal(encoded);
				wxMessageDialog resume(context->parent,
					_("A saved offline collaboration journal exists for this room. Resume and reconcile those edits?"),
					_("Resume offline collaboration edits"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
				resume.SetYesNoLabels(_("Resume"), _("Discard journal"));
				auto choice = resume.ShowModal();
				if (choice == wxID_CANCEL) { input = ConnectionInput{}; return; }
				if (choice == wxID_YES) {
					offline_session = true;
					offline_journal = std::move(saved);
					FlagGuard applying(applying_snapshot);
					ass::LoadSnapshot(*context->ass, offline_journal.local);
					context->ass->Commit(_("restore offline collaboration edits"), AssFile::COMMIT_NEW);
				}
				else remove_offline_journal();
			}
			catch (std::exception const& error) {
				wxMessageBox(fmt_tl("The offline collaboration journal could not be loaded: %s", error.what()),
					_("Invalid offline journal"), wxOK | wxICON_ERROR, context->parent);
				input = ConnectionInput{};
				return;
			}
		}
		if (!input.remember_passwords) forget_credentials(input);
		load_snapshot_on_join = mode == RoomMode::join && !offline_session;
		room = RoomJoined{};
		revision = 0;
		transport_failure_reported = false;
		joined_once = false;
		create_request_sent = false;
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
		if (name == "edit/find_replace") return true;
		if (maintenance.active) return maintenance.holder_id && *maintenance.holder_id == room.member_id;
		if (!room.lock_enabled) return true;
		auto starts = [&](char const* prefix) { return name.compare(0, std::char_traits<char>::length(prefix), prefix) == 0; };
		if (name == "edit/line/copy" || name == "grid/line/next" || name == "grid/line/prev" ||
			name == "time/next" || name == "time/prev" || starts("grid/fold/") || starts("grid/tag")) return true;
		if (name == "edit/undo") return can_collaborative_undo();
		if (name == "edit/redo") return can_collaborative_redo();
		if (starts("automation/") || name == "time/shift" || name == "time/continuous/start" ||
			name == "time/continuous/end" || name == "tool/resampleres" || name == "tool/time/postprocess" ||
			name == "tool/time/kanji") return false;
		bool mutation = starts("edit/") || starts("time/") || starts("visual/") || starts("subtitle/insert/") ||
			starts("grid/move/") || starts("grid/sort/") || name == "grid/swap" || name == "grid/line/next/create" ||
			name == "tool/styling_assistant/commit" || name == "tool/translation_assistant/commit" ||
			name == "tool/translation_assistant/insert_original";
		if (!mutation) return true;
		return owns_selected_lock_set();
	}
	bool can_modify_selected_rows() const {
		if (phase != Phase::joined) return true;
		if (maintenance.active) return maintenance.holder_id && *maintenance.holder_id == room.member_id;
		return !room.lock_enabled || owns_selected_lock_set();
	}
	bool request_global_replace(SearchReplaceSettings const& settings) {
		if (phase != Phase::joined) return false;
		if (pending_global_replace || !global_replace_batch_id.empty()) {
			wxMessageBox(_("Another collaborative replace operation is still pending."), _("Replace unavailable"), wxOK | wxICON_WARNING, context->parent);
			return true;
		}
		if (maintenance.active && (!maintenance.holder_id || *maintenance.holder_id != room.member_id)) {
			wxMessageBox(fmt_tl("Maintenance mode is currently held by %s.", maintenance.holder_name ? *maintenance.holder_name : from_wx(_("another member"))),
				_("Replace unavailable"), wxOK | wxICON_WARNING, context->parent);
			return true;
		}
		pending_global_replace = settings;
		if (maintenance.active) run_pending_global_replace();
		else if (!send("maintenance_request", "{}")) pending_global_replace.reset();
		return true;
	}
	LineCollaborationState line_state(AssDialogue const* line) const {
		if (phase != Phase::joined || !line) return {};
		auto id = ass::GetMetadataValue(*context->ass, line->ExtradataIds.get(), IdExtradataKey);
		if (room.lock_enabled) {
			auto holder = lock_holders.find(id);
			if (holder == lock_holders.end()) return {};
			auto name = lock_holder_names.find(id);
			return {
				holder->second == room.member_id ? LineCollaborationKind::owned_lock : LineCollaborationKind::remote_lock,
				name == lock_holder_names.end() ? std::string{} : name->second
			};
		}
		for (auto const& member : presence) {
			if (member.member_id != room.member_id && member.line_id && *member.line_id == id)
				return {LineCollaborationKind::remote_presence, member.nickname};
		}
		return {};
	}
	void show_active_line_status() const {
		auto state = line_state(context->selectionController->GetActiveLine());
		if (state.kind == LineCollaborationKind::remote_lock && !state.holder_name.empty())
			context->frame->StatusTimeout(fmt_tl("Active line is locked by %s.", state.holder_name));
	}
	void request_maintenance() { if (phase == Phase::joined) send("maintenance_request", "{}"); }
	void release_maintenance() { if (phase == Phase::joined) send("maintenance_release", "{}"); }
	void request_maintenance_cancel() { if (phase == Phase::joined) send("maintenance_cancel_request", "{}"); }
	void force_maintenance_cancel() { if (phase == Phase::joined) send("maintenance_cancel_force", "{}"); }
	bool maintenance_active() const { return phase == Phase::joined && maintenance.active; }
	bool maintenance_owned() const { return maintenance_active() && maintenance.holder_id && *maintenance.holder_id == room.member_id; }
	int line_comment_count(AssDialogue const* line) const {
		if (!line || (!sync.IsInitialized() && !offline_session)) return 0;
		auto id = ass::GetMetadataValue(*context->ass, line->ExtradataIds.get(), IdExtradataKey);
		auto const& comments = sync.IsInitialized() ? sync.Confirmed().snapshot.comments : offline_journal.baseline.comments;
		return static_cast<int>(std::count_if(comments.begin(), comments.end(), [&](Comment const& comment) { return comment.line_id == id; }));
	}
	void show_line_comments() {
		auto* active = context->selectionController->GetActiveLine();
		if (!active || (!sync.IsInitialized() && !offline_session)) return;
		auto id = ass::GetMetadataValue(*context->ass, active->ExtradataIds.get(), IdExtradataKey);
		auto const& snapshot = sync.IsInitialized() ? sync.Confirmed().snapshot : offline_journal.baseline;
		auto line = std::find_if(snapshot.lines.begin(), snapshot.lines.end(), [&](Line const& item) { return item.id == id; });
		std::vector<Comment> comments;
		for (auto const& comment : snapshot.comments) if (comment.line_id == id) comments.push_back(comment);
		bool writable = phase == Phase::joined && line != snapshot.lines.end() && !reconciliation_target;
		bool can_accept = writable && (!room.lock_enabled || (lock_holders.count(id) && lock_holders.at(id) == room.member_id));
		CommentsDialog dialog(context->parent, std::move(comments), line == snapshot.lines.end() ? std::string{} : line->fields.text, writable, can_accept);
		if (dialog.ShowModal() != wxID_OK || !writable) return;
		auto const& result = dialog.Result();
		if (result.action == CommentAction::create) {
			send("comment_create", EncodeCommentCreate(id, line->version, result.body, result.suggested_text));
			return;
		}
		if (result.comment_id.empty()) return;
		std::string state;
		if (result.action == CommentAction::accept) state = "accepted";
		else if (result.action == CommentAction::reject) state = "rejected";
		else if (result.action == CommentAction::resolve) state = "resolved";
		if (!state.empty()) send("comment_set_state", EncodeCommentSetState(result.comment_id, state));
	}
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
			context->frame->StatusTimeout(error.empty()
				? _("Collaborative undo/redo refused: nothing can be changed safely")
				: fmt_tl("Collaborative undo/redo refused: %s", error));
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
			context->frame->StatusTimeout(fmt_tl("Collaborative undo/redo refused: %s", error));
			return;
		}
		auto batch = sync.QueueLocalSnapshot(mint_batch_id(), target_state.snapshot);
		history_action = HistoryAction{batch.batch_id, undo};
		send("submit_batch", EncodeSubmitBatch(batch));
		apply_projected_snapshot();
	}

	void disconnect() {
		timer.Stop();
		if (phase == Phase::joined) {
			WireEnvelope leave;
			leave.type = "leave_room";
			leave.request_id = request_id("leave");
			leave.room_revision = revision;
			leave.payload_json = "{}";
			transport.Stop(std::move(leave));
		}
		else transport.Stop();
		input.access_password.assign(input.access_password.size(), '\0');
		input.room_password.assign(input.room_password.size(), '\0');
		input = ConnectionInput{};
		create_snapshot = Snapshot{};
		room = RoomJoined{};
		sync = SyncState{};
		rejected_batches.clear();
		lock_holders.clear();
		lock_holder_names.clear();
		desired_lock_ids.clear();
		desired_active_line_id.reset();
		lock_request_scheduled = false;
		lock_request_inflight = false;
		lock_selection_too_large = false;
		latest_lock_generation = 0;
		presence.clear();
		maintenance = MaintenanceStateMessage{};
		offline_session = false;
		offline_journal = OfflineJournal{};
		offline_journal_path.clear();
		reconciliation_target.reset();
		reconciliation_batch_id.clear();
		pending_global_replace.reset();
		global_replace_batch_id.clear();
		undo_history.clear();
		redo_history.clear();
		history_action.reset();
		deferred_persistent_messages.clear();
		mutation_depth = 0;
		transport_failure_reported = false;
		joined_once = false;
		create_request_sent = false;
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
bool CollaborationController::CanModifySelectedRows() const { return impl->can_modify_selected_rows(); }
bool CollaborationController::RequestGlobalReplace(SearchReplaceSettings const& settings) { return impl->request_global_replace(settings); }
LineCollaborationState CollaborationController::LineState(AssDialogue const* line) const { return impl->line_state(line); }
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
int CollaborationController::LineCommentCount(AssDialogue const* line) const { return impl->line_comment_count(line); }
void CollaborationController::ShowLineComments() { impl->show_line_comments(); }

} }
