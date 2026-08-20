// Copyright (c) 2010, Amar Takhar <verm@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file preferences_base.cpp
/// @brief Base preferences dialogue classes
/// @ingroup configuration_ui

#include "preferences_base.h"

#include "colour_button.h"
#include "compat.h"
#include "format.h"
#include "options.h"
#include "preferences.h"

#include <libaegisub/exception.h>
#include <libaegisub/path.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/dirdlg.h>
#include <wx/event.h>
#include <wx/fontdlg.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/settings.h>
#include <wx/treebook.h>

#undef Bool

#define OPTION_UPDATER(type, evttype, opt, body)                            \
	class type {                                                            \
		std::string name;                                                   \
		Preferences *parent;                                                \
	public:                                                                 \
		type(std::string const& n, Preferences *p) : name(n), parent(p) { } \
		void operator()(evttype& evt) {                                     \
			evt.Skip();                                                     \
			parent->SetOption(std::make_unique<agi::opt>(name, body));      \
		}                                                                   \
	}

OPTION_UPDATER(StringUpdater, wxCommandEvent, OptionValueString, from_wx(evt.GetString()));
OPTION_UPDATER(IntUpdater, wxSpinEvent, OptionValueInt, evt.GetInt());
OPTION_UPDATER(IntCBUpdater, wxCommandEvent, OptionValueInt, evt.GetInt());
OPTION_UPDATER(DoubleUpdater, wxSpinDoubleEvent, OptionValueDouble, evt.GetValue());
OPTION_UPDATER(BoolUpdater, wxCommandEvent, OptionValueBool, !!evt.GetInt());
OPTION_UPDATER(ColourUpdater, ValueEvent<agi::Color>, OptionValueColor, evt.Get());

class StringChoiceUpdater {
	std::string name;
	Preferences *parent;
	wxArrayString values;

public:
	StringChoiceUpdater(std::string const& n, Preferences *p, const wxArrayString &values) : name(n), parent(p), values(values) { }
	void operator()(wxCommandEvent& evt) {
		evt.Skip();
		parent->SetOption(std::make_unique<agi::OptionValueString>(name, from_wx(values[evt.GetInt()])));
	}
};

static void browse_button(wxTextCtrl *ctrl) {
	wxDirDialog dlg(nullptr, _("Please choose the folder:"), config::path->Decode(from_wx(ctrl->GetValue())).wstring());
	if (dlg.ShowModal() == wxID_OK) {
		wxString dir = dlg.GetPath();
		if (!dir.empty())
			ctrl->SetValue(dir);
	}
}

static void font_button(Preferences *parent, wxTextCtrl *name, wxSpinCtrl *size) {
	wxFont font = *wxNORMAL_FONT;
	wxString fontname = name->GetValue();
	if (!fontname.empty()) font.SetFaceName(fontname);
	font.SetPointSize(size->GetValue());
	font = wxGetFontFromUser(parent, font);
	if (font.IsOk()) {
		name->SetValue(font.GetFaceName());
		size->SetValue(font.GetPointSize());
		// wxGTK doesn't generate wxEVT_SPINCTRL from SetValue
		wxSpinEvent evt(wxEVT_SPINCTRL);
		evt.SetInt(font.GetPointSize());
		size->ProcessWindowEvent(evt);
	}
}

OptionPage::OptionPage(wxTreebook *book, Preferences *parent, wxString name, int style)
: wxScrolled<wxPanel>(book, -1, wxDefaultPosition, wxDefaultSize, wxVSCROLL)
, sizer(new wxBoxSizer(wxVERTICAL))
, parent(parent)
{
	if (style & PAGE_SUB)
		book->AddSubPage(this, name, true);
	else
		book->AddPage(this, name, true);

	if (style & PAGE_SCROLL)
		SetScrollbars(0, 20, 0, 50);
	else
		SetScrollbars(0, 0, 0, 0);
	DisableKeyboardScrolling();
}

template<class T>
void OptionPage::Add(PageSection section, wxString const& label, T *control) {
	section.sizer->Add(new wxStaticText(section.box, -1, label), 1, wxALIGN_CENTRE_VERTICAL);
	section.sizer->Add(control, wxSizerFlags().Expand());
}

void OptionPage::CellSkip(PageSection section) {
	section.sizer->AddStretchSpacer();
}

