// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_image_insert.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "imagemask_codec.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "selection_controller.h"
#include "subtitle_line_combiner.h"

#include <libaegisub/format.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include <wx/dialog.h>
#include <wx/checkbox.h>
#include <wx/filepicker.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/settings.h>

namespace typesetting::image_insert {
namespace {

constexpr char option_root[] = "Tool/Image Insert/";

std::string Option(char const *name) { return std::string(option_root) + name; }

wxString ImageWildcard() {
	return _("Image files (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp)|"
		"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp|All files (*.*)|*.*");
}

struct Settings {
	imagemask::ImportOptions image;
	bool use_alpha = false;
	int mode = 0;
	wxString directory;
	wxString basic_image;
	wxString basic_alpha;
};

Settings LoadSettings() {
	Settings settings;
	settings.image.compression = static_cast<int>(
		OPT_GET(Option("Compression"))->GetInt());
	if (OPT_GET(Option("Compression Version"))->GetInt() < 1) {
		// The first native Image insert build used 1 as its default. Move only
		// that old default to Image2ASS's value; preserve every custom setting.
		if (settings.image.compression == 1) settings.image.compression = 40;
		OPT_SET(Option("Compression"))->SetInt(settings.image.compression);
		OPT_SET(Option("Compression Version"))->SetInt(1);
	}
	settings.image.resize = OPT_GET(Option("Resize"))->GetDouble();
	settings.image.pixel_size = static_cast<int>(
		OPT_GET(Option("Pixel Size"))->GetInt());
	settings.use_alpha = OPT_GET(Option("Alpha Image"))->GetBool();
	settings.mode = static_cast<int>(OPT_GET(Option("Mode"))->GetInt());
	settings.directory = to_wx(OPT_GET(Option("Directory"))->GetString());
	settings.basic_image = to_wx(OPT_GET(Option("Basic Image"))->GetString());
	settings.basic_alpha = to_wx(OPT_GET(Option("Basic Alpha"))->GetString());
	return settings;
}

void SaveSettings(Settings const& settings) {
	OPT_SET(Option("Compression"))->SetInt(settings.image.compression);
	OPT_SET(Option("Compression Version"))->SetInt(1);
	OPT_SET(Option("Resize"))->SetDouble(settings.image.resize);
	OPT_SET(Option("Pixel Size"))->SetInt(settings.image.pixel_size);
	OPT_SET(Option("Alpha Image"))->SetBool(settings.use_alpha);
	OPT_SET(Option("Mode"))->SetInt(settings.mode);
	OPT_SET(Option("Directory"))->SetString(from_wx(settings.directory));
	OPT_SET(Option("Basic Image"))->SetString(from_wx(settings.basic_image));
	OPT_SET(Option("Basic Alpha"))->SetString(from_wx(settings.basic_alpha));
}

wxStaticText *AlphaInfo(wxWindow *parent) {
	auto info = new wxStaticText(parent, wxID_ANY,
		_("(Optional; black is opaque, white is transparent)"));
	info->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
	return info;
}

wxStaticText *PngAlphaInfo(wxWindow *parent) {
	auto info = new wxStaticText(parent, wxID_ANY,
		_("(PNG transparency is handled automatically; no alpha image is needed.)"));
	info->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
	return info;
}

class OptionsControls {
public:
	wxSizer *layout;
	wxSpinCtrl *compression;
	wxSpinCtrlDouble *resize;
	wxSpinCtrl *pixel_size;

