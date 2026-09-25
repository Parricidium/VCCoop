# Compile VCCoop puis assemble dist\out\VCCoop-<date>.zip (sans aucune donnee du jeu).
param([string]$Version = (Get-Date -Format 'yyyy.MM.dd'))
$root = Split-Path $PSScriptRoot
$out = cmd /c "`"$root\build.cmd`"" 2>&1
if ($LASTEXITCODE -ne 0) { $out | Select-String 'error'; throw "echec de compilation" }
$ErrorActionPreference = 'Stop'   # apres la compilation : vcvars ecrit sur stderr

$stage = "$PSScriptRoot\out\VCCoop-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$root\build\dinput8.dll" $stage
Copy-Item "$PSScriptRoot\files\*" $stage

$zip = "$PSScriptRoot\out\VCCoop-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$z = [System.IO.Compression.ZipFile]::Open($zip, 'Create')
foreach ($f in Get-ChildItem $stage -File) {
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $f.FullName, $f.Name) | Out-Null
}
$z.Dispose()
"Ecrit : $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) Ko)"
