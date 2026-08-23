// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "dialog_motion.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "dialog_progress.h"
#include "include/aegisub/context.h"
#include "motion_trim_encoder.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "typesetting_motion.h"
#include "video_controller.h"
#include "video_frame.h"

#include <libaegisub/fs.h>
#include <libaegisub/format.h>
#include <libaegisub/io.h>
#include <libaegisub/path.h>
#include <libaegisub/exception.h>
#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>

#include <algorithm>
#include <limits>
#include <iterator>
#include <optional>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dialog.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/filepicker.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

namespace {

std::optional<std::string> ClipboardText() {
	if (!wxTheClipboard->Open()) return std::nullopt;
	wxTextDataObject data;
	bool ok = wxTheClipboard->GetData(data);
	wxTheClipboard->Close();
	if (!ok) return std::nullopt;
	return from_wx(data.GetText());
}

wxString TrackLabel(typesetting::motion::Track const& track, size_t required_frames) {
	wxString label = wxString::Format(track.kind == typesetting::motion::TrackKind::CornerPin ?
		_("Mocha Corner Pin - %zu frames") : _("Mocha Transform Data - %zu frames"),
		track.samples.size());
	if (track.samples.size() != required_frames)
		label += wxString::Format(_(" - selected lines require %zu frames"), required_frames);
	return label;
}

wxString ResolveDirectory(agi::Context *context, wxString directory);
std::optional<std::string> FindMochaProject(agi::Context *context, wxString const& root,
	bool own_folder, wxString const& base);

class MotionApplyDialog final : public wxDialog {
	using Track = typesetting::motion::Track;
	agi::Context *context;
	std::optional<Track> primary;
	std::optional<Track> primary_corner;
	std::optional<Track> clip;
	std::optional<Track> clip_corner;
	wxStaticText *primary_label;
	wxStaticText *primary_corner_label;
	wxStaticText *clip_label;
	wxStaticText *clip_corner_label;
	wxTextCtrl *primary_text;
	wxTextCtrl *primary_corner_text;
	wxTextCtrl *clip_text;
	wxTextCtrl *clip_corner_text;
	wxCheckBox *separate_clip;
	wxButton *clip_import;
	wxButton *primary_corner_import;
	wxButton *clip_corner_import;
	wxNotebook *data_tabs;
	wxPanel *perspective_page;
	wxSpinCtrl *reference;
	wxStaticText *clip_reference_label;
	wxSpinCtrl *clip_reference;
	wxCheckBox *linear;
	wxCheckBox *clip_only;
	wxCheckBox *map_clips;
	wxCheckBox *border;
	wxCheckBox *shadow;
	wxCheckBox *blur;
	wxCheckBox *main_x;
	wxCheckBox *main_y;
	wxCheckBox *main_scale;
	wxCheckBox *main_rotate;
	wxCheckBox *main_perspective;
	wxCheckBox *clip_x;
	wxCheckBox *clip_y;
	wxCheckBox *clip_scale;
	wxCheckBox *clip_rotate;
	wxCheckBox *clip_perspective;
	wxCheckBox *interpolate_animations;
	wxStaticBoxSizer *apply_options;
	size_t required_frames = 0;
	/// Whether the main track came out of a Mocha project rather than out of its box. It then
	/// holds the whole of a projective track already, and its box holds a note about where it came
	/// from - so there is nothing there to read back, and no Corner Pin to ask for separately.
	bool primary_from_project = false;
	/// The same for the Corner Pin box. It is filled in with a note when the project carries
	/// perspective, but left open: pasting an export over it takes it back to being ordinary data.
	bool primary_corner_from_project = false;
	/// The frames the selection covers, which is what names the shot's trim - and so its project.
	int shot_first = 0;
	int shot_last = 0;

	/// Where the trim writes, and what it calls this shot. Both read exactly as the trim reads
	/// them, so the two always agree about which shot is which.
	static wxString MochaRoot() {
		return to_wx(OPT_GET("Tool/Motion/Trim/Directory")->GetString());
	}
	wxString ShotName() const {
		wxFileName video(context->project->VideoName().wstring());
		return video.GetName() + wxString::Format("[%d-%d]", shot_first, shot_last);
	}

	bool TrackReady(std::optional<Track> const& track) const {
		return track && track->samples.size() == required_frames;
	}

	bool AllTracksReady() const {
		if (!TrackReady(primary)) return false;
		if (separate_clip->GetValue() && !TrackReady(clip)) return false;
		// A track read from a project is a Corner Pin already: its perspective is in it, so there
		// is no second one to ask for. Asking anyway left Apply greyed out with nothing the user
		// could do about it.
		if (!primary_from_project && main_perspective->GetValue() &&
			!TrackReady(primary_corner)) return false;
		return !separate_clip->GetValue() || !clip_perspective->GetValue() ||
			TrackReady(clip_corner);
	}

	std::optional<Track> ParseTrackText(std::string const& text,
		typesetting::motion::TrackKind expected_kind, std::string& error) const {
		int width = 0, height = 0;
		context->ass->GetResolution(width, height);
		return typesetting::motion::ParseMocha(text, width, height, expected_kind, error);
	}

