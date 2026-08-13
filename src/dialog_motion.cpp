// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "dialog_motion.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "include/aegisub/context.h"
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
#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dialog.h>
#include <wx/filename.h>
#include <wx/filepicker.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
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

std::optional<std::string> ReadFile(wxString const& filename) {
	std::ifstream input(from_wx(filename), std::ios::binary);
	if (!input) return std::nullopt;
	std::ostringstream data;
	data << input.rdbuf();
	return data.str();
}

std::optional<std::string> ChooseMotionFile(wxWindow *parent) {
	wxFileDialog dialog(parent, _("Choose Mocha motion data"), wxString(), wxString(),
		_("Text files (*.txt)|*.txt|All files (*.*)|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (dialog.ShowModal() != wxID_OK) return std::nullopt;
	auto data = ReadFile(dialog.GetPath());
	if (!data)
		wxMessageBox(_("The selected motion data could not be read."), _("Motion"),
			wxOK | wxICON_ERROR, parent);
	return data;
}

wxString TrackLabel(typesetting::motion::Track const& track) {
	return wxString::Format(track.kind == typesetting::motion::TrackKind::CornerPin ?
		_("Mocha Corner Pin - %zu frames") : _("Mocha Transform Data - %zu frames"),
		track.samples.size());
}

class MotionApplyDialog final : public wxDialog {
	using Track = typesetting::motion::Track;
	agi::Context *context;
	std::optional<Track> primary;
	std::optional<Track> clip;
	wxStaticText *primary_label;
	wxStaticText *clip_label;
	wxTextCtrl *primary_text;
	wxTextCtrl *clip_text;
	wxCheckBox *separate_clip;
	wxButton *clip_import;
	wxButton *clip_file;
	wxSpinCtrl *reference;
	wxStaticText *clip_reference_label;
	wxSpinCtrl *clip_reference;
	wxCheckBox *relative;
	wxCheckBox *map_clips;
	wxCheckBox *border;
	wxCheckBox *shadow;
	wxCheckBox *blur;

	bool ParseEditor(wxTextCtrl *editor, std::optional<Track>& destination,
		wxStaticText *label, wxSpinCtrl *reference_control, bool report_error) {
		int width = 0, height = 0;
		context->ass->GetResolution(width, height);
		std::string error;
		auto text = from_wx(editor->GetValue());
		auto parsed = typesetting::motion::ParseMocha(text, width, height, error);
		if (!parsed) {
			destination.reset();
			label->SetLabel(text.empty() ? _("No track data") : _("Track data has not been validated"));
			if (report_error)
				wxMessageBox(to_wx(error), _("Motion"), wxOK | wxICON_ERROR, this);
			return false;
		}
		destination = std::move(parsed);
		label->SetLabel(TrackLabel(*destination));
		int old_value = reference_control->GetValue();
		reference_control->SetRange(1, static_cast<int>(destination->samples.size()));
		reference_control->SetValue(std::clamp(old_value, 1,
			static_cast<int>(destination->samples.size())));
		Layout();
		return true;
	}

	void ImportClipboard(wxTextCtrl *editor, std::optional<Track>& destination,
		wxStaticText *label, wxSpinCtrl *reference_control) {
		auto data = ClipboardText();
		if (!data || data->empty()) {
			wxMessageBox(_("The clipboard does not contain track data."), _("Motion"),
				wxOK | wxICON_WARNING, this);
			return;
		}
		editor->ChangeValue(to_wx(*data));
		ParseEditor(editor, destination, label, reference_control, true);
	}

	void LoadPrimary(wxCommandEvent&) {
		if (auto data = ChooseMotionFile(this)) {
			primary_text->ChangeValue(to_wx(*data));
			ParseEditor(primary_text, primary, primary_label, reference, true);
		}
	}

	void LoadClip(wxCommandEvent&) {
		if (auto data = ChooseMotionFile(this)) {
			clip_text->ChangeValue(to_wx(*data));
			ParseEditor(clip_text, clip, clip_label, clip_reference, true);
		}
	}

	void ToggleClip(wxCommandEvent&) {
		clip_import->Enable(separate_clip->GetValue());
		clip_file->Enable(separate_clip->GetValue());
		clip_text->Enable(separate_clip->GetValue());
		clip_label->Enable(separate_clip->GetValue());
		clip_reference_label->Enable(separate_clip->GetValue());
		clip_reference->Enable(separate_clip->GetValue());
	}

	void ApplyNow(wxCommandEvent&) {
		if (!ParseEditor(primary_text, primary, primary_label, reference, true)) return;
		if (separate_clip->GetValue() &&
			!ParseEditor(clip_text, clip, clip_label, clip_reference, true)) return;
		typesetting::motion::ApplyOptions options;
		options.reference_sample = static_cast<size_t>(reference->GetValue() - 1);
		if (separate_clip->GetValue())
			options.clip_reference_sample = static_cast<size_t>(clip_reference->GetValue() - 1);
		options.relative_to_selection = relative->GetValue();
		options.map_clips = map_clips->GetValue();
		options.scale_border = border->GetValue();
		options.scale_shadow = shadow->GetValue();
		options.scale_blur = blur->GetValue();
		std::string error;
		if (!typesetting::motion::Apply(context, *primary,
			separate_clip->GetValue() ? clip : std::nullopt, options, error)) {
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
		auto primary_row = new wxBoxSizer(wxHORIZONTAL);
		primary_label = new wxStaticText(this, wxID_ANY, _("No track data"));
		auto primary_import = new wxButton(this, wxID_ANY, _("Import track"));
		auto primary_file = new wxButton(this, wxID_ANY, _("Choose file..."));
		primary_row->Add(primary_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
		primary_row->Add(primary_import, 0, wxRIGHT, 6);
		primary_row->Add(primary_file, 0);
		main->Add(primary_row, 0, wxEXPAND | wxALL, 10);
		primary_text = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition,
			wxSize(760, 180), wxTE_MULTILINE | wxTE_DONTWRAP | wxHSCROLL);
		main->Add(primary_text, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

		separate_clip = new wxCheckBox(this, wxID_ANY, _("Track clips with separate data"));
		main->Add(separate_clip, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
		auto clip_row = new wxBoxSizer(wxHORIZONTAL);
		clip_label = new wxStaticText(this, wxID_ANY, _("No separate clip data selected"));
		clip_import = new wxButton(this, wxID_ANY, _("Import clip track"));
		clip_file = new wxButton(this, wxID_ANY, _("Choose file..."));
		clip_label->Enable(false);
		clip_import->Enable(false);
		clip_file->Enable(false);
		clip_row->Add(clip_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
		clip_row->Add(clip_import, 0, wxRIGHT, 6);
		clip_row->Add(clip_file, 0);
		main->Add(clip_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
		clip_text = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition,
			wxSize(760, 120), wxTE_MULTILINE | wxTE_DONTWRAP | wxHSCROLL);
		clip_text->Enable(false);
		main->Add(clip_text, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

		auto grid = new wxFlexGridSizer(2, 8, 12);
		grid->Add(new wxStaticText(this, wxID_ANY, _("Reference frame in data:")), 0,
			wxALIGN_CENTER_VERTICAL);
		reference = new wxSpinCtrl(this, wxID_ANY);
		int selection_start = std::numeric_limits<int>::max();
		for (auto line : context->selectionController->GetSelectedSet())
			selection_start = std::min(selection_start,
				context->videoController->FrameAtTime(line->Start, agi::vfr::START));
		int current = context->videoController->GetFrameN();
		int initial_reference = std::max(1, current - selection_start + 1);
		reference->SetRange(1, initial_reference);
		reference->SetValue(initial_reference);
		grid->Add(reference, 1, wxEXPAND);
		clip_reference_label = new wxStaticText(this, wxID_ANY,
			_("Reference frame in clip data:"));
		clip_reference = new wxSpinCtrl(this, wxID_ANY);
		clip_reference->SetRange(1, initial_reference);
		clip_reference->SetValue(reference->GetValue());
		clip_reference_label->Enable(false);
		clip_reference->Enable(false);
		grid->Add(clip_reference_label, 0, wxALIGN_CENTER_VERTICAL);
		grid->Add(clip_reference, 1, wxEXPAND);
		grid->AddGrowableCol(1, 1);
		main->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

		relative = new wxCheckBox(this, wxID_ANY, _("Data starts at the first selected frame"));
		relative->SetValue(true);
		map_clips = new wxCheckBox(this, wxID_ANY, _("Apply motion to clips"));
		map_clips->SetValue(true);
		border = new wxCheckBox(this, wxID_ANY, _("Scale border")); border->SetValue(true);
		shadow = new wxCheckBox(this, wxID_ANY, _("Scale shadow")); shadow->SetValue(true);
		blur = new wxCheckBox(this, wxID_ANY, _("Scale blur")); blur->SetValue(true);
		for (auto control : {relative, map_clips, border, shadow, blur})
			main->Add(control, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

		auto buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
		main->Add(buttons, 0, wxEXPAND | wxALL, 10);
		SetSizerAndFit(main);
		SetMinSize(GetSize());
		if (!clipboard.empty()) {
			primary_text->ChangeValue(to_wx(clipboard));
			ParseEditor(primary_text, primary, primary_label, reference, false);
		}
		primary_import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			ImportClipboard(primary_text, primary, primary_label, reference);
		});
		clip_import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			ImportClipboard(clip_text, clip, clip_label, clip_reference);
		});
		primary_file->Bind(wxEVT_BUTTON, &MotionApplyDialog::LoadPrimary, this);
		clip_file->Bind(wxEVT_BUTTON, &MotionApplyDialog::LoadClip, this);
		separate_clip->Bind(wxEVT_CHECKBOX, &MotionApplyDialog::ToggleClip, this);
		primary_text->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
			primary.reset();
			primary_label->SetLabel(_("Track data modified - Apply to validate"));
		});
		clip_text->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
			clip.reset();
			clip_label->SetLabel(_("Clip track modified - Apply to validate"));
		});
		Bind(wxEVT_BUTTON, &MotionApplyDialog::ApplyNow, this, wxID_OK);
	}
};

