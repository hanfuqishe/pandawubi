# build-data.ps1 —— 一键生成全部数据文件（代码仓里不含词库）
#
# 用法示例：
#   powershell -File tools\build-data.ps1 -RimeYaml "C:\Rime\wubi86.dict.yaml"
#   powershell -File tools\build-data.ps1 -RimeYaml "..." -EngSource "C:\...\engcore.dat" -OpenCC "C:\opencc\STCharacters.txt"
param(
    [Parameter(Mandatory = $true)][string]$RimeYaml,
    [string]$EngSource = '',
    [string]$OpenCC    = '',
    [string]$QqWords   = '',
    [switch]$SkipPack
)

$root  = Split-Path $PSScriptRoot -Parent
$data  = Join-Path $root 'data'
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $data, $build | Out-Null

Write-Output '=== 1/4 单字全码 (char_fullcodes.tsv) ==='
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-chartable.ps1') -YamlPath $RimeYaml
if ($LASTEXITCODE -ne 0) { Write-Error '单字全码生成失败'; exit 1 }

Write-Output '=== 2/4 主码表 (qqwubi86.dict.tsv) ==='
if ($QqWords -eq '') {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-dict.ps1') -YamlPath $RimeYaml -QqWords ''
} else {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-dict.ps1') -YamlPath $RimeYaml -QqWords $QqWords
}
if ($LASTEXITCODE -ne 0) { Write-Error '主码表生成失败'; exit 1 }

if (-not $SkipPack) {
    Write-Output '=== 3/4 打包二进制 (qqwubi86.bin) ==='
    $cli = Join-Path $build 'panda-cli.exe'
    if (Test-Path $cli) {
        & $cli (Join-Path $data 'qqwubi86.dict.tsv') pack (Join-Path $data 'qqwubi86.bin')
    } else {
        Write-Output '  跳过：先运行 build.bat 生成 build\panda-cli.exe'
    }
}

Write-Output '=== 4/4 可选数据 ==='
if ($EngSource -ne '') {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'extract-eng.ps1') -Source $EngSource
} else {
    Write-Output '  英文词表：未指定 -EngSource，跳过（英文补全不可用）'
}
if ($OpenCC -ne '') {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-simp2trad.ps1') -Source $OpenCC
} else {
    Write-Output '  简繁映射：未指定 -OpenCC，跳过（简繁输出无效果）'
}

Write-Output ''
Write-Output '完成。把 data\ 下生成的文件复制到 dist\data\ 即可运行。'