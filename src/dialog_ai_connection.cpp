// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#include "dialog_ai_connection.h"

#include "ai_client.h"
#include "compat.h"
#include "options.h"

#include <libaegisub/option.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/hyperlink.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

namespace {

class AIConnectionDialog final : public wxDialog {
	wxTextCtrl *key;
	wxTextCtrl *model;
	wxTextCtrl *selection_model;
	wxTextCtrl *image_model;
	wxTextCtrl *transcription_model;
	wxTextCtrl *karaoke_timing_model;
	wxTextCtrl *review_instructions;
	wxCheckBox *advanced_karaoke_timing;
	wxCheckBox *remember;
	wxStaticText *status;
	bool require_key;

	void UpdateStatus() {
		wxString text;
		switch (ai::GetApiKeySource()) {
			case ai::ApiKeySource::Session:
				text = _("API key is available for this Aegisub session.");
				break;
			case ai::ApiKeySource::Environment:
				text = _("API key is provided by the OPENAI_API_KEY environment variable.");
				break;
			case ai::ApiKeySource::CredentialManager:
				text = _("API key is stored securely in Windows Credential Manager.");
				break;
			case ai::ApiKeySource::None:
				text = _("No API key is configured.");
				break;
		}
		status->SetLabel(text);
		status->Wrap(520);
		Layout();
	}

	std::string EnteredOrSavedKey() const {
		auto entered = from_wx(key->GetValue());
		return entered.empty() ? ai::GetApiKey() : entered;
	}

	void OnTest(wxCommandEvent&) {
		auto api_key = EnteredOrSavedKey();
		if (api_key.empty()) {
			wxMessageBox(_("Enter an OpenAI API key first."), _("AI connection"),
				wxOK | wxICON_WARNING, this);
			return;
		}

		wxBusyCursor busy;
		try {
			ai::OpenAIClient client(api_key, from_wx(model->GetValue()),
				from_wx(transcription_model->GetValue()),
				from_wx(review_instructions->GetValue()));
			client.TestConnection();
			auto selection_model_name = from_wx(selection_model->GetValue());
			if (selection_model_name != from_wx(model->GetValue())) {
				ai::OpenAIClient selection_client(api_key, selection_model_name,
					from_wx(transcription_model->GetValue()));
				selection_client.TestConnection();
			}
			wxMessageBox(_("The OpenAI connection is working."), _("AI connection"),
				wxOK | wxICON_INFORMATION, this);
		}
		catch (std::exception const& error) {
			wxMessageBox(to_wx(error.what()), _("AI connection failed"),
				wxOK | wxICON_ERROR, this);
		}
	}

	void OnDelete(wxCommandEvent&) {
		if (wxMessageBox(_("Delete the API key stored by Aegisub on this computer?"),
			_("Delete API key"), wxYES_NO | wxICON_QUESTION, this) != wxYES)
			return;

		std::string error;
		if (!ai::DeleteStoredApiKey(&error)) {
			wxMessageBox(to_wx(error), _("The API key could not be deleted"),
				wxOK | wxICON_ERROR, this);
			return;
		}
		key->Clear();
		UpdateStatus();
	}

