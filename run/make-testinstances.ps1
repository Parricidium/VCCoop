# Cree les instances de test VCCoop-Joueur2..N a partir de VCCoop-Joueur1 (le jeu retrograde en 1.0).
# Les fichiers du jeu sont des liens durs (aucune copie) ; vccoop.ini, la DLL et les sauvegardes sont propres a chaque instance.
param([int]$Count = 2)
$base = 'D:\Games\COOPTEST\GTA Vice City'
$src = "$base\VCCoop-Joueur1"
$own = @('vccoop.ini', 'vccoop.log', 'dinput8.dll')
for ($n = 2; $n -le $Count; $n++) {
  $dst = "$base\VCCoop-Joueur$n"
  foreach ($f in Get-ChildItem $src -Recurse -File) {
    $rel = $f.FullName.Substring($src.Length + 1)
    if ($own -contains $rel -or $rel -like 'GTA Vice City User Files*') { continue }
    $target = Join-Path $dst $rel
    if (Test-Path $target) { continue }
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    # Les archives .img sont lues en E/S asynchrone : partagees par lien dur entre deux instances,
    # les lectures se bloquent. Elles sont donc copiees.
    if ($f.Extension -eq '.img') { Copy-Item $f.FullName $target } else { New-Item -ItemType HardLink -Path $target -Target $f.FullName | Out-Null }
  }
  $x = -4000 + ($n - 1) * 700
  Set-Content "$dst\vccoop.ini" -Encoding ascii -Value @"
[VCCoop]
Pseudo=Joueur$n
Fenetre=1
FenetreX=$x
FenetreY=100
ArrierePlan=1
ImagesParSeconde=30
Role=invite
Adresse=127.0.0.1
AutoDemarrer=1
Reseau=1
"@
  "instance $n : $dst"
}
