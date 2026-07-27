' ISO2Drive GUI launcher — double-click to open the GUI (no console window).
' Runs gui\iso2drive_gui.py with pythonw. Needs a Python with tkinter
' (this machine uses the MSYS2 mingw64 python); the GUI finds the iso2drive
' engine (iso2drive*.exe) sitting in this folder.
Option Explicit
Dim fso, sh, here, pyw
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh  = CreateObject("WScript.Shell")
here = fso.GetParentFolderName(WScript.ScriptFullName)
pyw = "C:\msys64\mingw64\bin\pythonw.exe"
If Not fso.FileExists(pyw) Then pyw = "pythonw.exe"
sh.CurrentDirectory = here
sh.Run """" & pyw & """ """ & here & "\gui\iso2drive_gui.py""", 0, False
