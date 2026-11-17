# build-chartable.ps1 —— 生成 data\char_fullcodes.tsv（单字 → 五笔86 全码）
#
# 输入：Rime 的 wubi86.dict.yaml（开源授权）
# 输出：char<TAB>fullcode，用于 build-dict.ps1 给多字词生成编码
#
# 用法：
#   powershell -File tools\build-chartable.ps1 -YamlPath "C:\Rime\wubi86.dict.yaml"
param(
    [Parameter(Mandatory = $true)][string]$YamlPath,
    [string]$OutPath = (Join-Path (Split-Path $PSScriptRoot -Parent) 'data\char_fullcodes.tsv')
)

if (-not (Test-Path $YamlPath)) { Write-Error "找不到词库: $YamlPath"; exit 1 }

$lines = [IO.File]::ReadAllLines($YamlPath, [Text.Encoding]::UTF8)
$inData = $false
$map = New-Object 'System.Collections.Generic.Dictionary[string,string]'

foreach ($line in $lines) {
    $t = $line.TrimEnd()
    if (-not $inData) {
        # Rime 词库正文以单独一行 "..." 开始
        if ($t -eq '...') { $inData = $true }
        continue
    }
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    $p = $t -split "[`t ]+"
    if ($p.Count -lt 2) { continue }
    $word = $p[0]
    $code = $p[1]
    # 只要单字、且是纯字母码（Rime 里可能带声调/数字后缀）
    if ($word.Length -ne 1) { continue }
    if ($code -notmatch '^[a-y]{1,4}$') { continue }
    if (-not $map.ContainsKey($word)) { $map[$word] = $code }
}

$out = New-Object System.Collections.Generic.List[string]
[void]$out.Add("char`tfullcode")
foreach ($kv in $map.GetEnumerator()) {
    [void]$out.Add($kv.Key + "`t" + $kv.Value)
}
[IO.File]::WriteAllLines($OutPath, $out, (New-Object System.Text.UTF8Encoding($false)))
Write-Output ("单字全码 " + $map.Count + " 条 -> " + $OutPath)
