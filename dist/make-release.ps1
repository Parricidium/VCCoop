# Compile VCCoop puis assemble dist\out\VCCoop-<date>.zip (sans aucune donnee du jeu).
# -Publier : cree la release v<Version> sur GitHub (Parricidium/VCCoop) avec le zip ; le lanceur (VCCoop.exe) la
# trouve au prochain demarrage de chaque joueur. -Notes : texte de la release.
param([string]$Version = (Get-Date -Format 'yyyy.MM.dd'), [switch]$Publier, [string]$Notes = '')
$root = Split-Path $PSScriptRoot
$out = cmd /c "`"$root\build.cmd`"" 2>&1
if ($LASTEXITCODE -ne 0) { $out | Select-String 'error'; throw "echec de compilation" }
$ErrorActionPreference = 'Stop'   # apres la compilation : vcvars ecrit sur stderr

$stage = "$PSScriptRoot\out\VCCoop-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$root\build\dinput8.dll" $stage
Copy-Item "$root\build\VCCoop.exe" $stage
Copy-Item "$PSScriptRoot\files\*" $stage -Recurse
Set-Content "$stage\VCCoop\version.txt" $Version -NoNewline -Encoding ASCII   # lue par le lanceur

$zip = "$PSScriptRoot\out\VCCoop-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$z = [System.IO.Compression.ZipFile]::Open($zip, 'Create')
foreach ($f in Get-ChildItem $stage -File -Recurse) {
    $rel = $f.FullName.Substring($stage.Length + 1).Replace('\', '/')
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $f.FullName, $rel) | Out-Null
}
$z.Dispose()
"Ecrit : $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) Ko)"
if ($Publier) {
    if (-not $Notes) { $Notes = "VCCoop $Version" }
    $nf = [System.IO.Path]::GetTempFileName()
    [System.IO.File]::WriteAllText($nf, $Notes, (New-Object System.Text.UTF8Encoding $false))
    gh release create "v$Version" $zip --repo Parricidium/VCCoop --title "VCCoop $Version" --notes-file $nf --latest
    $rc = $LASTEXITCODE
    Remove-Item $nf
    if ($rc -ne 0) { throw "echec de la publication GitHub" }
    "Publie : https://github.com/Parricidium/VCCoop/releases/tag/v$Version"
}
