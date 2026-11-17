# 经验性记录扫描器：在 wubi.dict 中定位 UTF-16 词条记录，统计可恢复数量与记录形状
param(
  [string]$Path = 'C:\Program Files (x86)\Tencent\QQWubi\2.4.629.400\wubi.dict',
  [int]$MaxSamples = 25
)
$b = [IO.File]::ReadAllBytes($Path)
$len = $b.Length
$declaredCount = [BitConverter]::ToUInt32($b, 12)
$declaredSize = [BitConverter]::ToUInt32($b, 8)
"file=$len declaredSize=$declaredSize declaredCount=$declaredCount"

$records = New-Object System.Collections.ArrayList
$i = 0x10
$cjkCount = 0
while ($i -lt $len - 2) {
  $cu = [BitConverter]::ToUInt16($b, $i)
  $isCjk = ($cu -ge 0x3400 -and $cu -le 0x9FFF) -or ($cu -ge 0xF900 -and $cu -le 0xFAFF)
  if ($isCjk) {
    # 收集连续 CJK 段（长度上限 32 个码元，允许中间夹 0x00 吗？不允许，严格连续）
    $sb = New-Object System.Text.StringBuilder
    $j = $i
    while ($j -lt $len - 2) {
      $c2 = [BitConverter]::ToUInt16($b, $j)
      $ok = ($c2 -ge 0x3400 -and $c2 -le 0x9FFF) -or ($c2 -ge 0xF900 -and $c2 -le 0xFAFF)
      if (-not $ok) { break }
      [void]$sb.Append([char]$c2)
      $j += 2
      if ($sb.Length -ge 32) { break }
    }
    $text = $sb.ToString()
    $tailLen = [Math]::Min(24, $len - $j)
    $tail = @()
    for ($k = 0; $k -lt $tailLen; $k += 2) { $tail += ('0x{0:X4}' -f [BitConverter]::ToUInt16($b, $j + $k)) }
    [void]$records.Add([pscustomobject]@{
      Offset = $i
      Length = $text.Length
      Text   = $text
      TailHex = (($b[$j..([Math]::Min($j + 15, $len - 1))]) | ForEach-Object { '{0:X2}' -f $_ }) -join ''
    })
    $i = $j
  } else {
    $i += 2
  }
}

"candidate CJK runs: $($records.Count)"
$byLen = $records | Group-Object Length | Sort-Object { [int]$_.Name }
"length distribution:"
$byLen | ForEach-Object { "   len={0,-3} count={1}" -f $_.Name, $_.Count }
"--- first $MaxSamples ---"
$records | Select-Object -First $MaxSamples | ForEach-Object { "   @0x{0:X6} len={1,-2} {2,-12} tail={3}" -f $_.Offset, $_.Length, $_.Text, $_.TailHex }
"--- random middle sample ---"
$records | Select-Object -Skip ([int]($records.Count * 0.5)) -First 10 | ForEach-Object { "   @0x{0:X6} len={1,-2} {2,-12} tail={3}" -f $_.Offset, $_.Length, $_.Text, $_.TailHex }
