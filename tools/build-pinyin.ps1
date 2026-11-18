# build-pinyin.ps1 - build data\pinyin.tsv for the Z reverse lookup (pinyin -> hanzi -> wubi code)
#   chars : Rime luna_pinyin.dict.yaml (ships with Weasel)  字<TAB>拼音
#   words : data\qqwubi86.dict.tsv (词 -> 五笔全码 + 权重)，拼音由各字的主读音拼起来
#   codes : data\char_fullcodes.tsv
# output: pinyin<TAB>字或词<TAB>五笔全码，按拼音分组，组内按词频从高到低
param(
    [string]$Source = 'C:\Program Files\Rime\weasel-0.17.4\data\luna_pinyin.dict.yaml',
    [string]$Codes  = '',
    [string]$Dict   = '',
    [string]$Out    = '',
    [int]$MaxWords  = 120000
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Codes) { $Codes = Join-Path $root 'data\char_fullcodes.tsv' }
if (-not $Dict)  { $Dict  = Join-Path $root 'data\qqwubi86.dict.tsv' }
if (-not $Out)   { $Out   = Join-Path $root 'data\pinyin.tsv' }
foreach ($f in @($Source, $Codes, $Dict)) { if (-not (Test-Path $f)) { throw "missing input: $f" } }

# 1) 字 -> 五笔全码
$codeMap = @{}
foreach ($line in [IO.File]::ReadAllLines($Codes, [Text.Encoding]::UTF8)) {
    $f = $line -split "`t"
    if ($f.Count -ge 2 -and $f[0].Length -eq 1 -and $f[1]) { $codeMap[$f[0]] = $f[1].Trim() }
}
"codes: $($codeMap.Count)"

# 2) 主码表：单字权重 / 词权重 + 词编码
$charWeight = @{}
$wordRows = New-Object System.Collections.Generic.List[object]
$header = $true
foreach ($line in [IO.File]::ReadAllLines($Dict, [Text.Encoding]::UTF8)) {
    if ($header) { $header = $false; continue }
    $f = $line -split "`t"
    if ($f.Count -lt 3) { continue }
    $w = 0L
    if (-not [long]::TryParse($f[2], [ref]$w)) { $w = 0 }
    $word = $f[1]
    if ($word.Length -eq 1) {
        if ($word -notmatch '^[\u3400-\u9FFF]$') { continue }
        if (-not $charWeight.ContainsKey($word) -or $charWeight[$word] -lt $w) { $charWeight[$word] = $w }
    } elseif ($word.Length -ge 2 -and $word.Length -le 4) {
        if ($word -notmatch '^[\u3400-\u9FFF]+$') { continue }
        $wordRows.Add([pscustomobject]@{ Word = $word; Code = $f[0]; Weight = $w })
    }
}
"char weights: $($charWeight.Count)   word rows: $($wordRows.Count)"

# 3) 繁体字过滤（simp2trad.dat: 65536 个 uint16，简体 -> 繁体）
$tradOnly = New-Object 'System.Collections.Generic.HashSet[int]'
$datPath = Join-Path $root 'data\simp2trad.dat'
if (Test-Path $datPath) {
    $bytes = [IO.File]::ReadAllBytes($datPath)
    if ($bytes.Length -ge 131072) {
        for ($i = 0; $i -lt 65536; $i++) {
            $v = [BitConverter]::ToUInt16($bytes, $i * 2)
            if ($v -ne 0 -and $v -ne $i) { [void]$tradOnly.Add([int]$v) }
        }
        for ($i = 0; $i -lt 65536; $i++) {
            $v = [BitConverter]::ToUInt16($bytes, $i * 2)
            if ($v -ne 0 -and $v -ne $i) { [void]$tradOnly.Remove([int]$i) }
        }
    }
}
"traditional-only: $($tradOnly.Count)"

