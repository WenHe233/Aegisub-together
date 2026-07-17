#define ARCH 64

#include "fragment_setupbase.iss"
#include "fragment_strings.iss"

[Setup]
AppID={{20ABE862-D228-445F-9024-5D014F2F4362}
DefaultDirName={commonpf}\Aegisub Together
PrivilegesRequired=poweruser
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64

#include "fragment_mainprogram.iss"
#include "fragment_associations.iss"
#include "fragment_codecs.iss"
#include "fragment_automation.iss"
#include "fragment_translations.iss"
#include "fragment_spelling.iss"
#include "fragment_runtimes.iss"

[Code]
#include "fragment_shell_code.iss"
#include "fragment_beautify_code.iss"

procedure InitializeWizard;
begin
  InitializeWizardBeautify;
end;

function InitializeSetup: Boolean;
begin
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Updates: String;
begin
  if CurStep = ssPostInstall then
  begin
    if WizardIsTaskSelected('checkforupdates') then
      Updates := 'true'
    else
      Updates := 'false';

    SaveStringToFile(
      ExpandConstant('{app}\installer_config.json'),
      FmtMessage('{"App": {"Auto": {"Check For Updates": %1}, "First Start": false, "Language": "%2"}}', [
        Updates,
        ExpandConstant('{language}')]),
      False);
  end;
end;