	OptionsControls(wxWindow *parent, Settings const& settings) {
		auto grid = new wxFlexGridSizer(1, 6, 0, 8);
		layout = grid;
		grid->AddGrowableCol(1, 1);
		grid->AddGrowableCol(3, 1);
		grid->AddGrowableCol(5, 1);
		grid->Add(new wxStaticText(parent, wxID_ANY, _("Compression:")),
			0, wxALIGN_CENTER_VERTICAL);
		compression = new wxSpinCtrl(parent, wxID_ANY);
		compression->SetRange(1, 3000);
		compression->SetValue(settings.image.compression);
		compression->SetToolTip(_("1 keeps every source colour exact; larger values merge nearby colours."));
		grid->Add(compression, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
		grid->Add(new wxStaticText(parent, wxID_ANY, _("Resize image (%):")),
			0, wxALIGN_CENTER_VERTICAL);
		resize = new wxSpinCtrlDouble(parent, wxID_ANY);
		resize->SetRange(1, 100);
		resize->SetIncrement(1);
		resize->SetDigits(1);
		resize->SetValue(settings.image.resize);
		grid->Add(resize, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
		grid->Add(new wxStaticText(parent, wxID_ANY, _("Pixel size:")),
			0, wxALIGN_CENTER_VERTICAL);
		pixel_size = new wxSpinCtrl(parent, wxID_ANY);
		pixel_size->SetRange(1, 250);
		pixel_size->SetValue(settings.image.pixel_size);
		grid->Add(pixel_size, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
	}

	void Store(Settings& settings) const {
		settings.image.compression = compression->GetValue();
		settings.image.resize = resize->GetValue();
		settings.image.pixel_size = pixel_size->GetValue();
	}
};

class InsertDialog final : public wxDialog {
	wxFilePickerCtrl *image;
	wxFilePickerCtrl *alpha;
	OptionsControls options;

public:
	InsertDialog(wxWindow *parent, Settings const& settings)
	: wxDialog(parent, wxID_ANY, _("Image insert"), wxDefaultPosition,
		wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, options(this, settings) {
		auto main = new wxBoxSizer(wxVERTICAL);
		auto content = new wxBoxSizer(wxVERTICAL);
		auto sources = new wxFlexGridSizer(4, 2, 4, 10);
		sources->AddGrowableCol(1, 1);
		sources->Add(new wxStaticText(this, wxID_ANY, _("Image:")),
			0, wxALIGN_CENTER_VERTICAL);
		image = new wxFilePickerCtrl(this, wxID_ANY, wxEmptyString,
			_("Select image"), ImageWildcard(), wxDefaultPosition, wxDefaultSize,
			wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
		sources->Add(image, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
		sources->AddSpacer(0);
		sources->Add(PngAlphaInfo(this), 0, wxALIGN_RIGHT | wxBOTTOM, 4);
		sources->Add(new wxStaticText(this, wxID_ANY, _("Alpha image:")),
			0, wxALIGN_CENTER_VERTICAL);
		alpha = new wxFilePickerCtrl(this, wxID_ANY, wxEmptyString,
			_("Select alpha image"), ImageWildcard(), wxDefaultPosition, wxDefaultSize,
			wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
		sources->Add(alpha, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
		sources->AddSpacer(0);
		sources->Add(AlphaInfo(this), 0, wxALIGN_RIGHT);
		content->Add(sources, 0, wxEXPAND);
		content->Add(options.layout, 0, wxEXPAND | wxTOP | wxBOTTOM, 12);
		content->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND);
		main->Add(content, 1, wxEXPAND | wxALL, 12);
		SetSizerAndFit(main);
		SetMinSize(FromDIP(wxSize(880, -1)));
		SetSize(wxSize(FromDIP(880), GetSize().GetHeight()));
	}

	wxString ImagePath() const { return image->GetPath(); }
	wxString AlphaPath() const { return alpha->GetPath(); }
	void Store(Settings& settings) const { options.Store(settings); }
};

class SettingsDialog final : public wxDialog {
	OptionsControls options;
	wxNotebook *tabs;
	wxFilePickerCtrl *basic_image;
	wxFilePickerCtrl *basic_alpha;
	wxCheckBox *dynamic_alpha;
	wxDirPickerCtrl *directory;

public:
	SettingsDialog(wxWindow *parent, Settings const& settings)
	: wxDialog(parent, wxID_ANY, _("Image insert settings"), wxDefaultPosition,
		wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, options(this, settings) {
		auto main = new wxBoxSizer(wxVERTICAL);
		auto content = new wxBoxSizer(wxVERTICAL);
		content->Add(options.layout, 0, wxEXPAND | wxBOTTOM, 12);

		tabs = new wxNotebook(this, wxID_ANY);
		auto basic = new wxPanel(tabs);
		auto basic_sizer = new wxBoxSizer(wxVERTICAL);
		auto basic_sources = new wxFlexGridSizer(4, 2, 4, 10);
		basic_sources->AddGrowableCol(1, 1);
		basic_sources->Add(new wxStaticText(basic, wxID_ANY, _("Image:")),
			0, wxALIGN_CENTER_VERTICAL);
		basic_image = new wxFilePickerCtrl(basic, wxID_ANY, settings.basic_image,
			_("Select image"), ImageWildcard(), wxDefaultPosition, wxDefaultSize,
			wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
		basic_sources->Add(basic_image, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
		basic_sources->AddSpacer(0);
		basic_sources->Add(PngAlphaInfo(basic), 0, wxALIGN_RIGHT | wxBOTTOM, 4);
		basic_sources->Add(new wxStaticText(basic, wxID_ANY, _("Alpha image:")),
			0, wxALIGN_CENTER_VERTICAL);
		basic_alpha = new wxFilePickerCtrl(basic, wxID_ANY, settings.basic_alpha,
			_("Select alpha image"), ImageWildcard(), wxDefaultPosition, wxDefaultSize,
			wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
		basic_sources->Add(basic_alpha, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
		basic_sources->AddSpacer(0);
		basic_sources->Add(AlphaInfo(basic), 0, wxALIGN_RIGHT);
		basic_sizer->Add(basic_sources, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
		basic_sizer->AddSpacer(12);
		basic->SetSizer(basic_sizer);
		tabs->AddPage(basic, _("Basic"));

		auto dynamic = new wxPanel(tabs);
		auto dynamic_sizer = new wxBoxSizer(wxVERTICAL);
		dynamic_alpha = new wxCheckBox(dynamic, wxID_ANY,
			_("Use a separate alpha image"));
		dynamic_alpha->SetValue(settings.use_alpha);
		dynamic_sizer->Add(dynamic_alpha, 0, wxLEFT | wxRIGHT | wxTOP, 12);
		auto folder_row = new wxFlexGridSizer(1, 2, 0, 10);
		folder_row->AddGrowableCol(1, 1);
		folder_row->Add(new wxStaticText(dynamic, wxID_ANY, _("Image folder:")),
			0, wxALIGN_CENTER_VERTICAL);
		directory = new wxDirPickerCtrl(dynamic, wxID_ANY, settings.directory,
			_("Select image folder"), wxDefaultPosition, wxDefaultSize,
			wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
		folder_row->Add(directory, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
		dynamic_sizer->Add(folder_row, 0, wxEXPAND | wxALL, 12);
		auto help = new wxStaticText(dynamic, wxID_ANY,
			_("Name files *.[ext] and *-alpha.[ext], where * is the subtitle line text. "
			  "Quick insert finds each selected line's image in this folder."));
		help->Wrap(FromDIP(520));
		dynamic_sizer->Add(help, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
		dynamic->SetSizer(dynamic_sizer);
		tabs->AddPage(dynamic, _("Dynamic"));
		tabs->SetSelection(std::clamp(settings.mode, 0, 1));
		content->Add(tabs, 1, wxEXPAND | wxBOTTOM, 12);
		content->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND);
		main->Add(content, 1, wxEXPAND | wxALL, 12);
		SetSizerAndFit(main);
		SetMinSize(FromDIP(wxSize(880, 430)));
		SetSize(FromDIP(wxSize(880, 430)));
	}

	Settings Result(Settings settings) const {
		options.Store(settings);
		settings.use_alpha = dynamic_alpha->GetValue();
		settings.mode = tabs->GetSelection();
		settings.directory = directory->GetPath();
		settings.basic_image = basic_image->GetPath();
		settings.basic_alpha = basic_alpha->GetPath();
		return settings;
	}
};

std::pair<int, int> Position(AssDialogue const& line) {
	std::string text = line.Text.get();
	static std::regex position(R"(\\pos\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*\))");
	static std::regex move(R"(\\move\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?))");
	std::smatch match;
	if (!std::regex_search(text, match, position) &&
		!std::regex_search(text, match, move)) return {0, 0};
	try {
		return {static_cast<int>(std::lround(std::stod(match[1].str()))),
			static_cast<int>(std::lround(std::stod(match[2].str())))};
	}
	catch (...) { return {0, 0}; }
}

std::optional<wxImage> LoadImage(wxString const& path, std::string& error) {
	if (path.empty()) return std::nullopt;
	wxImage image;
	if (!image.LoadFile(path)) {
		error = agi::format("The image could not be loaded: %s", from_wx(path));
		return std::nullopt;
	}
	return image;
}

struct Input {
	AssDialogue *line = nullptr;
	wxString image;
	wxString alpha;
};

bool Apply(agi::Context *context, std::vector<Input> const& inputs,
	Settings const& settings, std::string& error) {
	struct Work {
		AssDialogue *source = nullptr;
		std::vector<AssDialogue> output;
	};
	std::vector<Work> work;
	int const progress_per_image = 1000;
	int const progress_max = std::max(1,
		static_cast<int>(inputs.size()) * progress_per_image);
	wxProgressDialog progress(_("Image insert"), _("Loading image..."),
		progress_max, context->parent,
		wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);
	for (size_t input_index = 0; input_index < inputs.size(); ++input_index) {
		auto const& input = inputs[input_index];
		int base = static_cast<int>(input_index) * progress_per_image;
		auto message = [&](wxString const& action) {
			return wxString::Format(_("%s (%d of %d)"), action,
				static_cast<int>(input_index) + 1, static_cast<int>(inputs.size()));
		};
		progress.Update(base, message(_("Loading image")));
		if (!input.line || IsImageMaskLine(input.line)) {
			error = "Select ordinary subtitle lines to insert images.";
			return false;
		}
		auto image = LoadImage(input.image, error);
		if (!image) return false;
		auto alpha = LoadImage(input.alpha, error);
		if (!input.alpha.empty() && !alpha) return false;
		auto [x, y] = Position(*input.line);
		auto raster = imagemask::Prepare(*image, alpha ? &*alpha : nullptr,
			settings.image, x, y, error,
			[&](size_t complete, size_t total) {
				int value = base + 50 + static_cast<int>(
					complete * 425 / std::max<size_t>(1, total));
				progress.Update(value, message(_("Preparing image")));
			});
		if (!raster) return false;
		auto output = imagemask::Encode(*raster, *input.line,
			static_cast<int>(input.line->Start), static_cast<int>(input.line->End),
			[&](size_t complete, size_t total) {
				int value = base + 475 + static_cast<int>(
					complete * 500 / std::max<size_t>(1, total));
				progress.Update(value, message(_("Encoding image")));
			});
		if (output.empty()) {
			error = "The image is fully transparent; no subtitle rows were generated.";
			return false;
		}
		work.push_back({input.line, std::move(output)});
		progress.Update(base + progress_per_image, message(_("Image ready")));
	}

	Selection selection;
	AssDialogue *active = nullptr;
	std::vector<std::unique_ptr<AssDialogue>> removed;
	for (auto& item : work) {
		auto insert_at = context->ass->Events.iterator_to(*item.source);
		for (auto& row : item.output) {
			auto generated = new AssDialogue(std::move(row));
			context->ass->Events.insert(insert_at, *generated);
			selection.insert(generated);
			if (!active) active = generated;
		}
		context->ass->Events.erase(insert_at);
		removed.emplace_back(item.source);
	}
	context->selectionController->SetSelectionAndActive(std::move(selection), active);
	context->ass->Commit(_("insert image"), AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL);
	return true;
}

std::optional<wxString> DynamicFile(wxString const& directory,
	wxString const& stem, bool alpha) {
	static wxString const extensions[] = {
		"png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff", "webp"
	};
	for (auto const& extension : extensions) {
		wxFileName file(directory, stem + (alpha ? "-alpha." : ".") + extension);
		if (file.FileExists()) return file.GetFullPath();
	}
	return std::nullopt;
}

std::vector<AssDialogue *> Selected(agi::Context *context) {
	return context->selectionController->GetSortedSelection();
}

void ShowError(agi::Context *context, std::string const& error) {
	wxMessageBox(to_wx(error), _("Image insert"), wxOK | wxICON_WARNING, context->parent);
}

} // namespace

void Insert(agi::Context *context) {
	auto lines = Selected(context);
	if (lines.empty()) return;
	Settings settings = LoadSettings();
	InsertDialog dialog(context->parent, settings);
	if (dialog.ShowModal() != wxID_OK || dialog.ImagePath().empty()) return;
	dialog.Store(settings);
	std::vector<Input> inputs;
	for (auto line : lines) inputs.push_back({line, dialog.ImagePath(), dialog.AlphaPath()});
	std::string error;
	if (!Apply(context, inputs, settings, error)) ShowError(context, error);
}

void QuickInsert(agi::Context *context) {
	auto lines = Selected(context);
	if (lines.empty()) return;
	Settings settings = LoadSettings();
	std::vector<Input> inputs;
	if (settings.mode == 0) {
		if (settings.basic_image.empty() ||
			!wxFileName::FileExists(settings.basic_image)) {
			ShowError(context,
				"Set an existing Basic image in Image insert settings.");
			return;
		}
		if (!settings.basic_alpha.empty() &&
			!wxFileName::FileExists(settings.basic_alpha)) {
			ShowError(context,
				"The Basic alpha image set in Image insert settings does not exist.");
			return;
		}
		for (auto line : lines)
			inputs.push_back({line, settings.basic_image, settings.basic_alpha});
	}
	else {
		if (settings.directory.empty() || !wxFileName::DirExists(settings.directory)) {
			ShowError(context, "Set an existing Dynamic image folder in Image insert settings.");
			return;
		}
		for (auto line : lines) {
			wxString stem = to_wx(line->GetStrippedText());
			auto image = DynamicFile(settings.directory, stem, false);
			if (!image) {
				ShowError(context, agi::format("No dynamic image matches the selected line: %s",
					line->GetStrippedText()));
				return;
			}
			wxString alpha;
			if (settings.use_alpha) {
				auto found = DynamicFile(settings.directory, stem, true);
				if (!found) {
					ShowError(context, agi::format("No dynamic alpha image matches the selected line: %s",
						line->GetStrippedText()));
					return;
				}
				alpha = *found;
			}
			inputs.push_back({line, *image, alpha});
		}
	}
	std::string error;
	if (!Apply(context, inputs, settings, error)) ShowError(context, error);
}

void ShowSettings(agi::Context *context) {
	Settings settings = LoadSettings();
	SettingsDialog dialog(context->parent, settings);
	if (dialog.ShowModal() == wxID_OK) SaveSettings(dialog.Result(settings));
}

} // namespace typesetting::image_insert
