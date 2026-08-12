#include "../ass_dialogue.h"
#include "../compat.h"
#include "../image_mask_combiner.h"
#include "../utils.h"
#include "../selection_controller.h"
#include "../subs_controller.h"
#include "../typesetting_gradient.h"
#include "../include/aegisub/context.h"

#include <wx/dialog.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/button.h>

#include <cmath>
#include <memory>
#include <vector>
#include <string>
#include <iomanip>

static std::string GradientByChar(const std::string& text)
{
	struct Block {
		std::string tag;
		std::string txt;
	};

	auto splitUtf8 = [](const std::string &s) {
		std::vector<std::string> chars;
		for (size_t i = 0; i < s.size();) {
			unsigned char c = s[i];
			size_t len = 1;

			if ((c & 0x80) == 0)
				len = 1;
			else if ((c & 0xE0) == 0xC0)
				len = 2;
			else if ((c & 0xF0) == 0xE0)
				len = 3;
			else if ((c & 0xF8) == 0xF0)
				len = 4;

			chars.push_back(s.substr(i, len));
			i += len;
		}

		return chars;
	};

	auto parseBlocks = [](const std::string& s) {
		std::vector<Block> blocks;
		size_t i = 0;

		while (i < s.size()) {
			if (s[i] != '{') {
				i++;
				continue;
			}

			size_t end = s.find('}', i);
			if (end == std::string::npos)
				break;

			std::string tag = s.substr(i, end - i + 1);
			i = end + 1;

			size_t next = s.find('{', i);
			std::string txt;

			if (next == std::string::npos) {
				txt = s.substr(i);
				i = s.size();
			} else {
				txt = s.substr(i, next - i);
				i = next;
			}

			blocks.push_back({ tag, txt });
		}

		return blocks;
	};

	auto parseTags = [](const std::string& tag) {
		std::map<std::string,std::string> tags;
		size_t i = 0;

		while (i < tag.size()) {
			if (tag[i] != '\\') {
				i++;
				continue;
			}

			i++;
			size_t start = i;

			while (i < tag.size() && isalpha((unsigned char)tag[i]))
				i++;

			std::string name = tag.substr(start, i - start);
			start = i;

			while (i < tag.size() && tag[i] != '\\' && tag[i] != '}')
				i++;

			std::string val = tag.substr(start, i - start);
			tags[name] = val;
		}

		return tags;
	};

	auto interpolate = [](double f, double a, double b) {
		return a + (b - a) * f;
	};

	auto strToDouble = [](const std::string& s) {
		return strtod(s.c_str(), nullptr);
	};

	auto floatToStr = [](double v) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%.3f", v);

		std::string s(buf);

		while (!s.empty() && s.back() == '0')
			s.pop_back();

		if (!s.empty() && s.back() == '.')
			s.pop_back();

		return s;
	};

	auto colorInterp = [&](double f, std::string a, std::string b) {
		if (a.size() < 8 || b.size() < 8)
			return b;

		a = a.substr(2, 6);
		b = b.substr(2, 6);

		int b1 = strtol(a.substr(0, 2).c_str(), nullptr, 16);
		int g1 = strtol(a.substr(2, 2).c_str(), nullptr, 16);
		int r1 = strtol(a.substr(4, 2).c_str(), nullptr, 16);

		int b2 = strtol(b.substr(0, 2).c_str(), nullptr, 16);
		int g2 = strtol(b.substr(2, 2).c_str(), nullptr, 16);
		int r2 = strtol(b.substr(4, 2).c_str(), nullptr, 16);

		int b3 = (int)std::round(interpolate(f, b1, b2));
		int g3 = (int)std::round(interpolate(f, g1, g2));
		int r3 = (int)std::round(interpolate(f, r1, r2));

		b3 = std::clamp(b3, 0, 255);
		g3 = std::clamp(g3, 0, 255);
		r3 = std::clamp(r3, 0, 255);

		char buf[16];
		snprintf(buf, sizeof(buf), "&H%02X%02X%02X&", b3, g3, r3);

		return std::string(buf);
	};

	auto blocks = parseBlocks(text);

	if (blocks.size() < 2)
		return text;

	std::map<std::string,std::string> current_state;
	std::string result;

	for (size_t i = 1; i < blocks.size(); i++) {
		auto startTags = parseTags(blocks[i-1].tag);
		auto endTags = parseTags(blocks[i].tag);

		for (auto &p:startTags)
			current_state[p.first] = p.second;

		result += blocks[i-1].tag;

		auto chars = splitUtf8(blocks[i-1].txt);
		int total = chars.size();
		int idx = 1;
		std::map<std::string,std::string> char_state = current_state;
		bool first = true;

		for (auto &ch:chars) {
			if (first) {
				result += ch;
				first = false;

				continue;
			}

			if (isspace((unsigned char)ch[0])) {
				idx++;
				result += ch;

				continue;
			}

			double factor = (double)idx / total;
			idx++;
			std::string tagblock;

			for (auto &p:current_state) {
				std::string tag = p.first;
				std::string val;
				std::string eval;
				std::string sval;

				auto itEnd = endTags.find(tag);
				if (itEnd != endTags.end())
					eval = itEnd->second;
				else
					continue;				

				auto it = current_state.find(tag);
				if (it != current_state.end())
					sval = it->second;
				else
					sval = "0";

				if (sval == eval)
					continue;

				if (tag == "c" || tag == "2c" || tag == "3c" || tag == "4c")
					val = colorInterp(factor, sval, eval);
				else if (tag == "frz" || tag == "frx" || tag == "fry") {
					double a = strToDouble(sval);
					double b = strToDouble(eval);

					a = fmod(a, 360);
					b = fmod(b, 360);

					double d = b - a;

					if (fabs(d) > 180)
						a += (d*360) / fabs(d);

					double v = interpolate(factor, a, b);

					if (v < 0)
						v += 360;

					val = floatToStr(v);
				} else {
					double v = interpolate(factor, strToDouble(sval), strToDouble(eval));
					val = floatToStr(v);
				}

				if (val != char_state[tag]) {
					tagblock += "\\" + tag + val;
					char_state[tag] = val;
				}
			}

			if (!tagblock.empty())
				result +="{*" + tagblock +"}";

			result += ch;
		}
	}

	result += blocks.back().tag + blocks.back().txt;

	return result;
}