	void UpdateApplyState() {
		if (auto apply = FindWindow(wxID_OK)) apply->Enable(AllTracksReady());
	}

	void UpdateScaleOptions() {
		bool enabled = !clip_only->GetValue() && main_scale->GetValue();
		for (auto control : {border, shadow, blur}) {
			control->Enable(enabled);
			if (!enabled) control->SetValue(false);
		}
	}

	void UpdateClipOnlyControls() {
		bool enabled = !clip_only->GetValue();
		if (!enabled && separate_clip->GetValue()) {
			separate_clip->SetValue(false);
			UpdateDataControls();
		}
		separate_clip->Enable(enabled);
		for (auto control : {interpolate_animations, linear, map_clips})
			control->Enable(enabled);
		apply_options->GetStaticBox()->Enable(enabled);
		UpdateScaleOptions();
	}

	/// Find and read the Mocha project for the selected lines.
	///
	/// True when it produced a track. A project with nothing tracked in it is not a failure and
	/// not a track: it says so and leaves every setting alone, which is what a shot that was
	/// opened and never worked on has to do.
	bool LoadMochaProject() {
		auto found = FindMochaProject(context, ResolveDirectory(context, MochaRoot()),
			OPT_GET("Tool/Motion/Trim/Own Folder")->GetBool(), ShotName());
		if (!found) return false;

		std::string text;
		try {
			auto input = agi::io::Open(agi::fs::path(*found));
			text.assign(std::istreambuf_iterator<char>(*input),
				std::istreambuf_iterator<char>());
		}
		catch (...) { return false; }

		int width = 0, height = 0;
		context->ass->GetResolution(width, height);
		std::string error;
		auto project = typesetting::motion::ParseMochaProject(text, width, height, error);
		if (!project) {
			primary_label->SetLabel(error.empty() ?
				_("The Mocha project has nothing tracked in it") : to_wx(error));
			primary_label->SetForegroundColour(
				wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
			return false;
		}

		primary = std::move(project->track);
		wxString note = wxString::Format(_("Read from the Mocha project: %s, layer %s"),
			wxFileName(to_wx(*found)).GetFullName(), to_wx(project->layer));
		primary_text->ChangeValue(note);
		primary_text->SetEditable(false);

		// What the shot actually contains, which is not the same as what the tracker was allowed
		// to look for: a channel that never moved has nothing to apply.
		main_scale->SetValue(project->has_scale);
		main_rotate->SetValue(project->has_rotation);
		main_perspective->SetValue(project->has_perspective);
		primary_from_project = true;

		// The perspective is part of what was read, so its box says so too rather than sitting
		// there empty. It stays usable: an export pasted over this takes the place of what the
		// project said, which is the way back to doing it by hand.
		if (project->has_perspective) {
			primary_corner_from_project = true;
			primary_corner_text->ChangeValue(note);
			primary_corner_text->SetEditable(false);
			primary_corner_label->SetLabel(TrackLabel(*primary, required_frames));
			primary_corner_label->SetForegroundColour(
				wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
		}
		// Its own tab, so what was just read is what the box shows - and not the Corner Pin tab,
		// which a project track has nothing to put in.
		data_tabs->SetSelection(0);
		UpdateDataControls();

		bool length_matches = primary->samples.size() == required_frames;
		primary_label->SetLabel(TrackLabel(*primary, required_frames));
		primary_label->SetForegroundColour(length_matches ?
			wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT) : wxColour(190, 45, 45));
		reference->SetRange(1, static_cast<int>(primary->samples.size()));
		reference->SetValue(1);
		UpdateScaleOptions();
		UpdateApplyState();
		Layout();
		return true;
	}

	bool ParseEditor(wxTextCtrl *editor, std::optional<Track>& destination,
		wxStaticText *label, wxSpinCtrl *reference_control,
		typesetting::motion::TrackKind expected_kind, bool report_error) {
		std::string error;
		auto text = from_wx(editor->GetValue());
		auto parsed = ParseTrackText(text, expected_kind, error);
		if (!parsed) {
			destination.reset();
			label->SetLabel(text.empty() ? _("No track data") : _("No valid track data"));
			label->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
			UpdateApplyState();
			if (report_error)
				wxMessageBox(to_wx(error), _("Motion"), wxOK | wxICON_ERROR, this);
			return false;
		}
		destination = std::move(parsed);
		bool length_matches = destination->samples.size() == required_frames;
		label->SetLabel(TrackLabel(*destination, required_frames));
		label->SetForegroundColour(length_matches ?
			wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT) : wxColour(190, 45, 45));
		int old_value = reference_control->GetValue();
		reference_control->SetRange(1, static_cast<int>(destination->samples.size()));
		reference_control->SetValue(std::clamp(old_value, 1,
			static_cast<int>(destination->samples.size())));
		UpdateApplyState();
		Layout();
		return length_matches;
	}