# 4) 字 -> 拼音（取"权重最高"的那个读音做主读音；多音字只留一个，避免词拼音乱）
$charPy = @{}                       # 主读音（拼词用）
$charAllPy = @{}                    # 字 -> 所有读音（单字反查用，多音字都能查到）
$pyWeight = @{}
$inData = $false
foreach ($line in [IO.File]::ReadAllLines($Source, [Text.Encoding]::UTF8)) {
    if (-not $inData) { if ($line.Trim() -eq '...') { $inData = $true }; continue }
    if (-not $line -or $line.StartsWith('#')) { continue }
    $f = $line -split "`t"
    if ($f.Count -lt 2) { continue }
    $ch = $f[0]; $py = $f[1].Trim().ToLower()
    if ($ch.Length -ne 1) { continue }
    $cp = [int][char]$ch
    if (-not (($cp -ge 0x4E00 -and $cp -le 0x9FFF) -or $cp -eq 0x3007)) { continue }
    if ($py -notmatch '^[a-z]+$') { continue }
    if ($tradOnly.Contains($cp)) { continue }
    $wt = if ($charWeight.ContainsKey($ch)) { $charWeight[$ch] } else { [long]0 }
    if (-not $charAllPy.ContainsKey($ch)) { $charAllPy[$ch] = New-Object System.Collections.Generic.List[string] }
    if (-not $charAllPy[$ch].Contains($py)) { $charAllPy[$ch].Add($py) }
    $key = "$ch|$py"
    if (-not $pyWeight.ContainsKey($key)) { $pyWeight[$key] = $wt }
    $prev = $pyWeight[$key]
    if (-not $charPy.ContainsKey($ch) -or $wt -gt $prev) { $charPy[$ch] = $py; $pyWeight[$key] = $wt }
}
"chars with pinyin: $($charPy.Count)"

# 5) 汇总：字 + 词
$byPy = @{}
function Add-Row([string]$py, [string]$text, [string]$code, [long]$weight) {
    if (-not $text -or -not $py) { return }
    if (-not $script:byPy.ContainsKey($py)) { $script:byPy[$py] = New-Object System.Collections.Generic.List[object] }
    foreach ($e in $script:byPy[$py]) { if ($e.Char -eq $text) { return } }
    $script:byPy[$py].Add([pscustomobject]@{ Char = $text; Code = $code; Weight = $weight })
}
foreach ($ch in $charAllPy.Keys) {
    $code = if ($codeMap.ContainsKey($ch)) { $codeMap[$ch] } else { '' }
    if (-not $code) { continue }
    $wt = if ($charWeight.ContainsKey($ch)) { $charWeight[$ch] } else { [long]0 }
    foreach ($py in $charAllPy[$ch]) { Add-Row $py $ch $code $wt }
}

$picked = $wordRows | Sort-Object -Property @{ Expression = 'Weight'; Descending = $true } | Select-Object -First $MaxWords
$wordsAdded = 0
foreach ($r in $picked) {
    $py = ''
    $ok = $true
    foreach ($c in $r.Word.ToCharArray()) {
        $s = [string]$c
        if (-not $charPy.ContainsKey($s)) { $ok = $false; break }
        $py += $charPy[$s]
    }
    if (-not $ok) { continue }
    $before = $byPy.Count
    Add-Row $py $r.Word $r.Code $r.Weight
    $wordsAdded++
}
"words with pinyin: $wordsAdded"

# 6) 输出：拼音按字母序；同一拼音里按词频从高到低
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("pinyin`tchar`tcode")
$total = 0
foreach ($py in ($byPy.Keys | Sort-Object)) {
    $list = $byPy[$py] | Sort-Object -Property @{ Expression = 'Weight'; Descending = $true }
    foreach ($e in $list) { [void]$sb.AppendLine("$py`t$($e.Char)`t$($e.Code)"); $total++ }
}
[IO.File]::WriteAllText($Out, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
"syllables: $($byPy.Count)   rows: $total   out: $Out ($([Math]::Round((Get-Item $Out).Length/1KB,1)) KB)"