# Copies dinput8.dll + DishonoredVR.dll into the game folder.
param([string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Dishonored2')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path "$GameDir\Dishonored2.exe")) { throw "Dishonored2.exe not found in $GameDir" }
Copy-Item "$repo\build\bin\dinput8.dll" "$GameDir\dinput8.dll" -Force
Copy-Item "$repo\build\bin\DishonoredVR.dll" "$GameDir\DishonoredVR.dll" -Force
Copy-Item "$repo\build\bin\DishonoredVR.pdb" "$GameDir\DishonoredVR.pdb" -Force -ErrorAction SilentlyContinue
# the controller profiles: copied only where the game folder has none yet (a player's edits there are kept)
$bind = "$GameDir\VRControllerBindings"
New-Item -ItemType Directory -Force $bind | Out-Null
foreach ($j in Get-ChildItem "$repo\package\VRControllerBindings" -Filter *.json) {
    if (-not (Test-Path "$bind\$($j.Name)")) { Copy-Item $j.FullName "$bind\$($j.Name)"; "  added VRControllerBindings\$($j.Name)" }
}
"Deployed to $GameDir"
