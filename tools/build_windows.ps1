# VidScope Windows PowerShell Build Wrapper

$ScriptDir = $PSScriptRoot
$PythonBin = Get-Command python -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Path -First 1

if (-not $PythonBin) {
    $PythonBin = Get-Command py -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Path -First 1
}

if (-not $PythonBin) {
    Write-Error "[ERROR] Python executable not found in PATH. Please install Python 3.8+."
    exit 1
}

$BuildScript = Join-Path $ScriptDir "build.py"
& $PythonBin $BuildScript @args

exit $LASTEXITCODE
