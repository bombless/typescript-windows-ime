Option Explicit
Dim shell, fso, root, command
Set shell = CreateObject("WScript.Shell")
shell.Environment("PROCESS")("TSIME_FLAVOR") = "release"
Set fso = CreateObject("Scripting.FileSystemObject")
root = fso.GetParentFolderName(fso.GetParentFolderName(WScript.ScriptFullName))
command = Chr(34) & shell.ExpandEnvironmentStrings("%ComSpec%") & Chr(34) & " /d /s /c " & Chr(34) & "cd /d " & Chr(34) & root & Chr(34) & " && npm run dev" & Chr(34)
shell.Run command, 0, False
