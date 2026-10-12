param(
    [string]$Source = "$PSScriptRoot/dist/qt",
    [string]$Wix = "$PSScriptRoot/.deps/wix/wix.exe"
)
$ErrorActionPreference = 'Stop'
$version = (Get-Content "$PSScriptRoot/VERSION" -Raw).Trim()
if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw 'VERSION must contain major.minor.patch' }
$parts = $version.Split('.')
if ([int]$parts[0] -gt 255 -or [int]$parts[1] -gt 255 -or [int]$parts[2] -gt 65535) { throw 'VERSION exceeds MSI limits' }
$Source = (Resolve-Path -LiteralPath $Source).Path
$exe = Join-Path $Source 'TeamSpeakAudioControl.exe'
if ((Get-Item -LiteralPath $exe).VersionInfo.ProductVersion -ne $version) { throw 'Rebuild application: its version differs from VERSION' }
if (!(Test-Path -LiteralPath $Wix)) { throw 'Install WiX: dotnet tool install wix --version 4.0.6 --tool-path .deps/wix' }
$build = "$PSScriptRoot/build/msi"
New-Item -ItemType Directory -Force -Path $build | Out-Null
function Xml([string]$value) { [System.Security.SecurityElement]::Escape($value) }
function Id([string]$value) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { 'p' + ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($value.ToLowerInvariant())))).Replace('-', '').Substring(0, 32) }
    finally { $sha.Dispose() }
}
$components = [Collections.Generic.List[string]]::new()
function DirectoryXml([string]$path, [string]$relative) {
    $body = [Collections.Generic.List[string]]::new()
    foreach ($file in Get-ChildItem -LiteralPath $path -File | Sort-Object Name) {
        $rel = "$relative/$($file.Name)"
        $id = Id $rel
        # Stable identity across versions and build machines; one component per installed file.
        $guid = ([guid]::ParseExact((Id "TS6AudioControl/component/v1$rel").Substring(1), 'N')).ToString('B')
        $components.Add("<ComponentRef Id=`"$id`" />")
        $body.Add(@"
<Component Id="$id" Guid="$guid" Bitness="always64">
  <File Id="f$id" Source="$(Xml $file.FullName)" Name="$(Xml $file.Name)" />
  <RegistryValue Root="HKCU" Key="Software\TS6AudioControl\Installer\Files" Name="$id" Type="integer" Value="1" KeyPath="yes" />
  <RemoveFolder Id="r$id" On="uninstall" />
</Component>
"@)
    }
    foreach ($dir in Get-ChildItem -LiteralPath $path -Directory | Sort-Object Name) {
        $rel = "$relative/$($dir.Name)"
        $body.Add("<Directory Id=`"$(Id $rel)`" Name=`"$(Xml $dir.Name)`">$(DirectoryXml $dir.FullName $rel)</Directory>")
    }
    $body -join "`n"
}
$tree = DirectoryXml $Source ''
$xml = @"
<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">
 <Package Name="TeamSpeak Audio Control" Manufacturer="TS6 Audio Control" Version="$version"
          UpgradeCode="{E81AE8B1-1D50-4D29-91AD-40BAA937128B}" Scope="perUser" InstallerVersion="500" Compressed="yes" Language="1049" Codepage="1251">
  <MajorUpgrade DowngradeErrorMessage="Уже установлена более новая версия TeamSpeak Audio Control." AllowSameVersionUpgrades="yes" IgnoreLanguage="yes" Schedule="afterInstallInitialize" />
  <MediaTemplate EmbedCab="yes" CompressionLevel="high" />
  <Launch Condition="Installed OR VersionNT64" Message="64-bit Windows is required." />
  <Property Id="ARPNOMODIFY" Value="1" />
  <Property Id="ARPURLINFOABOUT" Value="https://github.com/gutjei/ts6-audio-control" />
  <Icon Id="AppIcon.ico" SourceFile="$(Xml "$PSScriptRoot/app/icon.ico")" />
  <Property Id="ARPPRODUCTICON" Value="AppIcon.ico" />
  <Property Id="MSIRESTARTMANAGERCONTROL" Value="Disable" />
  <UIRef Id="AudioControlWizard" />
  <StandardDirectory Id="LocalAppDataFolder">
   <Directory Id="UserPrograms" Name="Programs">
    <Directory Id="INSTALLFOLDER" Name="TS6AudioControl">
     $tree
     <Component Id="InstallRegistration" Guid="*" Bitness="always64">
      <RegistryValue Root="HKCU" Key="Software\TS6AudioControl\Installer" Name="InstallDir" Type="string" Value="[INSTALLFOLDER]" KeyPath="yes" />
      <RemoveFolder Id="RemoveInstallFolder" On="uninstall" />
     </Component>
    </Directory>
   </Directory>
  </StandardDirectory>
  <StandardDirectory Id="ProgramMenuFolder">
   <Directory Id="MenuFolder" Name="TeamSpeak Audio Control">
    <Component Id="MenuShortcut" Guid="*" Bitness="always64">
     <Shortcut Id="AppShortcut" Name="TeamSpeak Audio Control" Target="[INSTALLFOLDER]TeamSpeakAudioControl.exe" WorkingDirectory="INSTALLFOLDER" Icon="AppIcon.ico" IconIndex="0" />
     <RegistryValue Root="HKCU" Key="Software\TS6AudioControl\Installer" Name="Shortcut" Type="integer" Value="1" KeyPath="yes" />
     <RemoveFolder Id="RemoveMenuFolder" On="uninstall" />
    </Component>
   </Directory>
  </StandardDirectory>
  <Feature Id="Application" Title="TeamSpeak Audio Control" Level="1">
   $($components -join "`n")
   <ComponentRef Id="InstallRegistration" /><ComponentRef Id="MenuShortcut" />
  </Feature>
 </Package>
</Wix>
"@
$xml | Set-Content "$build/Package.wxs" -Encoding utf8
$env:DOTNET_ROLL_FORWARD = 'Major'
& $Wix extension add -g WixToolset.UI.wixext/4.0.6
if ($LASTEXITCODE -ne 0) { throw 'WiX UI extension installation failed' }
& $Wix build "$build/Package.wxs" "$PSScriptRoot/installer/Wizard.wxs" -arch x64 `
    -ext WixToolset.UI.wixext/4.0.6 -culture ru-RU -loc "$PSScriptRoot/installer/ru-RU.wxl" `
    -d "ApplicationFileId=f$(Id '/TeamSpeakAudioControl.exe')" `
    -out "$PSScriptRoot/dist/TeamSpeakAudioControl-windows-x64.msi"
if ($LASTEXITCODE -ne 0) { throw 'MSI build failed' }