	void ImportClipboard(wxTextCtrl *editor, std::optional<Track>& destination,
		wxStaticText *label, wxSpinCtrl *reference_control,
		typesetting::motion::TrackKind expected_kind) {
		auto data = ClipboardText();
		if (!data || data->empty()) {
			wxMessageBox(_("There is no valid motion track on the clipboard."), _("Motion"),
				wxOK | wxICON_WARNING, this);
			return;
		}
		std::string error;
		if (!ParseTrackText(*data, expected_kind, error)) {
			wxMessageBox(_("There is no valid motion track on the clipboard."), _("Motion"),
				wxOK | wxICON_WARNING, this);
			return;
		}
		// Pasting takes over from whatever was read out of the project, so the box goes back to
		// being one the user can work in.
		if (editor == primary_text) primary_from_project = false;
		if (editor == primary_corner_text) primary_corner_from_project = false;
		editor->SetEditable(true);
		editor->ChangeValue(to_wx(*data));
		ParseEditor(editor, destination, label, reference_control, expected_kind, false);
	}

	void UpdateDataControls(bool show_perspective = false) {
		bool separate = separate_clip->GetValue();
		clip_import->Enable(separate);
		clip_text->Enable(separate);
		clip_label->Enable(separate);
		clip_reference_label->Enable(separate);
		clip_reference->Enable(separate);
		for (auto control : {clip_x, clip_y, clip_scale, clip_rotate, clip_perspective})
			control->Enable(separate);

		bool main_enabled = main_perspective->GetValue();
		bool clip_enabled = separate && clip_perspective->GetValue();
		perspective_page->Enable(main_enabled || clip_enabled);
		for (auto control : {static_cast<wxWindow *>(primary_corner_import),
			static_cast<wxWindow *>(primary_corner_text),
			static_cast<wxWindow *>(primary_corner_label)}) control->Enable(main_enabled);
		for (auto control : {static_cast<wxWindow *>(clip_corner_import),
			static_cast<wxWindow *>(clip_corner_text),
			static_cast<wxWindow *>(clip_corner_label)}) control->Enable(clip_enabled);
		if (show_perspective && (main_enabled || clip_enabled)) data_tabs->SetSelection(1);
		UpdateApplyState();
	}

	void ToggleClip(wxCommandEvent&) {
		UpdateDataControls();
	}

	void ToggleClipOnly(wxCommandEvent&) {
		UpdateClipOnlyControls();
	}

	void TogglePerspective(wxCommandEvent&) {
		if (main_perspective->GetValue()) main_rotate->SetValue(true);
		if (clip_perspective->GetValue()) clip_rotate->SetValue(true);
		UpdateDataControls(true);
	}

	void ToggleScale(wxCommandEvent&) {
		UpdateScaleOptions();
	}

