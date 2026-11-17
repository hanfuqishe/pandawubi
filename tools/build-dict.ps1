# 合成替代品词库 v2
#   基座: Rime wubi86.dict.yaml 的全部 (词, 码, 权重)  —— 自带一级/二级简码与 Google 词频
#   补充: QQ五笔 系统词库中 Rime 未覆盖的词, 用五笔86规则生成全码
# 输出: code <TAB> word <TAB> weight <TAB> source
param(
  [string]$CharTable = 'D:\MobileSync\hanfu\src\wubi\data\char_fullcodes.tsv',
  [string]$QqWords   = 'D:\MobileSync\hanfu\src\wubi\data\wubi_words.tsv',
  [string]$YamlPath  = "$env:APPDATA\Rime\wubi86.dict.yaml",
  [string]$OutPath   = 'D:\MobileSync\hanfu\src\wubi\data\qqwubi86.dict.tsv'
)

$charCode = @{}
foreach ($l in [IO.File]::ReadAllLines($CharTable)) {
  $p = $l.Split("`t"); if ($p.Count -ge 2) { $charCode[$p[0]] = $p[1] }
}

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

$dict = @{}
$srcMap = @{}
$rimeWords = New-Object 'System.Collections.Generic.HashSet[string]'
$rimeCount = 0

foreach ($l in [IO.File]::ReadAllLines($YamlPath)) {
  if ($l.Length -eq 0 -or $l[0] -eq '#') { continue }
  $p = $l.Split("`t")
  if ($p.Count -lt 2) { continue }
  $w = $p[0]; $c = $p[1]
  if ($c -notmatch '^[a-y]{1,4}$') { continue }
  $wt = 0L
  if ($p.Count -ge 3) { [void][long]::TryParse($p[2], [ref]$wt) }
  $k = "$c|$w"
  if (-not $dict.ContainsKey($k) -or $wt -gt $dict[$k]) { $dict[$k] = $wt }
  $srcMap[$k] = 'rime'
  [void]$rimeWords.Add($w)
  $rimeCount++
}
"Rime 条目: $rimeCount  去重后: $($dict.Count)  独立词: $($rimeWords.Count)"

$qqNew = 0; $qqSkip = 0; $noCode = 0
$seenWord = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($l in [IO.File]::ReadAllLines($QqWords)) {
  if ($l.Length -eq 0 -or $l[0] -eq 'o') { continue }
  $p = $l.Split("`t"); if ($p.Count -lt 5) { continue }
  $word = $p[1]
  if ($word.Length -eq 0 -or -not $seenWord.Add($word)) { continue }
  if ($rimeWords.Contains($word)) { $qqSkip++; continue }
  $code = GenCode $word
  if ($null -eq $code) { $noCode++; continue }
  $k = "$code|$word"
  if ($dict.ContainsKey($k)) { continue }
  $dict[$k] = 0L
  $srcMap[$k] = 'qq'
  $qqNew++
}
"QQ 独有词新增: $qqNew   (Rime 已覆盖: $qqSkip, 缺字无法生成: $noCode)"
"最终条目: $($dict.Count)"

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("code`tword`tweight`tsource")
foreach ($k in $dict.Keys) {
  $i = $k.IndexOf('|')
  $c = $k.Substring(0, $i); $w = $k.Substring($i + 1)
  [void]$sb.AppendLine(("{0}`t{1}`t{2}`t{3}" -f $c, $w, $dict[$k], $srcMap[$k]))
}
[IO.File]::WriteAllText($OutPath, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
"输出: $OutPath ($((Get-Item $OutPath).Length) bytes)"

$lenHist = @{}
foreach ($k in $dict.Keys) { $L = $k.IndexOf('|'); $lenHist[$L]++ }
"码长分布: " + (($lenHist.GetEnumerator() | Sort-Object Name | ForEach-Object { "{0}码={1}" -f $_.Key,$_.Value }) -join '  ')
