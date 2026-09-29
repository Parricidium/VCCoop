@echo off
rem Compile VCCoop en dinput8.dll (Win32, /MT). Sortie : build\dinput8.dll
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /Fobuild\ src\*.cpp ^
   /LD /Febuild\dinput8.dll /link /MAP:build\dinput8.map /DEF:src\dinput8.def user32.lib kernel32.lib advapi32.lib ws2_32.lib winmm.lib || exit /b 1
echo OK build\dinput8.dll
rem Lanceur (VCCoop.exe) : fenetre PNG, mises a jour depuis GitHub
rc /nologo /fo build\launcher.res launcher\launcher.rc || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Fobuild\ launcher\launcher.cpp launcher\model3d.cpp build\launcher.res ^
   /Febuild\VCCoop.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib winhttp.lib comdlg32.lib shell32.lib ole32.lib ws2_32.lib advapi32.lib || exit /b 1
echo OK build\VCCoop.exe
