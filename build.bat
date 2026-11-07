@echo off
rem Builds build\BGELauncher.exe with the C# compiler that ships with Windows (.NET Framework 4.x).
rem The d3d9 hook is built separately: see src\bgehook\build.sh.
setlocal
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
if not exist "%~dp0build" mkdir "%~dp0build"
"%CSC%" /nologo /target:winexe /optimize+ /platform:anycpu /out:"%~dp0build\BGELauncher.exe" ^
  /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll ^
  /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll ^
  "%~dp0src\launcher\BGELauncher.cs"
