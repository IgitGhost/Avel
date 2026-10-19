# Dot-sourced by the scenarios that work with the sample programs (see Get-SampleInstallers.ps1).
# Expects $cli (avel-cli.exe) and $installers (the installers folder) to be set.

$samples = @(
    @{ kind = 'msi';  pattern = '^7-Zip ' },
    # Notepad++ also registers a sparse package named "Notepad++" for the Windows 11 context menu,
    # hence the separate pattern for "gone". Its uninstaller leaves that package behind, silent or
    # not; it's reported as a known leftover until Phase 3's leftover removal takes it away.
    @{ kind = 'nsis'; pattern = '^Notepad\+\+ \('; gone = '^Notepad\+\+'; known = '^Notepad\+\+$' },
    @{ kind = 'inno'; pattern = '^Inno Setup' },
    @{ kind = 'burn'; pattern = '^Microsoft Visual C\+\+ .*Redistributable \(x64\)' },
    @{ kind = 'msix'; pattern = '^Avel Test App$' }
)

function Install-Sample([string]$kind) {
    switch ($kind) {
        'msi'  { $p = Start-Process msiexec.exe "/i `"$installers\7z-x64.msi`" /qn /norestart" -Wait -PassThru }
        'nsis' { $p = Start-Process "$installers\npp-installer-x64.exe" '/S' -Wait -PassThru }
        'inno' { $p = Start-Process "$installers\innosetup-x64.exe" '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /ALLUSERS' -Wait -PassThru }
        'burn' { $p = Start-Process "$installers\vc_redist.x64.exe" '/install /quiet /norestart' -Wait -PassThru }
        'msix' { Add-AppxPackage -Path "$installers\AvelTestPackage.msix" -AllowUnsigned; return 0 }
    }
    return $p.ExitCode
}

function Get-Inventory {
    $output = & $cli list --json
    if ($LASTEXITCODE -ne 0) { throw "avel-cli list failed ($LASTEXITCODE)" }
    return ($output -join "`n" | ConvertFrom-Json).programs
}

function Find-Samples($programs) {
    foreach ($sample in $samples) {
        $found = @($programs | Where-Object { $_.listed_in_settings -and $_.name -match $sample.pattern })
        $sample.program = if ($found.Count -eq 1) { $found[0] } else { $null }
        $sample.matches = $found.Count
    }
}
