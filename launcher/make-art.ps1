# Compose le fond du lanceur (dist\files\VCCoop\interface\launcher.png, 1000x620, avec transparence) a partir des images
# de l'interface : carte arrondie + ombre douce, image du menu, panneau depoli a gauche, logo qui depasse de la carte.
# Le lanceur dessine ses textes et boutons par-dessus (coordonnees fixes, voir launcher.cpp).
Add-Type -AssemblyName System.Drawing
$ui = Join-Path (Split-Path $PSScriptRoot) 'dist\files\VCCoop\interface'
$W = 1000; $H = 620
$card = New-Object System.Drawing.RectangleF 20, 60, 960, 540
$panel = New-Object System.Drawing.RectangleF 48, 88, 360, 500

# Cadre des pixels visibles d'une image (alpha > 8)
function AlphaBox($img) {
    $b = New-Object System.Drawing.Bitmap $img
    $x0 = $b.Width; $y0 = $b.Height; $x1 = -1; $y1 = -1
    for ($y = 0; $y -lt $b.Height; $y += 2) { for ($x = 0; $x -lt $b.Width; $x += 2) {
        if ($b.GetPixel($x, $y).A -gt 8) { if ($x -lt $x0) { $x0 = $x }; if ($x -gt $x1) { $x1 = $x }; if ($y -lt $y0) { $y0 = $y }; if ($y -gt $y1) { $y1 = $y } }
    } }
    $b.Dispose()
    return New-Object System.Drawing.RectangleF ($x0 - 2), ($y0 - 2), ($x1 - $x0 + 5), ($y1 - $y0 + 5)
}

function RoundPath([System.Drawing.RectangleF]$r, [float]$rad) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $rad * 2
    $p.AddArc($r.X, $r.Y, $d, $d, 180, 90)
    $p.AddArc($r.Right - $d, $r.Y, $d, $d, 270, 90)
    $p.AddArc($r.Right - $d, $r.Bottom - $d, $d, $d, 0, 90)
    $p.AddArc($r.X, $r.Bottom - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

$bmp = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
$g.Clear([System.Drawing.Color]::Transparent)

# Ombre douce : couches arrondies de plus en plus larges et transparentes, decalees vers le bas.
for ($i = 18; $i -ge 1; $i--) {
    $r = New-Object System.Drawing.RectangleF ($card.X - $i), ($card.Y - $i + 8), ($card.Width + 2 * $i), ($card.Height + 2 * $i)
    $a = [int](9 * (1 - $i / 19.0) + 1)
    $b = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($a, 20, 10, 30))
    $g.FillPath($b, (RoundPath $r (26 + $i)))
}

# Carte : l'image du menu, a moitie de sa taille (1920x1080 -> 960x540).
$cardPath = RoundPath $card 26
$g.SetClip($cardPath)
$fond = [System.Drawing.Image]::FromFile((Join-Path $ui 'fond_menu.png'))
$g.DrawImage($fond, $card)
# leger degrade rose / orange en bas, facon coucher de soleil de Vice City
$grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 420), (New-Object System.Drawing.PointF 0, 600), ([System.Drawing.Color]::FromArgb(0, 255, 90, 140)), ([System.Drawing.Color]::FromArgb(70, 255, 110, 120))
$g.FillRectangle($grad, 20, 421, 960, 179)
# A gauche, sous le panneau, la mention legale de l'image est remplacee par un degrade pastel (rose en haut, peche en
# bas) qui se fond dans l'image vers le milieu : le panneau depoli le laisse deviner.
for ($x = 20; $x -lt 580; $x += 2) {
    $k = if ($x -lt 400) { 1.0 } else { 1.0 - ($x - 400) / 180.0 }
    $k = $k * $k * (3 - 2 * $k)
    $a = [int](255 * $k)
    $vg = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 59), (New-Object System.Drawing.PointF 0, 601), ([System.Drawing.Color]::FromArgb($a, 255, 222, 236)), ([System.Drawing.Color]::FromArgb($a, 255, 178, 158))
    $g.FillRectangle($vg, $x, 60, 2, 540)
    $vg.Dispose()
}

# Accroche a droite, facon carte postale : lettres espacees, filet degrade, sous-titre.
function SpacedText($gr, [string]$t, $font, $brush, [float]$cx, [float]$y, [float]$gap) {
    $sf = [System.Drawing.StringFormat]::GenericTypographic
    $ws = @(); $tot = 0
    foreach ($ch in $t.ToCharArray()) { $w = $gr.MeasureString([string]$ch, $font, 1000, $sf).Width; if ($ch -eq ' ') { $w = $font.Size * 0.35 }; $ws += $w; $tot += $w + $gap }
    $x = $cx - ($tot - $gap) / 2
    $i = 0
    foreach ($ch in $t.ToCharArray()) { $gr.DrawString([string]$ch, $font, $brush, $x, $y, $sf); $x += $ws[$i] + $gap; $i++ }
}
$g.TextRenderingHint = 'AntiAliasGridFit'
$ink = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(225, 40, 32, 48))
$f1 = New-Object System.Drawing.Font 'Segoe UI Light', 30, ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
$f2 = New-Object System.Drawing.Font 'Segoe UI', 13, ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
SpacedText $g 'GREETINGS FROM' $f1 $ink 715 262 9
SpacedText $g 'VICE CITY' $f1 $ink 715 302 13
$lb = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 575, 0), (New-Object System.Drawing.PointF 855, 0), ([System.Drawing.Color]::FromArgb(255, 255, 79, 139)), ([System.Drawing.Color]::FromArgb(255, 255, 138, 91))
$g.FillRectangle($lb, 575, 350, 280, 2)
SpacedText $g 'THE STORY MISSIONS  ·  2 TO 4 PLAYERS' $f2 (New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(210, 90, 76, 96))) 715 364 3.2

