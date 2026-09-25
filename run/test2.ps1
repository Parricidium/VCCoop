# Compile, installe dans les instances de test, lance-les (hors ecran), attend, capture, ferme, montre les journaux.
param([int]$Seconds = 25, [int]$Players = 2, [switch]$NoBuild, [switch]$KeepOpen)
$root = Split-Path $PSScriptRoot
$base = 'D:\Games\COOPTEST\GTA Vice City'
if (-not $NoBuild) {
  $out = cmd /c "`"$root\build.cmd`"" 2>&1
  if ($LASTEXITCODE -ne 0) { $out | Select-String 'error'; throw "echec de compilation" }
}
# Seulement nos instances de test : jamais un jeu lance par JD.
Get-Process gta-vc -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'D:\Games\COOPTEST\*' } | Stop-Process -Force
Start-Sleep -Milliseconds 500
$procs = @()
for ($n = 1; $n -le $Players; $n++) {
  $g = "$base\VCCoop-Joueur$n"
  Copy-Item "$root\build\dinput8.dll" $g -Force
  $procs += Start-Process "$g\gta-vc.exe" -WorkingDirectory $g -PassThru
  Start-Sleep -Milliseconds 1500
}
Start-Sleep $Seconds
foreach ($p in $procs) { $p.Refresh(); "pid $($p.Id) vivant=$(-not $p.HasExited)" }
& "$PSScriptRoot\capture.ps1"
if (-not $KeepOpen) { $procs | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue } }
for ($n = 1; $n -le $Players; $n++) { "--- Joueur$n"; Get-Content "$base\VCCoop-Joueur$n\vccoop.log" -Tail 15 }
