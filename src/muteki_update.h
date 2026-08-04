#pragma once

#include <wx/arrstr.h>

namespace agi { struct Context; }

namespace muteki_update {
bool RunHelperMode(wxArrayString const& args);
void LaunchPendingUpdate();
void CheckForUpdates(agi::Context *context);
void InstallSelectedVersion(agi::Context *context);
void ShowChangelog(agi::Context *context);
void UpdateAutomation(agi::Context *context);
}