struct TrimSettings {
	int format = 0;
	wxString directory;
	wxString ffmpeg;
	int crf = 18;
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
		!OPT_GET("Tool/Motion/Trim/FFmpeg")->GetString().empty() ||
		OPT_GET("Tool/Motion/Trim/CRF")->GetInt() != 18;
}

void ImportLegacyTrimSettings() {
	if (OPT_GET("Tool/Motion/Trim/Configured")->GetBool()) return;
	bool import_directory = OPT_GET("Tool/Motion/Trim/Directory")->GetString() == "?video";
	bool import_ffmpeg = OPT_GET("Tool/Motion/Trim/FFmpeg")->GetString().empty();

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
		if (auto encoder = JsonString(trim, "encBin");
			import_ffmpeg && encoder && !encoder->empty()) {
			OPT_SET("Tool/Motion/Trim/FFmpeg")->SetString(*encoder);
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
		to_wx(OPT_GET("Tool/Motion/Trim/FFmpeg")->GetString()),
		static_cast<int>(OPT_GET("Tool/Motion/Trim/CRF")->GetInt())
	};
}

wxString ResolveDirectory(agi::Context *context, wxString directory) {
	if (directory == "?video") return to_wx(context->project->VideoName().parent_path().string());
	if (directory == "?script") return to_wx(context->subsController->Filename().parent_path().string());
	return directory;
}

