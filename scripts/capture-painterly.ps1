param(
  [string]$Exe = ".\Build-painterly\bin\Release\glGenVk.exe",
  [string]$OutDir = ".\captures\painterly"
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
foreach ($pose in @("meadow", "forest", "mountain")) {
  $env:GLGEN_SMOKE_FRAMES = "120"
  $env:GLGEN_SMOKE_STYLE_POSE = $pose
  $env:GLGEN_SMOKE_CAPTURE = (Join-Path $OutDir "$pose.png")
  & $Exe
  if ($LASTEXITCODE -ne 0) { throw "Capture failed for $pose" }
}
Remove-Item Env:GLGEN_SMOKE_FRAMES,Env:GLGEN_SMOKE_STYLE_POSE,Env:GLGEN_SMOKE_CAPTURE
