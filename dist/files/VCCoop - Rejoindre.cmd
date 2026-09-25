@echo off
rem VCCoop : rejoint la partie coop d'un ami.
cd /d "%~dp0"
set /p VCCOOP_IP=Adresse de l'hote (IP) : 
if "%VCCOOP_IP%"=="" exit /b
start "" gta-vc.exe -vccoop invite %VCCOOP_IP%
