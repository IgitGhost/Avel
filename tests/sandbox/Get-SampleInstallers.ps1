<#
.SYNOPSIS
    Downloads the sample installers for the Windows Sandbox uninstall scenario, and builds a test MSIX.

.DESCRIPTION
    Fills tests\sandbox\installers (ignored by git) with one sample per installer technology:
      7-Zip (.msi)                    Windows Installer   https://www.7-zip.org
      Notepad++                       NSIS                latest GitHub release
      Inno Setup                      Inno Setup          latest GitHub release (jrsoftware/issrc)
      Visual C++ Redistributable x64  WiX Burn bundle     https://aka.ms/vs/17/release/vc_redist.x64.exe
      AvelTestPackage.msix            MSIX                built here, unsigned (needs Add-AppxPackage -AllowUnsigned)
    The installers are only ever run inside Windows Sandbox. Files already present are kept; use
    -Force to download them again. samples.json records each file's SHA-256 and signer.

.PARAMETER Force
    Download everything again.
#>
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is many times faster without the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$installers = Join-Path $PSScriptRoot 'installers'
New-Item -ItemType Directory -Force $installers | Out-Null

function Get-Sample([string]$Name, [string]$Url, [string]$FileName) {
    $target = Join-Path $installers $FileName
    if ((Test-Path $target) -and -not $Force) {
        Write-Host "  have      $FileName"
        return $target
    }
    Write-Host "  download  $FileName  <- $Url"
    Invoke-WebRequest -Uri $Url -OutFile $target -UseBasicParsing -Headers @{ 'User-Agent' = 'Avel-tests' }
    # Catch download pages served instead of the file: executables start with "MZ", MSI packages
    # are OLE compound files (D0 CF 11 E0).
    $stream = [IO.File]::OpenRead($target)
    $magic = New-Object byte[] 4
    [void]$stream.Read($magic, 0, 4)
    $stream.Dispose()
    $isExe = $magic[0] -eq 0x4D -and $magic[1] -eq 0x5A
    $isMsi = $magic[0] -eq 0xD0 -and $magic[1] -eq 0xCF -and $magic[2] -eq 0x11 -and $magic[3] -eq 0xE0
    if (-not ($isExe -or $isMsi)) {
        Remove-Item -LiteralPath $target
        throw "$Url did not return an installer."
    }
    return $target
}

function Get-LatestGitHubAsset([string]$Repository, [string]$Pattern) {
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/releases/latest" -UseBasicParsing `
        -Headers @{ 'User-Agent' = 'Avel-tests' }
    $asset = $release.assets | Where-Object { $_.name -match $Pattern } | Select-Object -First 1
    if (-not $asset) { throw "No asset matching '$Pattern' in the latest $Repository release." }
    return $asset
}

# An unsigned MSIX: the publisher carries the OID that marks a package as intentionally unsigned.
function New-TestPackage([string]$Target) {
    if ((Test-Path $Target) -and -not $Force) {
        Write-Host "  have      $(Split-Path $Target -Leaf)"
        return
    }
    $makeappx = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\makeappx.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $makeappx) { throw 'makeappx.exe not found; install the Windows SDK (part of the VS C++ workload).' }
    $exe = Get-ChildItem (Join-Path $repoRoot 'build\*\bin\avel_test_spawner.exe') -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $exe) { throw 'avel_test_spawner.exe not found; build the project first.' }

    $layout = Join-Path ([IO.Path]::GetTempPath()) "avel-msix-$PID"
    New-Item -ItemType Directory -Force $layout | Out-Null
    try {
        Copy-Item $exe.FullName (Join-Path $layout 'AvelTestApp.exe')
        Add-Type -AssemblyName System.Drawing
        foreach ($logo in @(@('StoreLogo.png', 50), @('Square44x44Logo.png', 44), @('Square150x150Logo.png', 150))) {
            $bitmap = New-Object System.Drawing.Bitmap $logo[1], $logo[1]
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            $graphics.Clear([System.Drawing.Color]::FromArgb(255, 0, 120, 90))
            $graphics.Dispose()
            $bitmap.Save((Join-Path $layout $logo[0]), [System.Drawing.Imaging.ImageFormat]::Png)
            $bitmap.Dispose()
        }
        $manifest = @'
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
         IgnorableNamespaces="uap rescap">
  <Identity Name="Avel.TestPackage" Version="1.0.0.0" ProcessorArchitecture="x64"
            Publisher="CN=Avel Tests, OID.2.25.311729368913984317654407730594956997722=1" />
  <Properties>
    <DisplayName>Avel Test Package</DisplayName>
    <PublisherDisplayName>Avel Tests</PublisherDisplayName>
    <Logo>StoreLogo.png</Logo>
  </Properties>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.19041.0" MaxVersionTested="10.0.26100.0" />
  </Dependencies>
  <Resources>
    <Resource Language="en-us" />
  </Resources>
  <Applications>
    <Application Id="App" Executable="AvelTestApp.exe" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="Avel Test App" Description="Sample package for Avel's uninstall tests"
                          BackgroundColor="transparent" Square150x150Logo="Square150x150Logo.png"
                          Square44x44Logo="Square44x44Logo.png" />
    </Application>
  </Applications>
  <Capabilities>
    <rescap:Capability Name="runFullTrust" />
  </Capabilities>
</Package>
'@
        Set-Content -Path (Join-Path $layout 'AppxManifest.xml') -Value $manifest -Encoding utf8
        Write-Host "  build     $(Split-Path $Target -Leaf)"
        $output = & $makeappx.FullName pack /d $layout /p $Target /o 2>&1
        if ($LASTEXITCODE -ne 0) { throw "makeappx failed:`n$($output -join "`n")" }
    } finally {
        Remove-Item -LiteralPath $layout -Recurse -Force
    }
}

Write-Host "Sample installers in $installers"
$samples = [ordered]@{}
$samples['msi'] = Get-Sample '7-Zip' 'https://www.7-zip.org/a/7z2600-x64.msi' '7z-x64.msi'
$npp = Get-LatestGitHubAsset 'notepad-plus-plus/notepad-plus-plus' '^npp\.[\d.]+\.Installer\.x64\.exe$'
$samples['nsis'] = Get-Sample 'Notepad++' $npp.browser_download_url 'npp-installer-x64.exe'
$inno = Get-LatestGitHubAsset 'jrsoftware/issrc' '^innosetup-[\d.]+-x64\.exe$'
$samples['inno'] = Get-Sample 'Inno Setup' $inno.browser_download_url 'innosetup-x64.exe'
$samples['burn'] = Get-Sample 'Visual C++ Redistributable' 'https://aka.ms/vs/17/release/vc_redist.x64.exe' 'vc_redist.x64.exe'
$samples['msix'] = Join-Path $installers 'AvelTestPackage.msix'
New-TestPackage $samples['msix']

$records = foreach ($kind in $samples.Keys) {
    $file = Get-Item $samples[$kind]
    $signature = Get-AuthenticodeSignature $file.FullName
    [ordered]@{
        kind = $kind
        file = $file.Name
        bytes = $file.Length
        sha256 = (Get-FileHash $file.FullName -Algorithm SHA256).Hash
        signature = "$($signature.Status)"
        signer = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
    }
}
$records | ConvertTo-Json | Set-Content (Join-Path $installers 'samples.json') -Encoding utf8
$records | ForEach-Object { [pscustomobject]$_ } | Format-Table kind, file, bytes, signature, signer -AutoSize
