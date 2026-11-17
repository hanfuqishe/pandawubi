# 提取 QQ五笔 用户词库 (2UWQ) 记录
# 记录布局（由 WBUser3.dict 十六进制实测确认）:
#   u16 tag = 0x5532 ('2U')
#   u16 recLen        // 整条记录字节数（含 tag）
#   u8  flags         // 0x81 等
#   u32 f1, u32 f2, u32 f3, u16 f4   // 共 15 字节固定区（语义未定）
#   char code[4]      // 五笔码，ASCII，短码后补 0
#   u8  wordLen
#   wchar word[wordLen]
param(
  [string]$DictPath = "$env:APPDATA\Tencent\QQWubi\SAccount\94af6dee31e85ea767e75a5d275da376\WBUser3.dict",
  [string]$OutPath  = 'D:\MobileSync\hanfu\src\wubi\data\qqwubi_user_words.tsv'
)
$fs = New-Object IO.FileStream($DictPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
try {
  $br = New-Object IO.BinaryReader($fs)
  $len = $fs.Length
  $magic = $br.ReadUInt32()
  $v1 = $br.ReadUInt16(); $v2 = $br.ReadUInt16()
  if ($magic -ne 0x51575532) { throw ("not 2UWQ: 0x{0:X}" -f $magic) }
  "2UWQ  u16={0}  u16={1}  size={2}" -f $v1, $v2, $len

  # 找第一条记录：扫描 '2U' 标签且记录长度自洽
  $pos = 4
  while ($pos -lt $len - 4) {
    $fs.Position = $pos
    $tag = $br.ReadUInt16()
    $rl = $br.ReadUInt16()
    if ($tag -eq 0x5532 -and $rl -ge 20 -and ($pos + $rl) -le $len) {
      # 校验：记录尾部应是 code[4] + wordLen + word
      $fs.Position = $pos + $rl - 1
      break
    }
    $pos += 1
  }
  "first record at 0x{0:X}" -f $pos

  $rows = New-Object System.Collections.ArrayList
  while ($pos -lt $len - 4) {
    $fs.Position = $pos
    $tag = $br.ReadUInt16()
    if ($tag -ne 0x5532) { break }
    $rl = $br.ReadUInt16()
    if ($rl -lt 20 -or ($pos + $rl) -gt $len) { break }
    $flags = $br.ReadByte()
    $f1 = $br.ReadUInt32(); $f2 = $br.ReadUInt32(); $f3 = $br.ReadUInt32(); $f4 = $br.ReadUInt16()
    $codeBytes = $br.ReadBytes(4)
    $wordLen = $br.ReadByte()
    if ($pos + $rl - $fs.Position -eq $wordLen * 2) {
      $chars = @()
      for ($i = 0; $i -lt $wordLen; $i++) { $chars += [char]$br.ReadUInt16() }
      $code = (-join ($codeBytes | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ }))
      [void]$rows.Add([pscustomobject]@{
        Off = $pos; Code = $code; Word = (-join $chars)
        Flags = $flags; F1 = $f1; F2 = $f2; F3 = $f3; F4 = $f4
      })
      $pos += $rl
    } else {
      $pos += 1
    }
  }
  "parsed records: $($rows.Count)"
  $dir = Split-Path $OutPath -Parent
  if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  $sb = New-Object System.Text.StringBuilder
  [void]$sb.AppendLine("code`tword`tflags`tf1`tf2`tf3`tf4`toffset")
  foreach ($r in $rows) { [void]$sb.AppendLine(("{0}`t{1}`t{2}`t{3}`t{4}`t{5}`t{6}`t0x{7:X}" -f $r.Code, $r.Word, $r.Flags, $r.F1, $r.F2, $r.F3, $r.F4, $r.Off)) }
  [IO.File]::WriteAllText($OutPath, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
  "written: $OutPath ($((Get-Item $OutPath).Length) bytes)"
  $rows | Select-Object -First 12 | ForEach-Object { "   {0,-6} {1,-10} f1={2} f2=0x{3:X} f3=0x{4:X}" -f $_.Code, $_.Word, $_.F1, $_.F2, $_.F3 }
  "独立码数: " + ($rows | Select-Object -ExpandProperty Code -Unique).Count
}
finally { $fs.Close() }
