Option Explicit
Dim shell, fs, folder, command, result
Set shell = CreateObject("WScript.Shell")
Set fs = CreateObject("Scripting.FileSystemObject")
folder = fs.GetParentFolderName(WScript.ScriptFullName)
shell.CurrentDirectory = folder
If Not fs.FileExists(folder & "\build\audio_mixer.dll") Or Not fs.FileExists(folder & "\build\audio_inventory.dll") Then
    MsgBox "Build the native mixer first: build-native.cmd. See README.md.", 16, "TeamSpeak Audio Control"
    WScript.Quit 1
End If
If fs.FileExists(folder & "\.venv\Scripts\pythonw.exe") Then
    command = """" & folder & "\.venv\Scripts\pythonw.exe"""
Else
    result = shell.Run("cmd /c where pyw.exe >nul 2>nul", 0, True)
    If result = 0 Then
        command = "pyw.exe -3"
    Else
        command = "pythonw.exe"
    End If
End If
On Error Resume Next
shell.Run command & " """ & folder & "\audio_router_gui.py""", 0, False
If Err.Number <> 0 Then
    MsgBox "Python could not be started. Follow the installation steps in README.md.", 16, "TeamSpeak Audio Control"
    WScript.Quit 1
End If
