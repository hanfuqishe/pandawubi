# extract-eng.ps1 —— 生成 data\eng_words.txt（英文单词补全词表）
#
# 两种来源：
#   1) QQ五笔 的 engcore.dat（魔数 NEPQ，长度前缀 ASCII 串）—— 仅限个人本地使用
#   2) 任意纯文本词表（每行一个单词）—— 用于开源分发
#
# 用法：
#   powershell -File tools\extract-eng.ps1 -Source "C:\...\engcore.dat"
#   powershell -File tools\extract-eng.ps1 -TextList "C:\...\words.txt"
param(
    [string]$Source   = 'C:\Program Files (x86)\Tencent\QQWubi\2.4.629.400\engcore.dat',
    [string]$TextList = '',
    [string]$OutFile  = (Join-Path (Split-Path $PSScriptRoot -Parent) 'data\eng_words.txt')
)

$words = New-Object System.Collections.Generic.List[string]

if ($TextList -ne '') {
    if (-not (Test-Path $TextList)) { Write-Error "找不到词表: $TextList"; exit 1 }
    foreach ($line in [IO.File]::ReadAllLines($TextList, [Text.Encoding]::UTF8)) {
        $w = $line.Trim()
        if ($w -match "^[A-Za-z][A-Za-z'\-]{1,39}$") { [void]$words.Add($w) }
    }
    Write-Output ("从文本词表读取: " + $words.Count)
}
else {
    if (-not (Test-Path $Source)) {
        Write-Error "找不到 engcore.dat: $Source`n可用 -TextList 指定纯文本词表代替。"
        exit 1
    }
    $b = [IO.File]::ReadAllBytes($Source)
    Write-Output ("读取 " + $b.Length + " 字节, 魔数 " + [Text.Encoding]::ASCII.GetString($b, 0, 4))

    # 长度前缀 ASCII 串: [len][bytes]
    $i = 0
    while ($i -lt $b.Length) {
        $len = $b[$i]
        if ($len -ge 2 -and $len -le 24 -and ($i + 1 + $len) -le $b.Length) {
            $ok = $true
            for ($k = 0; $k -lt $len; $k++) {
                $c = $b[$i + 1 + $k]
                if (-not (($c -ge 0x41 -and $c -le 0x5A) -or ($c -ge 0x61 -and $c -le 0x7A) -or
                          $c -eq 0x27 -or $c -eq 0x2D -or $c -eq 0x20)) { $ok = $false; break }
            }
            if ($ok) {
                [void]$words.Add([Text.Encoding]::ASCII.GetString($b, $i + 1, $len))
                $i = $i + 1 + $len
                continue
            }
        }
        $i++
    }
    Write-Output ("解析出词条: " + $words.Count)

    # 真正的字母序词表从 "Aachen" 开始，之前是短语/编码段，含噪声
    $start = $words.IndexOf('Aachen')
    if ($start -gt 0) {
        $words = $words.GetRange($start, $words.Count - $start)
        Write-Output ("裁掉前置噪声 " + $start + " 条, 剩余 " + $words.Count)
    }
}

# 清洗：纯字母 / 撇号 / 连字符，去重，保持原顺序
$seen = New-Object 'System.Collections.Generic.HashSet[string]'
$clean = New-Object System.Collections.Generic.List[string]
foreach ($w in $words) {
    if ($w -notmatch "^[A-Za-z][A-Za-z'\-]*$") { continue }
    if ($seen.Add($w)) { [void]$clean.Add($w) }
}

[IO.File]::WriteAllLines($OutFile, $clean, (New-Object System.Text.UTF8Encoding($false)))
Write-Output ("写出 " + $clean.Count + " 条 -> " + $OutFile)
