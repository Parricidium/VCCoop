# Lance les deux instances de test (hors ecran), attend, puis capture les fenetres toutes les $Every secondes.
param([int]$Wait = 40, [int]$Shots = 8, [double]$Every = 1, [switch]$NoBuild)
$root = Split-Path $PSScriptRoot
$base = 'D:\Games\COOPTEST\GTA Vice City'
if (-not $NoBuild) {
  $out = cmd /c "`"$root\build.cmd`"" 2>&1
  if ($LASTEXITCODE -ne 0) { $out | Select-String 'error'; throw "echec de compilation" }
}
Get-Process gta-vc -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'D:\Games\COOPTEST\*' } | Stop-Process -Force
Start-Sleep -Milliseconds 500
$procs = @()
for ($n = 1; $n -le 2; $n++) {
  $g = "$base\VCCoop-Joueur$n"
  Copy-Item "$root\build\dinput8.dll" $g -Force
  $procs += Start-Process "$g\gta-vc.exe" -WorkingDirectory $g -PassThru
  Start-Sleep -Milliseconds 1500
}
Start-Sleep $Wait
for ($k = 0; $k -lt $Shots; $k++) { & "$PSScriptRoot\capture.ps1" -Prefix "b$k" | Out-Null; Start-Sleep -Milliseconds ([int]($Every * 1000)) }
foreach ($p in $procs) { $p.Refresh(); "pid $($p.Id) vivant=$(-not $p.HasExited)" }
$procs | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