	void ApplyNow(wxCommandEvent&) {
		using typesetting::motion::TrackKind;
		if (primary_from_project) {
			if (!TrackReady(primary)) {
				wxMessageBox(_("The Mocha project does not cover the frames "
					"the selected lines need."), _("Motion"), wxOK | wxICON_ERROR, this);
				return;
			}
		}
		else if (!ParseEditor(primary_text, primary, primary_label, reference,
			TrackKind::Transform, true)) return;
		if (separate_clip->GetValue() &&
			!ParseEditor(clip_text, clip, clip_label, clip_reference,
				TrackKind::Transform, true)) return;
		// Only where the box holds data of its own. Holding the project's note there is nothing to
		// read back, and the perspective is in the main track anyway.
		if (!primary_corner_from_project && main_perspective->GetValue() &&
			!ParseEditor(primary_corner_text, primary_corner, primary_corner_label,
				reference, TrackKind::CornerPin, true)) {
			data_tabs->SetSelection(1);
			return;
		}
		if (separate_clip->GetValue() && clip_perspective->GetValue() &&
			!ParseEditor(clip_corner_text, clip_corner, clip_corner_label,
				clip_reference, TrackKind::CornerPin, true)) {
			data_tabs->SetSelection(1);
			return;
		}
		if (!AllTracksReady()) return;
		typesetting::motion::ApplyOptions options;
		bool use_apply_options = !clip_only->GetValue();
		options.reference_sample = static_cast<size_t>(reference->GetValue() - 1);
		if (separate_clip->GetValue())
			options.clip_reference_sample = static_cast<size_t>(clip_reference->GetValue() - 1);
		options.linear = use_apply_options && linear->GetValue();
		options.interpolate_animations = use_apply_options && interpolate_animations->GetValue();
		options.clip_only = clip_only->GetValue();
		options.map_clips = use_apply_options && map_clips->GetValue();
		options.scale_border = use_apply_options && border->GetValue();
		options.scale_shadow = use_apply_options && shadow->GetValue();
		options.scale_blur = use_apply_options && blur->GetValue();
		options.main = {main_x->GetValue(), main_y->GetValue(), main_scale->GetValue(),
			main_rotate->GetValue(), main_perspective->GetValue()};
		options.clip = {clip_x->GetValue(), clip_y->GetValue(), clip_scale->GetValue(),
			clip_rotate->GetValue(), clip_perspective->GetValue()};
		wxProgressDialog progress(_("Apply motion"), _("Preparing motion..."),
			1000, this, wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);
		int last_progress_value = -1;
		int last_progress_stage = -1;
		auto update_progress = [&](typesetting::motion::ApplyProgressStage stage,
			size_t complete, size_t total) {
			total = std::max<size_t>(1, total);
			int value = 0;
			wxString message;
			switch (stage) {
				case typesetting::motion::ApplyProgressStage::Preparing:
					value = static_cast<int>(complete * 50 / total);
					message = _("Preparing motion...");
					break;
				case typesetting::motion::ApplyProgressStage::Applying:
					value = 50 + static_cast<int>(complete * 800 / total);
					message = wxString::Format(_("Applying frame %zu of %zu"),
						complete, total);
					break;
				case typesetting::motion::ApplyProgressStage::Writing:
					value = 850 + static_cast<int>(complete * 150 / total);
					message = wxString::Format(_("Writing subtitle row %zu of %zu"),
						complete, total);
					break;
			}
			int stage_number = static_cast<int>(stage);
			if (value == last_progress_value && stage_number == last_progress_stage &&
				complete < total) return;
			last_progress_value = value;
			last_progress_stage = stage_number;
			progress.Update(std::clamp(value, 0, 1000), message);
		};
		std::string error;
		if (!typesetting::motion::Apply(context, *primary,
			main_perspective->GetValue() ? primary_corner : std::nullopt,
			separate_clip->GetValue() ? clip : std::nullopt,
			separate_clip->GetValue() && clip_perspective->GetValue() ?
				clip_corner : std::nullopt, options, error, update_progress)) {
			wxMessageBox(to_wx(error), _("Motion"), wxOK | wxICON_ERROR, this);
			return;
		}
		EndModal(wxID_OK);
	}

public:
	MotionApplyDialog(agi::Context *context, std::string clipboard)
	: wxDialog(context->parent, wxID_ANY, _("Apply motion"), wxDefaultPosition,
		wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, context(context) {
		auto main = new wxBoxSizer(wxVERTICAL);
		int selection_start = std::numeric_limits<int>::max();
		int selection_end = 0;
		for (auto line : context->selectionController->GetSelectedSet()) {
			selection_start = std::min(selection_start,
				context->videoController->FrameAtTime(line->Start, agi::vfr::START));
			selection_end = std::max(selection_end,
				context->videoController->FrameAtTime(line->End, agi::vfr::END));
		}
		required_frames = static_cast<size_t>(selection_end - selection_start + 1);
		shot_first = selection_start;
		shot_last = selection_end;
		data_tabs = new wxNotebook(this, wxID_ANY);

		auto transformation_page = new wxPanel(data_tabs);
		auto transformation = new wxBoxSizer(wxVERTICAL);
		auto primary_row = new wxBoxSizer(wxHORIZONTAL);
		primary_label = new wxStaticText(transformation_page, wxID_ANY,
			_("No Transformation Data"));
		auto primary_import = new wxButton(transformation_page, wxID_ANY,
			_("Import Transformation Data"));
		primary_row->Add(primary_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
		primary_row->Add(primary_import, 0);
		transformation->Add(primary_row, 0, wxEXPAND | wxALL, 10);
		primary_text = new wxTextCtrl(transformation_page, wxID_ANY, wxString(),
			wxDefaultPosition, wxSize(760, 160),
			wxTE_MULTILINE | wxTE_DONTWRAP | wxHSCROLL);
		transformation->Add(primary_text, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

		auto clip_mode_row = new wxBoxSizer(wxHORIZONTAL);
		clip_only = new wxCheckBox(transformation_page, wxID_ANY, _("Clip only"));
		separate_clip = new wxCheckBox(transformation_page, wxID_ANY,
			_("Track clips with separate data"));
		clip_mode_row->Add(clip_only, 0, wxRIGHT, 18);
		clip_mode_row->Add(separate_clip, 0);
		transformation->Add(clip_mode_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
		auto clip_row = new wxBoxSizer(wxHORIZONTAL);
		clip_label = new wxStaticText(transformation_page, wxID_ANY,
			_("No separate clip Transformation Data"));
		clip_import = new wxButton(transformation_page, wxID_ANY,
			_("Import clip Transformation Data"));
		clip_row->Add(clip_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
		clip_row->Add(clip_import, 0);
		transformation->Add(clip_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
		clip_text = new wxTextCtrl(transformation_page, wxID_ANY, wxString(),
			wxDefaultPosition, wxSize(760, 100),
			wxTE_MULTILINE | wxTE_DONTWRAP | wxHSCROLL);
		transformation->Add(clip_text, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
		transformation_page->SetSizer(transformation);
		data_tabs->AddPage(transformation_page, _("Transformation Data"), true);

		perspective_page = new wxPanel(data_tabs);
		auto perspective = new wxBoxSizer(wxVERTICAL);
		perspective->Add(new wxStaticText(perspective_page, wxID_ANY,
			_("Enable Perspective below, then paste the matching Mocha Corner Pin data.")),
			0, wxEXPAND | wxALL, 10);
		auto primary_corner_row = new wxBoxSizer(wxHORIZONTAL);
		primary_corner_label = new wxStaticText(perspective_page, wxID_ANY,
			_("No main Corner Pin data"));
		primary_corner_import = new wxButton(perspective_page, wxID_ANY,
			_("Import main Corner Pin"));
		primary_corner_row->Add(primary_corner_label, 1,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
		primary_corner_row->Add(primary_corner_import, 0);
		perspective->Add(primary_corner_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
		primary_corner_text = new wxTextCtrl(perspective_page, wxID_ANY, wxString(),
			wxDefaultPosition, wxSize(760, 160),
			wxTE_MULTILINE | wxTE_DONTWRAP | wxHSCROLL);
		perspective->Add(primary_corner_text, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
		auto clip_corner_row = new wxBoxSizer(wxHORIZONTAL);
		clip_corner_label = new wxStaticText(perspective_page, wxID_ANY,
			_("No clip Corner Pin data"));
		clip_corner_import = new wxButton(perspective_page, wxID_ANY,
			_("Import clip Corner Pin"));
		clip_corner_row->Add(clip_corner_label, 1,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
		clip_corner_row->Add(clip_corner_import, 0);
		perspective->Add(clip_corner_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
		clip_corner_text = new wxTextCtrl(perspective_page, wxID_ANY, wxString(),
			wxDefaultPosition, wxSize(760, 100),
			wxTE_MULTILINE | wxTE_DONTWRAP | wxHSCROLL);
		perspective->Add(clip_corner_text, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
		perspective_page->SetSizer(perspective);
		data_tabs->AddPage(perspective_page, _("Perspective - Corner pin"));
		main->Add(data_tabs, 1, wxEXPAND | wxALL, 10);

		auto main_components = new wxStaticBoxSizer(wxHORIZONTAL, this,
			_("Motion components"));
		main_x = new wxCheckBox(this, wxID_ANY, _("X")); main_x->SetValue(true);
		main_y = new wxCheckBox(this, wxID_ANY, _("Y")); main_y->SetValue(true);
		main_scale = new wxCheckBox(this, wxID_ANY, _("Scale")); main_scale->SetValue(true);
		main_rotate = new wxCheckBox(this, wxID_ANY, _("Rotation"));
		main_perspective = new wxCheckBox(this, wxID_ANY, _("Perspective"));
		for (auto control : {main_x, main_y, main_scale, main_rotate, main_perspective})
			main_components->Add(control, 0, wxALL | wxALIGN_CENTER_VERTICAL, 6);
		main->Add(main_components, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

		auto clip_components = new wxStaticBoxSizer(wxHORIZONTAL, this,
			_("Clip motion components"));
		clip_x = new wxCheckBox(this, wxID_ANY, _("X")); clip_x->SetValue(true);
		clip_y = new wxCheckBox(this, wxID_ANY, _("Y")); clip_y->SetValue(true);
		clip_scale = new wxCheckBox(this, wxID_ANY, _("Scale")); clip_scale->SetValue(true);
		clip_rotate = new wxCheckBox(this, wxID_ANY, _("Rotation"));
		clip_perspective = new wxCheckBox(this, wxID_ANY, _("Perspective"));
		for (auto control : {clip_x, clip_y, clip_scale, clip_rotate, clip_perspective}) {
			control->Enable(false);
			clip_components->Add(control, 0, wxALL | wxALIGN_CENTER_VERTICAL, 6);
		}
		main->Add(clip_components, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

		auto reference_group = new wxStaticBoxSizer(wxHORIZONTAL, this, _("Reference"));
		reference_group->Add(new wxStaticText(this, wxID_ANY, _("Main:")), 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		reference = new wxSpinCtrl(this, wxID_ANY);
		int current = context->videoController->GetFrameN();
		int initial_reference = std::max(1, current - selection_start + 1);
		reference->SetRange(1, initial_reference);
		reference->SetValue(initial_reference);
		reference_group->Add(reference, 1, wxRIGHT, 18);
		clip_reference_label = new wxStaticText(this, wxID_ANY,
			_("Clip:"));
		clip_reference = new wxSpinCtrl(this, wxID_ANY);
		clip_reference->SetRange(1, initial_reference);
		clip_reference->SetValue(reference->GetValue());
		clip_reference_label->Enable(false);
		clip_reference->Enable(false);
		reference_group->Add(clip_reference_label, 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		reference_group->Add(clip_reference, 1);
		main->Add(reference_group, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

		interpolate_animations = new wxCheckBox(this, wxID_ANY,
			_("Interpolate animations"));
		interpolate_animations->SetValue(true);
		linear = new wxCheckBox(this, wxID_ANY, _("Linear motion"));
		map_clips = new wxCheckBox(this, wxID_ANY, "Clip");
		border = new wxCheckBox(this, wxID_ANY, _("Scale border"));
		shadow = new wxCheckBox(this, wxID_ANY, _("Scale shadow"));
		blur = new wxCheckBox(this, wxID_ANY, _("Scale blur"));
		// On to begin with: a line that is being scaled wants its border, its shadow and its blur
		// scaled with it, and having to ask for all three every time was the common case.
		for (auto control : {border, shadow, blur}) control->SetValue(true);
		apply_options = new wxStaticBoxSizer(wxVERTICAL, this, _("Apply options"));
		auto option_grid = new wxFlexGridSizer(3, 8, 12);
		for (auto control : {interpolate_animations, linear, map_clips, border, shadow, blur})
			option_grid->Add(control, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
		for (int column = 0; column < 3; ++column) option_grid->AddGrowableCol(column, 1);
		apply_options->Add(option_grid, 1, wxEXPAND | wxALL, 6);
		main->Add(apply_options, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

		auto buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
		main->Add(buttons, 0, wxEXPAND | wxALL, 10);
		SetSizerAndFit(main);
		SetMinSize(GetSize());
		CenterOnParent();
		// The project Mocha saved for this shot, if it is where the trim would have put it. It
		// carries the whole of a projective track, so nothing has to be exported by hand.
		if (LoadMochaProject()) { }
		else if (!clipboard.empty()) {
			std::string error;
			if (ParseTrackText(clipboard, typesetting::motion::TrackKind::Transform, error)) {
				primary_text->ChangeValue(to_wx(clipboard));
				ParseEditor(primary_text, primary, primary_label, reference,
					typesetting::motion::TrackKind::Transform, false);
			}
			else primary_label->SetLabel(_("No valid track data on the clipboard"));
		}
		else primary_label->SetLabel(_("No valid track data on the clipboard"));
		primary_import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			ImportClipboard(primary_text, primary, primary_label, reference,
				typesetting::motion::TrackKind::Transform);
		});
		clip_import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			ImportClipboard(clip_text, clip, clip_label, clip_reference,
				typesetting::motion::TrackKind::Transform);
		});
		primary_corner_import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			ImportClipboard(primary_corner_text, primary_corner, primary_corner_label,
				reference, typesetting::motion::TrackKind::CornerPin);
		});
		clip_corner_import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			ImportClipboard(clip_corner_text, clip_corner, clip_corner_label,
				clip_reference, typesetting::motion::TrackKind::CornerPin);
		});
		clip_only->Bind(wxEVT_CHECKBOX, &MotionApplyDialog::ToggleClipOnly, this);
		separate_clip->Bind(wxEVT_CHECKBOX, &MotionApplyDialog::ToggleClip, this);
		main_perspective->Bind(wxEVT_CHECKBOX,
			&MotionApplyDialog::TogglePerspective, this);
		clip_perspective->Bind(wxEVT_CHECKBOX,
			&MotionApplyDialog::TogglePerspective, this);
		main_scale->Bind(wxEVT_CHECKBOX, &MotionApplyDialog::ToggleScale, this);
		primary_text->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
			ParseEditor(primary_text, primary, primary_label, reference,
				typesetting::motion::TrackKind::Transform, false);
		});
		clip_text->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
			ParseEditor(clip_text, clip, clip_label, clip_reference,
				typesetting::motion::TrackKind::Transform, false);
		});
		primary_corner_text->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
			ParseEditor(primary_corner_text, primary_corner, primary_corner_label, reference,
				typesetting::motion::TrackKind::CornerPin, false);
		});
		clip_corner_text->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
			ParseEditor(clip_corner_text, clip_corner, clip_corner_label, clip_reference,
				typesetting::motion::TrackKind::CornerPin, false);
		});
		Bind(wxEVT_BUTTON, &MotionApplyDialog::ApplyNow, this, wxID_OK);
		UpdateDataControls();
		UpdateClipOnlyControls();
	}
};

struct TrimSettings {
	int format = 0;
	wxString directory;
	bool own_folder = false;
};

std::optional<std::string> JsonString(json::Object const& object, char const *name) {
	auto it = object.find(name);
	if (it == object.end()) return std::nullopt;
	try { return static_cast<json::String const&>(it->second); }
	catch (json::Exception const&) { return std::nullopt; }
}

bool HasNativeTrimSettings() {
	return OPT_GET("Tool/Motion/Trim/Format")->GetInt() != 0 ||
		OPT_GET("Tool/Motion/Trim/Directory")->GetString() != "?video" ||
		OPT_GET("Tool/Motion/Trim/Own Folder")->GetBool();
}

void ImportLegacyTrimSettings() {
	if (OPT_GET("Tool/Motion/Trim/Configured")->GetBool()) return;
	bool import_directory = OPT_GET("Tool/Motion/Trim/Directory")->GetString() == "?video";

	try {
		auto input = agi::io::Open(config::path->Decode("?user/aegisub-motion.json"));
		json::UnknownElement root;
		json::Reader::Read(root, *input);
		auto const& object = static_cast<json::Object const&>(root);
		auto trim_it = object.find("trim");
		if (trim_it == object.end()) return;
		auto const& trim = static_cast<json::Object const&>(trim_it->second);

		bool imported = false;
		if (auto prefix = JsonString(trim, "prefix");
			import_directory && prefix && !prefix->empty()) {
			OPT_SET("Tool/Motion/Trim/Directory")->SetString(*prefix);
			imported = true;
		}
		if (imported || HasNativeTrimSettings())
			OPT_SET("Tool/Motion/Trim/Configured")->SetBool(true);
	}
	catch (...) {
		// Legacy settings are optional. Missing or malformed files leave the
		// native defaults untouched so a later attempt can still import them.
	}
}

TrimSettings GetTrimSettings() {
	ImportLegacyTrimSettings();
	return {
		static_cast<int>(OPT_GET("Tool/Motion/Trim/Format")->GetInt()),
		to_wx(OPT_GET("Tool/Motion/Trim/Directory")->GetString()),
		OPT_GET("Tool/Motion/Trim/Own Folder")->GetBool()
	};
}

/// The Mocha project belonging to the selected lines, in either of the two places Mocha writes
/// its autosave to - beside the trim, or inside the trim's own folder.
///
/// The name is built exactly as the trim builds it, from the video's name and the frames the
/// selection covers, so the two always agree about which shot this is.
std::optional<std::string> FindMochaProject(agi::Context *context, wxString const& root,
	bool own_folder, wxString const& base) {
	if (root.empty()) return std::nullopt;

	// Both places are looked in whichever way the trim is set, and the setting only decides which
	// to look in first. The setting says where the trim writes from now on; the shot in hand may
	// have been trimmed before it was changed.
	auto in_own_folder = [&]() -> std::optional<std::string> {
		wxFileName folder;
		folder.AssignDir(root);
		folder.AppendDir(base);
		folder.AppendDir("results");
		wxDir results(folder.GetPath());
		if (!results.IsOpened()) return std::nullopt;
		// Whatever it is called: in its own folder Mocha names the project after the image
		// sequence, and the user is free to rename it.
		wxString found;
		if (!results.GetFirst(&found, "*.mocha", wxDIR_FILES)) return std::nullopt;
		return from_wx(wxFileName(folder.GetPath(), found).GetFullPath());
	};
	auto beside_the_trim = [&]() -> std::optional<std::string> {
		wxFileName results;
		results.AssignDir(root);
		results.AppendDir("results");
		wxFileName named(results.GetPath(), base + ".mocha");
		if (named.FileExists()) return from_wx(named.GetFullPath());
		wxFileName plain(root, base + ".mocha");
		if (plain.FileExists()) return from_wx(plain.GetFullPath());
		return std::nullopt;
	};

	if (own_folder) {
		if (auto found = in_own_folder()) return found;
		return beside_the_trim();
	}
	if (auto found = beside_the_trim()) return found;
	return in_own_folder();
}

wxString ResolveDirectory(agi::Context *context, wxString directory) {
	if (directory == "?video") return to_wx(context->project->VideoName().parent_path().string());
	if (directory == "?script") return to_wx(context->subsController->Filename().parent_path().string());
	return directory;
}

} // namespace

void ShowMotionApplyDialog(agi::Context *context) {
	if (!context || !context->project->VideoProvider() ||
		context->selectionController->GetSelectedSet().empty()) return;
	MotionApplyDialog dialog(context, ClipboardText().value_or(std::string()));
	dialog.ShowModal();
}

void ShowMotionTrimSettings(agi::Context *context) {
	TrimSettings settings = GetTrimSettings();
	wxDialog dialog(context->parent, wxID_ANY, _("Trim settings"));
	auto main = new wxBoxSizer(wxVERTICAL);
	auto grid = new wxFlexGridSizer(2, 8, 12);
	grid->Add(new wxStaticText(&dialog, wxID_ANY, _("Output format:")), 0, wxALIGN_CENTER_VERTICAL);
	auto format = new wxChoice(&dialog, wxID_ANY);
	format->Append(_("JPEG image sequence"));
	format->Append(_("H.264 MP4 video"));
	format->SetSelection(std::clamp(settings.format, 0, 1));
	grid->Add(format, 1, wxEXPAND);
	grid->Add(new wxStaticText(&dialog, wxID_ANY, _("Output directory:")), 0, wxALIGN_CENTER_VERTICAL);
	auto directory = new wxDirPickerCtrl(&dialog, wxID_ANY,
		ResolveDirectory(context, settings.directory), _("Choose trim output directory"));
	grid->Add(directory, 1, wxEXPAND);
	grid->Add(new wxStaticText(&dialog, wxID_ANY, _("Organize output:")), 0,
		wxALIGN_CENTER_VERTICAL);
	auto own_folder = new wxCheckBox(&dialog, wxID_ANY,
		_("Put each trim in its own folder"));
	own_folder->SetValue(settings.own_folder);
	grid->Add(own_folder, 1, wxEXPAND);
	grid->AddGrowableCol(1, 1);
	main->Add(grid, 1, wxEXPAND | wxALL, 12);
	main->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 12);
	dialog.SetSizerAndFit(main);
	dialog.SetMinSize(dialog.FromDIP(wxSize(640, 260)));
	dialog.SetSize(dialog.FromDIP(wxSize(680, 280)));
	dialog.CentreOnParent();
	if (dialog.ShowModal() != wxID_OK) return;
	OPT_SET("Tool/Motion/Trim/Format")->SetInt(format->GetSelection());
	OPT_SET("Tool/Motion/Trim/Directory")->SetString(from_wx(directory->GetPath()));
	OPT_SET("Tool/Motion/Trim/Own Folder")->SetBool(own_folder->GetValue());
	OPT_SET("Tool/Motion/Trim/Configured")->SetBool(true);
}

void CreateMotionTrim(agi::Context *context) {
	if (!context || !context->project->VideoProvider()) return;
	auto selected = context->selectionController->GetSelectedSet();
	if (selected.empty()) return;
	int start_frame = std::numeric_limits<int>::max(), end_frame = 0;
	for (auto line : selected) {
		start_frame = std::min(start_frame,
			context->videoController->FrameAtTime(line->Start, agi::vfr::START));
		end_frame = std::max(end_frame,
			context->videoController->FrameAtTime(line->End, agi::vfr::END));
	}
	TrimSettings settings = GetTrimSettings();
	wxString output_root = ResolveDirectory(context, settings.directory);
	if (output_root.empty()) {
		ShowMotionTrimSettings(context);
		settings = GetTrimSettings();
		output_root = ResolveDirectory(context, settings.directory);
		if (output_root.empty()) return;
	}
	wxFileName video_name(context->project->VideoName().wstring());
	wxString base = video_name.GetName() + wxString::Format("[%d-%d]", start_frame, end_frame);
	wxString output_stem = base;
	if (settings.own_folder) {
		wxFileName trim_folder;
		trim_folder.AssignDir(output_root);
		trim_folder.AppendDir(base);
		output_root = trim_folder.GetPath();
		output_stem = "trim";
	}
	if (!wxDirExists(output_root) &&
		!wxFileName::Mkdir(output_root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
		wxMessageBox(_("The trim output directory could not be created."), _("Motion trim"),
			wxOK | wxICON_ERROR, context->parent);
		return;
	}
	int frame_count = end_frame - start_frame + 1;
	auto remove_manifest = [&] {
		for (auto const& stem : {base, output_stem}) {
			wxFileName manifest(output_root, stem + "-motion-trim.txt");
			if (manifest.FileExists()) wxRemoveFile(manifest.GetFullPath());
		}
	};
	if (settings.format == 0) {
		wxFileName first_output(output_root, output_stem + "-00001.jpg");
		if (first_output.FileExists() &&
			wxMessageBox(_("The JPEG trim already exists. Overwrite its image sequence?"),
				_("Motion trim"), wxYES_NO | wxICON_WARNING, context->parent) != wxYES)
			return;

		wxProgressDialog progress(_("Motion trim"), _("Writing JPEG frames..."),
			frame_count, context->parent,
			wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
		for (int frame = start_frame; frame <= end_frame; ++frame) {
			bool keep_going = progress.Update(frame - start_frame,
				wxString::Format(_("Writing frame %d of %d"), frame - start_frame + 1,
					frame_count));
			if (!keep_going) return;
			auto video_frame = context->videoController->GetFrame(frame, true);
			if (!video_frame) continue;
			wxImage image = GetImage(*video_frame);
			image.SetOption(wxIMAGE_OPTION_QUALITY, 95);
			wxFileName output(output_root,
				output_stem + wxString::Format("-%05d.jpg", frame - start_frame + 1));
			if (!image.SaveFile(output.GetFullPath(), wxBITMAP_TYPE_JPEG)) {
				wxMessageBox(_("A JPEG frame could not be written."), _("Motion trim"),
					wxOK | wxICON_ERROR, context->parent);
				return;
			}
		}

		remove_manifest();
		return;
	}

	wxFileName output(output_root, output_stem + ".mp4");
	if (output.FileExists() &&
		wxMessageBox(_("The H.264 trim already exists. Overwrite it?"), _("Motion trim"),
			wxYES_NO | wxICON_WARNING, context->parent) != wxYES)
		return;
	DialogProgress progress(context->parent, _("Motion trim"), _("Encoding H.264 video..."));
	std::string encode_error;
	bool encoded = false;
	try {
		progress.Run([&](agi::ProgressSink *sink) {
			encoded = EncodeMotionTrimH264(context, output.GetFullPath(), start_frame,
				end_frame, 18, [&](int complete, int total) {
					sink->SetProgress(complete, total);
					sink->SetMessage(agi::format("Encoding trim %d of %d",
						complete + 1, total));
					return !sink->IsCancelled();
				}, encode_error);
		});
	}
	catch (agi::UserCancelException const&) {
		return;
	}
	if (!encoded) {
		wxMessageBox(to_wx(encode_error), _("Motion trim"), wxOK | wxICON_ERROR,
			context->parent);
		return;
	}
	remove_manifest();
}