# Panneau depoli : le fond sous le panneau, reduit puis agrandi (flou), voile blanc.
$panelPath = RoundPath $panel 18
$small = New-Object System.Drawing.Bitmap 45, 60
$gs = [System.Drawing.Graphics]::FromImage($small)
$gs.InterpolationMode = 'HighQualityBilinear'
$gs.DrawImage($bmp, (New-Object System.Drawing.RectangleF 0, 0, 45, 60), $panel, [System.Drawing.GraphicsUnit]::Pixel)
$gs.Dispose()
$g.SetClip($panelPath)
$g.InterpolationMode = 'HighQualityBicubic'
$g.DrawImage($small, (New-Object System.Drawing.RectangleF ($panel.X - 6), ($panel.Y - 6), ($panel.Width + 12), ($panel.Height + 12)))
$g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(185, 250, 248, 250))), $panelPath)
$g.ResetClip()
$g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(150, 255, 255, 255)), 1.5), $panelPath)

# Liseré de la carte
$g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(110, 255, 255, 255)), 1.5), $cardPath)

# Logo : depasse du haut de la carte (la transparence du PNG le laisse flotter sur le bureau).
# Logo du lanceur et de la page GitHub : launcher\logo.png (docs\img\logo.png), distinct du logo.png affiche en jeu.
$logo = [System.Drawing.Image]::FromFile((Join-Path $PSScriptRoot 'logo.png'))
$src = AlphaBox $logo
$dh = 196.0; $dw = $dh * $src.Width / $src.Height
if ($dw -gt 220) { $dw = 220.0; $dh = $dw * $src.Height / $src.Width }
$dst = New-Object System.Drawing.RectangleF (228 - $dw / 2), 8, $dw, $dh
# halo blanc doux derriere le logo pour qu'il se lise sur le bureau
for ($i = 6; $i -ge 1; $i--) {
    $ia = New-Object System.Drawing.Imaging.ImageAttributes
    $cm = New-Object System.Drawing.Imaging.ColorMatrix
    $cm.Matrix00 = 0; $cm.Matrix11 = 0; $cm.Matrix22 = 0; $cm.Matrix33 = 0.10
    $cm.Matrix40 = 1; $cm.Matrix41 = 1; $cm.Matrix42 = 1
    $ia.SetColorMatrix($cm)
    foreach ($o in @(@(-$i, 0), @($i, 0), @(0, -$i), @(0, $i))) {
        $r = New-Object System.Drawing.Rectangle ([int]($dst.X + $o[0])), ([int]($dst.Y + $o[1])), ([int]$dst.Width), ([int]$dst.Height)
        $g.DrawImage($logo, $r, $src.X, $src.Y, $src.Width, $src.Height, [System.Drawing.GraphicsUnit]::Pixel, $ia)
    }
}
$g.DrawImage($logo, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)

$g.Dispose()
$out = Join-Path $ui 'launcher.png'
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$fond.Dispose(); $logo.Dispose(); $bmp.Dispose()
"Ecrit : $out"

# Icone du lanceur (launcher\vccoop.ico) : le logo, en PNG 256/48/32/16 dans un .ico.
$logo = [System.Drawing.Image]::FromFile((Join-Path $PSScriptRoot 'logo.png'))
$box = AlphaBox $logo
$side = [math]::Max($box.Width, $box.Height) + 8
$cx = $box.X + $box.Width / 2; $cy = $box.Y + $box.Height / 2
$imgs = @()
foreach ($s in 256, 48, 32, 16) {
    $b = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $gg = [System.Drawing.Graphics]::FromImage($b)
    $gg.InterpolationMode = 'HighQualityBicubic'; $gg.SmoothingMode = 'AntiAlias'; $gg.PixelOffsetMode = 'HighQuality'
    $gg.Clear([System.Drawing.Color]::Transparent)
    $gg.DrawImage($logo, (New-Object System.Drawing.RectangleF 0, 0, $s, $s), (New-Object System.Drawing.RectangleF ($cx - $side / 2), ($cy - $side / 2), $side, $side), [System.Drawing.GraphicsUnit]::Pixel)
    $gg.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $imgs += , @($s, $ms.ToArray())
    $b.Dispose()
}
$logo.Dispose()
$fs = [System.IO.File]::Create((Join-Path $PSScriptRoot 'vccoop.ico'))
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$imgs.Count)
$off = 6 + 16 * $imgs.Count
foreach ($i in $imgs) {
    $s = $i[0]; $len = $i[1].Length
    $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256)); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$len); $w.Write([uint32]$off)
    $off += $len
}
foreach ($i in $imgs) { $w.Write($i[1]) }
$w.Close()
"Ecrit : vccoop.ico"