	void OnOK(wxCommandEvent&) {
		auto entered = from_wx(key->GetValue());
		if (!entered.empty()) {
			if (remember->IsChecked()) {
				std::string error;
				if (!ai::StoreApiKey(entered, &error)) {
					wxMessageBox(to_wx(error), _("The API key could not be saved"),
						wxOK | wxICON_ERROR, this);
					return;
				}
			}
			else {
				ai::SetSessionApiKey(std::move(entered));
			}
		}

		if (require_key && ai::GetApiKey().empty()) {
			wxMessageBox(_("An OpenAI API key is required for AI subtitle review."),
				_("AI connection"), wxOK | wxICON_WARNING, this);
			return;
		}

		OPT_SET("AI/OpenAI/Model")->SetString(from_wx(model->GetValue()));
		OPT_SET("AI/OpenAI/Selection Model")->SetString(from_wx(selection_model->GetValue()));
		OPT_SET("AI/OpenAI/Image Model")->SetString(from_wx(image_model->GetValue()));
		OPT_SET("AI/OpenAI/Transcription Model")->SetString(from_wx(transcription_model->GetValue()));
		OPT_SET("AI/OpenAI/Karaoke Timing Model")->SetString(from_wx(karaoke_timing_model->GetValue()));
		OPT_SET("AI/OpenAI/Advanced Karaoke Timing")->SetBool(advanced_karaoke_timing->IsChecked());
		OPT_SET("AI/Review/Instructions")->SetString(from_wx(review_instructions->GetValue()));
		config::opt->Flush();
		EndModal(wxID_OK);
	}

public:
	AIConnectionDialog(wxWindow *parent, bool require_key)
	: wxDialog(parent, wxID_ANY, _("AI connection"), wxDefaultPosition,
		wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, require_key(require_key) {
		auto main = new wxBoxSizer(wxVERTICAL);

		auto intro = new wxStaticText(this, wxID_ANY,
			_("Each user connects with their own OpenAI API key. The key is never stored in subtitle files or Aegisub's configuration file."));
		intro->Wrap(520);
		main->Add(intro, wxSizerFlags().Expand().Border());

		status = new wxStaticText(this, wxID_ANY, "");
		main->Add(status, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));

		auto form = new wxFlexGridSizer(2, 8, 8);
		form->AddGrowableCol(1, 1);
		form->Add(new wxStaticText(this, wxID_ANY, _("New API key:")), 0, wxALIGN_CENTER_VERTICAL);
		key = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
		key->SetHint(_("Leave empty to keep using the current key"));
		form->Add(key, wxSizerFlags(1).Expand());

		form->Add(new wxStaticText(this, wxID_ANY, _("AI model:")), 0, wxALIGN_CENTER_VERTICAL);
		model = new wxTextCtrl(this, wxID_ANY, to_wx(OPT_GET("AI/OpenAI/Model")->GetString()));
		form->Add(model, wxSizerFlags(1).Expand());

		form->Add(new wxStaticText(this, wxID_ANY, _("AI selection model:")), 0, wxALIGN_CENTER_VERTICAL);
		selection_model = new wxTextCtrl(this, wxID_ANY,
			to_wx(OPT_GET("AI/OpenAI/Selection Model")->GetString()));
		form->Add(selection_model, wxSizerFlags(1).Expand());

		form->Add(new wxStaticText(this, wxID_ANY, _("Image model:")), 0, wxALIGN_CENTER_VERTICAL);
		image_model = new wxTextCtrl(this, wxID_ANY,
			to_wx(OPT_GET("AI/OpenAI/Image Model")->GetString()));
		form->Add(image_model, wxSizerFlags(1).Expand());

		form->Add(new wxStaticText(this, wxID_ANY, _("Transcription model:")), 0, wxALIGN_CENTER_VERTICAL);
		transcription_model = new wxTextCtrl(this, wxID_ANY,
			to_wx(OPT_GET("AI/OpenAI/Transcription Model")->GetString()));
		form->Add(transcription_model, wxSizerFlags(1).Expand());

		form->Add(new wxStaticText(this, wxID_ANY, _("Karaoke timing model:")), 0, wxALIGN_CENTER_VERTICAL);
		karaoke_timing_model = new wxTextCtrl(this, wxID_ANY,
			to_wx(OPT_GET("AI/OpenAI/Karaoke Timing Model")->GetString()));
		karaoke_timing_model->SetHint(_("Use whisper-1 for word timestamps"));
		form->Add(karaoke_timing_model, wxSizerFlags(1).Expand());
		main->Add(form, wxSizerFlags().Expand().Border());
		advanced_karaoke_timing = new wxCheckBox(this, wxID_ANY,
			_("Use advanced karaoke timing rules"));
		advanced_karaoke_timing->SetValue(
			OPT_GET("AI/OpenAI/Advanced Karaoke Timing")->GetBool());
		main->Add(advanced_karaoke_timing,
			wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		auto advanced_help = new wxStaticText(this, wxID_ANY,
			_("Disable this option to restore the previous AI timing behavior."));
		advanced_help->Wrap(520);
		main->Add(advanced_help, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		main->Add(new wxHyperlinkCtrl(this, wxID_ANY,
			_("Advanced karaoke timing guide"),
			"https://docs.karaokes.moe/contrib-guide/create-karaoke/advanced-timing/index.html"),
			wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		main->Add(new wxHyperlinkCtrl(this, wxID_ANY,
			_("Available OpenAI models and pricing"),
			"https://developers.openai.com/api/docs/models"),
			wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM));

		remember = new wxCheckBox(this, wxID_ANY,
			_("Store the key securely in Windows Credential Manager"));
#ifdef _WIN32
		remember->SetValue(true);
#else
		remember->SetValue(false);
		remember->Disable();
#endif
		main->Add(remember, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM));

		main->Add(new wxStaticText(this, wxID_ANY, _("Additional review instructions:")),
			wxSizerFlags().Border(wxLEFT | wxRIGHT));
		review_instructions = new wxTextCtrl(this, wxID_ANY,
			to_wx(OPT_GET("AI/Review/Instructions")->GetString()),
			wxDefaultPosition, wxSize(-1, 90), wxTE_MULTILINE);
		main->Add(review_instructions, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));

		auto actions = new wxBoxSizer(wxHORIZONTAL);
		auto test = new wxButton(this, wxID_ANY, _("Test connection"));
		auto remove = new wxButton(this, wxID_ANY, _("Delete stored key"));
		actions->Add(test);
		actions->Add(remove, wxSizerFlags().Border(wxLEFT));
		actions->AddStretchSpacer();
		actions->Add(new wxButton(this, wxID_CANCEL, _("Cancel")));
		actions->Add(new wxButton(this, wxID_OK, _("Save")), wxSizerFlags().Border(wxLEFT));
		main->Add(actions, wxSizerFlags().Expand().Border());

		SetSizerAndFit(main);
		SetMinSize(FromDIP(wxSize(600, 430)));
		CenterOnParent();

		test->Bind(wxEVT_BUTTON, &AIConnectionDialog::OnTest, this);
		remove->Bind(wxEVT_BUTTON, &AIConnectionDialog::OnDelete, this);
		Bind(wxEVT_BUTTON, &AIConnectionDialog::OnOK, this, wxID_OK);
		UpdateStatus();
	}
};

} // namespace

bool ShowAIConnectionDialog(wxWindow *parent, bool require_key) {
	AIConnectionDialog dialog(parent, require_key);
	return dialog.ShowModal() == wxID_OK && (!require_key || !ai::GetApiKey().empty());
}
