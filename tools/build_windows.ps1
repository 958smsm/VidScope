# VidScope Windows PowerShell build wrapper.
# No arguments => Release configure + build + runtime repair + tests.

$ErrorActionPreference = 'Stop'
$ScriptDir = $PSScriptRoot
$BuildScript = Join-Path $ScriptDir 'build.py'
$BuildArgs = @($args)

function Find-PythonCommand {
    $python = Get-Command python.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($python) {
        Write-Output $python.Source
        return
    }

    $py = Get-Command py.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($py) {
        Write-Output $py.Source
        Write-Output '-3'
    }
}

$PythonCommand = @(Find-PythonCommand)
if ($PythonCommand.Count -eq 0) {
    Write-Error '[ERROR] Python 3.8+ was not found. Install Python or add python.exe/py.exe to PATH.'
    exit 1
}

if ($BuildArgs.Count -eq 0) {
    Write-Host '[INFO] VidScope default: Release build + runtime repair + tests'
}

$PythonExe = $PythonCommand[0]
$PythonPrefixArgs = @()
if ($PythonCommand.Count -gt 1) {
    $PythonPrefixArgs = @($PythonCommand[1..($PythonCommand.Count - 1)])
}

& $PythonExe @PythonPrefixArgs $BuildScript @BuildArgs
exit $LASTEXITCODE
