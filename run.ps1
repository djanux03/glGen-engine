$ErrorActionPreference = "Stop"

$releaseExe = "Build\bin\Release\glGen.exe"
$defaultExe = "Build\bin\glGen.exe"

if (Test-Path $releaseExe) {
    & $releaseExe
} elseif (Test-Path $defaultExe) {
    & $defaultExe
} else {
    Write-Error "glGen.exe was not found. Build the project first."
}
