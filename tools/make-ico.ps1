# make-ico.ps1 —— 从一张 PNG 生成规范的多尺寸 ICO
#   <=64px 用 32bpp BMP(DIB) 条目（兼容性最好，语言栏/老 API 都吃）
#   256px 用 PNG 条目
# 用法: powershell -File tools/make-ico.ps1 -Source build\icon-panda-256.png -OutFile dist\panda.ico
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [int[]]$Sizes = @(16, 24, 32, 48, 64, 128, 256)
)
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $Source)) { Write-Error "找不到源图: $Source"; exit 1 }
$src = [System.Drawing.Image]::FromFile((Resolve-Path $Source))

function Render([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($src, 0, 0, $s, $s)
    $g.Dispose()
    return $bmp
}

function BmpPayload([System.Drawing.Bitmap]$bmp) {
    $s = $bmp.Width
    $rect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $raw = New-Object byte[] ($stride * $s)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $raw, 0, $raw.Length)
    $bmp.UnlockBits($data)

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    # BITMAPINFOHEADER
    $bw.Write([uint32]40)
    $bw.Write([int32]$s)
    $bw.Write([int32]($s * 2))     # XOR + AND
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]0)           # BI_RGB
    $bw.Write([uint32]($s * $s * 4))
    $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)
    # 像素：BMP 自下而上
    for ($y = $s - 1; $y -ge 0; $y--) {
        $bw.Write($raw, $y * $stride, $s * 4)
    }
    # AND 掩码：全 0（不透明）+ 行对齐 4 字节
    $maskStride = [int]([Math]::Ceiling($s / 32.0) * 4)
    $mask = New-Object byte[] ($maskStride * $s)
    $bw.Write($mask, 0, $mask.Length)
    $bw.Flush()
    [byte[]]$bytes = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()
    return ,$bytes
}

function PngPayload([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    [byte[]]$bytes = $ms.ToArray()
    $ms.Dispose()
    return $bytes
}

# 生成各尺寸载荷
$entries = @()
foreach ($s in $Sizes) {
    $bmp = Render $s
    if ($s -ge 256) { $payload = PngPayload $bmp } else { $payload = BmpPayload $bmp }
    $entries += [pscustomobject]@{ Size = $s; Data = $payload }
    $bmp.Dispose()
}

# 组装 ICO
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($out)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }
    $bw.Write([byte]$dim)
    $bw.Write([byte]$dim)
    $bw.Write([byte]0)
    $bw.Write([byte]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]$e.Data.Length)
    $bw.Write([uint32]$offset)
    $offset += $e.Data.Length
}
foreach ($e in $entries) { $bw.Write($e.Data) }
$bw.Flush()
[IO.File]::WriteAllBytes($OutFile, $out.ToArray())
$bw.Dispose(); $out.Dispose()
$src.Dispose()

# 校验：每个尺寸都要能读出来
$bad = 0
foreach ($s in $Sizes) {
    try {
        $i = New-Object System.Drawing.Icon($OutFile, $s, $s)
        if ($i.Width -le 0) { $bad++ }
        $i.Dispose()
    } catch { Write-Output ("  尺寸 $s 读取失败"); $bad++ }
}
Write-Output ("生成 " + $OutFile + " (" + (Get-Item $OutFile).Length + " 字节)，校验失败数=" + $bad)
