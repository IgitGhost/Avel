# Dot-sourced by the leftover scenarios: an oracle for what Avel must never report or remove,
# built from .NET's special folders, the profiles and the drive roots, independently of Avel's own
# protection rules, and a one-line description of a reported item. Expects a Fail function.

function Describe($item) {
    $location = $item.location
    $where = switch ($location.kind) {
        { $_ -in 'folder', 'file' } { $location.path }
        'registry-key' { $location.key }
        { $_ -in 'registry-value', 'value-edit' } { "$($location.key) : $($location.value)" }
        { $_ -in 'service', 'driver' } { $location.service }
        { $_ -in 'task', 'task-folder' } { $location.task }
        'package' { $location.package_full_name }
        default { $location.rule_id }
    }
    $why = ($item.evidence | ForEach-Object { $_.kind }) -join ', '
    return '{0,-12} {1,3}  {2,-14} {3}  [{4}]' -f $item.confidence, $item.score, $location.kind, $where, $why
}

# Folders that must never be reported, nor anything that contains them: every special folder .NET
# knows, every profile and the drive roots. Built here, independently of Avel.
$windows = [Environment]::GetFolderPath('Windows').TrimEnd('\')
$keep = [System.Collections.Generic.List[string]]::new()
foreach ($folder in [Enum]::GetValues([Environment+SpecialFolder])) {
    $path = [Environment]::GetFolderPath($folder)
    if ($path) { $keep.Add($path.TrimEnd('\')) }
}
Get-CimInstance Win32_UserProfile | ForEach-Object { $keep.Add($_.LocalPath.TrimEnd('\')) }
Get-PSDrive -PSProvider FileSystem | ForEach-Object { $keep.Add($_.Root.TrimEnd('\')) }

function Test-Under([string]$path, [string]$folder) {
    return $path -eq $folder -or $path.StartsWith("$folder\", [StringComparison]::OrdinalIgnoreCase)
}

function Check-Safe($items, [string]$label) {
    foreach ($item in $items) {
        $location = $item.location
        if ($location.kind -in 'folder', 'file') {
            $path = $location.path.TrimEnd('\')
            if (Test-Under $path $windows) { Fail "$($label): inside the Windows folder: $path" }
            foreach ($folder in $keep) {
                if (Test-Under $folder $path) { Fail "$($label): is or contains $($folder): $path" }
            }
        } elseif ($location.kind -in 'registry-key', 'registry-value') {
            if ($location.key -match '^HKLM\\SYSTEM\\' -or $location.key -notmatch '\\.+\\') {
                Fail "$($label): protected registry key: $($location.key)"
            }
        }
    }
}

