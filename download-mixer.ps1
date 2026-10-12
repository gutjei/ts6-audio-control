param(
    [string]$Repository = 'gutjei/ts6-audio-control',
    [string]$Destination = (Join-Path $PSScriptRoot '.deps/mixer')
)
$ErrorActionPreference = 'Stop'
$version = (Get-Content (Join-Path $PSScriptRoot 'mixer-version.txt') -Raw).Trim()
if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw 'Invalid mixer-version.txt' }
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Invalid repository' }
$tag = "mixer-v$version"
$metadata = & gh api "repos/$Repository/releases/tags/$tag"
if ($LASTEXITCODE -ne 0) { throw "Publish $tag before building the application." }
$release = ($metadata -join "`n") | ConvertFrom-Json
if ($release.draft -or $release.prerelease -or $release.tag_name -cne $tag) { throw 'Expected a published stable mixer release' }
$assets = @($release.assets | Where-Object { $_.name -ceq 'audio_mixer.dll' })
if ($assets.Count -ne 1 -or $assets[0].digest -cnotmatch '^sha256:[0-9a-f]{64}$') { throw 'Missing mixer DLL or GitHub SHA-256 digest' }
$digest = $assets[0].digest.Substring(7)
New-Item -ItemType Directory -Force $Destination | Out-Null
$dll = Join-Path $Destination 'audio_mixer.dll'
if (-not (Test-Path -LiteralPath $dll) -or (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -ine $digest) {
    & gh release download $tag --repo $Repository --pattern audio_mixer.dll --dir $Destination --clobber
    if ($LASTEXITCODE -ne 0) { throw 'Mixer download failed' }
}
if ((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -ine $digest) { throw 'Mixer SHA-256 mismatch' }
Write-Output "Verified $tag ($digest): $dll"
