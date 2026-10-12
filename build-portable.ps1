param(
    [string]$SourceDirectory = (Join-Path $PSScriptRoot 'dist/qt'),
    [string]$OutputFile = (Join-Path $PSScriptRoot 'dist/TeamSpeakAudioControl-portable.exe')
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
if (-not (Test-Path -LiteralPath (Join-Path $source 'TeamSpeakAudioControl.exe'))) { throw 'Run build-qt.cmd first.' }
$work = Join-Path $PSScriptRoot 'build/portable-payload'
New-Item -ItemType Directory -Force $work | Out-Null
$ddf = @('.OPTION EXPLICIT', '.Set Cabinet=on', '.Set Compress=on', '.Set CompressionType=LZX',
    '.Set CompressionMemory=21', '.Set MaxDiskSize=0', '.Set MaxCabinetSize=0',
    '.Set CabinetNameTemplate=payload.cab', ".Set DiskDirectoryTemplate=`"$work`"",
    ".Set InfFileName=`"$work/payload.inf`"", ".Set RptFileName=`"$work/payload.rpt`"")
$manifest = @()
$index = 0
foreach ($file in (Get-ChildItem -LiteralPath $source -Recurse -File | Sort-Object FullName)) {
    $relative = $file.FullName.Substring($source.TrimEnd('\').Length + 1).Replace('\', '/')
    if ($relative -match '[\r\n"]' -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Unsupported payload path: $relative" }
    $id = 'f{0:d6}' -f $index++
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest += "$id $hash $relative"
    $ddf += "`"$($file.FullName)`" $id"
}
[IO.File]::WriteAllLines("$work/payload.ddf", [string[]]$ddf, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllLines("$work/manifest.txt", [string[]]$manifest, [Text.UTF8Encoding]::new($false))
& "$env:SystemRoot/System32/makecab.exe" /F "$work/payload.ddf"
if ($LASTEXITCODE -ne 0) { throw 'CAB compression failed.' }
@("1 ICON `"$($PSScriptRoot.Replace('\','/'))/app/icon.ico`"", "101 RCDATA `"$($work.Replace('\','/'))/payload.cab`"", "102 RCDATA `"$($work.Replace('\','/'))/manifest.txt`"") |
    Set-Content -LiteralPath "$work/payload.rc" -Encoding ascii
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 C++ Build Tools are required.' }
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake.exe).Source }
& $cmake -S "$PSScriptRoot/portable" -B "$PSScriptRoot/build/portable" -G 'Visual Studio 17 2022' -A x64 "-DPORTABLE_RESOURCE_FILE=$($work.Replace('\','/'))/payload.rc"
if ($LASTEXITCODE -ne 0) { throw 'Launcher configuration failed.' }
& $cmake --build "$PSScriptRoot/build/portable" --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Launcher build failed.' }
$output = [IO.Path]::GetFullPath($OutputFile)
New-Item -ItemType Directory -Force (Split-Path -Parent $output) | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot/build/portable/Release/TeamSpeakAudioControl-portable.exe" -Destination $output -Force
Write-Output "Single-file portable application: $output"