wxControl *OptionPage::OptionAdd(PageSection section, const wxString &name, const char *opt_name, double min, double max, double inc) {
	parent->AddChangeableOption(opt_name);
	const auto opt = OPT_GET(opt_name);

	switch (opt->GetType()) {
		case agi::OptionType::Bool: {
			auto cb = new wxCheckBox(section.box, -1, name);
			section.sizer->Add(cb, 1, wxEXPAND, 0);
			cb->SetValue(opt->GetBool());
			cb->Bind(wxEVT_CHECKBOX, BoolUpdater(opt_name, parent));
			restorable_options.push_back({opt_name,
				[cb](agi::OptionValue const& value) { cb->SetValue(value.GetBool()); }});
			return cb;
		}

		case agi::OptionType::Int: {
			auto sc = new wxSpinCtrl(section.box, -1, std::to_wstring((int)opt->GetInt()), wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min, max, opt->GetInt());
			sc->Bind(wxEVT_SPINCTRL, IntUpdater(opt_name, parent));
			Add(section, name, sc);
			restorable_options.push_back({opt_name,
				[sc](agi::OptionValue const& value) { sc->SetValue(value.GetInt()); }});
			return sc;
		}

		case agi::OptionType::Double: {
			auto scd = new wxSpinCtrlDouble(section.box, -1, std::to_wstring(opt->GetDouble()), wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min, max, opt->GetDouble(), inc);
			scd->Bind(wxEVT_SPINCTRLDOUBLE, DoubleUpdater(opt_name, parent));
			Add(section, name, scd);
			restorable_options.push_back({opt_name,
				[scd](agi::OptionValue const& value) { scd->SetValue(value.GetDouble()); }});
			return scd;
		}

		case agi::OptionType::String: {
			auto text = new wxTextCtrl(section.box, -1 , to_wx(opt->GetString()));
			text->Bind(wxEVT_TEXT, StringUpdater(opt_name, parent));
			Add(section, name, text);
			restorable_options.push_back({opt_name,
				[text](agi::OptionValue const& value) { text->ChangeValue(to_wx(value.GetString())); }});
			return text;
		}

		case agi::OptionType::Color: {
			auto cb = new ColourButton(section.box, wxSize(40,10), false, opt->GetColor());
			cb->Bind(EVT_COLOR, ColourUpdater(opt_name, parent));
			Add(section, name, cb);
			restorable_options.push_back({opt_name,
				[cb](agi::OptionValue const& value) { cb->SetColor(value.GetColor()); }});
			return cb;
		}

		default:
			throw agi::InternalError("Unsupported type");
	}
}

wxTextCtrl *OptionPage::OptionAddMultiline(PageSection section, const char *opt_name) {
	parent->AddChangeableOption(opt_name);
	const auto opt = OPT_GET(opt_name);

	if (opt->GetType() != agi::OptionType::String) {
		throw agi::InternalError("Unsupported type for multiline option");
	}

	auto text = new wxTextCtrl(this, -1, to_wx(opt->GetString()), wxDefaultPosition, wxSize(-1, 200), wxTE_MULTILINE);
	text->Bind(wxEVT_TEXT, StringUpdater(opt_name, parent));
	section.sizer->Add(text, wxSizerFlags().Expand());
	return text;
}

void OptionPage::OptionChoice(PageSection section, const wxString &name, const wxArrayString &choices, const char *opt_name, bool translate) {
	parent->AddChangeableOption(opt_name);
	const auto opt = OPT_GET(opt_name);

	wxArrayString choices_translated;
	if (translate) {
		choices_translated.reserve(choices.size());
		std::transform(choices.begin(), choices.end(), std::back_inserter(choices_translated), [](const wxString &s) { return wxGetTranslation(s); });
	}

	auto cb = new wxComboBox(section.box, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, translate ? choices_translated : choices, wxCB_READONLY | wxCB_DROPDOWN);
	Add(section, name, cb);

	switch (opt->GetType()) {
		case agi::OptionType::Int: {
			int val = opt->GetInt();
			cb->Select(val < (int)choices.size() ? val : 0);
			cb->Bind(wxEVT_COMBOBOX, IntCBUpdater(opt_name, parent));
			restorable_options.push_back({opt_name,
				[cb](agi::OptionValue const& value) { cb->SetSelection(value.GetInt()); }});
			break;
		}
		case agi::OptionType::String: {
			wxString val(to_wx(opt->GetString()));
			if (translate)
				val = wxGetTranslation(val);

			if (cb->FindString(val) != wxNOT_FOUND)
				cb->SetStringSelection(val);
			else if (!choices.empty())
				cb->SetSelection(0);
			cb->Bind(wxEVT_COMBOBOX, StringChoiceUpdater(opt_name, parent, choices));
			restorable_options.push_back({opt_name, [cb, translate](agi::OptionValue const& value) {
				wxString selection = to_wx(value.GetString());
				if (translate) selection = wxGetTranslation(selection);
				cb->SetStringSelection(selection);
			}});
			break;
		}

		default:
			throw agi::InternalError("Unsupported type");
	}
}

