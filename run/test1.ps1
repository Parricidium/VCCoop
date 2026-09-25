# Compile, installe dans l'instance de test, lance le jeu N secondes, capture la fenetre, ferme, affiche le journal.
param([int]$Seconds = 25, [switch]$NoBuild, [switch]$KeepOpen)
$root = Split-Path $PSScriptRoot
$game = 'D:\Games\COOPTEST\GTA Vice City\VCCoop-Joueur1'
if (-not $NoBuild) {
  $out = cmd /c "`"$root\build.cmd`"" 2>&1
  if ($LASTEXITCODE -ne 0) { $out | Select-String 'error' ; throw "echec de compilation" }
}
Get-Process gta-vc -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Copy-Item "$root\build\dinput8.dll" $game -Force
$p = Start-Process "$game\gta-vc.exe" -WorkingDirectory $game -PassThru
Start-Sleep $Seconds
$p.Refresh()
"vivant=$(-not $p.HasExited)"
& "$PSScriptRoot\capture.ps1"
if (-not $KeepOpen) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Get-Content "$game\vccoop.log" -Tail 30
