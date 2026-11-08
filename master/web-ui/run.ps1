param(
    [int]$Port = 8765,
    [switch]$NoBrowser
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

python -c "import pysoem" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "未检测到 pysoem，正在安装依赖…" -ForegroundColor Yellow
    python -m pip install -r "$here\requirements.txt"
}

$argsList = @("$here\main.py", "--port", "$Port")
if ($NoBrowser) { $argsList += "--no-browser" }

python @argsList
