<#
.SYNOPSIS
    Prepares the current PowerShell session for building Avel.

.DESCRIPTION
    Enters the Visual Studio x64 developer environment (MSVC, Windows SDK, bundled CMake and
    Ninja) and sets VCPKG_ROOT and QT_ROOT_DIR, which CMakePresets.json reads.
    Dot-source it so the environment stays in your session:

        . .\scripts\dev-env.ps1
        cmake --preset release
        cmake --build --preset release
        ctest --preset release

.PARAMETER QtDir
    Path to the Qt MSVC kit. Defaults to $env:QT_ROOT_DIR, then D:\Qt\6.12.0\msvc2022_64.
#>
param(
    [string]$QtDir = $(if ($env:QT_ROOT_DIR) { $env:QT_ROOT_DIR } else { 'D:\Qt\6.12.0\msvc2022_64' })
)

# This script is dot-sourced, so it must not change session-wide preferences such as
# $ErrorActionPreference; failures are reported with explicit `throw`s instead.

$vsInstallerDir = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
$vsPath = & (Join-Path $vsInstallerDir 'vswhere.exe') -latest -prerelease -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw 'Visual Studio with the "Desktop development with C++" workload was not found.'
}

# VsDevCmd calls vswhere through PATH.
$env:Path = "$vsInstallerDir;$env:Path"

Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll') -ErrorAction Stop
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

# VsDevCmd appends its CMake and Ninja to PATH, so an older standalone CMake would win.
# Put the VS-bundled tools first.
$vsCMakeDir = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:Path = "$vsCMakeDir\CMake\bin;$vsCMakeDir\Ninja;$env:Path"

if (-not $env:VCPKG_ROOT) {
    $env:VCPKG_ROOT = Join-Path $vsPath 'VC\vcpkg'
}
if (-not (Test-Path (Join-Path $QtDir 'lib\cmake\Qt6'))) {
    throw "Qt MSVC kit not found at '$QtDir'. Pass -QtDir or set QT_ROOT_DIR."
}
$env:QT_ROOT_DIR = $QtDir

$cmakeVersion = ((& cmake --version) | Select-Object -First 1) -replace 'cmake version ', ''
Write-Host "Avel dev environment ready (VS: $vsPath, CMake: $cmakeVersion, Qt: $QtDir, vcpkg: $env:VCPKG_ROOT)"
