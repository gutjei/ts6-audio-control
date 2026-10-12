param(
    [string]$Destination = (Join-Path $PSScriptRoot '.deps/qt'),
    [string]$Mirror = 'https://download.qt.io'
)
$ErrorActionPreference = 'Stop'
$archiveName = '6.10.2-0-202601261212qtbase-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z'
$relative = "online/qtsdkrepository/windows_x86/desktop/qt6_6102/qt6_6102/qt.qt6.6102.win64_msvc2022_64/$archiveName"
$sha256 = 'C4CEDCC54D2036AB20B193DB113CF324D90F390647F831A522952CC9B158F38B'
$cache = Join-Path $PSScriptRoot '.deps/qt-sdk'
New-Item -ItemType Directory -Force $cache, $Destination | Out-Null
$archive = Join-Path $cache 'qtbase.7z'
$sevenZip = (Get-Command 7z.exe -ErrorAction SilentlyContinue).Source
if (-not $sevenZip) { $sevenZip = Join-Path $env:ProgramFiles '7-Zip/7z.exe' }
if (-not (Test-Path -LiteralPath $sevenZip)) { throw 'Install 7-Zip or add 7z.exe to PATH.' }
if (-not (Test-Path -LiteralPath $archive) -or (Get-FileHash $archive -Algorithm SHA256).Hash -ne $sha256) {
    Invoke-WebRequest ($Mirror.TrimEnd('/') + '/' + $relative) -OutFile $archive -TimeoutSec 180
}
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne $sha256) { throw 'Qt archive checksum mismatch.' }
& $sevenZip x -y $archive "-o$Destination"
if ($LASTEXITCODE -ne 0) { throw 'Qt extraction failed.' }
Write-Output "Qt 6.10.2 MSVC x64 SDK: $Destination"
