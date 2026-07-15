[Setup]
AppName=QuizPane 小窗刷题
AppVersion=0.2.4
DefaultDirName={autopf}\QuizPane
DefaultGroupName=QuizPane
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\小窗刷题.exe

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\小窗刷题"; Filename: "{app}\小窗刷题.exe"
Name: "{group}\题库制作器"; Filename: "{app}\题库制作器.exe"

[Run]
Filename: "{app}\小窗刷题.exe"; Description: "启动小窗刷题"; Flags: nowait postinstall skipifsilent
