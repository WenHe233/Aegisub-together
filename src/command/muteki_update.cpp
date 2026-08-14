#include "command.h"

#include "../muteki_update.h"

namespace {
using cmd::Command;

struct check final : Command {
	CMD_NAME("muteki-update/check")
	STR_MENU("&Update...")
	STR_DISP("Update")
	#ifdef _WIN32
	STR_HELP("Check for and install the latest Muteki Aegisub version")
	#else
	STR_HELP("Check for the latest Muteki Aegisub version and open its release page")
	#endif
	void operator()(agi::Context *c) override { muteki_update::CheckForUpdates(c); }
};

struct install_version final : Command {
	CMD_NAME("muteki-update/install-version")
	#ifdef _WIN32
	STR_MENU("Install a specific &version...")
	STR_DISP("Install a specific version")
	STR_HELP("Select and install a Muteki Aegisub version")
	#else
	STR_MENU("Open a specific &release...")
	STR_DISP("Open a specific release")
	STR_HELP("Select a Muteki Aegisub version and open its release page")
	#endif
	void operator()(agi::Context *c) override { muteki_update::InstallSelectedVersion(c); }
};

struct changelog final : Command {
	CMD_NAME("muteki-update/changelog")
	STR_MENU("&Changelog...")
	STR_DISP("Changelog")
	STR_HELP("Show the Muteki Aegisub changelog")
	void operator()(agi::Context *c) override { muteki_update::ShowChangelog(c); }
};

struct automation final : Command {
	CMD_NAME("muteki-update/automation")
	STR_MENU("Update Muteki &Automation scripts...")
	STR_DISP("Update Muteki Automation scripts")
	STR_HELP("Download, install and reload the Muteki Automation scripts")
	void operator()(agi::Context *c) override { muteki_update::UpdateAutomation(c); }
};
}

namespace cmd {
void init_muteki_update() {
	reg(std::make_unique<check>());
	reg(std::make_unique<install_version>());
	reg(std::make_unique<changelog>());
	reg(std::make_unique<automation>());
}
}
