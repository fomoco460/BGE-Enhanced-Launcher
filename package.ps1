# Assembles the release zip in dist\ from build\ (your builds) and third_party\ (downloaded binaries).
# Run build.bat and src\bgehook\build.sh first. See README.md "Building" for what goes in third_party\.
param([string]$Version = "1.0.0")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$name = "BGE-Enhanced-Launcher-v$Version"
$out = Join-Path $root "dist\$name"
$need = @("build\BGELauncher.exe", "build\d3d9.dll", "third_party\ReShade32.dll", "third_party\dxvk_d3d9.dll", "third_party\shaders\Shaders\ReShade.fxh",
          "third_party\dsoal\dsound.dll", "third_party\dsoal\dsoal-aldrv.dll", "build\eax.dll")
foreach ($n in $need) { if (-not (Test-Path (Join-Path $root $n))) { throw "Missing $n" } }

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
$payload = Join-Path $out "payload"
New-Item -ItemType Directory -Force "$payload\hook", "$payload\reshade-shaders", "$out\licenses" | Out-Null

Copy-Item "$root\build\BGELauncher.exe" $out
Copy-Item "$root\build\d3d9.dll" "$payload\hook\"
Copy-Item "$root\third_party\ReShade32.dll", "$root\third_party\dxvk_d3d9.dll" $payload
Copy-Item "$root\layer\ReShade32_BGE.json" $payload
Copy-Item "$root\third_party\dsoal" "$payload\dsoal" -Recurse
Copy-Item "$root\build\eax.dll" "$payload\dsoal\"
Copy-Item "$root\third_party\shaders\*" "$payload\reshade-shaders\" -Recurse
Copy-Item "$root\shaders\BGE_FSR_RCAS.fx", "$root\shaders\ReShadeUI.fxh" "$payload\reshade-shaders\Shaders\"
Copy-Item "$root\licenses\*" "$out\licenses\"
Copy-Item "$root\LICENSE" "$out\licenses\BGE-Enhanced-Launcher-LICENSE.txt"
Copy-Item "$root\THIRD-PARTY-NOTICES.md", "$root\README.md" $out

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$out\*" -DestinationPath $zip
Get-FileHash $zip -Algorithm SHA256 | ForEach-Object { "$($_.Hash.ToLower())  $name.zip" } | Set-Content "$zip.sha256"
"Built $zip"
