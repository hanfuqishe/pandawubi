# build-pinyin.ps1 - generate pinyin -> hanzi -> wubi code table for the Z reverse lookup
#   pinyin : Rime luna_pinyin.dict.yaml (ships with Weasel under data\)
#   code   : data\char_fullcodes.tsv
#   weight : data\qqwubi86.dict.tsv (single-char weights, to sort common chars first)
# output : data\pinyin.tsv    pinyin<TAB>char<TAB>wubi-code
param(
    [string]$Source = 'C:\Program Files\Rime\weasel-0.17.4\data\luna_pinyin.dict.yaml',
    [string]$Codes  = '',
    [string]$Dict   = '',
    [string]$Out    = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Codes) { $Codes = Join-Path $root 'data\char_fullcodes.tsv' }
if (-not $Dict)  { $Dict  = Join-Path $root 'data\qqwubi86.dict.tsv' }
if (-not $Out)   { $Out   = Join-Path $root 'data\pinyin.tsv' }
foreach ($f in @($Source, $Codes, $Dict)) { if (-not (Test-Path $f)) { throw "missing input: $f" } }

$codeMap = @{}
foreach ($line in [IO.File]::ReadAllLines($Codes, [Text.Encoding]::UTF8)) {
    $f = $line -split "`t"
    if ($f.Count -ge 2 -and $f[0].Length -eq 1 -and $f[1]) { $codeMap[$f[0]] = $f[1].Trim() }
}
"codes: $($codeMap.Count)"

$weightMap = @{}
$header = $true
foreach ($line in [IO.File]::ReadAllLines($Dict, [Text.Encoding]::UTF8)) {
    if ($header) { $header = $false; continue }
    $f = $line -split "`t"
    if ($f.Count -lt 3 -or $f[1].Length -ne 1) { continue }
    $w = 0L
    if (-not [long]::TryParse($f[2], [ref]$w)) { $w = 0 }
    if (-not $weightMap.ContainsKey($f[1]) -or $weightMap[$f[1]] -lt $w) { $weightMap[$f[1]] = $w }
}
"weights: $($weightMap.Count)"

# 2.5) 繁体字过滤：simp2trad.dat 是 65536 个 uint16（简体 -> 繁体），
#      凡是被别的字映射过去的（即"繁体形式"），除非它本身也是简化字，否则不收录
$tradOnly = New-Object 'System.Collections.Generic.HashSet[int]'
$datPath = Join-Path $root 'data\simp2trad.dat'
if (Test-Path $datPath) {
    $bytes = [IO.File]::ReadAllBytes($datPath)
    if ($bytes.Length -ge 131072) {
        for ($i = 0; $i -lt 65536; $i++) {
            $v = [BitConverter]::ToUInt16($bytes, $i * 2)
            if ($v -ne 0 -and $v -ne $i) { [void]$tradOnly.Add([int]$v) }
        }
        # 本身也是简化字（有自己映射）的，从"繁体"里去掉
        for ($i = 0; $i -lt 65536; $i++) {
            $v = [BitConverter]::ToUInt16($bytes, $i * 2)
            if ($v -ne 0 -and $v -ne $i) { [void]$tradOnly.Remove([int]$i) }
        }
    }
}
"traditional-only chars: $($tradOnly.Count)"

$byPy = @{}
$inData = $false
$total = 0; $kept = 0; $withCode = 0
foreach ($line in [IO.File]::ReadAllLines($Source, [Text.Encoding]::UTF8)) {
    if (-not $inData) { if ($line.Trim() -eq '...') { $inData = $true }; continue }
    if (-not $line -or $line.StartsWith('#')) { continue }
    $f = $line -split "`t"
    if ($f.Count -lt 2) { continue }
    $total++
    $ch = $f[0]
    $py = $f[1].Trim().ToLower()
    if ($ch.Length -ne 1) { continue }
    $cp = [int][char]$ch
    if (-not (($cp -ge 0x4E00 -and $cp -le 0x9FFF) -or $cp -eq 0x3007)) { continue }
    if ($py -notmatch '^[a-z]+$') { continue }
    if ($tradOnly.Count -gt 0 -and $tradOnly.Contains($cp)) { continue }   # 繁体形式不收
    $wubi = if ($codeMap.ContainsKey($ch)) { $codeMap[$ch] } else { '' }
    if ($wubi) { $withCode++ }
    $wt = if ($weightMap.ContainsKey($ch)) { $weightMap[$ch] } else { [long]0 }
    if (-not $byPy.ContainsKey($py)) { $byPy[$py] = New-Object System.Collections.Generic.List[object] }
    $dup = $false
    foreach ($e in $byPy[$py]) { if ($e.Char -eq $ch) { $dup = $true; break } }
    if ($dup) { continue }
    $byPy[$py].Add([pscustomobject]@{ Char = $ch; Code = $wubi; Weight = $wt })
    $kept++
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("pinyin`tchar`tcode")
foreach ($py in ($byPy.Keys | Sort-Object)) {
    $list = $byPy[$py] | Sort-Object -Property @{ Expression = 'Weight'; Descending = $true },
                                              @{ Expression = 'Char';   Descending = $false }
    foreach ($e in $list) { [void]$sb.AppendLine("$py`t$($e.Char)`t$($e.Code)") }
}
[IO.File]::WriteAllText($Out, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
"rows: $total  chars: $kept  withCode: $withCode  syllables: $($byPy.Count)"
"out: $Out  ($([Math]::Round((Get-Item $Out).Length/1KB,1)) KB)"