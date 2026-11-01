# Builds the release zip for players / testers: dist\DishonoredVR-<version>.zip
#   dinput8.dll, DishonoredVR.dll        the mod (C runtime linked in: no Visual C++ redistributable needed)
#   DishonoredVR.ini                     the settings, every one at its default and explained
#   README.txt                           install / setup / controls / troubleshooting ({REPO_URL} = -RepoUrl)
#   VRControllerBindings\*.json          the controller profiles (one per controller: layout, grips, trackpads...)
#   THIRD-PARTY-NOTICES.txt              the licences of what is linked in (Detours, fmt, Dear ImGui, OpenXR loader, jsoncpp)
# and keeps the matching symbols in local\releases\<version>-<commit>\ (tools\symbolize.py turns a tester's CRASH lines
# into functions with them). The version is set in src\CMakeLists.txt; the commit is appended by the build.
param([switch]$SkipBuild, [switch]$AllowDirty, [string]$RepoUrl = 'https://github.com/thecosmictangerine-cloud')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (-not $SkipBuild) { & "$repo\scripts\build.ps1" | Out-Host }

$vh = Get-Content "$repo\build\generated\Version.h" -Raw
$ver = [regex]::Match($vh, 'DVR_VERSION_STR "([^"]+)"').Groups[1].Value
$git = [regex]::Match($vh, 'DVR_GIT_STR "([^"]+)"').Groups[1].Value
if (-not $ver) { throw "no version in build\generated\Version.h" }
if ($git -like '*dirty*' -and -not $AllowDirty) {
    throw "the build is from uncommitted changes ($git): commit first, so a tester's log names a commit you can check out (or pass -AllowDirty for a test package)"
}

# --- the DLLs must not need anything a player's machine may lack ---
$dumpbin = Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC' -Recurse -Filter dumpbin.exe |
    Where-Object FullName -match 'Hostx64\\x64' | Select-Object -First 1
foreach ($d in 'dinput8.dll', 'DishonoredVR.dll') {
    $deps = & $dumpbin.FullName /dependents "$repo\build\bin\$d" | Select-String '^\s+\S+\.dll\s*$' | ForEach-Object { $_.Line.Trim() }
    $bad = $deps | Where-Object { $_ -match '^(msvcp|vcruntime|ucrtbase|api-ms-win-crt|concrt|openxr_loader)' }
    if ($bad) { throw "$d depends on $($bad -join ', '): the C runtime / OpenXR loader must be linked in" }
    "  $d imports: $($deps -join ', ')"
}

# --- stage ---
$name = "DishonoredVR-$ver"
$stage = "$repo\dist\$name"
Remove-Item $stage -Recurse -Force -EA SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$repo\build\bin\dinput8.dll", "$repo\build\bin\DishonoredVR.dll" $stage
# text files with Windows line endings (Notepad) and the version filled in
foreach ($t in 'DishonoredVR.ini', 'README.txt') {
    $text = (Get-Content "$repo\package\$t" -Raw) -replace "`r`n", "`n" -replace '\{VERSION\}', $ver -replace '\{REPO_URL\}', $RepoUrl
    [IO.File]::WriteAllText("$stage\$t", ($text -replace "`n", "`r`n"), [Text.UTF8Encoding]::new($false))
}
# the controller profiles, in their own folder next to the DLLs
New-Item -ItemType Directory -Force "$stage\VRControllerBindings" | Out-Null
foreach ($j in Get-ChildItem "$repo\package\VRControllerBindings" -Filter *.json) {
    $text = (Get-Content $j.FullName -Raw) -replace "`r`n", "`n"
    [IO.File]::WriteAllText("$stage\VRControllerBindings\$($j.Name)", ($text -replace "`n", "`r`n"), [Text.UTF8Encoding]::new($false))
}
# the licences of everything linked into the DLLs, from vcpkg's copies
$share = "$repo\build\vcpkg_installed\x64-windows-static\share"
$notice = "Dishonored 2 VR $ver includes the following third-party software, linked into DishonoredVR.dll.`r`n"
foreach ($p in @(
        @{ dir = 'detours';       what = 'Microsoft Detours (MIT License) - https://github.com/microsoft/Detours' },
        @{ dir = 'fmt';           what = '{fmt} (MIT License) - https://github.com/fmtlib/fmt' },
        @{ dir = 'imgui';         what = 'Dear ImGui (MIT License) - https://github.com/ocornut/imgui' },
        @{ dir = 'openxr-loader'; what = 'OpenXR SDK loader (Apache License 2.0 / MIT) - https://github.com/KhronosGroup/OpenXR-SDK-Source' },
        @{ dir = 'jsoncpp';       what = 'JsonCpp (MIT License / public domain), used by the OpenXR loader - https://github.com/open-source-parsers/jsoncpp' })) {
    $c = "$share\$($p.dir)\copyright"
    if (-not (Test-Path $c)) { throw "missing licence file $c" }
    $notice += "`r`n" + ('=' * 96) + "`r`n" + $p.what + "`r`n" + ('=' * 96) + "`r`n`r`n" + ((Get-Content $c -Raw) -replace "`r?`n", "`r`n")
}
[IO.File]::WriteAllText("$stage\THIRD-PARTY-NOTICES.txt", $notice, [Text.UTF8Encoding]::new($false))

# --- zip ---
$zip = "$repo\dist\$name.zip"
Remove-Item $zip -Force -EA SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $zip
# --- the symbols of exactly this build ---
$sym = "$repo\local\releases\$ver-$git"
New-Item -ItemType Directory -Force $sym | Out-Null
Copy-Item "$repo\build\bin\dinput8.dll", "$repo\build\bin\dinput8.pdb", "$repo\build\bin\DishonoredVR.dll", "$repo\build\bin\DishonoredVR.pdb" $sym -Force
Copy-Item $zip $sym -Force

"Package: $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"
Get-ChildItem $stage | ForEach-Object { "  {0,-26} {1,10:N0} bytes" -f $_.Name, $_.Length }
"SHA-256:"
foreach ($f in @($zip) + (Get-ChildItem $stage -Filter *.dll | ForEach-Object FullName)) { "  {0}  {1}" -f (Get-FileHash $f -Algorithm SHA256).Hash, (Split-Path $f -Leaf) }
"Symbols kept in $sym (python tools\symbolize.py <DishonoredVR.log> $sym)"