wxString Quote(wxString value) {
	value.Replace("\"", "\\\"");
	return "\"" + value + "\"";
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
	auto ffmpeg_label = new wxStaticText(&dialog, wxID_ANY, _("FFmpeg executable:"));
	grid->Add(ffmpeg_label, 0, wxALIGN_CENTER_VERTICAL);
	auto ffmpeg = new wxFilePickerCtrl(&dialog, wxID_ANY, settings.ffmpeg,
		_("Choose ffmpeg.exe"), _("Executables (*.exe)|*.exe|All files (*.*)|*.*"));
	grid->Add(ffmpeg, 1, wxEXPAND);
	auto crf_label = new wxStaticText(&dialog, wxID_ANY, _("H.264 CRF:"));
	grid->Add(crf_label, 0, wxALIGN_CENTER_VERTICAL);
	auto crf = new wxSpinCtrl(&dialog, wxID_ANY);
	crf->SetRange(0, 51); crf->SetValue(settings.crf);
	grid->Add(crf, 1, wxEXPAND);
	grid->AddGrowableCol(1, 1);
	main->Add(grid, 1, wxEXPAND | wxALL, 12);
	main->Add(new wxStaticText(&dialog, wxID_ANY,
		_("JPEG image sequences use FFmpeg when configured and export much faster. "
		  "Without FFmpeg, Aegisub exports them internally and is much slower. "
		  "H.264 MP4 also requires FFmpeg.")),
		0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
	main->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 12);
	dialog.SetSizerAndFit(main);
	auto update_fields = [&] {
		bool mp4 = format->GetSelection() == 1;
		ffmpeg_label->Show(!mp4);
		ffmpeg->Show(!mp4);
		crf_label->Show(mp4);
		crf->Show(mp4);
		main->Layout();
	};
	format->Bind(wxEVT_CHOICE, [&, update_fields](wxCommandEvent&) { update_fields(); });
	update_fields();
	if (dialog.ShowModal() != wxID_OK) return;
	OPT_SET("Tool/Motion/Trim/Format")->SetInt(format->GetSelection());
	OPT_SET("Tool/Motion/Trim/Directory")->SetString(from_wx(directory->GetPath()));
	OPT_SET("Tool/Motion/Trim/FFmpeg")->SetString(from_wx(ffmpeg->GetPath()));
	OPT_SET("Tool/Motion/Trim/CRF")->SetInt(crf->GetValue());
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
	if (!wxDirExists(output_root) &&
		!wxFileName::Mkdir(output_root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
		wxMessageBox(_("The trim output directory could not be created."), _("Motion trim"),
			wxOK | wxICON_ERROR, context->parent);
		return;
	}
	wxFileName video_name(context->project->VideoName().wstring());
	wxString base = video_name.GetName() + wxString::Format("[%d-%d]", start_frame, end_frame);
	int frame_count = end_frame - start_frame + 1;
	double start_seconds = context->videoController->TimeAtFrame(
		start_frame, agi::vfr::START) / 1000.0;
	if (settings.format == 0) {
		wxFileName first_output(output_root, base + "-00001.jpg");
		if (first_output.FileExists() &&
			wxMessageBox(_("The JPEG trim already exists. Overwrite its image sequence?"),
				_("Motion trim"), wxYES_NO | wxICON_WARNING, context->parent) != wxYES)
			return;

		wxString pattern = wxFileName(output_root, base + "-%05d.jpg").GetFullPath();
		if (!settings.ffmpeg.empty()) {
			if (!wxFileExists(settings.ffmpeg)) {
				wxMessageBox(_("The configured FFmpeg executable could not be found. "
					"Choose a valid executable, or clear it to use the slower internal exporter."),
					_("Motion trim"), wxOK | wxICON_ERROR, context->parent);
				return;
			}
			wxString command = Quote(settings.ffmpeg) +
				wxString::Format(" -y -ss %.6f -i ", start_seconds) +
				Quote(wxString(context->project->VideoName().wstring())) +
				wxString::Format(" -map 0:v:0 -frames:v %d -an -sn -q:v 1 "
					"-fps_mode passthrough -start_number 1 ", frame_count) + Quote(pattern);
			wxArrayString standard_output, standard_error;
			long status;
			{
				wxBusyCursor busy;
				status = wxExecute(command, standard_output, standard_error,
					wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE);
			}
			if (status != 0 || !first_output.FileExists()) {
				wxString message = _("FFmpeg could not create the JPEG image sequence.");
				if (!standard_error.empty()) message += "\n\n" + standard_error.Last();
				wxMessageBox(message, _("Motion trim"), wxOK | wxICON_ERROR, context->parent);
				return;
			}
		}
		else {
			if (wxMessageBox(_("FFmpeg is not configured. Aegisub can export the JPEG "
				"sequence internally, but it is much slower. Continue?"), _("Motion trim"),
				wxYES_NO | wxICON_INFORMATION, context->parent) != wxYES)
				return;
			wxProgressDialog progress(_("Motion trim"), _("Writing JPEG frames..."),
				frame_count, context->parent,
				wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT);
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
					base + wxString::Format("-%05d.jpg", frame - start_frame + 1));
				if (!image.SaveFile(output.GetFullPath(), wxBITMAP_TYPE_JPEG)) {
					wxMessageBox(_("A JPEG frame could not be written."), _("Motion trim"),
						wxOK | wxICON_ERROR, context->parent);
					return;
				}
			}
		}

		std::ofstream manifest(from_wx(
			wxFileName(output_root, base + "-motion-trim.txt").GetFullPath()));
		manifest << "source=" << context->project->VideoName().string() << "\n"
			<< "first_frame=" << start_frame << "\nlast_frame=" << end_frame << "\n"
			<< "frame_count=" << frame_count << "\n";
		wxMessageBox(wxString::Format(_("JPEG trim written as:\n%s"), pattern),
			_("Motion trim"), wxOK | wxICON_INFORMATION, context->parent);
		return;
	}

	if (settings.ffmpeg.empty() || !wxFileExists(settings.ffmpeg)) {
		wxMessageBox(_("Set the FFmpeg executable in Trim settings first."), _("Motion trim"),
			wxOK | wxICON_WARNING, context->parent);
		return;
	}
	wxFileName output(output_root, base + ".mp4");
	if (output.FileExists() &&
		wxMessageBox(_("The H.264 trim already exists. Overwrite it?"), _("Motion trim"),
			wxYES_NO | wxICON_WARNING, context->parent) != wxYES)
		return;
	wxString command = Quote(settings.ffmpeg) + " -y -i " +
		Quote(wxString(context->project->VideoName().wstring())) +
		wxString::Format(" -ss %.6f -map 0:v:0 -frames:v %d -an -c:v libx264 "
			"-preset medium -crf %d -pix_fmt yuv420p -vsync 0 ",
			start_seconds, frame_count, settings.crf) + Quote(output.GetFullPath());
	wxArrayString standard_output, standard_error;
	long status = wxExecute(command, standard_output, standard_error,
		wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE);
	if (status != 0 || !output.FileExists()) {
		wxString message = _("FFmpeg could not create the H.264 trim.");
		if (!standard_error.empty()) message += "\n\n" + standard_error.Last();
		wxMessageBox(message, _("Motion trim"), wxOK | wxICON_ERROR, context->parent);
		return;
	}
	std::ofstream manifest(from_wx(wxFileName(output_root,
		base + "-motion-trim.txt").GetFullPath()));
	manifest << "source=" << context->project->VideoName().string() << "\n"
		<< "first_frame=" << start_frame << "\nlast_frame=" << end_frame << "\n"
		<< "frame_count=" << frame_count << "\n";
	wxMessageBox(wxString::Format(_("H.264 trim written to:\n%s"), output.GetFullPath()),
		_("Motion trim"), wxOK | wxICON_INFORMATION, context->parent);
}