static std::string ReplaceTextKeepGradient(AssDialogue *line, const std::string &newText, bool keepGradient)
{
	auto splitUtf8 = [](const std::string &s) {
		std::vector<std::string> chars;

		for (size_t i = 0; i < s.size();) {
			unsigned char c = s[i];
			size_t len = 1;

			if ((c & 0x80) == 0)
				len = 1;
			else if ((c & 0xE0) == 0xC0)
				len = 2;
			else if ((c & 0xF0) == 0xE0)
				len = 3;
			else if ((c & 0xF8) == 0xF0)
				len = 4;

			chars.push_back(s.substr(i, len));
			i += len;
		}

		return chars;
	};

	std::string raw = line->Text.get();
	auto blocks = line->ParseTags();
	std::string prefix;

	for (auto &b : blocks) {
		if (b->GetType() == AssBlockType::OVERRIDE) {
			prefix = b->GetText();
			break;
		}
	}

	if (!keepGradient || raw.find("{*\\") == std::string::npos)
		return prefix + newText;

	struct EndPoint {
		int pos;
		std::string tag;
	};

	std::vector<EndPoint> endPoints;
	int charPos = 0;
	bool firstOverrideSeen = false;

	for (auto &b : blocks) {
		if (b->GetType() == AssBlockType::PLAIN) {
			auto chars = splitUtf8(b->GetText());
			charPos += chars.size();
		} else if (b->GetType() == AssBlockType::OVERRIDE) {
			std::string tag = b->GetText();

			if (!firstOverrideSeen) {
				firstOverrideSeen = true;
				continue;
			}

			if (tag.find("{*\\") != std::string::npos)
				continue;

			endPoints.push_back({ charPos, tag });
		}
	}

	if (endPoints.empty())
		return prefix + newText;

	auto oldChars = splitUtf8(line->GetStrippedText());
	auto newChars = splitUtf8(newText);

	int oldLen = oldChars.size();
	int newLen = newChars.size();

	std::vector<int> newPositions;

	for (auto &p : endPoints) {
		double ratio = (double)p.pos / (double)oldLen;
		int newPos = (int)(ratio * newLen);

		if (newPos < 0)
			newPos = 0;
		if (newPos > newLen)
			newPos = newLen;

		newPositions.push_back(newPos);
	}

	std::string result = prefix;
	int current = 0;

	for (size_t i = 0; i < endPoints.size(); i++) {
		int pos = newPositions[i];

		while (current < pos && current < newLen) {
			result += newChars[current];
			current++;
		}

		result += endPoints[i].tag;
	}

	while (current < newLen) {
		result += newChars[current];
		current++;
	}

	return GradientByChar(result);
}

