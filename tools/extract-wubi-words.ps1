# 从 QQ五笔 wubi.dict (SBWQ) 提取全部词条记录
# 记录布局（经 REA 反编译 FUN_1802d9600 + 结构验证确认）:
#   header: u32 magic 'SBWQ' | u32 version | u32 totalSize | u32 count
#   +0x10 : 索引区（bucket 槽位 + 子表）
#   0xFE68..EOF : 记录区，记录定长开销：
#       wchar word[]  (UTF-16LE, NUL 结尾)
#       u32 A
#       u32 B
#       u32 C
# 记录区起点 0xFE68，第一条 = 「工」(五笔 a)，可线性枚举到 EOF。
param(
  [string]$DictPath = 'C:\Program Files (x86)\Tencent\QQWubi\2.4.629.400\wubi.dict',
  [string]$OutPath  = 'D:\MobileSync\hanfu\src\wubi\data\wubi_words.tsv'
)
$b = [IO.File]::ReadAllBytes($DictPath)
$len = $b.Length
$magic = [BitConverter]::ToUInt32($b, 0)
$version = [BitConverter]::ToUInt32($b, 4)
$declaredSize = [BitConverter]::ToUInt32($b, 8)
$declaredCount = [BitConverter]::ToUInt32($b, 12)
if ($magic -ne 0x51574253) { throw "not SBWQ: magic=0x$('{0:X}' -f $magic)" }
if ($declaredSize -ne $len) { throw "size mismatch: declared=$declaredSize actual=$len" }
"SBWQ  version=0x{0:X}  size={1}  declaredCount={2}" -f $version, $declaredSize, $declaredCount

function ParseRec([long]$pos) {
  if ($pos + 16 -gt $len) { return $null }
  $q = $pos
  $sb = New-Object System.Text.StringBuilder
  $g = 0
  while ($q -lt $len - 2 -and $g -lt 24) {
    $cu = [BitConverter]::ToUInt16($b, $q)
    if ($cu -eq 0) { break }
    if ($cu -lt 0x20 -or ($cu -ge 0xD800 -and $cu -le 0xDFFF) -or $cu -eq 0xFFFF) { return $null }
    [void]$sb.Append([char]$cu)
    $q += 2; $g++
  }
  if ($q + 14 -gt $len) { return $null }
  if ([BitConverter]::ToUInt16($b, $q) -ne 0) { return $null }
  if ($sb.Length -lt 1) { return $null }
  return [pscustomobject]@{
    Off  = $pos
    Word = $sb.ToString()
    A    = [BitConverter]::ToUInt32($b, $q + 2)
    B    = [BitConverter]::ToUInt32($b, $q + 6)
    C    = [BitConverter]::ToUInt32($b, $q + 10)
    Next = $q + 14
  }
}

$pos = 0xFE68
$rows = New-Object System.Collections.ArrayList
while ($pos -lt $len - 2) {
  $r = ParseRec $pos
  if (-not $r) { break }
  [void]$rows.Add($r)
  $pos = $r.Next
}
"parsed={0}  declaredCount={1}  stopped@0x{2:X} (len 0x{3:X})" -f $rows.Count, $declaredCount, $pos, $len

$dir = Split-Path $OutPath -Parent
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
$utf8 = New-Object System.Text.UTF8Encoding($false)
$inline = New-Object System.Text.StringBuilder
[void]$inline.AppendLine("offset`tword`tA`tB`tC")
foreach ($r in $rows) { [void]$inline.AppendLine(("0x{0:X}`t{1}`t{2}`t{3}`t{4}" -f $r.Off, $r.Word, $r.A, $r.B, $r.C)) }
[IO.File]::WriteAllText($OutPath, $inline.ToString(), $utf8)
"written: $OutPath  ($((Get-Item $OutPath).Length) bytes)"

# 统计
$byLen = $rows | Group-Object { $_.Word.Length }
"词长分布: " + (($byLen | Sort-Object { [int]$_.Name } | ForEach-Object { "{0}字={1}" -f $_.Name, $_.Count }) -join '  ')
$dup = ($rows | Group-Object Word | Where-Object Count -gt 1).Count
"重复词条(同词多码): $dup"
$maxA = ($rows.A | Measure-Object -Maximum).Maximum
$maxB = ($rows.B | Measure-Object -Maximum).Maximum
"A max=0x{0:X}  B max=0x{1:X}" -f $maxA, $maxB
"前缀唯一码(不同A值个数): " + ($rows | Select-Object -ExpandProperty A -Unique).Count
