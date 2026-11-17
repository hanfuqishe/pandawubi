# 分析 QQ五笔 wubi.dict 记录里的 A/B/C 字段是否编码了候选排序
# 思路: 用五笔86规则为每个词算出码, 按码分组, 再看同码组内 A/B 与 Rime 词频的序关系
$ws = Split-Path $PSScriptRoot -Parent
$charTsv = Join-Path $ws 'data\char_fullcodes.tsv'
$wordsTsv = Join-Path $ws 'data\wubi_words.tsv'
$yaml = Join-Path $env:APPDATA 'Rime\wubi86.dict.yaml'

# ---- 字全码 ----
$charCode = @{}
foreach ($l in [IO.File]::ReadAllLines($charTsv)) {
    $p = $l.Split("`t")
    if ($p.Count -ge 2) { $charCode[$p[0]] = $p[1] }
}
"— 字表 $($charCode.Count)"

function GenCode([string]$word) {
    $n = $word.Length
    $codes = @()
    for ($i = 0; $i -lt $n; $i++) {
        $ch = [string]$word[$i]
        if (-not $charCode.ContainsKey($ch)) { return $null }
        $codes += $charCode[$ch]
    }
    switch ($n) {
        1 { return $codes[0] }
        2 { return ($codes[0].Substring(0,2) + $codes[1].Substring(0,2)) }
        3 { return ($codes[0].Substring(0,1) + $codes[1].Substring(0,1) + $codes[2].Substring(0,2)) }
        4 { return ($codes[0].Substring(0,1) + $codes[1].Substring(0,1) + $codes[2].Substring(0,1) + $codes[3].Substring(0,1)) }
        default { return ($codes[0].Substring(0,1) + $codes[1].Substring(0,1) + $codes[2].Substring(0,1) + $codes[$n-1].Substring(0,1)) }
    }
}

# ---- Rime 词频: word -> 最大权重 ----
$wordWeight = @{}
foreach ($l in [IO.File]::ReadAllLines($yaml)) {
    if ($l.Length -eq 0 -or $l[0] -eq '#') { continue }
    $p = $l.Split("`t")
    if ($p.Count -lt 3) { continue }
    $w = 0L
    if (-not [long]::TryParse($p[2], [ref]$w)) { continue }
    if (-not $wordWeight.ContainsKey($p[0]) -or $w -gt $wordWeight[$p[0]]) { $wordWeight[$p[0]] = $w }
}
"— Rime 词频条目 $($wordWeight.Count)"

# ---- 载入 QQ 记录并算码 ----
$groups = @{}    # code -> list of (word, A, B, C, weight)
$noCode = 0; $joined = 0; $withWeight = 0
foreach ($l in [IO.File]::ReadAllLines($wordsTsv)) {
    if ($l.Length -eq 0 -or $l[0] -eq 'o') { continue }
    $p = $l.Split("`t")
    if ($p.Count -lt 5) { continue }
    $word = $p[1]
    $code = GenCode $word
    if ($null -eq $code) { $noCode++; continue }
    $joined++
    $wt = 0L
    if ($wordWeight.ContainsKey($word)) { $wt = $wordWeight[$word]; $withWeight++ }
    if (-not $groups.ContainsKey($code)) { $groups[$code] = New-Object System.Collections.ArrayList }
    [void]$groups[$code].Add([pscustomobject]@{ W = $word; A = [uint32]$p[2]; B = [uint32]$p[3]; C = [uint32]$p[4]; Wt = $wt })
}
"— 算出码 $joined 条 (缺字 $noCode), 其中有 Rime 词频 $withWeight"

# ---- 同码组内序关系: 统计 A / B 与词频的一致对比例 ----
$multi = $groups.GetEnumerator() | Where-Object { $_.Value.Count -ge 2 }
"— 多候选组数 $($multi.Count)"

$concA = 0; $discA = 0; $concB = 0; $discB = 0
$tieA = 0; $tieB = 0
$samples = New-Object System.Collections.ArrayList
foreach ($kv in $multi) {
    $arr = $kv.Value | Where-Object { $_.Wt -gt 0 }
    if ($arr.Count -lt 2) { continue }
    for ($i = 0; $i -lt $arr.Count - 1; $i++) {
        for ($j = $i + 1; $j -lt $arr.Count; $j++) {
            $wi = $arr[$i].Wt; $wj = $arr[$j].Wt
            if ($wi -eq $wj) { continue }
            $wiBetter = $wi -gt $wj
            # A 更小 是否对应 词频更高
            if ($arr[$i].A -eq $arr[$j].A) { $tieA++ }
            elseif (($arr[$i].A -lt $arr[$j].A) -eq $wiBetter) { $concA++ } else { $discA++ }
            if ($arr[$i].B -eq $arr[$j].B) { $tieB++ }
            elseif (($arr[$i].B -lt $arr[$j].B) -eq $wiBetter) { $concB++ } else { $discB++ }
        }
    }
    if ($samples.Count -lt 8 -and $arr.Count -ge 2) {
        $desc = ($arr | Sort-Object Wt -Descending | Select-Object -First 4 | ForEach-Object { "$($_.W) A=$($_.A) B=$($_.B) wt=$($_.Wt)" }) -join ' | '
        [void]$samples.Add("  code=$($kv.Key): $desc")
    }
}

"— A: 一致 $concA / 不一致 $discA / 相等 $tieA   => 一致率 " + ("{0:P1}" -f ($concA / [Math]::Max(1, $concA + $discA)))
"— B: 一致 $concB / 不一致 $discB / 相等 $tieB   => 一致率 " + ("{0:P1}" -f ($concB / [Math]::Max(1, $concB + $discB)))
""
"样例（按 Rime 词频降序）:"
$samples | ForEach-Object { $_ }