void OptionPage::AddRestoreDefaultsButton() {
	auto content_sizer = sizer;
	sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(content_sizer, wxSizerFlags().Expand());

	auto button = new wxButton(this, -1, _("Restore default colors"));
	sizer->Add(button, wxSizerFlags().CenterHorizontal().Border(wxTOP, 5));
	button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		std::vector<std::unique_ptr<agi::OptionValue>> default_values;
		default_values.reserve(restorable_options.size());
		for (auto const& option : restorable_options) {
			auto default_value = OPT_GET(option.name)->CloneDefault();
			option.update_control(*default_value);
			default_values.push_back(std::move(default_value));
		}
		parent->SetOptionsImmediately(std::move(default_values));
	});
}

PageSection OptionPage::PageSizer(wxString name) {
	auto tmp_sizer = new wxStaticBoxSizer(wxHORIZONTAL, this, name);
	sizer->Add(tmp_sizer, 0,wxEXPAND, 5);
	auto flex = new wxFlexGridSizer(2,5,5);
	flex->AddGrowableCol(0,1);
	tmp_sizer->Add(flex, 1, wxEXPAND, 5);
	sizer->AddSpacer(8);
	return {flex, tmp_sizer->GetStaticBox()};
}

void OptionPage::OptionBrowse(PageSection section, const wxString &name, const char *opt_name, wxControl *enabler, bool do_enable) {
	parent->AddChangeableOption(opt_name);
	const auto opt = OPT_GET(opt_name);

	if (opt->GetType() != agi::OptionType::String)
		throw agi::InternalError("Option must be agi::OptionType::String for BrowseButton.");

	auto text = new wxTextCtrl(section.box, -1 , to_wx(opt->GetString()));
	text->SetMinSize(wxSize(160, -1));
	text->Bind(wxEVT_TEXT, StringUpdater(opt_name, parent));

	auto browse = new wxButton(section.box, -1, _("Browse..."));
	browse->Bind(wxEVT_BUTTON, std::bind(browse_button, text));

	auto button_sizer = new wxBoxSizer(wxHORIZONTAL);
	button_sizer->Add(text, wxSizerFlags(1).Expand());
	button_sizer->Add(browse, wxSizerFlags().Expand());

	Add(section, name, button_sizer);

	if (enabler) {
		if (do_enable) {
			EnableIfChecked(enabler, text);
			EnableIfChecked(enabler, browse);
		}
		else {
			DisableIfChecked(enabler, text);
			DisableIfChecked(enabler, browse);
		}
	}
}

void OptionPage::OptionFont(PageSection section, std::string opt_prefix) {
	const auto face_opt = OPT_GET(opt_prefix + "Font Face");
	const auto size_opt = OPT_GET(opt_prefix + "Font Size");

	parent->AddChangeableOption(face_opt->GetName());
	parent->AddChangeableOption(size_opt->GetName());

	auto font_name = new wxTextCtrl(section.box, -1, to_wx(face_opt->GetString()));
	font_name->SetMinSize(wxSize(160, -1));
	font_name->Bind(wxEVT_TEXT, StringUpdater(face_opt->GetName().c_str(), parent));
	font_name->SetHint(wxNORMAL_FONT->GetFaceName());

	auto font_size = new wxSpinCtrl(section.box, -1, std::to_wstring((int)size_opt->GetInt()), wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 3, 42, size_opt->GetInt());
	font_size->Bind(wxEVT_SPINCTRL, IntUpdater(size_opt->GetName().c_str(), parent));

	auto pick_btn = new wxButton(section.box, -1, _("Choose..."));
	pick_btn->Bind(wxEVT_BUTTON, std::bind(font_button, parent, font_name, font_size));

	auto button_sizer = new wxBoxSizer(wxHORIZONTAL);
	button_sizer->Add(font_name, wxSizerFlags(1).Expand());
	button_sizer->Add(pick_btn, wxSizerFlags().Expand());

	Add(section, _("Font Face"), button_sizer);
	Add(section, _("Font Size"), font_size);
}