void EditChangeText(agi::Context *c)
{
	AssDialogue *line = c->selectionController->GetActiveLine();
	if (!line)
		return;

	bool collapsedGradient = c->imageMask && c->imageMask->IsGradientGroup(line) &&
		c->imageMask->IsGroupStart(line) && c->imageMask->IsCollapsed(line);
	std::unique_ptr<AssDialogue> gradientSource;
	if (collapsedGradient) {
		auto source = typesetting::gradient::GroupSourceEntry(*c->ass, *line);
		if (!source.empty()) {
			try {
				gradientSource = std::make_unique<AssDialogue>(source);
			}
			catch (...) {
				collapsedGradient = false;
			}
		}
		else {
			collapsedGradient = false;
		}
	}

	std::string text = gradientSource ? gradientSource->GetStrippedText() :
		line->GetStrippedText();

	wxSize size(600,400);
	wxPoint parentPos = c->parent->GetScreenPosition();
	wxSize parentSize = c->parent->GetSize();
	wxPoint pos(parentPos.x + (parentSize.x - size.x) / 2, parentPos.y + (parentSize.y - size.y) / 2);
	wxDialog dialog(c->parent, wxID_ANY, _("Change text"), pos, size);
	wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);
	wxTextCtrl *textCtrl = new wxTextCtrl(&dialog, wxID_ANY, to_wx(text), wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_RICH2);
	wxCheckBox *keepGradients = new wxCheckBox( &dialog, wxID_ANY, _("Keep gradients"));

	keepGradients->SetValue(true);

	wxStdDialogButtonSizer *btnSizer = new wxStdDialogButtonSizer();
	btnSizer->AddButton(new wxButton(&dialog, wxID_OK));
	btnSizer->AddButton(new wxButton(&dialog, wxID_CANCEL));
	btnSizer->Realize();

	mainSizer->Add(textCtrl, 1, wxEXPAND | wxALL, 10);
	mainSizer->Add(keepGradients, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
	mainSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 10);

	dialog.SetSizer(mainSizer);

	dialog.Bind(wxEVT_CHAR_HOOK, [&](wxKeyEvent& e) {
		if (e.GetKeyCode() == WXK_RETURN) {
			if (e.ShiftDown()) {
				long pos = textCtrl->GetInsertionPoint();
				textCtrl->WriteText("\\N");
				textCtrl->SetInsertionPoint(pos + 2);
			} else {
				dialog.EndModal(wxID_OK);
			}

			return;
		}

		if (e.GetKeyCode() == WXK_ESCAPE) {
			dialog.EndModal(wxID_CANCEL);
			return;
		}

		e.Skip();
	});

	dialog.Show();
	dialog.CallAfter([=]{
		textCtrl->SetFocus();
		textCtrl->SetSelection(textCtrl->GetLastPosition(), 0);
	});

	if (dialog.ShowModal() != wxID_OK)
		return;
	std::string replacement = from_wx(textCtrl->GetValue());
	if (collapsedGradient && gradientSource) {
		gradientSource->Text = ReplaceTextKeepGradient(gradientSource.get(), replacement,
			keepGradients->GetValue());
		if (typesetting::gradient::RegenerateGroupText(c, *line, *gradientSource))
			return;
	}

	auto const& sel = c->selectionController->GetSelectedSet();
	for (auto selectedLine : sel) {
		selectedLine->Text = ReplaceTextKeepGradient(selectedLine, replacement,
			keepGradients->GetValue());
	}

	c->ass->Commit(_("change text"), AssFile::COMMIT_DIAG_TEXT);
}
