// Copyright (c) 2005-2010, Niels Martin Hansen
// Copyright (c) 2005-2010, Rodrigo Braz Monteiro
// Copyright (c) 2010, Amar Takhar
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//	 this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//	 this list of conditions and the following disclaimer in the documentation
//	 and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//	 may be used to endorse or promote products derived from this software
//	 without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "compat.h"
#include "command.h"

#include "../auto4_base.h"
#include "../dialogs.h"
#include "../frame_main.h"
#include "../include/aegisub/context.h"
#include "../libresrc/libresrc.h"
#include "../options.h"

#include <wx/menu.h>
#include <wx/timer.h>

static const std::vector<std::string>& RecentPrefixes() {
	static const std::vector<std::string> prefixes = {
		"nyaa/",
		"muteki/"
	};

	return prefixes;
}

static std::string FormatRecentName(std::string name) {
	// cut first 3 part
	for (int i = 0; i < 3; ++i) {
		size_t pos = name.find('/');

		if (pos == std::string::npos)
			break;

		name.erase(0, pos + 1);
	}

	// cut optional group (like "nyaa/")
	for (auto const& prefix : RecentPrefixes()) {
		if (name.rfind(prefix, 0) == 0) {
			name.erase(0, prefix.size());
			break;
		}
	}

	return name;
}

namespace {
	using cmd::Command;

struct reload_all final : public Command {
	CMD_NAME("am/reload")
	STR_MENU("&Reload Automation scripts")
	STR_DISP("Reload Automation scripts")
	STR_HELP("Reload all Automation scripts and rescan the autoload folder")

	void operator()(agi::Context *c) override {
		config::global_scripts->Reload();
		c->local_scripts->Reload();
		c->frame->StatusTimeout(_("Reloaded all Automation scripts"));
	}
};

struct reload_autoload final : public Command {
	CMD_NAME("am/reload/autoload")
	STR_MENU("R&eload autoload Automation scripts")
	STR_DISP("Reload autoload Automation scripts")
	STR_HELP("Rescan the Automation autoload folder")

	void operator()(agi::Context *c) override {
		config::global_scripts->Reload();
		c->frame->StatusTimeout(_("Reloaded autoload Automation scripts"));
	}
};

struct open_manager final : public Command {
	CMD_NAME("am/manager")
	CMD_ICON(automation_toolbutton)
	STR_MENU("&Automation...")
	STR_DISP("Automation")
	STR_HELP("Open automation manager")

	void operator()(agi::Context *c) override {
		ShowAutomationDialog(c);
	}
};

struct meta final : public Command {
	CMD_NAME("am/meta")
	CMD_ICON(automation_toolbutton)
	STR_MENU("&Automation...")
	STR_DISP("Automation")
	STR_HELP("Open automation manager. Ctrl: Rescan autoload folder. Ctrl+Shift: Rescan autoload folder and reload all automation scripts")

	void operator()(agi::Context *c) override {
		if (wxGetMouseState().CmdDown()) {
			if (wxGetMouseState().ShiftDown())
				cmd::call("am/reload", c);
			else
				cmd::call("am/reload/autoload", c);
		}
		else
			cmd::call("am/manager", c);
	}
};

struct automation_last final : public Command {
	CMD_NAME("am/last")
	CMD_ICON(redo_button)
	STR_MENU("Run last script")
	STR_DISP("Run last script")
	STR_HELP("Run last automation script, press twice fast to open last options")

	void operator()(agi::Context *c) override {
		static wxTimer *timer = nullptr;
		static agi::Context *timer_context = nullptr;

		auto recent = OPT_GET("Automation/Recent")->GetListString();

		recent.erase(
			std::remove_if(recent.begin(), recent.end(), [](std::string const& s) {
				return s.empty();
			}),
			recent.end());

		if (recent.empty())
			return;

		if (timer && timer->IsRunning()) {
			timer->Stop();

			wxMenu menu;
			const size_t max_items = std::min<size_t>(recent.size(), 10);

			for (size_t i = 0; i < max_items; ++i) {
				wxString label(FormatRecentName(recent[i]));

				const int id = wxID_HIGHEST + 1000 + int(i);
				menu.Append(id, label);

				menu.Bind(wxEVT_MENU, [c, recent, i](wxCommandEvent&) {
					cmd::call(recent[i], c);
				}, id);
			}

			c->frame->PopupMenu(&menu, c->frame->ScreenToClient(wxGetMousePosition()));

			return;
		}

		timer_context = c;

		if (!timer) {
			timer = new wxTimer(c->frame);

			c->frame->Bind(wxEVT_TIMER, [](wxTimerEvent&) {
				auto recent = OPT_GET("Automation/Recent")->GetListString();

				recent.erase(
					std::remove_if(recent.begin(), recent.end(), [](std::string const& s) {
						return s.empty();
					}),
					recent.end());

				if (!recent.empty() && timer_context)
					cmd::call(recent.front(), timer_context);
			}, timer->GetId());
		}

		timer->StartOnce(250);
	}
};

}

namespace cmd {
	void init_automation() {
		reg(std::make_unique<meta>());
		reg(std::make_unique<open_manager>());
		reg(std::make_unique<reload_all>());
		reg(std::make_unique<reload_autoload>());
		reg(std::make_unique<automation_last>());
	}
}
