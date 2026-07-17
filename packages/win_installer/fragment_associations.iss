[Files]
DestDir: {commontemplates}; Source: template.ass; DestName: AegisubTogether.ass

[Registry]
; File type registration
; Application registration for Open With dialogue
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "@{app}\aegisub-together.exe,-10000"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe"; ValueType: string; ValueName: "ApplicationCompany"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\shell"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\Applications\aegisub-together.exe\shell\open"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "@{app}\aegisub-together.exe,-10000"; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\Applications\aegisub-together.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\aegisub-together.exe"" ""%1"""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".ass"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".ssa"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".srt"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".sub"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".ttxt"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".txt"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".mkv"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".mka"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".mks"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".avi"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".mp3"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".mp4"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".aac"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".m4a"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".wav"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".ogg"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".avs"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".vpy"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".opus"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".h264"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".hevc"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".eac3"; ValueData: ""; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\aegisub-together.exe\SupportedTypes"; ValueType: string; ValueName: ".webm"; ValueData: ""; Flags: uninsdeletekey
; Class for general subtitle formats
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Subtitle.1"; ValueType: string; ValueName: ""; ValueData: "Aegisub Together subtitle file"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Subtitle.1"; ValueType: dword; ValueName: "EditFlags"; ValueData: $af0; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Subtitle.1"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "@{app}\aegisub-together.exe,-10101"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Subtitle.1\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\aegisub-together.exe,0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Subtitle.1\shell"; ValueType: string; ValueName: ""; ValueData: "open"; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.Subtitle.1\shell\open"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.Subtitle.1\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\aegisub-together.exe"" ""%L"""; Flags: uninsdeletekey
; Class for .ass files
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.ASSA.1"; ValueType: string; ValueName: ""; ValueData: "Aegisub Together Advanced SSA subtitles"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.ASSA.1"; ValueType: dword; ValueName: "EditFlags"; ValueData: $af0; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.ASSA.1"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "@{app}\aegisub-together.exe,-10102"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.ASSA.1\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\aegisub-together.exe,0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.ASSA.1\shell"; ValueType: string; ValueName: ""; ValueData: "open"; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.ASSA.1\shell\open"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.ASSA.1\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\aegisub-together.exe"" ""%L"""; Flags: uninsdeletekey
; Class for .ssa files
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.SSA.1"; ValueType: string; ValueName: ""; ValueData: "Aegisub Together SubStation Alpha subtitles"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.SSA.1"; ValueType: dword; ValueName: "EditFlags"; ValueData: $af0; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.SSA.1"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "@{app}\aegisub-together.exe,-10103"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.SSA.1\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\aegisub-together.exe,0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.SSA.1\shell"; ValueType: string; ValueName: ""; ValueData: "open"; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.SSA.1\shell\open"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.SSA.1\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\aegisub-together.exe"" ""%L"""; Flags: uninsdeletekey
; Class for .srt files
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.SRT.1"; ValueType: string; ValueName: ""; ValueData: "Aegisub Together SubRip text subtitles"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.SRT.1"; ValueType: dword; ValueName: "EditFlags"; ValueData: $af0; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.SRT.1"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "@{app}\aegisub-together.exe,-10104"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.SRT.1\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\aegisub-together.exe,0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.SRT.1\shell"; ValueType: string; ValueName: ""; ValueData: "open"; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.SRT.1\shell\open"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.SRT.1\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\aegisub-together.exe"" ""%L"""; Flags: uninsdeletekey
; Class for .ttxt files
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.TTXT.1"; ValueType: string; ValueName: ""; ValueData: "Aegisub Together MPEG-4 timed text"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.TTXT.1"; ValueType: dword; ValueName: "EditFlags"; ValueData: $af0; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.TTXT.1"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "@{app}\aegisub-together.exe,-10105"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.TTXT.1\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\aegisub-together.exe,0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.TTXT.1\shell"; ValueType: string; ValueName: ""; ValueData: "open"; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.TTXT.1\shell\open"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.TTXT.1\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\aegisub-together.exe"" ""%L"""; Flags: uninsdeletekey
; Class for .mks files
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.MKS.1"; ValueType: string; ValueName: ""; ValueData: "Aegisub Together Matroska subtitles"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.MKS.1"; ValueType: dword; ValueName: "EditFlags"; ValueData: $af0; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.MKS.1"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "@{app}\aegisub-together.exe,-10106"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.MKS.1\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\aegisub-together.exe,0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.MKS.1\shell"; ValueType: string; ValueName: ""; ValueData: "open"; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.MKS.1\shell\open"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.MKS.1\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\aegisub-together.exe"" ""%L"""; Flags: uninsdeletekey
; Class for .txt files
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.TXT.1"; ValueType: string; ValueName: ""; ValueData: "Aegisub Together raw text file"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.TXT.1"; ValueType: dword; ValueName: "EditFlags"; ValueData: $af0; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.TXT.1"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "@{app}\aegisub-together.exe,-10107"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.TXT.1\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\aegisub-together.exe,0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.TXT.1\shell"; ValueType: string; ValueName: ""; ValueData: "open"; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.TXT.1\shell\open"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.TXT.1\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\aegisub-together.exe"" ""%L"""; Flags: uninsdeletekey
; Class for undecideable media file types
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Media.1"; ValueType: string; ValueName: ""; ValueData: "Aegisub Together media file"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Media.1"; ValueType: dword; ValueName: "EditFlags"; ValueData: $af0; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Media.1"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "@{app}\aegisub-together.exe,-10108"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Media.1\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\aegisub-together.exe,0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Media.1\shell"; ValueType: string; ValueName: ""; ValueData: "open"; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.Media.1\shell\open"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.Media.1\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\aegisub-together.exe"" ""%L"""; Flags: uninsdeletekey
; Class for audio file types
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Audio.1"; ValueType: string; ValueName: ""; ValueData: "Aegisub Together audio file"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Audio.1"; ValueType: dword; ValueName: "EditFlags"; ValueData: $af0; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Audio.1"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "@{app}\aegisub-together.exe,-10109"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Audio.1\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\aegisub-together.exe,0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Audio.1\shell"; ValueType: string; ValueName: ""; ValueData: "open"; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.Audio.1\shell\open"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.Audio.1\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\aegisub-together.exe"" ""%L"""; Flags: uninsdeletekey
; Class for video file types
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Video.1"; ValueType: string; ValueName: ""; ValueData: "Aegisub Together video file"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Video.1"; ValueType: dword; ValueName: "EditFlags"; ValueData: $af0; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Video.1"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "@{app}\aegisub-together.exe,-10110"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Video.1\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\aegisub-together.exe,0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AegisubTogether.Video.1\shell"; ValueType: string; ValueName: ""; ValueData: "open"; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.Video.1\shell\open"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; SubKey: "SOFTWARE\Classes\AegisubTogether.Video.1\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\aegisub-together.exe"" ""%L"""; Flags: uninsdeletekey
; Default Programs registration
Root: HKLM; Subkey: "SOFTWARE\AegisubTogether"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\AegisubTogether\Capabilities"; ValueType: none
Root: HKLM; Subkey: "SOFTWARE\AegisubTogether\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "@{app}\aegisub-together.exe,-10001"
Root: HKLM; Subkey: "SOFTWARE\AegisubTogether\Capabilities\FileAssociations"; ValueType: none
Root: HKLM; Subkey: "SOFTWARE\AegisubTogether\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ass"; ValueData: "AegisubTogether.ASSA.1"
Root: HKLM; Subkey: "SOFTWARE\AegisubTogether\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ssa"; ValueData: "AegisubTogether.SSA.1"
Root: HKLM; Subkey: "SOFTWARE\AegisubTogether\Capabilities\FileAssociations"; ValueType: string; ValueName: ".srt"; ValueData: "AegisubTogether.SRT.1"
Root: HKLM; Subkey: "SOFTWARE\AegisubTogether\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ttxt"; ValueData: "AegisubTogether.TTXT.1"
Root: HKLM; Subkey: "SOFTWARE\AegisubTogether\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mks"; ValueData: "AegisubTogether.MKS.1"
Root: HKLM; Subkey: "SOFTWARE\RegisteredApplications"; ValueType: string; ValueName: "Aegisub Together"; ValueData: "SOFTWARE\AegisubTogether\Capabilities"; Flags: uninsdeletevalue
; Default handler for .ass
; Only register us as owner of the type if there isn't one already. Windows XP doesn't cooperate well when a type has no owner,
; even if everything else exists. If it already has an owner, use some other UI to take over ownership if the user desires.
; Only set perceived types for the main text-based subtitle formats.
; We only have a template file for .ass, that's the primary subtitle format.
; Register us as a valid ProgID for every type we can reasonably handle, and a few we might not be able to.
Root: HKLM; SubKey: "SOFTWARE\Classes\.ass"; ValueType: string; ValueData: "AegisubTogether.ASSA.1"; Flags: createvalueifdoesntexist
Root: HKLM; SubKey: "SOFTWARE\Classes\.ass"; ValueType: string; ValueName: "PerceivedType"; ValueData: "text"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Classes\.ass\AegisubTogether.ASSA.1"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\.ass\AegisubTogether.ASSA.1\ShellNew"; ValueType: string; ValueName: "FileName"; ValueData: "{commontemplates}\AegisubTogether.ass"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\.ass\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.ASSA.1"; Flags: uninsdeletevalue
; Default handler for .ssa
Root: HKLM; SubKey: "SOFTWARE\Classes\.ssa"; ValueType: string; ValueData: "AegisubTogether.SSA.1"; Flags: createvalueifdoesntexist
Root: HKLM; SubKey: "SOFTWARE\Classes\.ssa"; ValueType: string; ValueName: "PerceivedType"; ValueData: "text"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Classes\.ssa\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.SSA.1"; Flags: uninsdeletevalue
; Default handler for .srt
Root: HKLM; SubKey: "SOFTWARE\Classes\.srt"; ValueType: string; ValueData: "AegisubTogether.SRT.1"; Flags: createvalueifdoesntexist
Root: HKLM; SubKey: "SOFTWARE\Classes\.srt"; ValueType: string; ValueName: "PerceivedType"; ValueData: "text"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Classes\.srt\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.SRT.1"; Flags: uninsdeletevalue
; Default handler for .ttxt
Root: HKLM; SubKey: "SOFTWARE\Classes\.ttxt"; ValueType: string; ValueData: "AegisubTogether.TTXT.1"; Flags: createvalueifdoesntexist
Root: HKLM; SubKey: "SOFTWARE\Classes\.ttxt"; ValueType: string; ValueName: "PerceivedType"; ValueData: "text"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Classes\.ttxt\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.TTXT.1"; Flags: uninsdeletevalue
; Default handler for .mks
Root: HKLM; SubKey: "SOFTWARE\Classes\.mks"; ValueType: string; ValueData: "AegisubTogether.MKS.1"; Flags: createvalueifdoesntexist
Root: HKLM; SubKey: "SOFTWARE\Classes\.mks"; ValueType: string; ValueName: "PerceivedType"; ValueData: "text"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Classes\.mks\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.MKS.1"; Flags: uninsdeletevalue
; Support opening a bunch more types
Root: HKLM; Subkey: "SOFTWARE\Classes\.sub\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Subtitle.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.txt\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.TXT.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.mkv\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Video.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.mka\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Audio.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.avi\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Video.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.mp3\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Audio.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.mp4\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Media.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.aac\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Audio.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.m4a\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Audio.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.wav\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Audio.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.ogg\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Media.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.avs\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Video.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.vpy\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Media.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.opus\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Audio.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.h264\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Video.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.hevc\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Video.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.eac3\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Audio.1"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Classes\.webm\OpenWithProgids"; ValueType: string; ValueName: "AegisubTogether.Media.1"; Flags: uninsdeletevalue
