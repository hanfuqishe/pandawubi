# Helper: run a REA CLI command, persist full JSON evidence (pretty), print a compact summary.
# Usage: .\rea-run.ps1 -Name <label> -ReaArgs <string[]>
param(
  [Parameter(Mandatory = $true)][string]$Name,
  [Parameter(Mandatory = $true)][string[]]$ReaArgs
)
$ErrorActionPreference = 'Continue'
$ev = Join-Path $PSScriptRoot 'evidence'
if (-not (Test-Path $ev)) { New-Item -ItemType Directory -Force -Path $ev | Out-Null }
$utf8 = New-Object System.Text.UTF8Encoding($false)

$raw = & rea @ReaArgs 2>&1 | Out-String
$out = Join-Path $ev ($Name + '.json')
try {
  $obj = $raw | ConvertFrom-Json
  $pretty = $obj | ConvertTo-Json -Depth 100
  [IO.File]::WriteAllText($out, $pretty, $utf8)
}
catch {
  [IO.File]::WriteAllText($out, $raw, $utf8)
}
"SAVED $Name -> $((Get-Item $out).Length) bytes"
