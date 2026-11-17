# 五笔86 词码生成器 + 用 QQ五笔 用户词库做正确性验证
# 输入: Rime wubi86.dict.yaml (字->全码), QQ 用户词库 TSV (码<->词 真值)
# 规则(五笔86标准):
#   1字: 全码
#   2字: c1[:2] + c2[:2]
#   3字: c1[:1] + c2[:1] + c3[:2]
#   4字: 各字首码
#   >=5字: 第1/2/3字首码 + 末字首码
param(
  [string]$YamlPath = "$env:APPDATA\Rime\wubi86.dict.yaml",
  [string]$QqTsv    = 'D:\MobileSync\hanfu\src\wubi\data\qqwubi_user_words.tsv',
  [string]$OutPath  = 'D:\MobileSync\hanfu\src\wubi\data\char_fullcodes.tsv'
)

# ---- 1. 解析 Rime 表: 每个字取最长码作为全码 ----
$lines = [IO.File]::ReadAllLines($YamlPath)
$charCode = @{}          # char -> full code (longest)
$charWeight = @{}
foreach ($l in $lines) {
  if ($l.Length -eq 0 -or $l[0] -eq '#') { continue }
  $p = $l.Split("`t")
  if ($p.Count -lt 2) { continue }
  $w = $p[0]; $c = $p[1]
  if ($w.Length -ne 1) { continue }
  if ($c -notmatch '^[a-y]{1,4}$') { continue }
  if (-not $charCode.ContainsKey($w) -or $c.Length -gt $charCode[$w].Length) {
    $charCode[$w] = $c
  }
}
"字表: $($charCode.Count) 个字有全码"

# ---- 2. 码生成 ----
function GenCode([string]$word) {
  $n = $word.Length
  $codes = @()
  for ($i = 0; $i -lt $n; $i++) {
    $ch = [string]$word[$i]
    if (-not $charCode.ContainsKey($ch)) { return $null }   # 缺字
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

# ---- 3. 用 QQ 用户词库验证 ----
$rows = [IO.File]::ReadAllLines($QqTsv)
$total = 0; $exact = 0; $prefix = 0; $missChar = 0; $mismatch = 0
$badSamples = New-Object System.Collections.ArrayList
$missSamples = New-Object System.Collections.ArrayList
foreach ($l in $rows) {
  if ($l.Length -eq 0 -or $l[0] -eq 'c') { continue }
  $p = $l.Split("`t")
  if ($p.Count -lt 2) { continue }
  $qqCode = $p[0]; $word = $p[1]
  if ($qqCode.Length -eq 0) { continue }
  $total++
  $mine = GenCode $word
  if ($null -eq $mine) {
    $missChar++
    if ($missSamples.Count -lt 10) { [void]$missSamples.Add($word) }
    continue
  }
  if ($mine -eq $qqCode) { $exact++ }
  elseif ($mine.StartsWith($qqCode)) { $prefix++ }
  else {
    $mismatch++
    if ($badSamples.Count -lt 15) { [void]$badSamples.Add(("{0} QQ={1} mine={2}" -f $word, $qqCode, $mine)) }
  }
}
"验证样本: $total"
"  完全一致        : $exact  ({0:P1})" -f ($exact / $total)
"  简码前缀一致    : $prefix  ({0:P1})" -f ($prefix / $total)
"  不一致          : $mismatch  ({0:P1})" -f ($mismatch / $total)
"  字表缺字        : $missChar  ({0:P1})" -f ($missChar / $total)
"  合计正确率(含简码): {0:P2}" -f (($exact + $prefix) / $total)
"--- 不一致样本 ---"
$badSamples | ForEach-Object { "   $_" }
"--- 缺字样本 ---"
$missSamples -join ' '

# ---- 4. 导出字表 ----
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("char`tfullcode")
foreach ($kv in ($charCode.GetEnumerator() | Sort-Object Name)) { [void]$sb.AppendLine(("{0}`t{1}" -f $kv.Key, $kv.Value)) }
[IO.File]::WriteAllText($OutPath, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
"字表已导出: $OutPath"
