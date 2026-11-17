# build-simp2trad.ps1 —— 生成 data\simp2trad.dat（简→繁 16 位查表）
#
# 输入：对照文本，每行「简体`t繁体」或「简体 繁体」（繁简各一个字）
# 输出：65536 项的 uint16 表（文件 128 KB），索引=简体码位，值=繁体码位
#
# 用法：
#   powershell -File tools\build-simp2trad.ps1 -Source "C:\opencc\STCharacters.txt"
#
# OpenCC 的 STCharacters.txt 一行可能有多个候选（"發 发"），本脚本取第一个繁体字。
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$OutFile = (Join-Path (Split-Path $PSScriptRoot -Parent) 'data\simp2trad.dat')
)

if (-not (Test-Path $Source)) { Write-Error "找不到对照表: $Source"; exit 1 }

$map = New-Object 'System.Collections.Generic.Dictionary[int,int]'
$lines = [IO.File]::ReadAllLines($Source, [Text.Encoding]::UTF8)
foreach ($line in $lines) {
    $t = $line.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    $parts = $t -split "[`t ]+"
    if ($parts.Count -lt 2) { continue }
    $s = $parts[0]
    if ($s.Length -lt 1) { continue }
    $simp = [int][char]$s[0]
    $trad = 0
    foreach ($cand in $parts[1].Split(' ')) {
        if ($cand.Length -ge 1) { $trad = [int][char]$cand[0]; break }
    }
    if ($trad -ne 0) { $map[$simp] = $trad }
}

# 65536 项，默认恒等映射
$buf = New-Object byte[] 131072
for ($i = 0; $i -lt 65536; $i++) {
    $buf[$i * 2]     = [byte]($i -band 0xFF)
    $buf[$i * 2 + 1] = [byte](($i -shr 8) -band 0xFF)
}
foreach ($kv in $map.GetEnumerator()) {
    $buf[$kv.Key * 2]     = [byte]($kv.Value -band 0xFF)
    $buf[$kv.Key * 2 + 1] = [byte](($kv.Value -shr 8) -band 0xFF)
}

[IO.File]::WriteAllBytes($OutFile, $buf)
Write-Output ("映射 " + $map.Count + " 个字 -> " + $OutFile + " (" + $buf.Length + " 字节)")
