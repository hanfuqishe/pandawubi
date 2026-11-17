# 生成 Panda五笔 的图标（多尺寸 ICO，不依赖外部库）
param(
    [string]$OutFile = 'D:\MobileSync\hanfu\src\wubi\dist\ppwubi.ico',
    [string]$Glyph = '五',
    [string]$Badge = 'PP'
)
Add-Type -AssemblyName System.Drawing

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    $r = [Math]::Max(2, [int]($size * 0.22))
    $d = $r * 2
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($size - $d, 0, $d, $d, 270, 90)
    $path.AddArc($size - $d, $size - $d, $d, $d, 0, 90)
    $path.AddArc(0, $size - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point(0, 0)),
        (New-Object System.Drawing.Point($size, $size)),
        [System.Drawing.Color]::FromArgb(255, 26, 54, 102),
        [System.Drawing.Color]::FromArgb(255, 15, 128, 126))
    $g.FillPath($brush, $path)

    $penW = [Math]::Max(1, $size / 32)
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(70, 255, 255, 255), $penW)
    $g.DrawPath($pen, $path)

    $fam = New-Object System.Drawing.FontFamily("Microsoft YaHei")
    $font = New-Object System.Drawing.Font($fam, ($size * 0.52), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $g.DrawString($Glyph, $font, $white, (New-Object System.Drawing.RectangleF(0, (-$size * 0.10), $size, $size)), $fmt)

    if ($size -ge 32) {
        $bf = New-Object System.Drawing.Font($fam, ($size * 0.22), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
        $bbr = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(235, 255, 214, 102))
        $bfmt = New-Object System.Drawing.StringFormat
        $bfmt.Alignment = [System.Drawing.StringAlignment]::Far
        $bfmt.LineAlignment = [System.Drawing.StringAlignment]::Far
        $g.DrawString($Badge, $bf, $bbr, (New-Object System.Drawing.RectangleF(0, 0, ($size - $size * 0.08), ($size - $size * 0.06))), $bfmt)
        $bf.Dispose(); $bbr.Dispose(); $bfmt.Dispose()
    }
    $g.Dispose()
    return $bmp
}

# 转成 32bpp BGRA 行数组（自底向上）
function Get-BgraRows([System.Drawing.Bitmap]$bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $buf = New-Object byte[] ($stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
    $bmp.UnlockBits($data)
    return ,@($buf, $stride, $w, $h)
}

function New-BmpPayload([System.Drawing.Bitmap]$bmp) {
    $info = Get-BgraRows $bmp
    $buf = $info[0]; $stride = $info[1]; $w = $info[2]; $h = $info[3]
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    # BITMAPINFOHEADER
    $bw.Write([uint32]40)
    $bw.Write([int32]$w)
    $bw.Write([int32]($h * 2))
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]0)
    $bw.Write([uint32]($w * $h * 4))
    $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)
    # XOR 位图（自底向上）
    for ($y = $h - 1; $y -ge 0; $y--) {
        $bw.Write($buf, $y * $stride, $w * 4)
    }
    # AND 掩码（全 0 = 不透明），每行按 4 字节对齐
    $maskStride = [Math]::Floor(($w + 31) / 32) * 4
    $mask = New-Object byte[] ($maskStride * $h)
    $bw.Write($mask, 0, $mask.Length)
    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()
    return ,$bytes
}

function New-PngPayload([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return ,$bytes
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$payloads = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    if ($s -ge 256) { $payloads += , (New-PngPayload $bmp) }
    else            { $payloads += , (New-BmpPayload $bmp) }
    $bmp.Dispose()
}

$fs = [System.IO.File]::Create($OutFile)
$w = New-Object System.IO.BinaryWriter($fs)
$w.Write([uint16]0)          # reserved
$w.Write([uint16]1)          # type = icon
$w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $w.Write([byte]$(if ($s -ge 256) { 0 } else { $s }))
    $w.Write([byte]$(if ($s -ge 256) { 0 } else { $s }))
    $w.Write([byte]0)        # colorCount
    $w.Write([byte]0)        # reserved
    $w.Write([uint16]1)      # planes
    $w.Write([uint16]32)     # bitCount
    $w.Write([uint32]$payloads[$i].Length)
    $w.Write([uint32]$offset)
    $offset += $payloads[$i].Length
}
foreach ($p in $payloads) { $w.Write($p, 0, $p.Length) }
$w.Flush(); $w.Dispose(); $fs.Dispose()
Write-Output ("生成: " + $OutFile + "  " + (Get-Item $OutFile).Length + " bytes  " + $sizes.Count + " 个尺寸")
$ico = New-Object System.Drawing.Icon($OutFile)
Write-Output ("验证: Icon 加载成功 " + $ico.Width + "x" + $ico.Height)
$ico.Dispose()