void OptionPage::EnableIfChecked(wxControl *cbx, wxControl *ctrl) {
	auto cb = dynamic_cast<wxCheckBox*>(cbx);
	if (!cb) return;

	ctrl->Enable(cb->IsChecked());
	cb->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent& evt) { ctrl->Enable(!!evt.GetInt()); evt.Skip(); });
}

void OptionPage::DisableIfChecked(wxControl *cbx, wxControl *ctrl) {
	auto cb = dynamic_cast<wxCheckBox*>(cbx);
	if (!cb) return;

	ctrl->Enable(!cb->IsChecked());
	cb->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent& evt) { ctrl->Enable(!evt.GetInt()); evt.Skip(); });
}

static std::vector<std::string> source_rows_to_vector(std::vector<wxTextCtrl *> const& rows) {
	std::vector<std::string> values;

	for (auto row : rows)
		values.push_back(from_wx(row->GetValue()));

	if (values.empty())
		values.emplace_back("");

	return values;
}

void OptionPage::OptionLanguageFilterList(PageSection section, const char *opt_name) {
	parent->AddChangeableOption(opt_name);
	const auto opt = OPT_GET(opt_name);

	if (opt->GetType() != agi::OptionType::ListString)
		throw agi::InternalError("Option must be agi::OptionType::ListString for LanguageFilterList.");

	auto panel = new wxPanel(section.box);
	auto outer = new wxBoxSizer(wxVERTICAL);
	auto rows_sizer = new wxBoxSizer(wxVERTICAL);
	auto add_button = new wxButton(panel, -1, _("Add"));
	panel->SetSizer(outer);

	// The example characters are injected rather than written into the msgid: a
	// non-ASCII msgid is looked up through a narrow-to-wide conversion in the
	// current locale, which both breaks the lookup and mangles the fallback text.
	auto help = new wxStaticText(panel, -1, agi::wxformat(
		_("A font is offered when it has every character of one group. Separate alternative groups with a slash: %s accepts a font that has either both lower case letters or both upper case ones."),
		wxString::FromUTF8("őű/ŐŰ")));
	help->Wrap(430);
	help->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));

	struct FilterRow {
		wxTextCtrl *label;
		wxTextCtrl *condition;
		wxCheckBox *filtered;
	};
	auto rows = std::make_shared<std::vector<FilterRow>>();

	auto relayout = [=] {
		rows_sizer->Layout();
		outer->Layout();
		panel->Layout();
		section.box->Layout();
		this->Layout();
		this->FitInside();
	};

	auto save = [=] {
		std::vector<std::string> values;
		for (auto const& row : *rows) {
			wxString label = row.label->GetValue();
			label.Trim(true).Trim(false);
			wxString condition = row.condition->GetValue();
			condition.Trim(true).Trim(false);
			if (label.empty() || condition.empty()) continue;
			values.push_back(from_wx(label + "|" + condition +
				(row.filtered->GetValue() ? "|1" : "|0")));
		}
		parent->SetOption(std::make_unique<agi::OptionValueListString>(opt_name, values));
	};

	auto add_row = std::make_shared<std::function<void(wxString, wxString, bool)>>();
	*add_row = [=](wxString label, wxString condition, bool filtered) {
		auto row = new wxBoxSizer(wxHORIZONTAL);
		auto label_ctrl = new wxTextCtrl(panel, -1, label);
		label_ctrl->SetMinSize(wxSize(110, -1));
		auto condition_ctrl = new wxTextCtrl(panel, -1, condition);
		condition_ctrl->SetMinSize(wxSize(150, -1));
		auto filtered_ctrl = new wxCheckBox(panel, -1, _("On by default"));
		filtered_ctrl->SetValue(filtered);
		auto remove = new wxButton(panel, -1, _("Remove"));

		row->Add(label_ctrl, wxSizerFlags(2).Expand());
		row->Add(condition_ctrl, wxSizerFlags(3).Expand().Border(wxLEFT, 5));
		row->Add(filtered_ctrl, wxSizerFlags().Center().Border(wxLEFT, 5));
		row->Add(remove, wxSizerFlags().Center().Border(wxLEFT, 5));

		rows->push_back({label_ctrl, condition_ctrl, filtered_ctrl});

		remove->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) {
			rows->erase(std::remove_if(rows->begin(), rows->end(),
				[&](FilterRow const& item) { return item.label == label_ctrl; }), rows->end());
			rows_sizer->Detach(row);
			label_ctrl->Destroy();
			condition_ctrl->Destroy();
			filtered_ctrl->Destroy();
			remove->Destroy();
			delete row;
			save();
			relayout();
		});
		auto on_change = [=](wxCommandEvent& evt) { evt.Skip(); save(); };
		label_ctrl->Bind(wxEVT_TEXT, on_change);
		condition_ctrl->Bind(wxEVT_TEXT, on_change);
		filtered_ctrl->Bind(wxEVT_CHECKBOX, on_change);

		rows_sizer->Add(row, wxSizerFlags().Expand().Border(wxBOTTOM, 4));
	};

	for (auto const& definition : opt->GetListString()) {
		wxString value = to_wx(definition);
		wxString label = value.BeforeFirst('|');
		wxString rest = value.AfterFirst('|');
		wxString condition = rest.BeforeFirst('|');
		if (label.empty() && condition.empty()) continue;
		(*add_row)(label, condition, rest.AfterFirst('|') == "1");
	}

	add_button->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) {
		(*add_row)("", "", false);
		save();
		relayout();
	});

	outer->Add(help, wxSizerFlags().Expand().Border(wxBOTTOM, 6));
	outer->Add(rows_sizer, wxSizerFlags().Expand());
	outer->Add(add_button, wxSizerFlags().Border(wxTOP, 4));
	section.sizer->Add(panel, wxSizerFlags(1).Expand());
	relayout();
}

