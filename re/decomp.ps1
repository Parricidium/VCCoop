# Decompile des fonctions de gta-vc 1.0 via Ghidra (sans analyse).
# Exemples : .\decomp.ps1 -Out winmain 0x600410 xref:0x6f253c
#            .\decomp.ps1 -Out asm -Script Disasm.java 0x601530 40
[CmdletBinding(PositionalBinding=$false)]
param([Parameter(Mandatory)][string]$Out, [string]$Script = 'Decomp.java', [switch]$Write, [Parameter(ValueFromRemainingArguments)][string[]]$Targets)
$S = 'C:\Users\JD\AppData\Local\Temp\claude\D--1---AI-ClaudeAI-Arma3WebUI\ec369333-e8f8-4e13-a1aa-f9e57621ded8\scratchpad'
$gh = "$S\gh"; $re = "$S\vcre"
if (-not (Test-Path "$gh\support")) { New-Item -ItemType Junction -Path $gh -Target 'D:\1 - AI\ClaudeAI\MafiaCoop\tools\ghidra_12.1.3_PUBLIC' | Out-Null }
if (-not (Test-Path "$re\proj")) { New-Item -ItemType Junction -Path $re -Target $PSScriptRoot | Out-Null }
$env:JAVA_HOME = 'C:\Program Files\Microsoft\jdk-21.0.12.101-hotspot'
$outFile = "$re\out\$Out.c"
& "$gh\support\analyzeHeadless.bat" "$re\proj" VCRE -process gta-vc-1.0.exe -noanalysis $(if (-not $Write) { '-readOnly' }) `
  -scriptPath "$re\scripts" -postScript $Script $outFile @Targets *> "$re\out\decomp.log"
if (Test-Path $outFile) { "Ecrit : $PSScriptRoot\out\$Out.c ($((Get-Content $outFile).Count) lignes)" } else { Get-Content "$re\out\decomp.log" -Tail 20 }
