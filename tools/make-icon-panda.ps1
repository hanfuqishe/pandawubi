# make-icon-panda.ps1 —— 生成 Panda五笔 图标（熊猫 + 五笔徽标）
# 用法: powershell -File tools\make-icon-panda.ps1 -OutFile dist\ppwubi.ico
param(
    [string]$OutFile = 'D:\MobileSync\hanfu\src\wubi\dist\ppwubi.ico'
)
Add-Type -AssemblyName System.Drawing

$sizes = @(16, 24, 32, 48, 64, 128, 256)

function New-PandaBitmap([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

    # 圆角底板：深色渐变，让白熊猫跳出来
    $r = [Math]::Max(3, [int]($S * 0.22))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($S - $d, 0, $d, $d, 270, 90)
    $path.AddArc($S - $d, $S - $d, $d, $d, 0, 90)
    $path.AddArc(0, $S - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    $c1 = [System.Drawing.Color]::FromArgb(255, 38, 58, 74)
    $c2 = [System.Drawing.Color]::FromArgb(255, 16, 28, 40)
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point(0, 0)), (New-Object System.Drawing.Point($S, $S)), $c1, $c2)
    $g.FillPath($br, $path)
    $br.Dispose()

    # 顶部高光
    $hl = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(28, 255, 255, 255))
    $g.FillPath($hl, $path)
    $hl.Dispose()

    $black = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 24, 26, 30))
    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)

    # 耳朵（在头后面）
    $earD = $S * 0.30
    $g.FillEllipse($black, [single]($S * 0.10), [single]($S * 0.08), [single]$earD, [single]$earD)
    $g.FillEllipse($black, [single]($S * 0.60), [single]($S * 0.08), [single]$earD, [single]$earD)

    # 头
    $headX = $S * 0.13; $headY = $S * 0.16; $headW = $S * 0.74; $headH = $S * 0.66
    $g.FillEllipse($white, [single]$headX, [single]$headY, [single]$headW, [single]$headH)

    # 黑眼圈
    $patchW = $S * 0.20; $patchH = $S * 0.26
    $g.FillEllipse($black, [single]($S * 0.245), [single]($S * 0.36), [single]$patchW, [single]$patchH)
    $g.FillEllipse($black, [single]($S * 0.555), [single]($S * 0.36), [single]$patchW, [single]$patchH)

    # 眼神光
    $eyeW = $S * 0.075
    $g.FillEllipse($white, [single]($S * 0.295), [single]($S * 0.425), [single]$eyeW, [single]$eyeW)
    $g.FillEllipse($white, [single]($S * 0.605), [single]($S * 0.425), [single]$eyeW, [single]$eyeW)

    # 鼻子
    $noseW = $S * 0.13; $noseH = $S * 0.085
    $g.FillEllipse($black, [single](($S - $noseW) / 2), [single]($S * 0.615), [single]$noseW, [single]$noseH)

    # 嘴（两段弧）
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 24, 26, 30), [single][Math]::Max(1.0, $S * 0.028))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawArc($pen, [single]($S * 0.40), [single]($S * 0.665), [single]($S * 0.20), [single]($S * 0.13), 20, 140)
    $pen.Dispose()

    # 五笔徽标：右下角绿色圆角块 + 白字「五」
    if ($S -ge 24) {
        $bs = $S * 0.36
        $bx = $S - $bs - $S * 0.045
        $by = $S - $bs - $S * 0.045
        $br2 = [Math]::Max(2, [int]($bs * 0.26))
        $bp = New-Object System.Drawing.Drawing2D.GraphicsPath
        $dd = $br2 * 2
        $bp.AddArc([single]$bx, [single]$by, [single]$dd, [single]$dd, 180, 90)
        $bp.AddArc([single]($bx + $bs - $dd), [single]$by, [single]$dd, [single]$dd, 270, 90)
        $bp.AddArc([single]($bx + $bs - $dd), [single]($by + $bs - $dd), [single]$dd, [single]$dd, 0, 90)
        $bp.AddArc([single]$bx, [single]($by + $bs - $dd), [single]$dd, [single]$dd, 90, 90)
        $bp.CloseFigure()
        $green = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 46, 160, 96))
        $g.FillPath($green, $bp)
        $green.Dispose()
        $bp.Dispose()

        $fontSize = [single]($bs * 0.66)
        $font = New-Object System.Drawing.Font('Microsoft YaHei', $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
        $fmt = New-Object System.Drawing.StringFormat
        $fmt.Alignment = [System.Drawing.StringAlignment]::Center
        $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
        $rect = New-Object System.Drawing.RectangleF([single]$bx, [single]$by, [single]$bs, [single]$bs)
        $g.DrawString('五', $font, $white, $rect, $fmt)
        $font.Dispose(); $fmt.Dispose()
    }

    $black.Dispose(); $white.Dispose(); $path.Dispose(); $g.Dispose()
    return $bmp
}

# 按尺寸生成 PNG 载荷（32bpp ARGB -> PNG）
    $outDir = Split-Path -Path $OutFile -Parent
$entries = @()
foreach ($s in $sizes) {
    $bmp = New-PandaBitmap $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $entries += [pscustomobject]@{ Size = $s; Data = $ms.ToArray() }
    if ($s -eq 256) {
        $previewPath = $outDir + '\icon-panda-256.png'
        $bmp.Save($previewPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $ms.Dispose(); $bmp.Dispose()
}

# 组装 ICO（所有尺寸都用 PNG 载荷，Vista+ 支持）
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($out)
$bw.Write([uint16]0)               # reserved
$bw.Write([uint16]1)               # type = icon
$bw.Write([uint16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $bw.Write([byte]$(if ($e.Size -ge 256) { 0 } else { $e.Size }))
    $bw.Write([byte]$(if ($e.Size -ge 256) { 0 } else { $e.Size }))
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$e.Data.Length)
    $bw.Write([uint32]$offset)
    $offset += $e.Data.Length
}
foreach ($e in $entries) { $bw.Write($e.Data) }
$bw.Flush()
[IO.File]::WriteAllBytes($OutFile, $out.ToArray())
$bw.Dispose(); $out.Dispose()

# 校验
$ico = New-Object System.Drawing.Icon($OutFile, 32, 32)
Write-Output ("验证: Icon 加载成功 " + $ico.Width + "x" + $ico.Height + "  文件 " + (Get-Item $OutFile).Length + " 字节")
$ico.Dispose()
