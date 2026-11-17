# make-cand-art2.ps1 - candidate window art (panda holding bamboo), PNG with alpha
param(
    [string]$OutFile = (Join-Path (Split-Path $PSScriptRoot -Parent) 'src\tsf\panda_bamboo.png'),
    [int]$Size = 128
)
Add-Type -AssemblyName System.Drawing
$S = $Size
$bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$g.Clear([System.Drawing.Color]::Transparent)
function B([int]$a, [int]$r, [int]$gg, [int]$b) { New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb($a, $r, $gg, $b)) }

$stalkW = $S * 0.085
$stalkX = $S * 0.66
$stalkTop = $S * 0.05
$stalkH = $S * 0.90
$sb = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    (New-Object System.Drawing.PointF([single]$stalkX, [single]0)),
    (New-Object System.Drawing.PointF([single]($stalkX + $stalkW), [single]0)),
    [System.Drawing.Color]::FromArgb(255, 142, 200, 106),
    [System.Drawing.Color]::FromArgb(255, 84, 144, 68))
$g.FillRectangle($sb, [single]$stalkX, [single]$stalkTop, [single]$stalkW, [single]$stalkH)
$sb.Dispose()
$np = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(190, 68, 124, 56), [single][Math]::Max(1.5, $S * 0.014))
for ($k = 1; $k -le 4; $k++) {
    $yy = $stalkTop + $stalkH * $k / 5
    $g.DrawLine($np, [single]($stalkX - $S * 0.008), [single]$yy, [single]($stalkX + $stalkW + $S * 0.008), [single]$yy)
}
$np.Dispose()

$leaf = B 255 124 186 94
$leafDefs = @(
    @{ cx = 0.72; cy = 0.09; w = 0.27; h = 0.10; a = -34 },
    @{ cx = 0.87; cy = 0.16; w = 0.24; h = 0.09; a = 14 },
    @{ cx = 0.69; cy = 0.21; w = 0.22; h = 0.08; a = 42 }
)
foreach ($d in $leafDefs) {
    $st = $g.Save()
    $g.TranslateTransform([single]($S * $d.cx), [single]($S * $d.cy))
    $g.RotateTransform([single]$d.a)
    $w = $S * $d.w; $h = $S * $d.h
    $g.FillEllipse($leaf, [single](-$w / 2), [single](-$h / 2), [single]$w, [single]$h)
    $g.Restore($st)
}

$white = B 255 252 252 252
$black = B 255 38 40 46
$blush = B 110 255 148 158

$bodyW = $S * 0.50; $bodyH = $S * 0.34
$bodyX = $S * 0.10; $bodyY = $S * 0.55
$g.FillEllipse($white, [single]$bodyX, [single]$bodyY, [single]$bodyW, [single]$bodyH)

$earD = $S * 0.20
$headW = $S * 0.56; $headH = $S * 0.50
$headX = $S * 0.08; $headY = $S * 0.19
$g.FillEllipse($black, [single]($headX + $headW * 0.02), [single]($headY - $earD * 0.32), [single]$earD, [single]$earD)
$g.FillEllipse($black, [single]($headX + $headW * 0.78), [single]($headY - $earD * 0.32), [single]$earD, [single]$earD)
$g.FillEllipse($white, [single]$headX, [single]$headY, [single]$headW, [single]$headH)

foreach ($side in @(0, 1)) {
    $st = $g.Save()
    $cx = $headX + $headW * (0.30 + 0.32 * $side)
    $cy = $headY + $headH * 0.46
    $g.TranslateTransform([single]$cx, [single]$cy)
    if ($side -eq 0) { $g.RotateTransform(-18) } else { $g.RotateTransform(18) }
    $ew = $headW * 0.26; $eh = $headH * 0.34
    $g.FillEllipse($black, [single](-$ew / 2), [single](-$eh / 2), [single]$ew, [single]$eh)
    $g.FillEllipse($white, [single](-$ew * 0.18), [single](-$eh * 0.32), [single]($ew * 0.34), [single]($ew * 0.34))
    $g.Restore($st)
}

$g.FillEllipse($blush, [single]($headX + $headW * 0.08), [single]($headY + $headH * 0.62), [single]($headW * 0.15), [single]($headH * 0.10))
$g.FillEllipse($blush, [single]($headX + $headW * 0.77), [single]($headY + $headH * 0.62), [single]($headW * 0.15), [single]($headH * 0.10))

$nw = $headW * 0.14; $nh = $headH * 0.09
$g.FillEllipse($black, [single]($headX + ($headW - $nw) / 2), [single]($headY + $headH * 0.64), [single]$nw, [single]$nh)

$mp = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 60, 62, 68), [single][Math]::Max(1.4, $S * 0.011))
$mp.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$mp.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawArc($mp, [single]($headX + $headW * 0.41), [single]($headY + $headH * 0.72), [single]($headW * 0.18), [single]($headH * 0.13), 20, 140)
$mp.Dispose()

$pawD = $S * 0.13
$g.FillEllipse($black, [single]($S * 0.60), [single]($S * 0.58), [single]$pawD, [single]$pawD)
$g.FillEllipse($black, [single]($S * 0.56), [single]($S * 0.73), [single]($pawD * 0.92), [single]($pawD * 0.92))
$g.FillEllipse($black, [single]($S * 0.18), [single]($S * 0.79), [single]($S * 0.15), [single]($S * 0.10))

$white.Dispose(); $black.Dispose(); $blush.Dispose(); $leaf.Dispose()
$g.Dispose()
$bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("OK " + $OutFile + " " + (Get-Item $OutFile).Length)