void OptionPage::OptionBrowseList(PageSection section, const char *opt_name) {
	parent->AddChangeableOption(opt_name);
	const auto opt = OPT_GET(opt_name);

	if (opt->GetType() != agi::OptionType::ListString)
		throw agi::InternalError("Option must be agi::OptionType::ListString for BrowseList.");

	auto panel = new wxPanel(section.box);
	auto outer = new wxBoxSizer(wxVERTICAL);
	auto rows_sizer = new wxBoxSizer(wxVERTICAL);
	auto add_button = new wxButton(panel, -1, _("Add"));

	panel->SetSizer(outer);

	auto rows = std::make_shared<std::vector<wxTextCtrl *>>();

	auto relayout = [=] {
		rows_sizer->Layout();
		outer->Layout();
		panel->Layout();
		section.box->Layout();
		this->Layout();
		this->FitInside();
	};

	auto save = [=] {
		parent->SetOption(std::make_unique<agi::OptionValueListString>(
			opt_name,
			source_rows_to_vector(*rows)
		));
	};

	auto add_row = std::make_shared<std::function<void(wxString)>>();

	*add_row = [=](wxString value) {
		auto row = new wxBoxSizer(wxHORIZONTAL);

		auto text = new wxTextCtrl(panel, -1, value);
		text->SetMinSize(wxSize(260, -1));

		auto browse = new wxButton(panel, -1, _("Browse..."));

		row->Add(text, wxSizerFlags(1).Expand());
		row->Add(browse, wxSizerFlags().Border(wxLEFT, 5));

		rows->push_back(text);

		if (rows->size() > 1) {
			auto remove = new wxButton(panel, -1, _("Remove"));
			row->Add(remove, wxSizerFlags().Border(wxLEFT, 5));

			remove->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) {
				rows->erase(std::remove(rows->begin(), rows->end(), text), rows->end());

				rows_sizer->Detach(row);

				text->Destroy();
				browse->Destroy();
				remove->Destroy();
				delete row;

				save();
				relayout();
			});
		}

		text->Bind(wxEVT_TEXT, [=](wxCommandEvent& evt) {
			evt.Skip();
			save();
		});

		browse->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) {
			wxDirDialog dlg(
				panel,
				_("Please choose the folder:"),
				config::path->Decode(from_wx(text->GetValue())).wstring()
			);

			if (dlg.ShowModal() == wxID_OK) {
				wxString dir = dlg.GetPath();
				if (!dir.empty()) {
					text->SetValue(dir);
					save();
				}
			}
		});

		rows_sizer->Add(row, wxSizerFlags().Expand().Border(wxBOTTOM, 5));
		relayout();
	};

	auto values = opt->GetListString();
	if (values.empty())
		values.emplace_back("");

	for (auto const& value : values)
		(*add_row)(to_wx(value));

	add_button->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) {
		(*add_row)("");
		save();
		relayout();
	});

	outer->Add(rows_sizer, wxSizerFlags().Expand());
	outer->Add(add_button, wxSizerFlags().Border(wxTOP, 2));

	section.sizer->Add(panel, wxSizerFlags(1).Expand());
	section.sizer->AddSpacer(0);
}
