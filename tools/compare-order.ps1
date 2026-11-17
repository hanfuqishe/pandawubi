# 双端候选顺序对比：我们 vs QQ五笔
# 用法：先填 data\qq-order.tsv 的 qq 列（用逗号分隔候选词），再运行本脚本
param(
    [string]$QqFile = 'D:\MobileSync\hanfu\src\wubi\data\qq-order.tsv',
    [string]$Cli    = 'D:\MobileSync\hanfu\src\wubi\build\panda-cli.exe',
    [string]$Dict   = 'D:\MobileSync\hanfu\src\wubi\data\qqwubi86.bin',
    [int]$SortMode  = 0
)

# 我们这一侧：直接调 CLI 的 order 命令
$ours = @{}
$out = & $Cli $Dict order $SortMode 2>&1
foreach ($line in $out) {
    if ($line -notmatch "`t") { continue }
    $parts = $line.Split("`t")
    $code = $parts[0]
    $list = @()
    for ($i = 1; $i -lt $parts.Count; $i++) {
        $m = [regex]::Match($parts[$i], '^\d+:(.+)$')
        if ($m.Success) { $list += $m.Groups[1].Value }
    }
    if ($code) { $ours[$code] = $list }
}

if (-not (Test-Path $QqFile)) {
    "未找到 $QqFile —— 下面是模板，把 QQ五笔 里看到的顺序填到第二列（逗号分隔）："
    ""
    foreach ($k in $ours.Keys) { "$k`t" }
    exit 2
}

$rows = [IO.File]::ReadAllLines($QqFile) | Where-Object { $_ -and -not $_.StartsWith('#') }
$total = 0; $top1 = 0; $top2 = 0; $top3set = 0
"码        我们首位      QQ首位        首位一致  前3重合"
"----------------------------------------------------------"
foreach ($r in $rows) {
    $p = $r.Split("`t")
    if ($p.Count -lt 2) { continue }
    $code = $p[0].Trim()
    $qq = @($p[1].Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if ($qq.Count -eq 0) { continue }
    if (-not $ours.ContainsKey($code)) { "  (本地方案里没有码 $code)"; continue }

    $mine = $ours[$code]
    $total++
    $o1 = if ($mine.Count -gt 0) { $mine[0] } else { '' }
    $q1 = $qq[0]
    if ($o1 -eq $q1) { $top1++ }
    if ($qq.Count -gt 1 -and $mine.Count -gt 1 -and $mine[1] -eq $qq[1]) { $top2++ }
    $mineTop3 = @($mine | Select-Object -First 3)
    $qqTop3 = @($qq | Select-Object -First 3)
    $inter = @($mineTop3 | Where-Object { $qqTop3 -contains $_ })
    $overlap = $inter.Count
    if ($overlap -ge 2) { $top3set++ }

    "{0,-9} {1,-12} {2,-12}  {3,-8} {4}" -f $code, $o1, $q1, $(if ($o1 -eq $q1) { 'YES' } else { 'no' }), "$overlap/3"
}

""
if ($total -eq 0) { "没有可对比的行（第二列为空）"; exit 2 }
"共对比 $total 个码："
"  首位一致      : $top1 / $total  ({0:P0})" -f ($top1 / $total)
"  第二位也一致  : $top2 / $total"
"  前 3 候选重合 : $top3set / $total  (至少 2 个相同)"
if ($top1 -eq $total) { "  结论: 首位排序与 QQ五笔 完全一致" }
elseif ($top1 -ge [Math]::Ceiling($total * 0.6)) { "  结论: 首位大体一致" }
else { "  结论: 差异明显，需要调权重或改用 QQ 顺序" }
exit 0
