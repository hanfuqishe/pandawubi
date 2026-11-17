param([Parameter(Mandatory=$true)][string]$Path)
# Mechanical PE import/export table dump (shell-derived context, not REA Evidence).
$fs = [IO.File]::OpenRead($Path)
try {
  $br = New-Object IO.BinaryReader($fs)
  $fs.Position = 0x3C; $peOff = $br.ReadInt32()
  $fs.Position = $peOff; $sig = $br.ReadUInt32()
  $machine = $br.ReadUInt16()
  $numSec = $br.ReadUInt16()
  $fs.Position = $peOff + 20; $optSize = $br.ReadUInt16()
  $optOff = $peOff + 24
  $fs.Position = $optOff; $magic = $br.ReadUInt16()
  $pe32plus = ($magic -eq 0x20B)
  $ddBase = 96
  if ($pe32plus) { $ddBase = 112 }
  $fs.Position = $optOff + $ddBase
  $ddOff = $fs.Position
  $expRva = $br.ReadUInt32(); $expSize = $br.ReadUInt32()
  $impRva = $br.ReadUInt32(); $impSize = $br.ReadUInt32()
  $secOff = $optOff + $optSize
  $sections = @()
  for ($i=0; $i -lt $numSec; $i++) {
    $fs.Position = $secOff + $i*40
    $name = [Text.Encoding]::ASCII.GetString($br.ReadBytes(8)).Trim([char]0)
    $vs = $br.ReadUInt32(); $va = $br.ReadUInt32(); $rs = $br.ReadUInt32(); $rp = $br.ReadUInt32()
    $sections += [pscustomobject]@{Name=$name; VA=$va; VSize=$vs; RawPtr=$rp; RawSize=$rs}
  }
  function ToOff([uint32]$rva) {
    foreach ($s in $sections) { if ($rva -ge $s.VA -and $rva -lt ($s.VA + [Math]::Max($s.VSize,$s.RawSize))) { return [int]($rva - $s.VA + $s.RawPtr) } }
    return -1
  }
  "FILE: $Path"
  "machine=0x{0:X4} pe32plus={1} sections={2}" -f $machine,$pe32plus,$numSec
  if ($expRva -ne 0) {
    $o = ToOff $expRva
    if ($o -ge 0) {
      $fs.Position = $o + 24; $nNames = $br.ReadInt32(); $fs.Position = $o + 32; $namesRva = $br.ReadInt32()
      "EXPORTS count=$nNames"
      $no = ToOff ([uint32]$namesRva)
      for ($i=0; $i -lt [Math]::Min($nNames,80); $i++) {
        $fs.Position = $no + $i*4; $nr = $br.ReadUInt32()
        $so = ToOff $nr
        if ($so -ge 0) { $fs.Position=$so; $sb=@(); while(($c=$br.ReadByte()) -ne 0){ $sb+=$c }; "   " + [Text.Encoding]::ASCII.GetString([byte[]]$sb) }
      }
    }
  } else { "EXPORTS none" }
  if ($impRva -ne 0) {
    $o = ToOff $impRva
    if ($o -ge 0) {
      "IMPORTS"
      $idx = 0
      while ($true) {
        $fs.Position = $o + $idx*20
        $ilt = $br.ReadUInt32(); $ts = $br.ReadUInt32(); $fc = $br.ReadUInt32(); $nmRva = $br.ReadUInt32(); $iat = $br.ReadUInt32()
        if ($nmRva -eq 0 -and $fc -eq 0 -and $ilt -eq 0) { break }
        $no2 = ToOff $nmRva
        $dll = ''
        if ($no2 -ge 0) { $fs.Position=$no2; $sb=@(); while(($c=$br.ReadByte()) -ne 0){$sb+=$c}; $dll=[Text.Encoding]::ASCII.GetString([byte[]]$sb) }
        "  [$dll]"
        $fns = @()
        $tblRva = if ($ilt -ne 0) { $ilt } else { $iat }
        $to = ToOff $tblRva
        $step = 4
        if ($pe32plus) { $step = 8 }
        if ($to -ge 0) {
          for ($k=0; $k -lt 400; $k++) {
            $fs.Position = $to + $k*$step
            $v = if ($pe32plus) { $br.ReadUInt64() } else { [uint64]$br.ReadUInt32() }
            if ($v -eq 0) { break }
            $ordFlag = if ($pe32plus) { 0x8000000000000000 } else { 0x80000000 }
            if (($v -band $ordFlag) -ne 0) { $fns += ('#' + ($v -band 0xFFFF)) }
            else { $so2 = ToOff ([uint32]$v + 2); if ($so2 -ge 0) { $fs.Position=$so2; $sb2=@(); while(($c=$br.ReadByte()) -ne 0){$sb2+=$c}; $fns += [Text.Encoding]::ASCII.GetString([byte[]]$sb2) } }
          }
        }
        "     " + ($fns -join ', ')
        $idx++
        if ($idx -gt 40) { break }
      }
    }
  } else { "IMPORTS none" }
}
finally { $fs.Close() }
