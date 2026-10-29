# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario trace). Phase 5a and 5b acceptance
# test for Avel Trace, the install tracker, and removing programs by their traces (docs/trace.md),
# checked against an independent oracle:
# avel_capture (tests/helpers/capture.cpp) captures every file on C: and the registry roots the
# tracker watches right before and right after each traced installation, and the diff of the two
# captures is what really changed. In rounds 0, 0b and 1 both captures are taken while the trace is
# open, so the trace must account for every line of the diff, unless its item demonstrably
# changed again inside the trace but outside the captures (Get-AgainReason); around `trace run`
# (rounds 2-4) they can't be, and a line is put down to the moments between a capture and the
# trace's own window (from its trace.json) only with evidence: its creation, last write or key's
# last write time lies there (for a gone key or value, that of the nearest key above it still
# there), or a watch of the volume's names saw it go away (or a created one appear) then. A created
# item the volume's journal has no record of between the captures is logged as KNOWN in any round.
#   U. the tracker's unit tests that need elevation (USN journal, installer watch, registry
#      snapshot) or the Sandbox's user profile;
#   0. idle: a minute with nothing but a stock app opened and closed (opened once before the trace
#      too, as a profile in use has long done): no change may be reported
#      that isn't noise, and the trace accounts for every change the oracle saw. What it doesn't
#      is logged as KNOWN in the later rounds;
#   0b. a container's base layer: one of Windows' files deleted and another renamed between
#      `trace start` and `trace stop`: exactly that deletion and rename, no tombstone as created;
#   1. "Contoso Trace Fixture" (trace_fixture.ps1) installed between `trace start` and `trace stop`:
#      completeness (every line of the oracle's diff is accounted for by a reported change of the
#      same kind), precision (every reported change that isn't noise is in the oracle's diff), and
#      each of the fixture's items and known values;
#   1 (5b). the trace linked to the fixture by itself and its items tied to it; the user adds their
#      own PATH entries; `force` reverts the fixture by its trace (Revert: the oracle's diff between
#      the capture before the install and the one after the removal holds exactly the KNOWN set,
#      Test-Revert; exact PATH, SharedDLLs and CONTOSO_HOME), and `undo` gives back exactly what was
#      there before the removal (Undo: state dumps, and the oracle's diff, Test-Undo, with a manual
#      trace around the removal and undo telling Windows' own activity apart). Then `trace export`,
#      `trace delete`, `trace import` and `trace link`; the fixture's own uninstaller removes its
#      folder and key, and `force --trace <the imported trace>` reverts the rest, judged the same way;
#   2-4. 7-Zip (MSI), Notepad++ (NSIS) and Inno Setup through `trace run`: it must end by itself
#      (installer-done) and be complete against the oracle; precision exceptions are logged;
#   2-4 (5b). `scan` previews each with its trace; `uninstall --quiet --remove-leftovers` runs its
#      uninstaller and removes what the trace saw the installer create, and Test-Revert judges what
#      is left against the capture before the install;
#   5. the fixture through `trace run`, starting its app at the end: done within 45 s, the app
#      listed as left running and still running;
#   6. a `trace run` killed after its snapshot is finished by `trace stop`; a USN journal deleted
#      and recreated during a trace makes it incomplete (C: "replaced"), the registry still reported.
# Rounds 1-5 keep what they traced in $traced (trace id, oracle captures, results), for the 5b
# rounds to compare against.
# Writes results\trace.json (avel-cli's output), results\trace.log, results\trace-oracle-<round>.txt
# (each round's oracle diff, every line with what accounted for it; for the 5b checks, what judged
# it) and, last, results\trace.txt.

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$installers = 'C:\Avel\sandbox\installers'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$scenario = 'trace'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\oracle.ps1"
. "$PSScriptRoot\trace_fixture.ps1"

# In Temp, which the tracker sets aside, since rounds 0 and 1 capture while their trace is open.
$captures = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Temp\AvelCaptures'  # Long names
$sid = $traceUserSid
$traced = [ordered]@{}
# How long Windows gets to finish reacting to a round (the Start menu to new shortcuts, ...)
# before the next capture.
$settleSeconds = 15

# The oracle's roots: C: and the registry the tracker watches (HKLM\SOFTWARE, the services, the
# user's Software and classes), plus the environment keys whose values it saves exactly.
$oracleSpecs = @(
    'files:C:\',
    'reg:HKLM\SOFTWARE',
    'reg:HKLM\SYSTEM\CurrentControlSet\Services',
    'reg:HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'reg:HKCU\Software',
    'reg:HKCU\Environment',
    "reg:HKU\$($sid)_Classes"
)
# The same roots as Avel prints them.
$oracleRoots = @('C:\', 'HKLM\SOFTWARE', 'HKLM\SYSTEM\CurrentControlSet\Services',
                 'HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment', "HKU\$sid\Software",
                 "HKU\$sid\Environment")

# The coarse exclude list, as data; each round logs how many lines every pattern took out.
# Never captured (avel_capture --exclude; a folder with everything in it): Windows' own working
# state, folders nobody can read, the mapped folders and the captures themselves.
$captureSkip = @(
    '^C:\\Windows\\Logs$',
    '^C:\\Windows\\Prefetch$',
    '^C:\\Windows\\Temp$',
    '^C:\\Windows\\SoftwareDistribution$',
    '^C:\\Windows\\WinSxS$',
    '^C:\\Windows\\servicing$',
    '^C:\\Windows\\System32\\config$',
    '^C:\\Windows\\System32\\winevt$',
    '^C:\\Windows\\System32\\LogFiles$',
    '^C:\\\$Recycle\.Bin$',
    '^C:\\System Volume Information$',
    '^C:\\Avel$',
    '^C:\\AvelTest$',
    ('^' + [regex]::Escape($captures) + '$'),
    '^C:\\(pagefile|swapfile|hiberfil)\.sys$',
    '^C:\\DumpStack\.log(\.tmp)?$'
)
# Captured, but left out of the comparison: what this harness and Avel write after a trace ends,
# and Windows' background activity, which goes on while the oracle captures.
$diffExclude = @(
    '^C:\\Users\\[^\\]+\\AppData\\Local\\Temp(\\|$)',
    '^C:\\ProgramData\\Avel(\\|$)',
    # Avel's quarantine of what a removal took, kept by the change set (docs/changes.md).
    '^C:\\\$Avel\.Quarantine(\\|$)',
    '^C:\\Users\\[^\\]+\\NTUSER\.DAT',
    '^C:\\Users\\[^\\]+\\AppData\\Local\\Microsoft\\Windows\\UsrClass\.dat',
    '^C:\\Windows\\ServiceProfiles\\[^\\]+\\NTUSER\.DAT',
    '^C:\\ProgramData\\Microsoft\\Windows Defender(\\|$)',
    '^C:\\ProgramData\\Microsoft\\Windows\\WER(\\|$)',
    '^C:\\ProgramData\\Microsoft\\Diagnosis(\\|$)',
    '^C:\\ProgramData\\Microsoft\\Network\\Downloader(\\|$)',
    '^C:\\ProgramData\\USO(Private|Shared)(\\|$)',
    '^C:\\Windows\\appcompat\\Programs(\\|$)',
    '^C:\\Windows\\System32\\sru(\\|$)',
    '^C:\\Windows\\System32\\wbem\\Repository(\\|$)',
    '^C:\\Windows\\System32\\catroot2(\\|$)',
    '^C:\\Windows\\ServiceState(\\|$)',
    # The shell's index of apps: a database it keeps open. The journal records each kind of change
    # to a file once per open, so writes to a file open and written since before the trace leave
    # no record in it (docs/trace.md, Limits); the tracker sets the folder aside as noise.
    '^C:\\Users\\[^\\]+\\AppData\\Local\\Packages\\MicrosoftWindows\.Client\.CBS_[^\\]+\\LocalState\\Search(\\|$)',
    # The same for the shell's WebView2 quota database and its journal (the folder is noise too).
    '^C:\\Users\\[^\\]+\\AppData\\Local\\Packages\\MicrosoftWindows\.Client\.CBS_[^\\]+\\LocalState\\EBWebView\\Default\\WebStorage\\QuotaManager(-journal)?$',
    # Not in the hive: Win32 makes up a language's performance counter names and help texts from
    # the perf?###.dat files, which the trace reports (as noise); the tracker reads the hive
    # itself (docs/trace.md, "Registry: the snapshot").
    '^HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Perflib\\(CurrentLanguage|[0-9a-f]{3})\|(Counter|Help)$',
    # Subtrees the tracker doesn't read either (docs/trace.md, "Registry: the snapshot").
    '^HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing(\\|\||$)',
    '^HKLM\\SYSTEM\\CurrentControlSet\\Services\\bam(\\|\||$)',
    '^HKLM\\SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces(\\|\||$)',
    '^HKLM\\SYSTEM\\CurrentControlSet\\Services\\NlaSvc\\Parameters\\Internet(\\|\||$)',
    '^HKLM\\SOFTWARE\\Microsoft\\Windows Defender(\\|\||$)',
    '^HKLM\\SOFTWARE\\Microsoft\\Cryptography\\RNG\|Seed$',
    '^HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tasks\\[^\\|]+\|DynamicInfo$',
    '^HKU\\[^\\]+\\Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Compatibility Assistant(\\|\||$)'
)
$diffExcludeRegex = [regex]::new('(?:' + ($diffExclude -join ')|(?:') + ')', 'IgnoreCase')

# Differences the trace doesn't account for that are logged as KNOWN instead of failing, as
# pattern = reason. Tune it from results\trace-oracle-<round>.txt.
$knownChanges = [ordered]@{
}

# Paths the oracle saw change in round 0 outside the trace's window: Windows' own activity.
$background = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)

$separators = [char[]]@('\', '|')
$taskCache = 'HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache'
$servicesKey = 'HKLM\SYSTEM\CurrentControlSet\Services'
$firewallRules = "$servicesKey\SharedAccess\Parameters\FirewallPolicy\FirewallRules"

# The same silent switches as Install-Sample in samples.ps1; the MSI is given to `trace run` as is.
# `entry`: the name of the sample's own Uninstall entry, which its trace's primary link must be.
$traceSamples = @(
    @{ label = 'r2-7zip'; name = '7-Zip (MSI)'; entry = '^7-Zip '; command = @("$installers\7z-x64.msi", '/qn', '/norestart') },
    @{ label = 'r3-notepadpp'; name = 'Notepad++ (NSIS)'; entry = '^Notepad\+\+ '
       command = @("$installers\npp-installer-x64.exe", '/S') },
    @{ label = 'r4-innosetup'; name = 'Inno Setup'; entry = '^Inno Setup '
       command = @("$installers\innosetup-x64.exe", '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/ALLUSERS') }
)

# ---------------------------------------------------------------------------------------------
# avel-cli

# Quotes an argument the way CommandLineToArgvW reads it back (Invoke-Cli joins with spaces).
function Format-Argument([string]$argument) {
    if ($argument -and $argument -notmatch '[\s"]') { return $argument }
    $escaped = [regex]::Replace($argument, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

# avel-cli trace <arguments>. Keeps its output for trace.json as plain strings: those from
# Get-Content -Raw carry note properties, which ConvertTo-Json would write as objects.
function Invoke-Trace([string[]]$arguments, [string]$label, [int]$timeoutSeconds = 600) {
    $result = Invoke-Cli (@('trace') + @($arguments | ForEach-Object { Format-Argument $_ })) $label $timeoutSeconds
    $raw[$label] = @{ exit = $result.exit; stdout = "$($result.text)"; stderr = "$($result.errors)" }
    return $result
}

# `trace list --json`, one trace object at a time (stdout only, see force.ps1).
function Get-TraceList {
    $text = (& $cli trace list --json) -join "`n"
    if ($LASTEXITCODE -ne 0 -or -not $text) { return }
    foreach ($trace in ($text | ConvertFrom-Json)) { $trace }
}

# Discards a trace a failed round left open, so the next round can start one.
function Close-OpenTrace {
    foreach ($trace in @(Get-TraceList | Where-Object { $_.state -in 'starting', 'open', 'stopping' })) {
        Log "  a trace is still $($trace.state) ($($trace.id)): cancelling it"
        $null = Invoke-Trace @('cancel') "cancel-$($trace.id)"
    }
}

# The full result (noise too) of a trace that `stop` or `run` printed.
function Get-AllChanges($result, [string]$label) {
    $id = $result.json.trace.id
    if (-not $id) { return $null }
    $show = Invoke-Trace @('show', $id, '--all', '--json') "$label-show-all"
    if (-not $show.json) { Fail "$($label): trace show --all printed no JSON (exit $($show.exit))"; return $result.json }
    return $show.json
}

function Invoke-Native([string]$file, [string]$arguments) {
    $out = Join-Path $env:TEMP 'avel-native.out'
    $process = Start-Process $file $arguments -Wait -PassThru -NoNewWindow -RedirectStandardOutput $out `
        -RedirectStandardError "$out.err"
    return $process.ExitCode
}

function Format-Change($change) {
    $text = '{0,-9} {1,-15} {2}' -f $change.change, $change.location.kind, (Where-Text $change.location)
    if ($change.old_path) { $text += " (was $($change.old_path))" }
    if ($change.contents) { $text += " [$($change.contents) inside]" }
    if ($change.replaced) { $text += ' [replaced]' }
    if ($change.noise -and $change.noise -ne 'none') { $text += " <$($change.noise)>" }
    return $text
}

function Write-TraceSummary($json) {
    $trace = $json.trace
    Log ('  trace {0}: {1}, mode {2}, stop reason {3}; {4} change(s), {5} noise; registry {6} keys, {7} values, snapshot {8} ms, diff {9} ms' -f
         $trace.id, $trace.state, $trace.mode, $trace.stop_reason, $trace.counts.changes, $trace.counts.noise,
         $trace.registry.keys, $trace.registry.values, $trace.registry.snapshot_ms, $trace.registry.diff_ms)
    foreach ($volume in @($trace.volumes | Where-Object { $_ })) {
        Log "    volume $($volume.root): $($volume.status)$(if ($volume.created_journal) { ' (journal created)' })"
    }
    foreach ($reason in @($trace.incomplete_reasons | Where-Object { $_ })) { Log "    incomplete: $($reason | ConvertTo-Json -Compress)" }
    if ($trace.watch) {
        $left = @($trace.watch.left_running | Where-Object { $_ } | ForEach-Object { "$($_.pid) $($_.image)" })
        Log "    watch: end $($trace.watch.end), exit code $($trace.watch.exit_code), reboot requested $($trace.watch.reboot_requested), left running: $($left -join ', ')"
    }
}

function Write-NoiseCounts($all) {
    $groups = @($all.changes | Where-Object { $_ -and $_.noise -ne 'none' } | Group-Object noise | Sort-Object Count -Descending)
    Log "  noise by reason: $(if ($groups) { @($groups | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', ' } else { 'none' })"
}

# ---------------------------------------------------------------------------------------------
# The oracle

# While a trace is open nothing is logged, the log being a file too: Write-Held keeps the lines
# until Stop-Hold logs them.
$held = $null
function Start-Hold { $script:held = [System.Collections.Generic.List[string]]::new() }
function Write-Held([string]$text) { if ($null -ne $script:held) { $script:held.Add($text) } else { Log $text } }
function Stop-Hold {
    $lines = $script:held
    $script:held = $null
    foreach ($text in @($lines)) { if ($null -ne $text) { Log $text } }
}

# Captures the oracle's roots into <captures>\<name>.txt, logging how long each root took and what
# the skip list left out. Returns the capture's path.
function New-OracleCapture([string]$name) {
    $path = Join-Path $captures "$name.txt"
    $summary = @(New-Capture $path $oracleSpecs $captureSkip)
    Write-Held "  oracle capture $($name): $(@($summary | Where-Object { $_ -like 'total:*' }) -replace '^total: ', '')"
    Write-Held "    $(@($summary | Where-Object { $_ -like 'spec *' } | ForEach-Object { $_ -replace '^spec ', '' }) -join '; ')"
    $skipped = @($summary | Where-Object { $_ -match '^excluded [1-9]' } | ForEach-Object { $_ -replace '^excluded ', '' })
    if ($skipped) { Write-Held "    not captured (lines, pattern): $($skipped -join '; ')" }
    return $path
}

function Get-FileTimeNow { return [DateTime]::UtcNow.ToFileTimeUtc() }

function Format-FileTime([long]$time) { return [DateTime]::FromFileTimeUtc($time).ToString('HH:mm:ss.fff') }

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

public static class AvelKeyTime {
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    static extern int RegOpenKeyExW(IntPtr key, string subKey, int options, int access, out IntPtr result);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    static extern int RegQueryInfoKeyW(IntPtr key, IntPtr className, IntPtr classLength, IntPtr reserved,
        IntPtr subKeys, IntPtr maxSubKeyLength, IntPtr maxClassLength, IntPtr values, IntPtr maxValueNameLength,
        IntPtr maxValueLength, IntPtr securityDescriptor, out long lastWriteTime);
    [DllImport("advapi32.dll")] static extern int RegCloseKey(IntPtr key);

    const int KEY_QUERY_VALUE = 0x0001, KEY_WOW64_64KEY = 0x0100;

    // The last write time (FILETIME, UTC) of HKLM\<subKey> or HKU\<subKey>, or 0.
    public static long LastWrite(bool machine, string subKey) {
        IntPtr root = new IntPtr(machine ? unchecked((int)0x80000002) : unchecked((int)0x80000003));
        IntPtr key;
        if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, out key) != 0) return 0;
        try {
            long time;
            return RegQueryInfoKeyW(key, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero,
                IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, out time) == 0 ? time : 0;
        } finally {
            RegCloseKey(key);
        }
    }
}

// A volume's USN journal and its items' ids, as fsutil's CSV spells them, for evidence that
// nothing on the volume touched an item.
public static class AvelJournal {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition,
        uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr device, uint code, IntPtr input, int inputSize,
        out UsnJournalData output, int outputSize, out int returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(IntPtr file, out FileInformation information);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);

    [StructLayout(LayoutKind.Sequential)]
    struct UsnJournalData {
        public ulong JournalId; public long FirstUsn; public long NextUsn; public long LowestValidUsn;
        public long MaxUsn; public ulong MaximumSize; public ulong AllocationDelta;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct FileInformation {
        public uint Attributes; public uint CreatedLow, CreatedHigh, AccessedLow, AccessedHigh, WrittenLow, WrittenHigh;
        public uint VolumeSerial, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }

    const uint GENERIC_READ = 0x80000000, FILE_READ_ATTRIBUTES = 0x80, SHARE_ALL = 7, OPEN_EXISTING = 3;
    const uint FLAG_BACKUP_SEMANTICS = 0x02000000, FLAG_OPEN_REPARSE_POINT = 0x00200000;
    const uint FSCTL_QUERY_USN_JOURNAL = 0x000900F4;

    // The journal of `volume` ("C:"): its id, first and next USN; null if it can't be read.
    public static long[] Query(string volume) {
        IntPtr handle = CreateFileW(@"\\.\" + volume, GENERIC_READ, 3, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (handle == new IntPtr(-1)) return null;
        try {
            UsnJournalData data;
            int returned;
            if (!DeviceIoControl(handle, FSCTL_QUERY_USN_JOURNAL, IntPtr.Zero, 0, out data,
                    Marshal.SizeOf(typeof(UsnJournalData)), out returned, IntPtr.Zero)) return null;
            return new long[] { unchecked((long)data.JournalId), data.FirstUsn, data.NextUsn };
        } finally {
            CloseHandle(handle);
        }
    }

    // The id of the item at `path` (32 hex digits), or null.
    public static string IdOf(string path) {
        IntPtr handle = CreateFileW(@"\\?\" + path, FILE_READ_ATTRIBUTES, SHARE_ALL, IntPtr.Zero, OPEN_EXISTING,
            FLAG_BACKUP_SEMANTICS | FLAG_OPEN_REPARSE_POINT, IntPtr.Zero);
        if (handle == new IntPtr(-1)) return null;
        try {
            FileInformation information;
            if (!GetFileInformationByHandle(handle, out information)) return null;
            return string.Format("0000000000000000{0:x8}{1:x8}", information.IndexHigh, information.IndexLow);
        } finally {
            CloseHandle(handle);
        }
    }
}

// Notes when names below a folder go away (deleted, or renamed away) and appear (created, or
// renamed or moved in): the evidence for a deletion between a capture and the trace, which no time
// on disk shows, and for a file moved in, which keeps its creation time. Only names, so it is
// light; if its buffer overflows, what was lost is simply not evidence. A folder renamed or moved
// as a whole is noted by its own name only, not by those of what it holds.
public sealed class AvelNameWatch : IDisposable {
    readonly FileSystemWatcher watcher;
    readonly List<KeyValuePair<long, string>> removed = new List<KeyValuePair<long, string>>();
    readonly List<KeyValuePair<long, string>> added = new List<KeyValuePair<long, string>>();
    public bool Overflowed;

    public AvelNameWatch(string root) {
        watcher = new FileSystemWatcher(root);
        watcher.IncludeSubdirectories = true;
        watcher.NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName;
        watcher.InternalBufferSize = 65536;
        watcher.Created += (sender, e) => Add(added, e.FullPath);
        watcher.Deleted += (sender, e) => Add(removed, e.FullPath);
        watcher.Renamed += (sender, e) => { Add(removed, e.OldFullPath); Add(added, e.FullPath); };
        watcher.Error += (sender, e) => { Overflowed = true; };
        watcher.EnableRaisingEvents = true;
    }

    static void Add(List<KeyValuePair<long, string>> list, string path) {
        long now = DateTime.UtcNow.ToFileTimeUtc();
        lock (list) { list.Add(new KeyValuePair<long, string>(now, path)); }
    }

    static long Find(List<KeyValuePair<long, string>> list, string path, long from, long to) {
        long found = 0;
        lock (list) {
            foreach (var entry in list) {
                if (entry.Key >= from && entry.Key <= to &&
                    string.Equals(entry.Value, path, StringComparison.OrdinalIgnoreCase)) {
                    found = entry.Key;
                }
            }
        }
        return found;
    }

    // When `path` was last noted going away (appearing) between `from` and `to` (FILETIMEs, UTC),
    // or 0.
    public long RemovedAt(string path, long from, long to) { return Find(removed, path, from, to); }
    public long AddedAt(string path, long from, long to) { return Find(added, path, from, to); }

    // Whether `path` was noted going away or appearing at `from` or later.
    public bool NotedSince(string path, long from) {
        return Find(removed, path, from, long.MaxValue) != 0 || Find(added, path, from, long.MaxValue) != 0;
    }

    public void Dispose() { watcher.Dispose(); }
}
'@

# When the item of an oracle line was created or last written, as far as it shows (FILETIME, UTC;
# 0 when unknown): a created file's or folder's creation time, a modified file's last write time
# in the capture after, and for a created or modified key or value its key's last write time.
# Nothing of its own tells when something was deleted (see Get-NearestKeyWrite for keys and values).
function Get-ChangeTime($line) {
    try {
        if ($line.kind -in 'F', 'D') {
            if ($line.sign -eq '+') {
                $time = if ($line.kind -eq 'D') { [IO.Directory]::GetCreationTimeUtc($line.path) } else { [IO.File]::GetCreationTimeUtc($line.path) }
                return $time.ToFileTimeUtc()
            }
            if ($line.sign -eq '~' -and $line.kind -eq 'F') {
                $fields = $line.attributes -split '\s+'
                if ($fields.Count -ge 2 -and $fields[1] -match '^[0-9a-fA-F]+$') { return [Convert]::ToInt64($fields[1], 16) }
            }
            return 0
        }
        if (($line.kind -in 'K', 'V') -and ($line.sign -in '+', '~')) {
            return Get-KeyLastWrite $(if ($line.kind -eq 'V') { $line.path.Substring(0, $line.path.IndexOf('|')) } else { $line.path })
        }
    } catch { }
    return 0
}

# The last write time (FILETIME, UTC) of the key an oracle path names (HKLM\... or HKU\...), or 0.
function Get-KeyLastWrite([string]$key) {
    if ($key -match '^HKLM\\(.+)$') { return [AvelKeyTime]::LastWrite($true, $Matches[1]) }
    if ($key -match '^HKU\\(.+)$') { return [AvelKeyTime]::LastWrite($false, $Matches[1]) }
    return 0
}

# For a gone key or value (an oracle path, "<key>" or "<key>|<value name>"), the nearest key above
# it that is there now (a value's own key, when it is) and that key's last write time (FILETIME,
# UTC), or $null: deleting a subkey or a value writes the key that held it, so that time is when
# the deletion happened unless the key was written again since, as a created key's own last write
# time is for its creation.
function Get-NearestKeyWrite([string]$path) {
    $bar = $path.IndexOf('|')
    $key = if ($bar -ge 0) { $path.Substring(0, $bar) } else { Split-Path $path }
    while ($key -match '^(HKLM|HKU)\\.') {
        if (Test-RegistryItem $key) {
            $time = Get-KeyLastWrite $key
            if ($time -le 0) { return $null }
            return @{ key = $key; time = $time }
        }
        $key = Split-Path $key
    }
    return $null
}

# Whether the key or value an oracle path names ("<key>" or "<key>|<value name>") is there now.
function Test-RegistryItem([string]$path) {
    $bar = $path.IndexOf('|')
    $key = if ($bar -ge 0) { $path.Substring(0, $bar) } else { $path }
    if ($key -notmatch '^(HKLM|HKU)\\(.+)$') { return $false }
    $hive = if ($Matches[1] -eq 'HKLM') { [Microsoft.Win32.RegistryHive]::LocalMachine } else { [Microsoft.Win32.RegistryHive]::Users }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $opened = $base.OpenSubKey($Matches[2])
        if (-not $opened) { return $false }
        try {
            return $bar -lt 0 -or (@($opened.GetValueNames()) -contains $path.Substring($bar + 1))
        } finally {
            $opened.Dispose()
        }
    } catch {
        return $false
    } finally {
        $base.Dispose()
    }
}

# The folder above an oracle line's item that `gap`'s name watch saw go away (-Removed) or appear,
# and when (@{ path; time }), or $null: a folder renamed or moved as a whole takes what it holds
# along, and the watch notes only the folder's own name. Only when the watch didn't overflow and,
# from then on, noted nothing more of the folder, nor of the item or a folder between them.
function Find-MovedFolder($line, $gap, [switch]$Removed) {
    $watch = $gap.watch
    if (-not $watch -or $watch.Overflowed) { return $null }
    $path = $line.path
    $i = $path.LastIndexOf([char]'\')
    while ($i -gt 2) {
        $folder = $path.Substring(0, $i)
        $time = if ($Removed) { $watch.RemovedAt($folder, $gap.from, $gap.to) } else { $watch.AddedAt($folder, $gap.from, $gap.to) }
        if ($time -gt 0) {
            if ($watch.NotedSince($folder, $time + 1)) { return $null }
            for ($j = $path.Length; $j -gt $i; $j = $path.LastIndexOf([char]'\', $j - 1)) {
                if ($watch.NotedSince($path.Substring(0, $j), $time)) { return $null }
            }
            return @{ path = $folder; time = $time }
        }
        $i = $path.LastIndexOf([char]'\', $i - 1)
    }
    return $null
}

# Why an oracle line is outside the trace's window, or $null: its change time lies in one of
# `gaps`, the moments between a capture and the trace (@{ from; to; keysTo; what; watch },
# FILETIMEs; keysTo, when there, ends the gap for keys and values), or, for a file or folder, the
# gap's name watch saw it go away (deleted) or appear (created: one moved in keeps its creation
# time) then, by itself or with a folder above it (see Find-MovedFolder). For a gone key or value,
# the last write time of the nearest key above it that is still there (see Get-NearestKeyWrite;
# run 1: a user's MuiCache\1656 deleted as 1657 replaced it, seconds after 7-Zip's trace stopped).
function Get-GapReason($line, $gaps) {
    if (-not $gaps) { return $null }
    $file = $line.kind -in 'F', 'D'
    if ($line.sign -eq '-') {
        if (-not $file) {
            if ($line.kind -notin 'K', 'V') { return $null }
            $above = Get-NearestKeyWrite $line.path
            if (-not $above) { return $null }
            foreach ($gap in $gaps) {
                $to = if ($gap.keysTo) { $gap.keysTo } else { $gap.to }
                if ($above.time -ge $gap.from -and $above.time -le $to) {
                    return "changed $($gap.what) (gone; $($above.key), the nearest key above it that is still there, was last written at $(Format-FileTime $above.time) UTC, between $(Format-FileTime $gap.from) and $(Format-FileTime $to))"
                }
            }
            return $null
        }
        foreach ($gap in $gaps) {
            $time = if ($gap.watch) { $gap.watch.RemovedAt($line.path, $gap.from, $gap.to) } else { 0 }
            if ($time -gt 0) {
                return "changed $($gap.what) (seen going away at $(Format-FileTime $time) UTC, between $(Format-FileTime $gap.from) and $(Format-FileTime $gap.to))"
            }
        }
        foreach ($gap in $gaps) {
            $moved = Find-MovedFolder $line $gap -Removed
            if ($moved) {
                return "changed $($gap.what) (seen going away with its folder $($moved.path) at $(Format-FileTime $moved.time) UTC, between $(Format-FileTime $gap.from) and $(Format-FileTime $gap.to))"
            }
        }
        return $null
    }
    $time = Get-ChangeTime $line
    foreach ($gap in $gaps) {
        $to = if (-not $file -and $gap.keysTo) { $gap.keysTo } else { $gap.to }
        if ($time -gt 0 -and $time -ge $gap.from -and $time -le $to) {
            $what = if (-not $file) { 'its key was last written' } elseif ($line.sign -eq '+') { 'created' } else { 'last written' }
            return "changed $($gap.what) ($what at $(Format-FileTime $time) UTC, between $(Format-FileTime $gap.from) and $(Format-FileTime $to))"
        }
    }
    if ($file -and $line.sign -eq '+') {
        foreach ($gap in $gaps) {
            $seen = if ($gap.watch) { $gap.watch.AddedAt($line.path, $gap.from, $gap.to) } else { 0 }
            if ($seen -gt 0) {
                return "changed $($gap.what) (seen appearing at $(Format-FileTime $seen) UTC, between $(Format-FileTime $gap.from) and $(Format-FileTime $gap.to))"
            }
        }
        foreach ($gap in $gaps) {
            $moved = Find-MovedFolder $line $gap
            if ($moved) {
                return "changed $($gap.what) (seen appearing with its folder $($moved.path) at $(Format-FileTime $moved.time) UTC, between $(Format-FileTime $gap.from) and $(Format-FileTime $gap.to))"
            }
        }
    }
    return $null
}

# What an uncovered line's evidence says, for the log: when its item was created or last written.
function Get-ChangeNote($line) {
    if ($line.sign -eq '-' -and $line.kind -in 'K', 'V') {
        $above = Get-NearestKeyWrite $line.path
        if ($above) { return " (gone; $($above.key) above it last written at $(Format-FileTime $above.time) UTC)" }
        return ''
    }
    if ($line.sign -eq '-' -or $line.kind -notin 'F', 'D', 'K', 'V') { return '' }
    $time = Get-ChangeTime $line
    if ($time -le 0) { return '' }
    $what = if ($line.kind -in 'K', 'V') { 'its key last written' } elseif ($line.sign -eq '+') { 'created' } else { 'last written' }
    return " ($what at $(Format-FileTime $time) UTC)"
}

# When the trace looked (FILETIMEs, UTC): at the journals from `trace_start` (in the trace's own
# trace.json) until it stopped, at the registry from the end of its snapshot (at least snapshot_ms
# later; Install and trace launches the installer after it) until it compared it, after it stopped.
# The stop is known to the second only, so the window after the trace starts at the second it
# stopped in: Install and trace stops once nothing of the installer has run for seconds and the
# disk has been quiet (docs/trace.md), so that second holds nothing of it. Without the file, it is
# avel-cli's run. For the log, `usns` names the USNs it read C:'s journal from and up to.
function Get-TraceWindow([string]$id, [long]$runStart, [long]$runEnd) {
    $window = @{ start = $runStart; keys = $runStart; stop = $runEnd; usns = $null }
    try {
        $info = Get-Content -Raw -LiteralPath (Join-Path $env:ProgramData "Avel\Traces\$id\trace.json") | ConvertFrom-Json
        $volume = @($info.volumes | Where-Object { $_.root -eq 'C:\' })[0]
        if ($volume) { $window.usns = "USN $($volume.start_usn) to $($volume.next_usn)" }
        if ([long]$info.trace_start -gt $runStart) {
            $window.start = [long]$info.trace_start
            $window.keys = $window.start + [long]$info.registry.snapshot_ms * 10000
        }
        if ($null -ne $info.stopped_unix) {
            $stop = [DateTimeOffset]::FromUnixTimeSeconds([long]$info.stopped_unix).UtcDateTime.ToFileTimeUtc()
            if ($stop -gt $window.start -and $stop -le $runEnd) { $window.stop = $stop }
        }
    } catch {
        Log "  the trace's own trace.json can't be read ($($_.Exception.Message)): its window is avel-cli's run"
    }
    return $window
}

# What a trace whose captures are inside it saw outside them (see Get-AgainReason): the trace's
# journal window on C: (its journal's id, and the USNs it read from and up to, from its own
# trace.json), the journal's next USN before the capture before (`before`, from
# [AvelJournal]::Query) and before the capture after (`post`), and when the capture after began and
# the stop ended (FILETIMEs). $null without the trace's window.
function New-InsideWindow([string]$id, $before, $post, [long]$postTime, [long]$stopTime) {
    try {
        $info = Get-Content -Raw -LiteralPath (Join-Path $env:ProgramData "Avel\Traces\$id\trace.json") | ConvertFrom-Json
        $volume = @($info.volumes | Where-Object { $_.root -eq 'C:\' -and $_.status -eq 'ok' })[0]
    } catch {
        Log "  the trace's own trace.json can't be read ($($_.Exception.Message))"
        return $null
    }
    if (-not $volume -or -not $before -or -not $post) { return $null }
    $journal = [long]$volume.journal_id
    if ($before[0] -ne $journal -or $post[0] -ne $journal) { return $null }
    $window = @{ journal = $journal; start = [long]$volume.start_usn; stop = [long]$volume.next_usn; pre = $before[2]
                 post = $post[2]; postTime = $postTime; stopTime = $stopTime }
    Log "  the trace's journal: USN $($window.start) to $($window.stop); the capture before from $($window.pre), the one after from $($window.post)"
    return $window
}

# A path as the oracle spells it: services below CurrentControlSet, a user's classes below
# Software\Classes.
function ConvertTo-OraclePath([string]$path) {
    $path = $path -replace '^HKLM\\SYSTEM\\ControlSet00[0-9]\\', 'HKLM\SYSTEM\CurrentControlSet\'
    return $path -replace '^HKU\\([^\\]+)_Classes(?=\\|$)', 'HKU\$1\Software\Classes'
}

# A scheduled task stands for its key in the TaskCache's tree, its entries there by id and its file
# below System32\Tasks; for a new task folder, also the folder's key and folder.
function Get-TaskTargets([string]$task) {
    $targets = @(@{ path = "$taskCache\Tree$task"; container = $true },
                 @{ path = "C:\Windows\System32\Tasks$task"; container = $false })
    $tree = Get-Item -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tree$task" `
        -ErrorAction SilentlyContinue
    # The Task Scheduler writes the id with two NULs at the end; GetValue drops only one.
    $id = if ($tree) { "$($tree.GetValue('Id'))".TrimEnd([char]0) } else { '' }
    if ($tree -and -not $id) { Write-Held "    task $($task): no Id in TaskCache\Tree, its entries by id can't be named" }
    if ($id) {
        foreach ($list in 'Tasks', 'Plain', 'Boot', 'Logon', 'Maintenance') { $targets += @{ path = "$taskCache\$list\$id"; container = $true } }
    }
    $folder = $task.Substring(0, [Math]::Max(0, $task.LastIndexOf('\')))
    while ($folder) {
        $targets += @{ path = "$taskCache\Tree$folder"; container = $true }
        $targets += @{ path = "C:\Windows\System32\Tasks$folder"; container = $true }
        $folder = $folder.Substring(0, [Math]::Max(0, $folder.LastIndexOf('\')))
    }
    return $targets
}

# What a reported location stands for in the oracle: paths, each a container (accounting for
# everything below it) or not. The first is the item's own.
function Get-OracleTargets($location) {
    switch ($location.kind) {
        'folder' { return @{ path = $location.path; container = $true } }
        'file' { return @{ path = $location.path; container = $false } }
        'registry-key' { return @{ path = (ConvertTo-OraclePath $location.key); container = $true } }
        'registry-value' { return @{ path = (ConvertTo-OraclePath $location.key) + '|' + $location.value; container = $false } }
        { $_ -in 'service', 'driver' } { return @{ path = "$servicesKey\$($location.service)"; container = $true } }
        'firewall-rule' { return @{ path = "$firewallRules|$($location.rule_id)"; container = $false } }
        'task' { return Get-TaskTargets $location.task }
        'task-folder' {
            return @(@{ path = "$taskCache\Tree$($location.task)"; container = $true },
                     @{ path = "C:\Windows\System32\Tasks$($location.task)"; container = $true })
        }
    }
}

function New-PathMap { New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase) }

function Add-Cover($sets, [string]$path, [bool]$container, [string]$noise) {
    if (-not $path) { return }
    if (-not $noise) { $noise = 'none' }
    $map = if ($container) { $sets.container } else { $sets.exact }
    $old = $null
    if (-not $map.TryGetValue($path, [ref]$old) -or $old -ne 'none') { $map[$path] = $noise }
}

# What a trace's items account for in the oracle's diff, by sign ("+" created, "~" modified, "-"
# deleted): exact paths, and containers that also account for everything below them, each with
# the noise reason of the item ("none" wins). Known values account for their value.
function New-Coverage($json) {
    $cover = @{}
    foreach ($sign in '+', '~', '-') { $cover[$sign] = @{ exact = New-PathMap; container = New-PathMap } }
    $signs = @{ created = '+'; deleted = '-'; modified = '~'; renamed = '+' }
    foreach ($item in @($json.changes | Where-Object { $_ })) {
        $sign = $signs[$item.change]
        if (-not $sign) { continue }
        foreach ($target in @(Get-OracleTargets $item.location)) {
            Add-Cover $cover[$sign] $target.path $target.container $item.noise
        }
        if ($item.change -eq 'renamed' -and $item.old_path) {
            Add-Cover $cover['-'] $item.old_path ($item.location.kind -eq 'folder') $item.noise
        }
    }
    foreach ($value in @($json.known_values | Where-Object { $_ })) {
        $sign = if ($null -eq $value.before) { '+' } elseif ($null -eq $value.after) { '-' } else { '~' }
        Add-Cover $cover[$sign] ((ConvertTo-OraclePath $value.key) + '|' + $value.name) $false 'none'
    }
    return $cover
}

# The noise reason of what accounts for `path`, or $null.
function Find-Cover($sets, [string]$path) {
    $noise = $null
    if ($sets.exact.TryGetValue($path, [ref]$noise) -or $sets.container.TryGetValue($path, [ref]$noise)) { return $noise }
    $i = $path.IndexOfAny($separators)
    while ($i -ge 0) {
        if ($sets.container.TryGetValue($path.Substring(0, $i), [ref]$noise)) { return $noise }
        $i = $path.IndexOfAny($separators, $i + 1)
    }
    return $null
}

function Test-UnderOracle([string]$path) {
    foreach ($root in $oracleRoots) {
        if ($path.StartsWith($root, 'OrdinalIgnoreCase') -and
            ($path.Length -eq $root.Length -or $root.EndsWith('\') -or "$($path[$root.Length])" -in '\', '|')) { return $true }
    }
    return $false
}

# The pattern that leaves `path` out of the oracle's comparison: a skip pattern matching it or a
# folder or key above it, or a diff pattern matching it. $null if none does.
function Get-ExclusionOf([string]$path) {
    $candidate = $path
    while ($candidate) {
        foreach ($pattern in $captureSkip) { if ($candidate -match $pattern) { return $pattern } }
        $i = $candidate.LastIndexOfAny($separators)
        $candidate = if ($i -gt 0) { $candidate.Substring(0, $i) } else { '' }
    }
    foreach ($pattern in $diffExclude) { if ($path -match $pattern) { return $pattern } }
    return $null
}

# Whether the oracle can read the folder or key that holds `path` (or the nearest one above it that
# exists). Captures note what they can't read as "!" lines, which a diff doesn't show while they
# stay unreadable.
function Test-OracleCanRead([string]$path) {
    $i = $path.LastIndexOfAny($separators)
    $parent = if ($i -gt 0) { $path.Substring(0, $i) } else { $path }
    try {
        if ($parent -match '^[A-Za-z]:') {
            while ($parent.Length -gt 3 -and -not [IO.Directory]::Exists($parent)) { $parent = Split-Path $parent }
            if ($parent.Length -le 2) { $parent += '\' }
            $null = [IO.Directory]::EnumerateFileSystemEntries($parent).GetEnumerator().MoveNext()
            return $true
        }
        $hive = if ($parent -match '^HKLM(\\|$)') { [Microsoft.Win32.Registry]::LocalMachine } else { [Microsoft.Win32.Registry]::Users }
        $subkey = $parent -replace '^(HKLM|HKU)\\?', ''
        while ($subkey) {
            $key = $hive.OpenSubKey($subkey)
            if ($key) { $null = $key.GetSubKeyNames(); $key.Close(); return $true }
            $subkey = $subkey.Substring(0, [Math]::Max(0, $subkey.LastIndexOf('\')))
        }
        return $true
    } catch {
        return $false
    }
}

# Completeness: every line of the oracle's diff that the exclude list leaves in must be accounted
# for by a reported change of the same kind (noise counts): "+" by created (or a known value that
# didn't exist), "~" by modified (or a known value), "-" by deleted or renamed. A line whose change
# time lies in one of `gaps` (see Get-GapReason) is logged as KNOWN instead, and so is one whose
# item changed again inside the trace but outside the captures (`inside`, see Get-AgainReason) and a
# created or gone file or folder the volume's journal has no record of between the captures
# (`journal`, see Get-JournalSilence). Writes results\trace-oracle-<label>.txt with what accounted for each line.
# -Calibrate (round 0): what nothing accounts for fails too, and is remembered: later rounds log it
# as KNOWN.
function Test-Completeness($lines, $all, [string]$label, [switch]$Calibrate, $Gaps = @(), $Journal = $null,
                           $Inside = $null) {
    $journalState = @{}
    $cover = New-Coverage $all
    $others = @{ '+' = @('~', '-'); '~' = @('+', '-'); '-' = @('+', '~') }
    $names = @{ '+' = 'created'; '~' = 'modified'; '-' = 'deleted' }
    $excludedBy = [ordered]@{}
    $coveredBy = [ordered]@{}
    $knownLines = [System.Collections.Generic.List[string]]::new()
    $problems = [System.Collections.Generic.List[string]]::new()
    $problemFiles = [System.Collections.Generic.List[object]]::new()
    $detail = [System.Collections.Generic.List[string]]::new()
    $unreadable = 0
    foreach ($line in $lines) {
        if ($diffExcludeRegex.IsMatch($line.path)) {
            $pattern = @($diffExclude | Where-Object { $line.path -match $_ })[0]
            $excludedBy[$pattern] = 1 + [int]$excludedBy[$pattern]
            $detail.Add("excluded $pattern`t$($line.text)")
            continue
        }
        $noise = Find-Cover $cover[$line.sign] $line.path
        if ($noise) {
            $coveredBy[$noise] = 1 + [int]$coveredBy[$noise]
            $detail.Add("covered ($noise)`t$($line.text)")
            continue
        }
        $instead = @($others[$line.sign] | Where-Object { Find-Cover $cover[$_] $line.path })
        $reason = Get-KnownReason $line.path $knownChanges
        # Outside the trace's window, whatever the trace says about the item.
        $gap = Get-GapReason $line $Gaps
        if (-not $gap) { $gap = Get-AgainReason $line $Inside $journalState }
        if ($gap) {
            $class = "KNOWN: $gap"
        } elseif ($instead) {
            $class = "MISMATCH: reported as $($names[$instead[0]])"
        } elseif ($background.Contains($line.path)) {
            $class = 'KNOWN: not accounted for while idle either'
        } elseif ($reason) {
            $class = "KNOWN: $reason"
        } elseif ($line.kind -eq '!') {
            $class = 'unreadable'
        } elseif ($silence = Get-JournalSilence $line $Journal $journalState) {
            $class = "KNOWN: $silence"
        } else {
            $class = 'UNCOVERED'
            if ($Calibrate) { $null = $background.Add($line.path) }
        }
        $detail.Add("$class`t$($line.text)")
        if ($class.StartsWith('KNOWN')) { $knownLines.Add("$($class): $($line.text)") }
        elseif ($class -eq 'unreadable') { $unreadable++ }
        else {
            $note = if ($Gaps -and $class -eq 'UNCOVERED') { Get-ChangeNote $line } else { '' }
            $problems.Add("$($class): $($line.text)$note")
            if ($line.kind -in 'F', 'D') { $problemFiles.Add($line) }
        }
    }
    [IO.File]::WriteAllLines((Join-Path $results "trace-oracle-$label.txt"), [string[]]$detail, [Text.UTF8Encoding]::new($false))

    $covered = 0
    foreach ($count in $coveredBy.Values) { $covered += $count }
    $excluded = 0
    foreach ($count in $excludedBy.Values) { $excluded += $count }
    $byNoise = @($coveredBy.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', '
    Log ("  oracle: {0} difference(s): {1} accounted for by the trace{2}, {3} excluded, {4} known, {5} unreadable, {6} not accounted for" -f
         @($lines).Count, $covered, $(if ($byNoise) { " (by noise reason: $byNoise)" }), $excluded, $knownLines.Count, $unreadable,
         $problems.Count)
    if ($excluded) {
        Log "    excluded from the comparison (lines, pattern): $(@($excludedBy.GetEnumerator() | ForEach-Object { "$($_.Value) $($_.Key)" }) -join '; ')"
    }
    $shown = 0
    foreach ($text in $knownLines) {
        if ($shown++ -ge 100) { Log "    ... and $($knownLines.Count - 100) more (see trace-oracle-$label.txt)"; break }
        Log "    $text"
    }
    if ($problems.Count -eq 0) { Log "  OK: complete: the trace accounts for every change the oracle saw"; return }
    Fail "$($label): incomplete: $($problems.Count) change(s) the oracle saw aren't reported as such"
    $shown = 0
    foreach ($text in $problems) {
        if ($shown++ -ge 300) { Log "      ... and $($problems.Count - 300) more (see trace-oracle-$label.txt)"; break }
        Log "      $text"
    }
    Write-ItemRecords @($problemFiles | Select-Object -First 5) $journalState
}

# The volume's USN journal as fsutil reads it (its CSV: USN, name, name length, reason, time,
# attributes, id, parent id), raw: `byId` (id to its records, in journal order) and `idsByName`
# (name to the ids of the items that had it). $null if it can't be read.
function Read-JournalRecords {
    try {
        $records = @(& fsutil.exe usn readjournal C: csv)  # stdout only (see Invoke-UnitTests)
    } catch {
        Log "    fsutil usn readjournal failed: $($_.Exception.Message)"
        return $null
    }
    $fields = [regex]'^\d+,"([^"]*)",\d+,0x[0-9a-fA-F]+,"[^"]*","[^"]*",0x[0-9a-fA-F]+,"[^"]*",([0-9a-fA-F]{32}),'
    $byId = @{}
    $idsByName = @{}
    foreach ($text in $records) {
        $match = $fields.Match($text)
        if (-not $match.Success) { continue }
        $name = $match.Groups[1].Value
        $id = $match.Groups[2].Value
        $list = $byId[$id]
        if ($null -eq $list) { $list = [Collections.Generic.List[string]]::new(); $byId[$id] = $list }
        $list.Add($text)
        $ids = $idsByName[$name]
        if ($null -eq $ids) { $ids = [Collections.Generic.List[string]]::new(); $idsByName[$name] = $ids }
        if (-not $ids.Contains($id)) { $ids.Add($id) }
    }
    return @{ byId = $byId; idsByName = $idsByName }
}

# The journal's records, read once per `state` (a hashtable), with the journal's state then.
function Get-JournalRecords($state) {
    if (-not $state.Contains('records')) {
        $state.records = Read-JournalRecords
        $state.now = [AvelJournal]::Query('C:')
    }
    return $state.records
}

# Why nothing on the volume made a created or gone file or folder of an oracle line what it is, or
# $null: the journal has no record between `window.before` and `window.after` (its id, first and
# next USN before the capture before and after the capture after) of the item's id, when it is
# there now; when it isn't (gone, or created and gone again), of its name in its folder, nor of that
# folder or one above it being created, deleted or renamed. And it still has every record since
# then. A container's base-layer item can show in one capture's listing and not in the other's that
# way: the Sandbox's base layer is the host's own system files, which no record on the volume tells
# of.
function Get-JournalSilence($line, $window, $state) {
    if (-not $window -or -not $window.before -or -not $window.after -or $line.sign -notin '+', '-' -or
        $line.kind -notin 'F', 'D') { return $null }
    $id = [AvelJournal]::IdOf($line.path)
    $folders = @()
    if (-not $id) {
        for ($folder = Split-Path $line.path; $folder.Length -gt 3; $folder = Split-Path $folder) {
            $folderId = [AvelJournal]::IdOf($folder)
            if (-not $folderId) { return $null }
            $folders += $folderId
        }
        if (-not $folders) { return $null }
    }
    $journal = Get-JournalRecords $state
    $now = $state.now
    $from = $window.before[2]
    $to = $window.after[2]
    if (-not $journal -or -not $now -or $now[0] -ne $window.before[0] -or $window.after[0] -ne $window.before[0] -or
        $now[1] -gt $from) { return $null }
    $did = if ($line.sign -eq '+') { 'made it' } else { 'took it away' }
    if ($id) {
        foreach ($text in @($journal.byId[$id])) {
            if (-not $text) { continue }
            $usn = [long]$text.Substring(0, $text.IndexOf(','))
            if ($usn -ge $from -and $usn -lt $to) { return $null }
        }
        return "no record of it (id $id) in the volume's journal between the captures (USN $from to $to): nothing on the volume $did"
    }
    $fields = [regex]'^(\d+),"([^"]*)",\d+,0x([0-9a-fA-F]+),"[^"]*","[^"]*",0x[0-9a-fA-F]+,"[^"]*",[0-9a-fA-F]{32},([0-9a-fA-F]{32}),'
    $name = Split-Path $line.path -Leaf
    foreach ($named in @($journal.idsByName[$name] | Where-Object { $_ })) {
        foreach ($text in $journal.byId[$named]) {
            $match = $fields.Match($text)
            if (-not $match.Success -or $match.Groups[2].Value -ne $name -or $match.Groups[4].Value -ne $folders[0]) { continue }
            $usn = [long]$match.Groups[1].Value
            if ($usn -ge $from -and $usn -lt $to) { return $null }
        }
    }
    $moves = 0x100 -bor 0x200 -bor 0x1000 -bor 0x2000  # FILE_CREATE, FILE_DELETE, RENAME_OLD_NAME, RENAME_NEW_NAME
    foreach ($folderId in $folders) {
        foreach ($text in @($journal.byId[$folderId] | Where-Object { $_ })) {
            $match = $fields.Match($text)
            if (-not $match.Success) { continue }
            $usn = [long]$match.Groups[1].Value
            if ($usn -ge $from -and $usn -lt $to -and ([Convert]::ToInt64($match.Groups[3].Value, 16) -band $moves)) { return $null }
        }
    }
    return "not there now, and no record of its name in its folder (id $($folders[0])) in the volume's journal between the captures (USN $from to $to), nor of that folder or one above it moving: nothing on the volume $did"
}

# Why an oracle line of a round whose captures are inside the trace (`inside`, see
# New-InsideWindow) isn't what the trace saw, or $null: its item changed again inside the trace,
# after the capture after or before the capture before, with evidence. For a file or folder, a
# journal record of its name in its folder: after the capture after, one that takes the name away
# ("+" or "~", the item gone now) or brings it back ("-", the item there now); before the capture
# before, one that creates it ("-", gone now) or takes it away ("+", there now). For a key or value
# after the capture after, gone now ("+") or back ("-"), and its key (a key's parent) last written
# between the capture after and the stop.
function Get-AgainReason($line, $inside, $state) {
    if (-not $inside -or $line.sign -notin '+', '~', '-') { return $null }
    if ($line.kind -in 'K', 'V') {
        if ($line.sign -eq '~') { return $null }
        $there = Test-RegistryItem $line.path
        if (($line.sign -eq '+') -eq $there) { return $null }
        $key = if ($line.kind -eq 'V') { $line.path.Substring(0, $line.path.IndexOf('|')) } else { $line.path }
        if (($line.kind -eq 'K' -and $line.sign -eq '+') -or -not (Test-RegistryItem $key)) { $key = Split-Path $key }
        $time = Get-KeyLastWrite $key
        if ($time -lt $inside.postTime -or $time -gt $inside.stopTime) { return $null }
        $what = if ($there) { 'is back' } else { 'is gone' }
        return "changed again inside the trace after the capture after: it $what, $key last written at $(Format-FileTime $time) UTC"
    }
    if ($line.kind -notin 'F', 'D') { return $null }
    $there = Test-Path -LiteralPath $line.path
    $checks = @()
    $adds = 0x100 -bor 0x2000     # FILE_CREATE, RENAME_NEW_NAME
    $removes = 0x200 -bor 0x1000  # FILE_DELETE, RENAME_OLD_NAME
    if ($line.sign -in '+', '~' -and -not $there) {
        $checks += @{ from = $inside.post; to = $inside.stop; reasons = $removes; what = 'after the capture after: its name went away' }
    }
    if ($line.sign -eq '-' -and $there) {
        $checks += @{ from = $inside.post; to = $inside.stop; reasons = $adds; what = 'after the capture after: its name came back' }
    }
    if ($line.sign -eq '-' -and -not $there) {
        $checks += @{ from = $inside.start; to = $inside.pre; reasons = $adds; what = 'before the capture before: it was created' }
    }
    if ($line.sign -eq '+' -and $there) {
        $checks += @{ from = $inside.start; to = $inside.pre; reasons = $removes; what = 'before the capture before: its name went away' }
    }
    if (-not $checks) { return $null }
    $folder = [AvelJournal]::IdOf((Split-Path $line.path))
    $journal = Get-JournalRecords $state
    if (-not $folder -or -not $journal -or -not $state.now -or $state.now[0] -ne $inside.journal -or
        $state.now[1] -gt $inside.start) { return $null }
    $name = Split-Path $line.path -Leaf
    $fields = [regex]'^(\d+),"([^"]*)",\d+,0x([0-9a-fA-F]+),"([^"]*)","[^"]*",0x[0-9a-fA-F]+,"[^"]*",[0-9a-fA-F]{32},([0-9a-fA-F]{32}),'
    foreach ($id in @($journal.idsByName[$name] | Where-Object { $_ })) {
        foreach ($text in $journal.byId[$id]) {
            $match = $fields.Match($text)
            if (-not $match.Success -or $match.Groups[2].Value -ne $name -or $match.Groups[5].Value -ne $folder) { continue }
            $usn = [long]$match.Groups[1].Value
            $reason = [Convert]::ToInt64($match.Groups[3].Value, 16)
            foreach ($check in $checks) {
                if ($usn -ge $check.from -and $usn -lt $check.to -and ($reason -band $check.reasons)) {
                    return "changed again inside the trace $($check.what) (USN $($usn): $($match.Groups[4].Value))"
                }
            }
        }
    }
    return $null
}

# How an item looks to programs now, for the log.
function Get-ItemFacts([string]$path) {
    $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    if (-not $item) { return 'not there now' }
    return "created $(Format-FileTime $item.CreationTimeUtc.ToFileTimeUtc()) UTC, written $(Format-FileTime $item.LastWriteTimeUtc.ToFileTimeUtc()) UTC, attributes $($item.Attributes)"
}

# Diagnostics: for the files and folders of `lines`, how they look to programs now, and every
# journal record of the item and of its folder by their ids (of the items that had its name, when
# it is gone; of the folders those items' records name, when its folder is gone too), raw.
function Write-ItemRecords($lines, $state) {
    if (-not @($lines).Count) { return }
    $journal = Get-JournalRecords $state
    if (-not $journal) { return }
    $parent = [regex]'^\d+,"[^"]*",\d+,0x[0-9a-fA-F]+,"[^"]*","[^"]*",0x[0-9a-fA-F]+,"[^"]*",[0-9a-fA-F]{32},([0-9a-fA-F]{32}),'
    foreach ($line in $lines) {
        $id = [AvelJournal]::IdOf($line.path)
        $folder = Split-Path $line.path
        $folderId = [AvelJournal]::IdOf($folder)
        Log "    $($line.path): $(Get-ItemFacts $line.path), id $id; its folder: id $folderId"
        $ids = [Collections.Generic.List[string]]::new()
        if ($id) {
            $ids.Add($id)
        } elseif ($null -ne $journal.idsByName[(Split-Path $line.path -Leaf)]) {
            $leaf = Split-Path $line.path -Leaf
            $named = @($journal.idsByName[$leaf])
            # Those whose records name its folder, when its folder is there.
            $inFolder = @(if ($folderId) {
                $named | Where-Object { @($journal.byId[$_] | Where-Object { $_ -and $parent.Match($_).Groups[1].Value -eq $folderId }).Count }
            })
            if ($inFolder) {
                Log "      gone: the journal's items named $leaf in its folder: $($inFolder.Count) of $($named.Count) (the last 10 shown)"
                $named = $inFolder
            } elseif ($folderId) {
                Log "      gone: the journal's items named $($leaf): $($named.Count), none in its folder (the last 10 shown)"
            } else {
                Log "      gone: the journal's items named $($leaf): $($named.Count) (the last 10 shown)"
            }
            foreach ($namedId in @($named | Select-Object -Last 10)) { $ids.Add($namedId) }
        }
        if ($folderId) {
            $ids.Add($folderId)
        } else {
            $folders = @(foreach ($item in @($ids)) {
                foreach ($text in @($journal.byId[$item] | Where-Object { $_ })) {
                    $match = $parent.Match($text)
                    if ($match.Success) { $match.Groups[1].Value }
                }
            }) | Select-Object -Unique
            if ($folders) { Log "      its folder gone too: the folders its records name: $(@($folders).Count)" }
            foreach ($item in @($folders | Select-Object -Last 5)) { if (-not $ids.Contains($item)) { $ids.Add($item) } }
        }
        foreach ($item in $ids) {
            $records = @($journal.byId[$item] | Where-Object { $_ })
            $found = @($records | Select-Object -Last 20)
            Log "      item $($item), its last $($found.Count) record(s) of $($records.Count):"
            foreach ($text in $found) { Log "        $text" }
        }
    }
}

# Precision: every reported change that isn't noise must be in the oracle's diff: created where it
# has "+", deleted "-", modified "~" (a modified folder or key: any line at or below it), renamed
# "+" and "-" at the old path. Items outside the oracle's roots or under its exclusions are logged.
# Services, tasks and firewall rules are checked by the round itself, so they are only logged.
# -Strict: an exception fails; otherwise it is logged.
function Test-Precision($json, $lines, [string]$label, [switch]$Strict) {
    $signs = New-PathMap
    $anyBelow = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $plusBelow = New-Object 'System.Collections.Generic.Dictionary[string,int]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in $lines) {
        $old = $null
        $null = $signs.TryGetValue($line.path, [ref]$old)
        $signs[$line.path] = "$old$($line.sign)"
        $i = $line.path.IndexOfAny($separators)
        while ($i -ge 0) {
            $prefix = $line.path.Substring(0, $i)
            $null = $anyBelow.Add($prefix)
            if ($line.sign -eq '+') {
                $count = 0
                $null = $plusBelow.TryGetValue($prefix, [ref]$count)
                $plusBelow[$prefix] = $count + 1
            }
            $i = $line.path.IndexOfAny($separators, $i + 1)
        }
    }
    $expected = @{ created = '+'; deleted = '-'; modified = '~'; renamed = '+' }
    $checked = 0
    $exceptions = 0
    foreach ($item in @($json.changes | Where-Object { $_ -and $_.noise -eq 'none' })) {
        $text = Format-Change $item
        $targets = @(Get-OracleTargets $item.location)
        if (-not $targets -or -not $expected[$item.change]) { Log "    not checked ($($item.location.kind)): $text"; continue }
        $path = $targets[0].path
        if (-not (Test-UnderOracle $path)) { Log "    outside the oracle's roots: $text"; continue }
        $exclusion = Get-ExclusionOf $path
        if ($exclusion) { Log "    under the oracle's exclusion $($exclusion): $text"; continue }
        $checked++
        $found = $null
        $null = $signs.TryGetValue($path, [ref]$found)
        if ($item.change -eq 'modified' -and $targets[0].container) {
            $ok = [bool]$found -or $anyBelow.Contains($path)
        } else {
            $ok = [bool]$found -and $found.Contains($expected[$item.change])
        }
        if ($ok -and $item.change -eq 'renamed' -and $item.old_path) {
            $was = $null
            $null = $signs.TryGetValue($item.old_path, [ref]$was)
            $ok = [bool]$was -and $was.Contains('-')
        }
        if ($ok) {
            if ($item.change -eq 'created' -and $targets[0].container -and $null -ne $item.contents) {
                $count = 0
                $null = $plusBelow.TryGetValue($path, [ref]$count)
                if ($count -ne $item.contents) { Log "    contents: $text; the oracle has $count line(s) below it" }
            }
            continue
        }
        if (-not $found -and -not (Test-OracleCanRead $path)) { Log "    in a folder or key the oracle can't read: $text"; continue }
        $exceptions++
        $why = if ($found) { "the oracle has '$found' there" } else { 'the oracle saw no change there' }
        if ($item.location.kind -in 'service', 'driver', 'task', 'task-folder', 'firewall-rule') {
            Log "    $($item.location.kind), checked separately: $text ($why)"
        } elseif ($Strict) {
            Fail "$($label): imprecise: $text ($why)"
        } else {
            Log "    precision exception: $text ($why)"
        }
    }
    Log "  precision: $checked reported change(s) checked against the oracle, $exceptions exception(s)"
}

# ---------------------------------------------------------------------------------------------
# The fixture's expectations

function ConvertFrom-Hex([string]$hex) {
    $bytes = New-Object byte[] ([int]($hex.Length / 2))
    for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [Convert]::ToByte($hex.Substring(2 * $i, 2), 16) }
    return , $bytes
}

function Get-RegistryText($value) {
    if ($null -eq $value) { return $null }
    return [Text.Encoding]::Unicode.GetString((ConvertFrom-Hex $value.data)).TrimEnd([char]0)
}

# The known values against Get-TraceFixtureKnownValues; `paths`: the PATH values read right before
# the trace (Get-TraceFixturePaths).
function Test-KnownValues($values, [string]$label, $paths) {
    $values = @($values | Where-Object { $_ })
    foreach ($value in $values) {
        Log "    known value: $($value.spot) $($value.key) : $($value.name): $(if ($value.before) { "type $($value.before.type)" } else { 'none' }) -> $(if ($value.after) { "type $($value.after.type)" } else { 'none' })"
    }
    foreach ($want in @(Get-TraceFixtureKnownValues)) {
        $what = "known value $($want.spot) $($want.key) : $($want.name)"
        $found = @($values | Where-Object { $_.spot -eq $want.spot -and $_.name -eq $want.name -and (ConvertTo-OraclePath $_.key) -eq $want.key })
        if ($found.Count -ne 1) { Fail "$($label): $what is listed $($found.Count) time(s)"; continue }
        $value = $found[0]
        $problems = @()
        if ($want.Contains('append')) {
            $before = $paths[$want.spot]
            $after = if (-not $before) { $want.append } elseif ($before.EndsWith(';')) { $before + $want.append } else { "$before;$($want.append)" }
            if ($null -eq $before) {
                if ($null -ne $value.before) { $problems += 'a value before, though there was none' }
            } elseif ($null -eq $value.before -or "$($value.before_text)".TrimEnd([char]0) -cne $before) {
                $problems += "before_text isn't the value before the trace"
            }
            if ($null -eq $value.after -or "$($value.after_text)".TrimEnd([char]0) -cne $after) {
                $problems += "after_text isn't the value before the trace plus ;$($want.append)"
            }
            foreach ($side in 'before', 'after') {
                $data = $value.$side
                if ($null -eq $data) { continue }
                if ($data.type -ne 2) { $problems += "$side is type $($data.type), not REG_EXPAND_SZ" }
                if ((Get-RegistryText $data) -cne "$($value."$($side)_text")".TrimEnd([char]0)) { $problems += "$side data and text differ" }
            }
        } else {
            if ($null -eq $want.before) {
                if ($null -ne $value.before) { $problems += 'a value before, though there was none' }
            } elseif ($null -eq $value.before -or $value.before.data -ne $want.before) {
                $problems += "before is $($value.before.data), not $($want.before)"
            }
            if ($want.Contains('after_text')) {
                if ("$($value.after_text)".TrimEnd([char]0) -cne $want.after_text) { $problems += "after_text is '$($value.after_text)'" }
            } elseif ($null -eq $value.after -or $value.after.data -ne $want.after) {
                $problems += "after is $($value.after.data), not $($want.after)"
            }
        }
        if ($problems) { Fail "$($label): $($what): $($problems -join '; ')" }
        else { Log "  OK: $what" }
    }
}

# The trace of the fixture's install, item by item (Get-TraceFixtureItems), and its known values.
function Test-FixtureTrace($json, [string]$label, $paths) {
    $changes = @($json.changes | Where-Object { $_ })
    $ok = 0
    $rule = Get-TraceFixtureRuleId
    foreach ($want in @(Get-TraceFixtureItems)) {
        $where = if ($want.kind -eq 'firewall-rule') { $rule } else { $want.where }
        $what = "$($want.change) $($want.kind) $where"
        $found = @($changes | Where-Object { $_.change -eq $want.change -and $_.location.kind -eq $want.kind -and (Where-Text $_.location) -eq $where })
        if ($found.Count -eq 1) { $ok++; continue }
        if ($found.Count -gt 1) { Fail "$($label): reported $($found.Count) times: $what"; continue }
        $near = @($changes | Where-Object { $text = Where-Text $_.location; $text -and ($text -eq $where -or $where.StartsWith("$text\", 'OrdinalIgnoreCase') -or $text.StartsWith("$where\", 'OrdinalIgnoreCase')) })
        Fail "$($label): not reported: $what$(if ($near) { '; reported instead: ' + (@($near | ForEach-Object { Format-Change $_ }) -join '; ') })"
    }
    Log "  $ok of $(@(Get-TraceFixtureItems).Count) expected items reported as expected"
    $folder = @($changes | Where-Object { $_.location.kind -eq 'folder' -and $_.location.path -eq $traceRoot })
    if ($folder.Count -eq 1 -and -not ($folder[0].contents -gt 0)) { Fail "$($label): the program's folder has contents $($folder[0].contents)" }
    $inside = @($changes | Where-Object { $_.location.kind -in 'folder', 'file' -and $_.location.path.StartsWith("$traceRoot\", 'OrdinalIgnoreCase') })
    if ($inside) { Fail "$($label): the program's folder isn't one item: $(@($inside | ForEach-Object { Format-Change $_ }) -join '; ')" }
    elseif ($folder.Count -eq 1) { Log "  OK: the program's folder is one item with $($folder[0].contents) inside" }
    Test-KnownValues $json.known_values $label $paths
}

# ---------------------------------------------------------------------------------------------
# 5b: removing by the trace (docs/trace.md, "Uninstalling by the trace")

# The user's own PATH entries, added after the fixture's trace: a removal by the trace must keep
# them, and the rest of the value byte for byte. The user's own is unexpanded, as setx leaves it.
$userPathEntries = [ordered]@{ 'machine-environment' = 'C:\Tools\bin'; 'user-environment' = '%USERPROFILE%\bin' }

# Where a removal never touches what an installer created: Windows' folder and keys, shared names
# and packaged apps' places. Restated here from docs/leftovers.md ("Protection"), so a created item
# the scan didn't offer is put down to protection only when it lies in one of them.
$protectedFolderNames = @('Microsoft', 'Microsoft Games', 'Temp', 'Programs', 'Common', 'Common Files', 'Clients',
    'Downloads', 'Desktop', 'Internet Explorer', 'Windows', 'Windows NT', 'Windows Photo Viewer', 'Windows Mail',
    'Windows Defender', 'Windows Media Player', 'Uninstall Information', 'Reference Assemblies',
    'InstallShield Installation Information', 'Installer', 'winsxs', 'WindowsApps', 'DirectX', 'DirectXRedist',
    'Package Cache')
$protectedKeyNames = @('Microsoft', 'Wow6432Node', 'Windows', 'Classes', 'Clients', 'RegisteredApplications')
$volumeSystemFolders = @('System Volume Information', '$Recycle.Bin', 'Recovery', 'Config.Msi', '$WinREAgent',
    '$Windows.~BT', '$Windows.~WS', '$SysReset', 'Boot', 'EFI')
$protectedSubtrees = @(
    @{ path = "$env:ProgramFiles\WindowsApps"; what = "packaged apps' folder" },
    @{ path = "$env:ProgramData\Package Cache"; what = "WiX Burn's package cache" },
    @{ path = "$env:ProgramData\Avel"; what = "Avel's own data" })
# Keys Windows relies on below SOFTWARE (a user's Software too), and the keys above them; "*" is any
# one key, so Windows Installer's Products key stays while a product's key below it may go.
$coreKeyPatterns = @('Classes\CLSID', 'Classes\Interface', 'Classes\TypeLib', 'Classes\AppID',
    'Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache', 'Microsoft\Windows\CurrentVersion\Run',
    'Microsoft\Windows\CurrentVersion\RunOnce', 'Microsoft\Windows\CurrentVersion\Uninstall',
    'Microsoft\Windows\CurrentVersion\App Paths', 'Microsoft\Windows\CurrentVersion\SharedDLLs',
    'Microsoft\Windows\CurrentVersion\Installer\*', 'Microsoft\Windows\CurrentVersion\Installer\UserData\*\*',
    'Microsoft\Windows\CurrentVersion\Installer\Managed\*\Installer\*', 'Classes\Installer\*', 'Microsoft\Installer\*',
    'Microsoft\Windows\CurrentVersion\Shell Extensions\Approved', 'Microsoft\Windows\CurrentVersion\Policies',
    'Microsoft\Windows NT\CurrentVersion', 'Policies', 'RegisteredApplications')
# What Windows keeps for a registered package below SOFTWARE (a user's Software too), restated from
# docs/leftovers.md ("package-registration"), and the machine's ProgramData\Packages: it goes only
# with its package, never as keys or folders of their own (Notepad++'s sparse package, run 2).
$packageKeyPatterns = @('Classes\PackagedCom', 'Classes\ActivatableClasses', 'Classes\Extensions\ContractId',
    'Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel',
    'Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppContainer',
    'Microsoft\Windows\CurrentVersion\AppModel', 'Microsoft\Windows\CurrentVersion\Appx',
    'Microsoft\Windows\CurrentVersion\AppHost\IndexedDB', 'Microsoft\Windows\CurrentVersion\BackgroundAccessApplications',
    'Microsoft\SecurityManager\CapAuthz')

# avel-cli <arguments>, each quoted as needed, its output kept for trace.json as plain strings.
function Invoke-Avel([string[]]$arguments, [string]$label, [int]$timeoutSeconds = 600) {
    $result = Invoke-Cli @($arguments | ForEach-Object { Format-Argument $_ }) $label $timeoutSeconds
    $raw[$label] = @{ exit = $result.exit; stdout = "$($result.text)"; stderr = "$($result.errors)" }
    return $result
}

# Whether the key `parts` (its names below SOFTWARE) is one `pattern` describes, or above one.
function Test-LeadsTo([string[]]$parts, [string[]]$pattern) {
    if ($parts.Count -gt $pattern.Count) { return $false }
    for ($i = 0; $i -lt $parts.Count; $i++) {
        if ($pattern[$i] -ne '*' -and $pattern[$i] -ne $parts[$i]) { return $false }
    }
    return $true
}

# Why a removal never touches a key (HKLM\... or HKU\<sid>\...), or with -Value a value in it; or $null.
function Get-ProtectedKey([string]$key, [switch]$Value) {
    if ($key -match '^HKLM\\(SYSTEM|SECURITY|SAM|HARDWARE|BCD00000000|COMPONENTS|DRIVERS|SCHEMA)(\\|$)') {
        return "in the system hive $($Matches[1])"
    }
    if ($key -match '^HKLM\\SOFTWARE\\(WOW6432Node\\)?Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache(\\|$)') {
        return "the Task Scheduler's own records"
    }
    if ($Value) { return $null }  # A value in a key Windows relies on (a Run entry) is fine on its own
    $relative = (ConvertTo-OraclePath $key) -replace '^HKLM(\\|$)', '' -replace '^HKU\\[^\\]+(\\|$)', ''
    $parts = @($relative -split '\\' | Where-Object { $_ })
    if ($parts.Count -le 1) { return "a hive's top-level key" }
    if ($parts[0] -eq 'SOFTWARE') {
        $rest = @($parts | Select-Object -Skip 1)
        if ($rest[0] -eq 'WOW6432Node') {
            $rest = @($rest | Select-Object -Skip 1)
        } elseif ($rest.Count -ge 2 -and $rest[0] -eq 'Classes' -and $rest[1] -eq 'WOW6432Node') {
            $rest = @('Classes') + @($rest | Select-Object -Skip 2)
        }
        if (-not $rest.Count) { return 'a key Windows relies on (SOFTWARE)' }
        foreach ($pattern in $coreKeyPatterns) {
            if (Test-LeadsTo $rest ($pattern -split '\\')) { return "a key Windows relies on ($pattern)" }
        }
    }
    if ($protectedKeyNames -contains $parts[-1]) { return "named like a shared key ($($parts[-1]))" }
    return $null
}

# Why a removal never touches a file or folder, or $null.
function Get-ProtectedFolder([string]$path) {
    $path = $path.TrimEnd('\')
    if (Test-Under $path $windows) { return 'in the Windows folder' }
    if ($path -match '^[A-Za-z]:\\([^\\]+)' -and $volumeSystemFolders -contains $Matches[1]) { return "in a volume's system folder ($($Matches[1]))" }
    foreach ($subtree in $protectedSubtrees) { if (Test-Under $path $subtree.path) { return "in $($subtree.what)" } }
    if ($path -match '^[A-Za-z]:\\Users\\[^\\]+\\AppData\\Local\\Packages(\\|$)') { return "in packaged apps' data" }
    foreach ($folder in $keep) { if (Test-Under $folder $path) { return "is or holds the special folder $folder" } }
    $name = Split-Path $path -Leaf
    $stem = if ($name.Contains('.')) { $name.Substring(0, $name.LastIndexOf('.')) } else { $name }
    if ($protectedFolderNames -contains $name -or $protectedFolderNames -contains $stem) { return "named like a shared folder ($name)" }
    return $null
}

# Why a removal never touches a reported location, or $null.
function Get-ProtectedPlace($location) {
    switch ($location.kind) {
        { $_ -in 'folder', 'file' } { return Get-ProtectedFolder $location.path }
        'registry-key' { return Get-ProtectedKey $location.key }
        'registry-value' { return Get-ProtectedKey $location.key -Value }
        { $_ -in 'task', 'task-folder' } { if ($location.task -match '^\\Microsoft(\\|$)') { return "Windows' own task" } }
    }
    return $null
}

# The same for what an oracle line names.
function Get-ProtectedLine($line) {
    switch ($line.kind) {
        { $_ -in 'D', 'F' } { return Get-ProtectedFolder $line.path }
        'K' { return Get-ProtectedKey $line.path }
        'V' { return Get-ProtectedKey $line.path.Substring(0, $line.path.IndexOf('|')) -Value }
    }
    return $null
}

# Which of $packageKeyPatterns (or ProgramData\Packages) a reported location is a package's
# registration in: at or below it, or above it (as the product guards it); $null if none.
function Get-PackageRegistrationPlace($location) {
    if ($location.kind -in 'folder', 'file') {
        if (Test-Under $location.path.TrimEnd('\') (Join-Path $env:ProgramData 'Packages')) { return 'ProgramData\Packages' }
        return $null
    }
    if ($location.kind -notin 'registry-key', 'registry-value') { return $null }
    $relative = (ConvertTo-OraclePath $location.key) -replace '^HKLM(\\|$)', '' -replace '^HKU\\[^\\]+(\\|$)', ''
    $parts = @($relative -split '\\' | Where-Object { $_ })
    if ($parts.Count -lt 2 -or $parts[0] -ne 'SOFTWARE') { return $null }
    $rest = @($parts | Select-Object -Skip 1)
    if ($rest[0] -eq 'WOW6432Node') {
        $rest = @($rest | Select-Object -Skip 1)
    } elseif ($rest.Count -ge 2 -and $rest[0] -eq 'Classes' -and $rest[1] -eq 'WOW6432Node') {
        $rest = @('Classes') + @($rest | Select-Object -Skip 2)
    }
    foreach ($pattern in $packageKeyPatterns) {
        $want = $pattern -split '\\'
        $under = $rest.Count -ge $want.Count
        for ($i = 0; $under -and $i -lt $want.Count; $i++) { $under = $rest[$i] -eq $want[$i] }
        if ($under -or ($location.kind -eq 'registry-key' -and (Test-LeadsTo $rest $want))) { return $pattern }
    }
    return $null
}

# The packages a removal's scan offered (`json`: uninstall's or force's), by full name:
# @{ full; family; user; offered (its level); state and reason (its plan item's, if planned) }.
function Get-RoundPackages($json) {
    $packages = [ordered]@{}
    $scan = if ($json.leftover_scan) { $json.leftover_scan } else { $json }
    foreach ($item in @(@($scan.leftovers) + @($json.leftover_removal.items) | Where-Object { $_ -and $_.location.kind -eq 'package' })) {
        $full = "$($item.location.package_full_name)"
        if (-not $full) { continue }
        if (-not $packages.Contains($full)) {
            $fields = $full -split '_'  # Name_Version_Architecture_ResourceId_PublisherId
            $packages[$full] = @{ full = $full; family = "$($fields[0])_$($fields[-1])"; user = "$($item.location.user_sid)"
                                  offered = $item.confidence; state = $null; reason = $null; registered = $null }
        }
        if ($item.state) { $packages[$full].state = $item.state; $packages[$full].reason = $item.reason }
    }
    return $packages
}

# Whether `package` (see Get-RoundPackages) is registered now: in its user's package repository, or
# for any user as Get-AppxPackage sees it.
function Test-PackageRegistered($package) {
    $user = if ($package.user) { $package.user } else { $traceUserSid }
    if (Test-RegistryItem "HKU\$user\Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository\Packages\$($package.full)") {
        return $true
    }
    try {
        return @(Get-AppxPackage -AllUsers -ErrorAction Stop | Where-Object { $_.PackageFullName -eq $package.full }).Count -gt 0
    } catch {
        return $false
    }
}

# Whether the registration at a reported location, still there, is `package`'s: its path names the
# package (its full or family name, in any case), or a key or folder below it does, or, for an
# AppContainer mapping, its Moniker value is the package's family.
function Test-NamesPackage($location, $package) {
    $where = Where-Text $location
    foreach ($name in $package.full, $package.family) { if ($where.IndexOf($name, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return $true } }
    $path = if ($location.kind -in 'folder', 'file') { $location.path } else {
        (ConvertTo-OraclePath $location.key) -replace '^HKLM\\', 'Registry::HKEY_LOCAL_MACHINE\' -replace '^HKU\\', 'Registry::HKEY_USERS\'
    }
    try {
        foreach ($child in @(Get-ChildItem -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue)) {
            foreach ($name in $package.full, $package.family) { if ("$($child.PSChildName)" -eq $name) { return $true } }
        }
        if ($location.kind -eq 'registry-key') {
            $moniker = (Get-ItemProperty -LiteralPath $path -Name Moniker -ErrorAction SilentlyContinue).Moniker
            if ($moniker -and $moniker -eq $package.family) { return $true }
        }
    } catch { }
    return $false
}

function New-ItemMap { New-Object 'System.Collections.Generic.Dictionary[string,object]' ([StringComparer]::OrdinalIgnoreCase) }

function Add-Item($sets, [string]$path, [bool]$container, $item) {
    if (-not $path) { return }
    $map = if ($container) { $sets.container } else { $sets.exact }
    if (-not $map.ContainsKey($path)) { $map[$path] = $item }
}

# A trace's change, or one of its known values (as @{ change = 'known'; known }), as one line of text
# and as a key that names it.
function Format-Item($item) {
    if ($item.change -eq 'known') { return "known value $($item.known.spot) $($item.known.key) : $($item.known.name)" }
    return Format-Change $item
}

function Get-ItemKey($item) {
    if ($item.change -eq 'known') { return "known|$(ConvertTo-OraclePath $item.known.key)|$($item.known.name)" }
    return "$($item.change)|$($item.location.kind)|$(Where-Text $item.location)"
}

# What the install's own changes (not noise) and known values stand for in the oracle, by sign, as
# New-Coverage, with the item itself. A known value stands for its value whatever the sign.
function New-ItemCoverage($json) {
    $cover = @{}
    foreach ($sign in '+', '~', '-') { $cover[$sign] = @{ exact = New-ItemMap; container = New-ItemMap } }
    $signs = @{ created = '+'; deleted = '-'; modified = '~'; renamed = '+' }
    foreach ($item in @($json.changes | Where-Object { $_ -and $_.noise -eq 'none' })) {
        $sign = $signs[$item.change]
        if (-not $sign) { continue }
        foreach ($target in @(Get-OracleTargets $item.location)) {
            if ($target) { Add-Item $cover[$sign] $target.path $target.container $item }
        }
        if ($item.change -eq 'renamed' -and $item.old_path) {
            Add-Item $cover['-'] $item.old_path ($item.location.kind -eq 'folder') $item
        }
    }
    foreach ($value in @($json.known_values | Where-Object { $_ })) {
        $known = [pscustomobject]@{ change = 'known'; known = $value }
        foreach ($sign in '+', '~', '-') { Add-Item $cover[$sign] ((ConvertTo-OraclePath $value.key) + '|' + $value.name) $false $known }
    }
    return $cover
}

# Reported items (a scan's leftovers, a removal's items) by "<kind>|<location>".
function New-LocationMap($items) {
    $map = New-ItemMap
    foreach ($item in @($items | Where-Object { $_ -and $_.location })) {
        $key = "$($item.location.kind)|$(Where-Text $item.location)"
        if (-not $map.ContainsKey($key)) { $map[$key] = $item }
    }
    return , $map
}

# The item of `map` at a location given by its kind and text, or else the folder or key that holds
# it: @{ item; container (bool) }, or $null.
function Find-LocationItem($map, [string]$kind, [string]$where) {
    $found = $null
    if ($map.TryGetValue("$kind|$where", [ref]$found)) { return @{ item = $found; container = $false } }
    $parentKind = $null
    $path = $where
    if ($kind -in 'folder', 'file') {
        $parentKind = 'folder'
    } elseif ($kind -in 'registry-key', 'registry-value') {
        $parentKind = 'registry-key'
        if ($kind -eq 'registry-value') {
            $path = $where.Substring(0, [Math]::Max(0, $where.LastIndexOf(' : ')))
            if ($map.TryGetValue("registry-key|$path", [ref]$found)) { return @{ item = $found; container = $true } }
        }
    }
    if (-not $parentKind) { return $null }
    for ($i = $path.LastIndexOf('\'); $i -gt 2; $i = $path.LastIndexOf('\', $i - 1)) {
        if ($map.TryGetValue("$parentKind|$($path.Substring(0, $i))", [ref]$found)) { return @{ item = $found; container = $true } }
    }
    return $null
}

# What a removal changed, done or partly done, as the oracle spells it: its items' own targets (a
# task its key in TaskCache\Tree and its file, not the folders above), a value edit its value.
function New-Footprint($removal) {
    $sets = @{ exact = New-ItemMap; container = New-ItemMap }
    foreach ($item in @($removal.items | Where-Object { $_ -and $_.state -in 'done', 'partial' })) {
        $location = $item.location
        $what = "$($location.kind) $(Where-Text $location)"
        if ($location.kind -eq 'value-edit') {
            Add-Item $sets ((ConvertTo-OraclePath $location.key) + '|' + $location.value) $false $what
        } elseif ($location.kind -eq 'task') {
            Add-Item $sets "$taskCache\Tree$($location.task)" $true $what
            Add-Item $sets "C:\Windows\System32\Tasks$($location.task)" $false $what
        } else {
            foreach ($target in @(Get-OracleTargets $location)) { if ($target) { Add-Item $sets $target.path $target.container $what } }
        }
    }
    return $sets
}

# What became of an item the installer created, changed or deleted that the oracle still sees
# changed after a removal by the trace: @{ class = KNOWN or FAIL; reason; protected }.
function Get-RevertVerdict($item, $context) {
    if ($item.change -eq 'known') {
        $value = $item.known
        if ($context.paths -and $value.name -eq 'Path' -and $context.paths.Contains($value.spot)) {
            return $context.paths[$value.spot].Clone()
        }
        $basis = @($context.use.values | Where-Object {
            $_ -and (ConvertTo-OraclePath $_.key) -eq (ConvertTo-OraclePath $value.key) -and $_.name -eq $value.name
        } | ForEach-Object { $_.basis })[0]
        return @{ class = 'FAIL'; reason = "the installer's change to it wasn't taken back (basis: $(if ($basis) { $basis } else { 'not reported' }))" }
    }
    $location = $item.location
    $where = Where-Text $location
    $own = $context.fixture -and $context.fixture.items.Contains((Get-ItemKey $item))
    if ($item.change -ne 'created') {
        return @{ class = 'KNOWN'; reason = "it existed before the install ($($item.change)); a removal by the trace never offers it (shown only)" }
    }
    $found = Find-LocationItem $context.scan $location.kind $where
    if ($location.kind -eq 'firewall-rule') {
        if ($found) { return @{ class = 'KNOWN'; reason = "firewall rules are reported (here at $($found.item.confidence)), never removed" } }
        return @{ class = 'FAIL'; reason = "a firewall rule the scan didn't report" }
    }
    if ($found) {
        $reported = $found.item
        $level = $reported.confidence
        $evidence = @($reported.evidence | ForEach-Object { if ($_.detail) { "$($_.kind)($($_.detail))" } else { $_.kind } }) -join ', '
        $inside = if ($found.container) { " (inside the reported $(Where-Text $reported.location))" } else { '' }
        if ($level -in 'good', 'very-good') {
            if ($reported.location.kind -eq 'driver') { return @{ class = 'KNOWN'; reason = 'drivers are shown only, never removed' } }
            $planned = $null
            $null = $context.removal.TryGetValue("$($reported.location.kind)|$(Where-Text $reported.location)", [ref]$planned)
            $what = if ($planned) { "planned, but $($planned.state) ($($planned.reason))" } else { 'not planned' }
            return @{ class = 'FAIL'; reason = "offered at $level $($reported.score) [$evidence]$inside, $what, and still there" }
        }
        $tied = if (@($reported.evidence | Where-Object { $_.kind -eq 'traced-install-root' }).Count) { 'tied' } else { 'untied' }
        if ($own) { return @{ class = 'FAIL'; reason = "the fixture's own item, left at $level $($reported.score) [$evidence]$inside" } }
        return @{ class = 'KNOWN'; reason = "left below the removal threshold, $($tied): $level $($reported.score) [$evidence]$inside" }
    }
    # A package's registration goes with its package, which a removal takes only with
    # --allow-irreversible: KNOWN while the package the scan offered is still registered and its
    # plan left it, never when the package went and its registration stayed.
    $place = Get-PackageRegistrationPlace $location
    $package = if ($place) { @($context.packages.Values | Where-Object { Test-NamesPackage $location $_ })[0] } else { $null }
    if ($package) {
        if ($null -eq $package.registered) { $package.registered = Test-PackageRegistered $package }
        $plan = if ($package.state) { "planned at $($package.offered), $($package.state) ($($package.reason))" } else { "offered at $($package.offered), not planned" }
        if ($package.registered -and (-not $package.state -or ($package.state -eq 'skipped' -and $package.reason -eq 'not-undoable'))) {
            return @{ class = 'KNOWN'; reason = "a package's registration ($place), which goes only with its package $($package.full), still registered: $plan, as a package goes only with --allow-irreversible" }
        }
        $now = if ($package.registered) { 'still registered' } else { 'no longer registered' }
        return @{ class = 'FAIL'; reason = "a package's registration ($place), left with its package $($package.full) $now ($plan)" }
    }
    $rule = Get-ProtectedPlace $location
    if ($rule -and $own) { return @{ class = 'FAIL'; reason = "the fixture's own item, never offered: protected ($rule)" } }
    if ($rule) { return @{ class = 'KNOWN'; reason = "a protected creation ($rule), never offered"; protected = $true } }
    return @{ class = 'FAIL'; reason = "still there, yet the scan didn't report it and it isn't in a protected place" }
}

# Revert: what a removal by the trace leaves of an installation. Every line of the oracle's diff
# between the capture before the install and `after` (the capture after the removal) is judged by
# the install's own change (not noise) or known value that accounts for it, once per item
# (Get-RevertVerdict): it must be gone, or be KNOWN: what existed before (never offered), a firewall
# rule (never removed), a protected creation (each line below it protected too), what the scan left
# below the removal threshold (with its level; never one of the fixture's own), or a PATH value's
# exact check (-PathVerdicts). A line nothing of the install accounts for isn't the installer's:
# the trace's noise, or changed outside its trace or after the install (logged), unless the
# removal's footprint covers it, which fails: the removal changed what the install didn't make.
# -Fixture (@{ items; required }): the items that must all be gone, and the KNOWN ones that must
# be there. Writes results\trace-oracle-<label>.txt.
function Test-Revert($record, [string]$after, $json, [string]$label, $Fixture = $null, $PathVerdicts = $null) {
    $lines = @(Get-CaptureDiff $record.pre $after)
    $own = New-ItemCoverage $record.all
    $noise = New-Coverage $record.all
    $footprint = New-Footprint $json.leftover_removal
    $installed = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in $record.lines) { $null = $installed.Add($line.path) }
    $scan = if ($json.leftover_scan) { $json.leftover_scan } else { $json }
    $context = @{ scan = (New-LocationMap $scan.leftovers); removal = (New-LocationMap $json.leftover_removal.items)
                  use = @($scan.traces | Where-Object { $_ })[0]; fixture = $Fixture; paths = $PathVerdicts
                  packages = (Get-RoundPackages $json) }
    $others = @{ '+' = @('~', '-'); '~' = @('+', '-'); '-' = @('+', '~') }
    $verdicts = [ordered]@{}
    $notOwn = [ordered]@{}
    $later = [System.Collections.Generic.List[string]]::new()
    $problems = [System.Collections.Generic.List[string]]::new()
    $detail = [System.Collections.Generic.List[string]]::new()
    $excluded = 0
    foreach ($line in $lines) {
        if ($diffExcludeRegex.IsMatch($line.path)) { $excluded++; $detail.Add("excluded`t$($line.text)"); continue }
        $item = Find-Cover $own[$line.sign] $line.path
        if (-not $item) {
            foreach ($sign in $others[$line.sign]) { $item = Find-Cover $own[$sign] $line.path; if ($item) { break } }
        }
        if ($item) {
            $key = Get-ItemKey $item
            $verdict = $verdicts[$key]
            if (-not $verdict) {
                $verdict = Get-RevertVerdict $item $context
                $verdict.text = Format-Item $item
                $verdict.lines = [System.Collections.Generic.List[string]]::new()
                $verdicts[$key] = $verdict
            }
            $verdict.lines.Add($line.text)
            $class = $verdict.class
            if ($verdict.protected -and -not (Get-ProtectedLine $line)) {
                $problems.Add("left inside the protected creation $($verdict.text), and not protected itself: $($line.text)")
                $class = 'PROBLEM'
            }
            $detail.Add("$($class): $($verdict.reason)`t$($line.text)")
            continue
        }
        $removedBy = Find-Cover $footprint $line.path
        if ($removedBy) {
            $what = if ($line.sign -eq '-') { 'the removal took away what was there before the install' } else { 'changed where the removal worked' }
            $problems.Add("$what ($removedBy): $($line.text)")
            $detail.Add("PROBLEM: $what ($removedBy)`t$($line.text)")
            continue
        }
        $reason = Find-Cover $noise[$line.sign] $line.path
        $why = if ($reason) { "the install's trace: $reason" } elseif ($installed.Contains($line.path)) { "changed outside the install's trace" } else { 'changed after the install' }
        $notOwn[$why] = 1 + [int]$notOwn[$why]
        if ($why -eq 'changed after the install') { $later.Add($line.text) }
        $detail.Add("not the installer's ($why)`t$($line.text)")
    }
    [IO.File]::WriteAllLines((Join-Path $results "trace-oracle-$label.txt"), [string[]]$detail, [Text.UTF8Encoding]::new($false))

    $known = @($verdicts.Values | Where-Object { $_.class -eq 'KNOWN' })
    $failed = @($verdicts.Values | Where-Object { $_.class -ne 'KNOWN' })
    $notOwnCount = 0
    foreach ($count in $notOwn.Values) { $notOwnCount += $count }
    Log ("  revert: {0} difference(s) between the capture before the install and the one after the removal: {1} excluded, {2} not the installer's{3}; {4} of its item(s) KNOWN, {5} left that shouldn't be, {6} other problem(s)" -f
         $lines.Count, $excluded, $notOwnCount, $(if ($notOwnCount) { ' (' + (@($notOwn.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', ') + ')' }),
         $known.Count, $failed.Count, $problems.Count)
    $shown = 0
    foreach ($text in $later) {
        if ($shown++ -ge 10) { Log "      ... and $($later.Count - 10) more changed after the install (see trace-oracle-$label.txt)"; break }
        Log "      changed after the install: $text"
    }
    foreach ($verdict in $known) { Log "    KNOWN: $($verdict.reason): $($verdict.text) ($($verdict.lines.Count) line(s))" }
    foreach ($verdict in $failed) {
        Fail "$($label): $($verdict.reason): $($verdict.text) ($($verdict.lines.Count) line(s))"
        foreach ($text in @($verdict.lines | Select-Object -First 3)) { Log "      $text" }
    }
    foreach ($text in $problems) { Fail "$($label): $text" }
    if ($Fixture) {
        foreach ($entry in $Fixture.required.GetEnumerator()) {
            if (-not $verdicts.Contains($entry.Key)) { Fail "$($label): must stay, but the oracle sees it as before the install: $($entry.Value)" }
        }
    }
    if (-not $failed -and -not $problems.Count) { Log '  OK: the removal took back everything the installer did but the KNOWN set' }
}

# The shell's record of a shortcut it saw created: a value of a user's UFH\SHC, named by a counter.
$shortcutRecord = '^HKU\\[^\\]+\\Software\\Microsoft\\Windows\\CurrentVersion\\UFH\\SHC\|'
# The shell's record of the handlers it has seen for a file type: an empty REG_NONE value named after
# each one's ProgID, in a user's Explorer\FileExts\<extension>\OpenWithProgids, which it writes by
# itself (run 2: for 27 of Windows' own types, between a removal and its undo).
$handlerRecordKey = '^HKU\\[^\\]+\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\[^\\|]+(\\OpenWithProgids)?$'
$handlerRecordValue = '^HKU\\[^\\]+\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\[^\\|]+\\OpenWithProgids\|'

# The non-noise changes of the fixture's install that aren't the fixture's own (Windows' shell
# reacting to its shortcut: the Start menu's state), by what they stand for in the oracle: their
# paths may change again when the shortcut goes and comes back. `shortcuts`: the type, size and
# hash (the oracle's attributes) of each UFH\SHC value the install's oracle diff (`lines`) shows
# created: the shell records a shortcut again under a new name when it comes back.
function New-ShellCoverage($json, $fixture, $lines) {
    $sets = @{ exact = New-ItemMap; container = New-ItemMap
               shortcuts = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal) }
    foreach ($item in @($json.changes | Where-Object { $_ -and $_.noise -eq 'none' })) {
        $where = Where-Text $item.location
        if ($fixture.items.Contains((Get-ItemKey $item)) -or $item.location.kind -in 'task', 'task-folder' -or
            $where -like 'C:\Windows\System32\Tasks\*') { continue }
        foreach ($target in @(Get-OracleTargets $item.location)) { if ($target) { Add-Item $sets $target.path $target.container (Format-Change $item) } }
    }
    foreach ($line in @($lines | Where-Object { $_ -and $_.sign -eq '+' -and $_.kind -eq 'V' -and $_.path -match $shortcutRecord })) {
        $null = $sets.shortcuts.Add($line.attributes)
    }
    return $sets
}

# Undo: the oracle's diff between `before` (the capture before the removal) and `after` (the one
# after its undo) must be empty, but for what Windows did meanwhile: the noise of `watch` (a manual
# trace around the removal and the undo, --all), the Task Scheduler's new id and file for a task
# registered again (`tasks`, its targets before and after; its definition and security are compared
# by the state dumps), the shell's own state the install changed too (`shell`), the shell's record
# of the fixture's shortcut made again when the undo gave the shortcut back (a new UFH\SHC value
# with the same data as one the install made, `shell.shortcuts`), the shell's records of the
# handlers it has seen for file types (FileExts, see $handlerRecordKey), and a container's base-layer
# item that the volume's journal has no record of between the captures (`journal`, see
# Get-JournalSilence; run 1: C:\Windows\PrintDialog\en-US). A line where the removal worked fails
# whatever else accounts for it. Writes results\trace-oracle-<label>.txt.
function Test-Undo([string]$before, [string]$after, $watch, $removal, [string]$label, $tasks, $shell, $journal) {
    $journalState = @{}
    $lines = @(Get-CaptureDiff $before $after)
    $noise = if ($watch) { New-Coverage $watch } else { $null }
    $footprint = New-Footprint $removal
    $taskSets = @{ exact = New-ItemMap; container = New-ItemMap }
    foreach ($target in @($tasks)) { if ($target) { Add-Item $taskSets $target.path $target.container 'task' } }
    $detail = [System.Collections.Generic.List[string]]::new()
    $byNoise = [ordered]@{}
    $known = [ordered]@{}
    $problems = [System.Collections.Generic.List[string]]::new()
    $excluded = 0
    foreach ($line in $lines) {
        if ($diffExcludeRegex.IsMatch($line.path)) { $excluded++; $detail.Add("excluded`t$($line.text)"); continue }
        if (Find-Cover $taskSets $line.path) {
            $why = "the Task Scheduler registers a restored task anew: a new id, its file written again (the state dump compares its definition and security)"
            $known[$why] = 1 + [int]$known[$why]
            $detail.Add("KNOWN: $why`t$($line.text)")
            continue
        }
        $removed = Find-Cover $footprint $line.path
        if ($removed) {
            $problems.Add("not given back exactly ($removed): $($line.text)")
            $detail.Add("PROBLEM: not given back exactly ($removed)`t$($line.text)")
            continue
        }
        $reason = $null
        if ($noise) {
            foreach ($sign in @($line.sign) + @('+', '~', '-' | Where-Object { $_ -ne $line.sign })) {
                $found = Find-Cover $noise[$sign] $line.path
                if ($found -and $found -ne 'none') { $reason = $found; break }
                if ($found -and -not $reason) { $reason = 'none' }
            }
        }
        if ($reason -and $reason -ne 'none') {
            $byNoise[$reason] = 1 + [int]$byNoise[$reason]
            $detail.Add("noise ($reason)`t$($line.text)")
            continue
        }
        $shellItem = if ($shell) { Find-Cover $shell $line.path } else { $null }
        if ($shellItem) {
            $why = "the shell's own state, which it changed when the fixture was installed too ($shellItem)"
            $known[$why] = 1 + [int]$known[$why]
            $detail.Add("KNOWN: $why`t$($line.text)")
            continue
        }
        if ($shell -and $line.sign -eq '+' -and $line.kind -eq 'V' -and $line.path -match $shortcutRecord -and
            $shell.shortcuts.Contains($line.attributes)) {
            $why = 'the shell recorded the shortcut the undo gave back once more: a new UFH\SHC value, the same as the one it made when the fixture was installed'
            $known[$why] = 1 + [int]$known[$why]
            $detail.Add("KNOWN: $why`t$($line.text)")
            continue
        }
        # Created, never changed or taken: a key that holds a record also holds whatever else is
        # below it (UserChoice), and each of those lines is judged on its own.
        if ($line.sign -eq '+' -and (($line.kind -eq 'K' -and $line.path -match $handlerRecordKey) -or
                                     ($line.kind -eq 'V' -and $line.path -match $handlerRecordValue -and $line.attributes -match '^0 0 '))) {
            $why = "the shell recorded the handlers it has seen for file types (an empty REG_NONE value named after each one's ProgID, in FileExts\<extension>\OpenWithProgids)"
            $known[$why] = 1 + [int]$known[$why]
            $detail.Add("KNOWN: $why`t$($line.text)")
            continue
        }
        if (-not $reason -and ($silence = Get-JournalSilence $line $journal $journalState)) {
            $why = "a container's base-layer item: $silence ($($line.text))"
            $known[$why] = 1 + [int]$known[$why]
            $detail.Add("KNOWN: $silence`t$($line.text)")
            continue
        }
        $what = if (-not $watch) { 'no watch trace tells' } elseif ($reason) { 'the watch trace reports it as a change that is not noise' } else { "the watch trace doesn't account for it" }
        $problems.Add("changed between the removal and its undo, not where the removal worked, and $($what): $($line.text)$(Get-ChangeNote $line)")
        $detail.Add("PROBLEM: $what`t$($line.text)")
    }
    [IO.File]::WriteAllLines((Join-Path $results "trace-oracle-$label.txt"), [string[]]$detail, [Text.UTF8Encoding]::new($false))
    $noisy = 0
    foreach ($count in $byNoise.Values) { $noisy += $count }
    $knownCount = 0
    foreach ($count in $known.Values) { $knownCount += $count }
    Log ("  undo: {0} difference(s) between the capture before the removal and the one after its undo: {1} excluded, {2} noise{3}, {4} known, {5} problem(s)" -f
         $lines.Count, $excluded, $noisy, $(if ($noisy) { ' (' + (@($byNoise.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', ') + ')' }),
         $knownCount, $problems.Count)
    foreach ($entry in $known.GetEnumerator()) { Log "    KNOWN: $($entry.Key): $($entry.Value) line(s)" }
    if (-not $problems.Count) { Log '  OK: undo gave back exactly what was there before the removal (the oracle)'; return }
    Fail "$($label): $($problems.Count) difference(s) after the undo"
    $shown = 0
    foreach ($text in $problems) {
        if ($shown++ -ge 50) { Log "      ... and $($problems.Count - 50) more (see trace-oracle-$label.txt)"; break }
        Log "      $text"
    }
}

# A scan's `traces` (leftover_scan's, or scan's own): one use, of trace `traceId` by `programId`,
# whose counts add up to the trace's changes that aren't noise (each is an item, gone, shown only or
# protected) and whose value edits are the values planned (undo or restore). Returns the use.
function Test-TraceUse($scan, [string]$label, [string]$traceId, [string]$programId, $record) {
    $uses = @($scan.traces | Where-Object { $_ })
    if ($uses.Count -ne 1) { Fail "$($label): the scan used $($uses.Count) trace(s), not 1" }
    if (-not $uses) { return $null }
    $use = $uses[0]
    Log ('  trace use: {0} by {1}: {2} item(s) ({3} tied), {4} value edit(s), {5} gone, {6} shown only, {7} protected' -f
         $use.trace_id, $use.program_id, $use.items, $use.tied, $use.value_edits, $use.gone, $use.shown_only, $use.protected)
    foreach ($value in @($use.values | Where-Object { $_ })) { Log "    value $($value.key) : $($value.name): $($value.basis)" }
    $problems = @()
    if ($use.trace_id -ne $traceId) { $problems += "trace $($use.trace_id), not $traceId" }
    if ($use.program_id -ne $programId) { $problems += "program $($use.program_id), not $programId" }
    $changes = @($record.all.changes | Where-Object { $_ -and $_.noise -eq 'none' }).Count
    $sum = [int]$use.items + [int]$use.gone + [int]$use.shown_only + [int]$use.protected
    if ($sum -ne $changes) { $problems += "items, gone, shown only and protected add up to $sum, but the trace has $changes change(s) that aren't noise" }
    $planned = @($use.values | Where-Object { $_ -and $_.basis -in 'undo', 'restore' }).Count
    if ([int]$use.value_edits -ne $planned) { $problems += "$($use.value_edits) value edit(s), but $planned value(s) planned" }
    if ([int]$use.tied -gt [int]$use.items) { $problems += 'more tied than items' }
    if ($problems) { Fail "$($label): leftover_scan.traces: $($problems -join '; ')" }
    else { Log '  OK: the trace use names the trace and the program, and its counts add up' }
    return $use
}

# The scan's items that carry a trace's evidence, for the log.
function Write-TracedItems($scan) {
    $items = @($scan.leftovers | Where-Object { $_ -and @($_.evidence | Where-Object { $_.kind -like 'traced-*' }).Count })
    Log "  the scan's items with the trace's evidence: $($items.Count)"
    foreach ($item in $items) {
        $where = Where-Text $item.location
        if ($item.location.kind -eq 'value-edit') { $where += " ($($item.location.edit))" }
        $evidence = @($item.evidence | ForEach-Object { if ($_.detail) { "$($_.kind)($($_.detail))" } else { $_.kind } }) -join ', '
        Log ('    {0,-12} {1,3}  {2,-14} {3}  [{4}]' -f $item.confidence, $item.score, $item.location.kind, $where, $evidence)
    }
}

# Every item of a removal done, or left to go with what contains it; a package is removed only with
# --allow-irreversible. -Optional: no removal at all is fine (nothing was planned).
function Test-RemovalDone($removal, [string]$label, [switch]$Optional) {
    if (-not $removal) {
        if ($Optional) { Log '  nothing was planned for removal' } else { Fail "$($label): nothing was removed (no leftover_removal)" }
        return
    }
    $items = @($removal.items | Where-Object { $_ })
    Log "  change set $($removal.set_id): $($items.Count) item(s), $($removal.quarantined_bytes) bytes quarantined"
    foreach ($item in $items) {
        $where = Where-Text $item.location
        if ($item.location.kind -eq 'value-edit') { $where += " ($($item.location.edit))" }
        Log ('    {0,-4} {1,-26} {2,-11} {3,-15} {4}' -f $item.index, "$($item.state) ($($item.reason))", $item.confidence, $item.location.kind, $where)
        if ($item.state -eq 'done' -or ($item.state -eq 'skipped' -and $item.reason -eq 'contained')) { continue }
        if ($item.location.kind -eq 'package' -and $item.state -eq 'skipped' -and $item.reason -eq 'not-undoable') {
            Log '      KNOWN: a package goes only with --allow-irreversible (undo couldn''t put it back)'
            continue
        }
        Fail "$($label): $($item.location.kind) $where is $($item.state) ($($item.reason))"
    }
}

# A user's own PATH entry, appended (or with -Remove taken out) as a user would, the value kept
# REG_EXPAND_SZ.
function Set-UserPathEntry([string]$spot, [string]$entry, [switch]$Remove) {
    $base = if ($spot -eq 'machine-environment') { [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64') }
            else { [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', 'Registry64') }
    $key = $base.OpenSubKey($(if ($spot -eq 'machine-environment') { $traceMachineEnvironment } else { 'Environment' }), $true)
    if (-not $key) { return }
    try {
        $old = $key.GetValue('Path', $null, 'DoNotExpandEnvironmentNames')
        if ($Remove) {
            if (-not $old) { return }
            $kept = @($old -split ';' | Where-Object { $_ -ne $entry })
            if ($kept.Count -ne @($old -split ';').Count) { $key.SetValue('Path', ($kept -join ';'), 'ExpandString') }
        } else {
            $new = if (-not $old) { $entry } elseif ($old.EndsWith(';')) { $old + $entry } else { "$old;$entry" }
            $key.SetValue('Path', $new, 'ExpandString')
        }
    } finally {
        $key.Close()
    }
}

function Read-Value($key, [string]$name) {
    if (-not $key -or @($key.GetValueNames()) -notcontains $name) { return $null }
    return @{ kind = "$($key.GetValueKind($name))"; data = $key.GetValue($name, $null, 'DoNotExpandEnvironmentNames') }
}

# The fixture's values in the known spots as they are now.
function Read-FixtureValues {
    $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    $user = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', 'Registry64')
    $values = @{}
    $key = $machine.OpenSubKey($traceMachineEnvironment)
    $values['machine-environment'] = Read-Value $key 'Path'
    $values.home = Read-Value $key 'CONTOSO_HOME'
    if ($key) { $key.Close() }
    $key = $user.OpenSubKey('Environment')
    $values['user-environment'] = Read-Value $key 'Path'
    if ($key) { $key.Close() }
    $key = $machine.OpenSubKey($traceSharedDlls)
    $values.shared = Read-Value $key $traceShared
    $values.own = Read-Value $key "$traceRoot\shell.dll"
    if ($key) { $key.Close() }
    return $values
}

# The fixture's values right after a removal by its trace, exactly: each PATH the value before the
# install plus the user's own entry (returned as Test-Revert's -PathVerdicts, by spot), the shared
# DLL's count back at 1, its own count and CONTOSO_HOME gone.
function Get-FixturePathVerdicts($values, $paths, [string]$label) {
    $verdicts = @{}
    foreach ($spot in $userPathEntries.Keys) {
        $before = $paths[$spot]
        $mine = $userPathEntries[$spot]
        $want = if (-not $before) { $mine } elseif ($before.EndsWith(';')) { $before + $mine } else { "$before;$mine" }
        $now = $values[$spot]
        $problems = @()
        if (-not $now) {
            $problems += 'it is gone'
        } else {
            if ($now.kind -ne 'ExpandString') { $problems += "it is $($now.kind), not REG_EXPAND_SZ" }
            if ("$($now.data)" -cne $want) { $problems += "it is '$($now.data)', not '$want'" }
        }
        if ($problems) {
            Log "  $spot Path after the removal: $($problems -join '; ')"
            $verdicts[$spot] = @{ class = 'FAIL'; reason = "not the value before the install plus the user's own entry ($($problems -join '; '))" }
        } else {
            Log "  OK: $spot Path is exactly the value before the install plus the user's own entry $mine"
            $verdicts[$spot] = @{ class = 'KNOWN'; reason = "the user's own entry $mine, added after the install, stays and the installer's is gone (checked exactly)" }
        }
    }
    if (-not $values.shared -or $values.shared.kind -ne 'DWord' -or [int64]$values.shared.data -ne 1) {
        Fail "$($label): the SharedDLLs count of $traceShared is $(if ($values.shared) { "$($values.shared.kind) $($values.shared.data)" } else { 'gone' }), not back at 1"
    } else { Log "  OK: the SharedDLLs count of $traceShared is back at 1" }
    if ($values.own) { Fail "$($label): the SharedDLLs count of $traceRoot\shell.dll is still there ($($values.own.data))" }
    else { Log "  OK: the SharedDLLs count of $traceRoot\shell.dll is gone" }
    if ($values.home) { Fail "$($label): CONTOSO_HOME is still there ('$($values.home.data)')" }
    else { Log '  OK: CONTOSO_HOME is gone' }
    return $verdicts
}

# The fixture's items and what Test-Revert needs to know about them: `items` (change|kind|where),
# `created` (its created items), `required` (the KNOWN set that must stay: Theme, the firewall rule
# and both PATH values with the user's own entries), and its state dump specs: what a removal takes,
# what stays and the values it edits.
function New-FixtureContext([string]$ruleId) {
    $items = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $created = @()
    foreach ($want in @(Get-TraceFixtureItems)) {
        $where = if ($want.kind -eq 'firewall-rule') { $ruleId } else { $want.where }
        $null = $items.Add("$($want.change)|$($want.kind)|$where")
        if ($want.change -eq 'created') { $created += @{ kind = $want.kind; where = $where } }
    }
    $environment = "HKLM\$traceMachineEnvironment"
    $windows = 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion'
    return @{
        ruleId = $ruleId; items = $items; created = $created
        required = [ordered]@{
            'modified|registry-value|HKLM\SOFTWARE\Contoso Shared : Theme' = 'Theme, which the installer changed'
            "created|firewall-rule|$ruleId" = 'the firewall rule'
            "known|$environment|Path" = "the machine Path, with the user's own entry"
            "known|HKU\$traceUserSid\Environment|Path" = "the user's Path, with the user's own entry"
        }
        removedSpecs = @("file:$traceRoot", "key:$windows\Uninstall\Contoso Trace", 'key:HKLM\SOFTWARE\Contoso Trace',
            "key:HKU\$traceUserSid\Software\Contoso Trace", 'file:C:\ProgramData\Contoso Trace', "file:$env:APPDATA\Contoso Trace",
            "file:$traceStartMenu", "value:$windows\Run|ContosoTraceUpdater", "key:$windows\App Paths\ContosoTrace.exe",
            "key:HKLM\SOFTWARE\Classes\CLSID\$traceClsid", 'key:HKLM\SOFTWARE\Classes\*\shellex\ContextMenuHandlers\ContosoTrace',
            "value:$windows\Shell Extensions\Approved|$traceClsid", "service:$traceService", "task:$traceTask")
        stayingSpecs = @("value:$firewallRules|$ruleId", 'value:HKLM\SOFTWARE\Contoso Shared|Theme', "file:$(Split-Path $traceShared)")
        valueSpecs = @("value:$environment|Path", "value:$environment|CONTOSO_HOME", "value:HKU\$traceUserSid\Environment|Path",
            "value:HKLM\$traceSharedDlls|$traceShared", "value:HKLM\$traceSharedDlls|$traceRoot\shell.dll")
    }
}

# The fixture in a removal by its trace: each of its created items planned (or inside what is),
# tied to it in the scan; its five value edits planned; what stays never planned. -ProgramGone:
# its own uninstaller took its folder and Uninstall key already.
function Test-FixturePlan($json, $fixture, [string]$label, [switch]$ProgramGone) {
    $planned = New-LocationMap $json.leftover_removal.items
    $scan = New-LocationMap $json.leftover_scan.leftovers
    $ok = 0
    $wanted = 0
    foreach ($want in $fixture.created) {
        if ($want.kind -eq 'firewall-rule') { continue }
        if ($ProgramGone -and (($want.kind -eq 'folder' -and $want.where -eq $traceRoot) -or
                               ($want.kind -eq 'registry-key' -and $want.where -like '*\Uninstall\Contoso Trace'))) { continue }
        $wanted++
        $what = "$($want.kind) $($want.where)"
        $found = Find-LocationItem $planned $want.kind $want.where
        if (-not $found) { Fail "$($label): the fixture's $what isn't planned for removal"; continue }
        $reported = $null
        $null = $scan.TryGetValue("$($want.kind)|$($want.where)", [ref]$reported)
        $tie = if ($reported) { @($reported.evidence | Where-Object { $_.kind -eq 'traced-install-root' } | ForEach-Object { $_.detail })[0] } else { $null }
        $how = if ($found.container) { "inside the planned $(Where-Text $found.item.location)" } else { "$($found.item.state) ($($found.item.reason))" }
        if (-not $tie) {
            $evidence = if ($reported) { "$($reported.confidence): " + (@($reported.evidence | ForEach-Object { $_.kind }) -join ', ') } else { 'not in the scan by itself' }
            Fail "$($label): the fixture's $what isn't tied to it ($evidence)"
            continue
        }
        Log "    $($reported.confidence), tied ($tie): $what, $how"
        $ok++
    }
    Log "  $ok of the fixture's $wanted created item(s) planned and tied to it"
    $edits = @(
        @{ key = "HKLM\$traceMachineEnvironment"; name = 'Path'; edit = 'remove-path-entries' },
        @{ key = "HKU\$traceUserSid\Environment"; name = 'Path'; edit = 'remove-path-entries' },
        @{ key = "HKLM\$traceMachineEnvironment"; name = 'CONTOSO_HOME'; edit = 'delete-value' },
        @{ key = "HKLM\$traceSharedDlls"; name = $traceShared; edit = 'decrement-shared-dll' },
        @{ key = "HKLM\$traceSharedDlls"; name = "$traceRoot\shell.dll"; edit = 'delete-value' })
    $made = @($json.leftover_removal.items | Where-Object { $_ -and $_.location.kind -eq 'value-edit' })
    foreach ($want in $edits) {
        $found = @($made | Where-Object { (ConvertTo-OraclePath $_.location.key) -eq $want.key -and $_.location.value -eq $want.name })
        $what = "$($want.key) : $($want.name)"
        if ($found.Count -ne 1) { Fail "$($label): the value edit of $what is planned $($found.Count) time(s)"; continue }
        $edit = $found[0]
        if ($edit.location.edit -ne $want.edit) { Fail "$($label): the value edit of $what is $($edit.location.edit), not $($want.edit)" }
        else { Log "  OK: value edit $($edit.location.edit) of $what at $($edit.confidence): $($edit.state)" }
    }
    $restores = @($made | Where-Object { $_.location.edit -eq 'restore-value' })
    if ($restores) { Fail "$($label): $($restores.Count) value(s) to restore, which the fixture never changes" }
    $stays = @("firewall-rule|$($fixture.ruleId)", 'registry-value|HKLM\SOFTWARE\Contoso Shared : Theme',
               'registry-key|HKLM\SOFTWARE\Contoso Shared', "folder|$(Split-Path $traceShared)", "file|$traceShared")
    foreach ($key in $stays) { if ($planned.ContainsKey($key)) { Fail "$($label): planned what must stay: $key" } }
}

# After the stop, the fixture's trace is linked to it by itself (trace show --json: links) and its
# created items are tied to it (tied, tie). Returns the program's id, or $null.
function Test-FixtureLinks([string]$id, $fixture) {
    $show = Invoke-Trace @('show', $id, '--json') 'r1-show'
    if (-not $show.json) { Fail "round 1 (5b): trace show printed no JSON (exit $($show.exit))"; return $null }
    $links = @($show.json.trace.links | Where-Object { $_ })
    foreach ($link in $links) {
        Log "  link: $($link.name) ($($link.program_id)) [$($link.matched_by)]$(if ($link.primary) { ', primary' }); key $($link.uninstall_key), folder $($link.install_location)"
    }
    $program = $null
    $primary = @($links | Where-Object { $_.primary })
    if ($primary.Count -ne 1) {
        Fail "round 1 (5b): the trace has $($primary.Count) primary link(s) after the stop, not 1"
    } else {
        $link = $primary[0]
        $program = $link.program_id
        $problems = @()
        if ($link.name -ne 'Contoso Trace') { $problems += "name '$($link.name)'" }
        if ($link.matched_by -ne 'created-key') { $problems += "matched by $($link.matched_by)" }
        if ($link.uninstall_key -ne 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso Trace') { $problems += "Uninstall key $($link.uninstall_key)" }
        if ("$($link.install_location)".TrimEnd('\') -ne $traceRoot) { $problems += "folder '$($link.install_location)'" }
        if ($problems) { Fail "round 1 (5b): the trace's link isn't the fixture: $($problems -join ', ')" }
        else { Log '  OK: the trace linked itself to the fixture when it stopped (by the Uninstall key it created)' }
    }
    $changes = @($show.json.changes | Where-Object { $_ })
    $tied = 0
    foreach ($want in $fixture.created) {
        $change = @($changes | Where-Object { $_.change -eq 'created' -and $_.location.kind -eq $want.kind -and (Where-Text $_.location) -eq $want.where })[0]
        if (-not $change) { continue }  # Round 1 fails it
        if ($change.tied) { $tied++; Log "    tied ($($change.tie)): $($want.kind) $($want.where)" }
        else { Fail "round 1 (5b): the fixture's $($want.kind) $($want.where) isn't tied to it (tie: $($change.tie))" }
    }
    Log "  $tied of the fixture's $($fixture.created.Count) created item(s) tied to it"
    foreach ($change in @($changes | Where-Object { -not $_.tied })) { Log "    not tied: $(Format-Change $change)" }
    return $program
}

# trace export: one file, an "avel-trace" of schema 1 holding the trace and its user.
function Test-TraceExport([string]$id, [string]$file) {
    if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
    $export = Invoke-Trace @('export', $id, $file) 'r1-export'
    if ($export.exit -ne 0 -or -not (Test-Path -LiteralPath $file)) { Fail "round 1 (5b): trace export exited with $($export.exit)"; return }
    try {
        $data = [IO.File]::ReadAllText($file, [Text.Encoding]::UTF8) | ConvertFrom-Json
    } catch {
        Fail "round 1 (5b): the exported file isn't JSON: $($_.Exception.Message)"
        return
    }
    $users = @($data.users | Where-Object { $_ } | ForEach-Object { $_.sid })
    $problems = @()
    if ($data.format -ne 'avel-trace' -or $data.schema_version -ne 1) { $problems += "format $($data.format), schema $($data.schema_version)" }
    if ($data.trace.id -ne $id) { $problems += "trace $($data.trace.id)" }
    if ($users -notcontains $sid) { $problems += "users $($users -join ', ')" }
    if ($problems) { Fail "round 1 (5b): the exported file isn't this trace's: $($problems -join '; ')" }
    else { Log "  OK: trace export wrote $((Get-Item -LiteralPath $file).Length) bytes, users: $($users -join ', ')" }
}

# trace delete, then trace import of the exported file (a new trace, its origin named, linked again
# to the fixture), then trace link by hand. Returns the imported trace's id, or $null.
function Import-FixtureTrace([string]$id, [string]$file, [string]$program) {
    $delete = Invoke-Trace @('delete', $id) 'r1-delete'
    if ($delete.exit -ne 0) { Fail "round 1 (5b): trace delete exited with $($delete.exit)" }
    elseif (@(Get-TraceList | Where-Object { $_.id -eq $id })) { Fail "round 1 (5b): trace $id is still listed after trace delete" }
    else { Log "  OK: trace $id deleted" }
    $import = Invoke-Trace @('import', $file, '--json') 'r1-import'
    $info = $import.json.trace
    if ($import.exit -ne 0 -or -not $info -or -not $info.id) { Fail "round 1 (5b): trace import exited with $($import.exit)"; return $null }
    $remapped = if ($info.imported.remapped) { "$($info.imported.remapped.from_sid) -> $($info.imported.remapped.to_sid)" } else { 'none' }
    Log "  imported as $($info.id): $($info.state), origin $($info.imported.origin_id), remapped $remapped, not remapped: $(@($info.imported.not_remapped) -join ', ')"
    foreach ($link in @($info.links | Where-Object { $_ })) { Log "    link: $($link.name) ($($link.program_id)) [$($link.matched_by)]$(if ($link.primary) { ', primary' })" }
    $primary = @($info.links | Where-Object { $_ -and $_.primary })[0]
    $problems = @()
    if ($info.state -ne 'complete') { $problems += "state $($info.state)" }
    if ($info.imported.origin_id -ne $id) { $problems += "origin $($info.imported.origin_id)" }
    if (-not $primary -or $primary.program_id -ne $program) { $problems += "primary link $($primary.program_id), not $program" }
    if ($problems) { Fail "round 1 (5b): trace import: $($problems -join '; ')" }
    else { Log "  OK: imported as a new trace of $id, linked to the installed fixture again" }
    $link = Invoke-Trace @('link', $info.id, $program, '--json') 'r1-link'
    $links = @($link.json.trace.links | Where-Object { $_ })
    $first = $links[0]
    if ($link.exit -ne 0 -or -not $first -or $first.program_id -ne $program -or $first.matched_by -ne 'manual' -or -not $first.primary -or
        @($links | Where-Object { $_.program_id -eq $program }).Count -ne 1) {
        Fail "round 1 (5b): trace link: exit $($link.exit), links $(@($links | ForEach-Object { "$($_.program_id) [$($_.matched_by)]$(if ($_.primary) { ' primary' })" }) -join '; ')"
    } else { Log '  OK: trace link made the fixture its primary link, by hand' }
    return $info.id
}

# A removal by the trace and its undo, inside a manual trace (the watch) whose noise tells
# Windows' own activity apart: state dumps and oracle captures before the removal, after it (once
# Windows has reacted) and after the undo, and the fixture's values right after the removal. The
# watch is deleted afterwards. Returns what it saw.
function Invoke-FixtureRevert([string]$label, [string[]]$force, $fixture) {
    Close-OpenTrace
    $specs = @($fixture.removedSpecs) + @($fixture.stayingSpecs) + @($fixture.valueSpecs)
    $revert = @{ tasks = @() }
    $start = Invoke-Trace @('start', '--description', "watch: $label", '--json') "$label-watch-start"
    $watching = $start.exit -eq 0 -and $start.json.trace.state -eq 'open'
    if (-not $watching) { Fail "$($label): the watch trace didn't start (exit $($start.exit)); Windows' own activity can't be told apart" }
    Start-Hold
    try {
        $revert.tasks += @(Get-TaskTargets $traceTask)
        $revert.dumpBefore = Split-StateDump (Get-StateDump $specs)
        $revert.journal = @{ before = [AvelJournal]::Query('C:'); after = $null }
        $revert.before = New-OracleCapture "$label-before"
        $revert.run = Invoke-Avel (@($force) + '--json') "$label-force" 600
        Start-Sleep -Seconds $settleSeconds
        $revert.values = Read-FixtureValues
        $revert.dumpRemoved = Split-StateDump (Get-StateDump $specs)
        $revert.after = New-OracleCapture "$label-after"
        $set = if ($revert.run.json) { $revert.run.json.leftover_removal.set_id } else { $null }
        if ($set) {
            $revert.undo = Invoke-Avel @('undo', $set, '--json') "$label-undo" 600
            Start-Sleep -Seconds $settleSeconds
        }
        $revert.undone = New-OracleCapture "$label-undone"
        $revert.journal.after = [AvelJournal]::Query('C:')
        $revert.dumpUndone = Split-StateDump (Get-StateDump $specs)
        $revert.tasks += @(Get-TaskTargets $traceTask)
    } finally {
        if ($watching) { $stop = Invoke-Trace @('stop', '--json') "$label-watch-stop" }
        Stop-Hold
    }
    if ($watching -and $stop.json) {
        Log '  the watch trace around the removal and its undo:'
        Write-TraceSummary $stop.json
        foreach ($link in @($stop.json.trace.links | Where-Object { $_ })) { Log "    linked to $($link.name) ($($link.program_id)) [$($link.matched_by)]" }
        $revert.watch = Get-AllChanges $stop "$label-watch"
        Write-NoiseCounts $revert.watch
        $null = Invoke-Trace @('delete', $stop.json.trace.id) "$label-watch-delete"
    } elseif ($watching) {
        Fail "$($label): the watch trace's stop printed no JSON (exit $($stop.exit))"
    }
    return $revert
}

# A removal of the fixture by its trace (Invoke-FixtureRevert): force's JSON (the program and its
# trace, the trace use, the plan), what the removal left (state dumps, exact values, Test-Revert)
# and its undo (state dumps, Test-Undo).
function Test-FixtureRevert($revert, $record, $fixture, [string]$traceId, [string]$programId, [string]$label, $shell,
                            [switch]$ProgramGone) {
    $run = $revert.run
    if (-not $run -or -not $run.json) { Fail "$($label): force printed no JSON (exit $(if ($run) { $run.exit }))"; return }
    $json = $run.json
    if ($run.exit -ne 0) { Fail "$($label): force exited with $($run.exit)" }
    $programs = @($json.programs | Where-Object { $_ })
    if ($programs.Count -ne 1 -or $programs[0].id -ne $programId -or $programs[0].trace -ne $traceId) {
        Fail "$($label): force's programs are $(@($programs | ForEach-Object { "$($_.id) by trace $($_.trace)" }) -join '; '), not $programId by trace $traceId"
    } else {
        Log "  OK: force reverted $programId by trace $traceId"
    }
    $use = Test-TraceUse $json.leftover_scan $label $traceId $programId $record
    if ($use) {
        $values = @($use.values | Where-Object { $_ })
        if ($values.Count -ne 5 -or @($values | Where-Object { $_.basis -ne 'undo' })) {
            Fail "$($label): want the fixture's 5 values planned as undo, got $(@($values | ForEach-Object { "$($_.name) $($_.basis)" }) -join ', ')"
        }
        if ($ProgramGone -and [int]$use.gone -lt 2) { Fail "$($label): $($use.gone) traced item(s) gone, but the fixture's own uninstaller took its folder and Uninstall key" }
    }
    Write-TracedItems $json.leftover_scan
    Test-RemovalDone $json.leftover_removal $label
    Check-Safe @($json.leftover_scan.leftovers) "$label scan"
    if ($json.leftover_removal) { Check-Safe @($json.leftover_removal.items) "$label removal" }
    Test-FixturePlan $json $fixture $label -ProgramGone:$ProgramGone
    foreach ($spec in $fixture.removedSpecs) {
        if ($revert.dumpRemoved[$spec] -notmatch '^<missing>') { Fail "$($label): not removed: $spec" }
    }
    foreach ($spec in $fixture.stayingSpecs) {
        if ($revert.dumpRemoved[$spec] -ne $revert.dumpBefore[$spec]) { Fail "$($label): changed what must stay: $spec" }
    }
    $paths = Get-FixturePathVerdicts $revert.values $record.paths $label
    Test-Revert $record $revert.after $json "$label-revert" -Fixture $fixture -PathVerdicts $paths
    if (-not $revert.undo) { Fail "$($label): there was no change set to undo"; return }
    Log "  undo: $($revert.undo.json.state), exit $($revert.undo.exit)"
    if ($revert.undo.json.state -ne 'restored') { Fail "$($label): undo: state $($revert.undo.json.state)" }
    Compare-State $revert.dumpBefore $revert.dumpUndone "$($label): undo gave back exactly what was there (state dumps)"
    Test-Undo $revert.before $revert.undone $revert.watch $json.leftover_removal "$label-undo" $revert.tasks $shell $revert.journal
}

# 5b of round 1: the fixture, still installed, reverted by its trace with `force` and undone; then
# its trace exported, deleted, imported and linked by hand, its own uninstaller run (its folder and
# key go), and the rest reverted with `force --trace <the imported trace>` (the program is gone, so
# the target is trace:<id>) and undone. Leaves the fixture removed and the user's entries gone.
function Invoke-FixtureRevertRound($record) {
    Log ''
    Log 'Round 1 (5b): the fixture reverted by its trace with force and undone; its trace exported, deleted, imported and linked by hand; its own uninstaller run, the rest reverted with force --trace and undone'
    if (-not $record -or -not $record.id) { Fail 'round 1 (5b): there is no trace of the fixture to revert'; return }
    if (-not (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso Trace')) { Fail 'round 1 (5b): the fixture is not installed'; return }
    try {
        $id = $record.id
        $fixture = New-FixtureContext (Get-TraceFixtureRuleId)
        $program = Test-FixtureLinks $id $fixture
        if (-not $program) { $program = 'Contoso Trace' }  # Its exact name
        $file = Join-Path $captures 'r1-fixture.trace.json'
        Test-TraceExport $id $file
        foreach ($spot in $userPathEntries.Keys) { Set-UserPathEntry $spot $userPathEntries[$spot] }
        Log "  the user added their own Path entries: $(@($userPathEntries.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', ')"
        $shell = New-ShellCoverage $record.all $fixture $record.lines

        Log ''
        Log "  A. force $program --yes --end-processes, then undo"
        $revert = Invoke-FixtureRevert 'r1-force' @('force', $program, '--yes', '--end-processes') $fixture
        Test-FixtureRevert $revert $record $fixture $id $program 'r1-force' $shell

        Log ''
        Log '  B. trace export, delete, import and link; the fixture''s own uninstaller; force --trace <the imported trace>, then undo'
        $imported = Import-FixtureTrace $id $file $program
        if (-not $imported) { return }
        $uninstaller = Start-Process "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -Wait -PassThru -WindowStyle Hidden `
            -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$traceRoot\uninstall.ps1`"")
        $left = @($traceRoot, 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso Trace' | Where-Object { Test-Path -LiteralPath $_ })
        if ($uninstaller.ExitCode -ne 0 -or $left) { Fail "round 1 (5b): the fixture's own uninstaller exited with $($uninstaller.ExitCode), left: $($left -join ', ')" }
        else { Log '  the fixture''s own uninstaller removed its folder and its Uninstall key' }
        Start-Sleep -Seconds $settleSeconds  # Windows' reactions to it, before the watch
        $revert = Invoke-FixtureRevert 'r1-force-trace' @('force', '--trace', $imported, '--yes', '--end-processes') $fixture
        Test-FixtureRevert $revert $record $fixture $imported "trace:$imported" 'r1-force-trace' $shell -ProgramGone
    } finally {
        Remove-TraceFixture
        foreach ($spot in $userPathEntries.Keys) { Set-UserPathEntry $spot $userPathEntries[$spot] -Remove }
    }
}

# 5b of rounds 2-4, mode 1: the sample uninstalled with its trace. `scan` previews it; `uninstall
# --quiet --remove-leftovers` runs its own uninstaller, then what the trace saw its installer create
# joins the leftovers and what is Good or better goes. results[].trace names the trace, the trace
# use's counts add up, every planned item goes, Test-Revert judges what is left against the capture
# before the install, and retention starts the uninstalled program's trace's 30 days.
function Invoke-UninstallByTrace($sample, [int]$number) {
    Log ''
    Log "Round $number (5b): $($sample.name) uninstalled with its trace (uninstall --quiet --remove-leftovers)"
    $record = $traced[$sample.label]
    if (-not $record -or -not $record.id) { Fail "round $number (5b): there is no trace of $($sample.name)"; return }
    $label = "$($sample.label)-uninstall"
    $links = @($record.result.trace.links | Where-Object { $_ })
    foreach ($link in $links) { Log "  link: $($link.name) ($($link.program_id)) [$($link.matched_by)]$(if ($link.primary) { ', primary' })" }
    $link = @($links | Where-Object { $_.primary })[0]
    if (-not $link) { Fail "$($label): the trace isn't linked to any program, so nothing is uninstalled by it"; return }
    # Uninstalling another entry the installer's trace saw registered (run 1: Windows' compatibility
    # fix database, which Windows installed meanwhile) would remove the wrong program.
    if ($link.name -notmatch $sample.entry) { Fail "$($label): the trace's primary link is $($link.name), not $($sample.name)'s own entry; not uninstalled"; return }
    $program = $link.program_id

    $preview = Invoke-Avel @('scan', $program, '--all-levels', '--json') "$label-scan" 300
    if (-not $preview.json) {
        Fail "$($label): scan printed no JSON (exit $($preview.exit))"
    } else {
        $programs = @($preview.json.programs | Where-Object { $_ })
        if ($programs.Count -ne 1 -or $programs[0].trace -ne $record.id) { Fail "$($label): scan's programs are $(@($programs | ForEach-Object { "$($_.id) by trace $($_.trace)" }) -join '; '), not by trace $($record.id)" }
        else { Log "  OK: scan previews $program with trace $($record.id)" }
        $use = Test-TraceUse $preview.json "$label scan" $record.id $program $record
        if ($use -and [int]$use.tied -lt 1) { Fail "$($label): scan: nothing the trace saw is tied to the program while it is installed" }
    }

    $run = Invoke-Avel @('uninstall', $program, '--quiet', '--yes', '--remove-leftovers', '--no-restore-point', '--json') $label 900
    Start-Sleep -Seconds $settleSeconds
    $after = New-OracleCapture "$label-after"
    if (-not $run.json) { Fail "$($label): uninstall printed no JSON (exit $($run.exit))"; return }
    $json = $run.json
    # Not $results: that is the results folder, which the functions called from here use too.
    $outcomes = @($json.results | Where-Object { $_ })
    foreach ($outcome in $outcomes) { Log "  $($outcome.name): $($outcome.outcome), exit code $($outcome.exit_code), $([int]$outcome.elapsed_ms) ms, trace $($outcome.trace)" }
    if ($outcomes.Count -ne 1 -or $outcomes[0].outcome -notin 'removed', 'reboot-required') { Fail "$($label): the uninstall didn't remove $($sample.name)" }
    if ($outcomes.Count -ge 1 -and $outcomes[0].trace -ne $record.id) { Fail "$($label): results[].trace is '$($outcomes[0].trace)', not $($record.id)" }
    elseif ($outcomes.Count -ge 1) { Log "  OK: results[].trace names the round's trace" }
    $use = Test-TraceUse $json.leftover_scan $label $record.id $program $record
    if ($use -and [int]$use.gone -lt 1) { Fail "$($label): nothing the trace saw is gone after the uninstaller ran" }
    Write-TracedItems $json.leftover_scan
    Test-RemovalDone $json.leftover_removal $label -Optional
    Check-Safe @($json.leftover_scan.leftovers) "$label scan"
    if ($json.leftover_removal) { Check-Safe @($json.leftover_removal.items) "$label removal" }
    Test-Revert $record $after $json "$label-revert"
    $show = Invoke-Trace @('show', $record.id, '--json') "$label-show"
    if (-not $show.json) { Fail "$($label): trace show printed no JSON (exit $($show.exit))" }
    elseif (-not $show.json.trace.unresolved_since) { Fail "$($label): the uninstalled program's trace has no unresolved_since: retention didn't start its 30 days" }
    else { Log "  OK: the trace is kept 30 days from $($show.json.trace.unresolved_since)" }
}

# ---------------------------------------------------------------------------------------------
# Rounds

function Invoke-Round([string]$name, [scriptblock]$body) {
    try {
        & $body
    } catch {
        Fail "$($name): $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
        try { Close-OpenTrace } catch { Log "  cleanup failed: $($_.Exception.Message)" }
    }
}

# One traced installation between two oracle captures. -Launch: `trace run` with this command line,
# between the captures; what the oracle saw change between a capture and the trace is logged as
# KNOWN where its change time shows it (`gaps` in the record). Otherwise -Install runs between
# `trace start` and `trace stop` (its output is kept as `install`), and the captures are taken
# while the trace is open, the second once Windows has had time to react. Returns the round's
# record, also kept in $traced, or $null.
function Invoke-TracedRound([string]$label, [string]$description, [string[]]$Launch, [scriptblock]$Install) {
    Close-OpenTrace
    Start-Sleep -Seconds $settleSeconds  # Windows' reactions to the round before
    $installed = $null
    $gaps = @()
    $journalBefore = $null
    $journalAfter = $null
    $inside = $null
    if ($Launch) {
        $watch = [AvelNameWatch]::new('C:\')
        try {
            $journalBefore = [AvelJournal]::Query('C:')
            $preStart = Get-FileTimeNow
            $pre = New-OracleCapture "$label-pre"
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $runStart = Get-FileTimeNow
            $result = Invoke-Trace (@('run', '--description', $description, '--json', '--') + $Launch) "$label-run" 1200
            $runEnd = Get-FileTimeNow
            $seconds = $clock.Elapsed.TotalSeconds
            $post = New-OracleCapture "$label-post"
            $postEnd = Get-FileTimeNow
            $journalAfter = [AvelJournal]::Query('C:')
        } finally {
            $watch.Dispose()
        }
        if ($watch.Overflowed) { Log '  the name watch overflowed: items around the trace may lack evidence' }
        $window = Get-TraceWindow "$($result.json.trace.id)" $runStart $runEnd
        $gaps = @(@{ from = $preStart; to = $window.start; keysTo = $window.keys; what = 'before the trace started'; watch = $watch },
                  @{ from = $window.stop; to = $postEnd; what = 'after the trace stopped'; watch = $watch })
        Log ("  outside the trace (UTC): {0} to {1} (keys and values: to {2}), {3} to {4}" -f (Format-FileTime $preStart),
             (Format-FileTime $window.start), (Format-FileTime $window.keys), (Format-FileTime $window.stop), (Format-FileTime $postEnd))
        if ($window.usns -and $journalBefore -and $journalAfter) {
            Log "  the trace's journal: $($window.usns); before the capture before at $($journalBefore[2]), after the capture after at $($journalAfter[2])"
        }
    } else {
        $start = Invoke-Trace @('start', '--description', $description, '--json') "$label-start"
        if ($start.exit -ne 0 -or $start.json.trace.state -ne 'open') {
            Fail "$($label): trace start: exit $($start.exit), state $($start.json.trace.state)"
            return $null
        }
        Start-Hold
        try {
            $journalBefore = [AvelJournal]::Query('C:')
            $pre = New-OracleCapture "$label-pre"
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $installed = & $Install
            $seconds = $clock.Elapsed.TotalSeconds
            Start-Sleep -Seconds $settleSeconds
            $journalPost = [AvelJournal]::Query('C:')
            $postTime = Get-FileTimeNow
            $post = New-OracleCapture "$label-post"
            $journalAfter = [AvelJournal]::Query('C:')
        } finally {
            $result = Invoke-Trace @('stop', '--json') "$label-stop"
            $stopTime = Get-FileTimeNow
            Stop-Hold
        }
        $inside = New-InsideWindow "$($result.json.trace.id)" $journalBefore $journalPost $postTime $stopTime
    }
    if (-not $result.json) { Fail "$($label): avel-cli printed no JSON (exit $($result.exit))"; return $null }
    Write-TraceSummary $result.json
    if ($result.json.trace.stop_reason -eq 'reboot-pending') {
        Fail "$($label): the installer asked for a restart; the trace stays open, stopping it"
        $null = Invoke-Trace @('stop', '--json') "$label-stop-after-reboot-request"
    }
    $all = Get-AllChanges $result $label
    Write-NoiseCounts $all
    $record = @{ id = $result.json.trace.id; pre = $pre; post = $post; result = $result.json; all = $all
                 seconds = $seconds; install = $installed; lines = @(Get-CaptureDiff $pre $post); gaps = $gaps
                 journal = @{ before = $journalBefore; after = $journalAfter }; inside = $inside }
    $traced[$label] = $record
    return $record
}

function Invoke-UnitTests {
    Log ''
    Log 'Unit tests that need elevation (USN journal, installer watch, registry snapshot, trace session) or the Sandbox''s user profile'
    # stdout only: under 'Stop', Windows PowerShell turns a native program's stderr into an error.
    # KeysThatDenyAccessAreUnreadable skips wherever SeBackupPrivilege can be enabled, as it always
    # can elevated; the host runs it.
    $filter = 'UsnLiveTest.*:InstallerWatch*:RegistrySnapshot*:TraceSessionLiveTest.*:UserProfilesTest.*' +
              '-RegistrySnapshotTest.KeysThatDenyAccessAreUnreadable'
    $tests = @(& "$bin\avel_core_tests.exe" "--gtest_filter=$filter")
    $code = $LASTEXITCODE
    $tests | Where-Object { $_ -match '^\[  (PASSED|FAILED|SKIPPED) ' } | ForEach-Object { Log "  $_" }
    # What each failed test printed, from its RUN line on.
    $output = [System.Collections.Generic.List[string]]::new()
    foreach ($line in $tests) {
        if ($line -match '^\[ RUN      \] ') { $output.Clear() }
        $output.Add($line)
        if ($line -match '^\[  FAILED  \] ([^ ]+) \(') {
            Log "  output of $($Matches[1]):"
            foreach ($text in @($output | Select-Object -First 40)) { Log "    $text" }
        }
    }
    $ran = 0
    foreach ($line in $tests) { if ($line -match '^\[==========\] ([0-9]+) tests? from .* ran') { $ran = [int]$Matches[1] } }
    if ($code -ne 0 -or $ran -eq 0 -or ($tests -match '^\[  SKIPPED \]')) {
        Fail "the tracker's unit tests did not all pass elevated (exit $code, $ran ran)"
    } else {
        Log "  OK: $ran unit tests pass elevated"
    }
}

# Opens and closes a stock app the way a user does (through the shell); says how it went.
function Invoke-StockApp([string]$app) {
    $name = Split-Path $app -Leaf
    try {
        $process = Start-Process $app -PassThru
        Start-Sleep -Seconds 3
        $null = $process.CloseMainWindow()
        if (-not $process.WaitForExit(10000)) { $process.Kill(); return "$name started and closed (killed)" }
        return "$name started and closed"
    } catch {
        return "$name failed: $($_.Exception.Message)"
    }
}

function Invoke-IdleRound {
    Log ''
    Log 'Round 0: idle - a minute with nothing installed, a stock app opened and closed'
    Close-OpenTrace
    # Notepad is a packaged app now, missing from the Sandbox. Winver keeps no settings (Character
    # Map saves its own, a real change).
    $app = @('winver.exe', 'notepad.exe' | ForEach-Object { Join-Path $env:SystemRoot "System32\$_" } |
        Where-Object { Test-Path -LiteralPath $_ })[0]
    if ($app) {
        # Once before the trace: the shell's first launch of a program caches its name in Explorer's
        # MuiCache (creating the key in a new profile), the user's change, never noise (removal
        # relies on it), and one a profile in use has long had.
        Log "  before the trace: $(Invoke-StockApp $app)"
        Start-Sleep -Seconds $settleSeconds
    }
    $start = Invoke-Trace @('start', '--description', 'idle', '--json') 'r0-start'
    if ($start.exit -ne 0 -or $start.json.trace.state -ne 'open') {
        Fail "trace start: exit $($start.exit), state $($start.json.trace.state)"
        return
    }
    Start-Hold
    try {
        $journalBefore = [AvelJournal]::Query('C:')
        $pre = New-OracleCapture 'r0-idle-pre'
        Start-Sleep -Seconds 60
        $appNote = if ($app) { Invoke-StockApp $app } else { 'no stock app found' }
        Start-Sleep -Seconds $settleSeconds
        $journalPost = [AvelJournal]::Query('C:')
        $postTime = Get-FileTimeNow
        $post = New-OracleCapture 'r0-idle-post'
        $journalAfter = [AvelJournal]::Query('C:')
    } finally {
        $stop = Invoke-Trace @('stop', '--json') 'r0-stop'
        $stopTime = Get-FileTimeNow
        Stop-Hold
    }
    Log "  $appNote"
    $inside = New-InsideWindow "$($stop.json.trace.id)" $journalBefore $journalPost $postTime $stopTime
    if (-not $stop.json) { Fail "trace stop printed no JSON (exit $($stop.exit))"; return }
    $json = $stop.json
    Write-TraceSummary $json
    if ($json.trace.state -ne 'complete') { Fail "idle: the trace is $($json.trace.state)" }
    if ($json.trace.stop_reason -ne 'manual') { Fail "idle: stop reason $($json.trace.stop_reason), not manual" }
    $changes = @($json.changes | Where-Object { $_ })
    if ($changes.Count -or $json.trace.counts.changes) {
        Fail "idle: $([Math]::Max($changes.Count, [int]$json.trace.counts.changes)) change(s) that aren't noise"
        foreach ($change in $changes) { Log "      $(Format-Change $change)" }
    } else {
        Log '  OK: no change that isn''t noise'
    }
    $all = Get-AllChanges $stop 'r0'
    Write-NoiseCounts $all
    Test-Completeness @(Get-CaptureDiff $pre $post) $all 'r0-idle' -Calibrate -Journal @{ before = $journalBefore; after = $journalAfter } `
        -Inside $inside
    Log "  $($background.Count) path(s) the trace didn't account for while idle; later rounds log them as KNOWN"
}

function Invoke-FixtureRound {
    Log ''
    Log 'Round 1: Contoso Trace Fixture installed between trace start and trace stop'
    Remove-TraceFixture
    Initialize-TraceFixture
    $paths = Get-TraceFixturePaths
    $command = @(Get-TraceFixtureCommand)
    $file = $command[0]
    $arguments = @($command | Select-Object -Skip 1 | ForEach-Object { Format-Argument $_ }) -join ' '
    # A closure sees its variables, but not this script's functions.
    $install = { (Start-Process $file $arguments -Wait -PassThru -WindowStyle Hidden).ExitCode }.GetNewClosure()
    $record = Invoke-TracedRound 'r1-fixture' 'Contoso Trace Fixture' -Install $install
    if (-not $record) { return }
    $record.paths = $paths  # For 5b's exact checks
    $json = $record.result
    if ($record.install -ne 0) { Fail "the fixture's installer returned $($record.install)" }
    if ($json.trace.state -ne 'complete' -or $json.trace.mode -ne 'manual' -or $json.trace.stop_reason -ne 'manual') {
        Fail "round 1: state $($json.trace.state), mode $($json.trace.mode), stop reason $($json.trace.stop_reason)"
    }
    foreach ($change in @($json.changes | Where-Object { $_ })) { Log "    $(Format-Change $change)" }
    Test-Completeness $record.lines $record.all 'r1-fixture' -Journal $record.journal -Inside $record.inside
    Test-Precision $json $record.lines 'round 1' -Strict
    Test-FixtureTrace $json 'round 1' $paths
}

function Invoke-SampleRound($sample, [int]$number) {
    Log ''
    Log "Round $($number): $($sample.name) through trace run"
    if (-not (Test-Path $sample.command[0])) { Fail "$($sample.command[0]) is missing (run Get-SampleInstallers.ps1)"; return }
    $record = Invoke-TracedRound $sample.label $sample.name -Launch $sample.command
    if (-not $record) { return }
    $json = $record.result
    Log "  took $([int]$record.seconds) s"
    if ($json.trace.stop_reason -ne 'installer-done') { Fail "$($sample.name): stop reason $($json.trace.stop_reason), not installer-done" }
    if ($json.trace.state -ne 'complete' -or $json.trace.mode -ne 'launch') { Fail "$($sample.name): state $($json.trace.state), mode $($json.trace.mode)" }
    if ($json.trace.watch.exit_code -notin 0, 3010) { Fail "$($sample.name): the installer exited with $($json.trace.watch.exit_code)" }
    foreach ($change in @($json.changes | Where-Object { $_ })) { Log "    $(Format-Change $change)" }
    Test-Completeness $record.lines $record.all $sample.label -Gaps $record.gaps -Journal $record.journal
    Test-Precision $json $record.lines $sample.label
}

function Invoke-RunAfterRound {
    Log ''
    Log 'Round 5: the fixture through trace run, starting its app at the end'
    Close-OpenTrace
    Remove-TraceFixture
    Initialize-TraceFixture
    $paths = Get-TraceFixturePaths
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $run = Invoke-Trace (@('run', '--description', 'Contoso Trace Fixture, run after', '--json', '--') +
                         @(Get-TraceFixtureCommand -RunAfter)) 'r5-run-after' 300
    $seconds = $clock.Elapsed.TotalSeconds
    if (-not $run.json) { Fail "trace run printed no JSON (exit $($run.exit))"; return }
    $json = $run.json
    $traced['r5-run-after'] = @{ id = $json.trace.id; result = $json; seconds = $seconds }
    Write-TraceSummary $json
    if ($json.trace.stop_reason -ne 'installer-done' -or $json.trace.watch.end -ne 'installer-done') {
        Fail "round 5: stop reason $($json.trace.stop_reason), watch end $($json.trace.watch.end)"
    }
    if ($seconds -ge 45) { Fail "round 5: trace run took $([int]$seconds) s, not under 45 s" }
    else { Log "  OK: trace run ended by itself after $([int]$seconds) s" }
    $left = @($json.trace.watch.left_running | Where-Object { $_ -and $_.image -like "*\Contoso Trace\app.exe" })
    if (-not $left) { Fail 'round 5: the app is not listed as left running' }
    else { Log "  OK: the app is listed as left running (pid $($left[0].pid))" }
    $running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$traceRoot\app.exe" })
    if (-not $running) { Fail 'round 5: the app is not running any more' }
    else { Log '  OK: the app is still running'; $running | Stop-Process -Force }
    Test-FixtureTrace $json 'round 5' $paths
}

function Invoke-KilledRunRound {
    Log ''
    Log 'Round 6a: trace run killed after its snapshot; trace stop must finish the trace'
    Close-OpenTrace
    Remove-TraceFixture
    Initialize-TraceFixture
    $before = @(Get-TraceList | ForEach-Object { $_.id })
    $arguments = @(@('trace', 'run', '--description', 'killed', '--json', '--') + @(Get-TraceFixtureCommand) |
        ForEach-Object { Format-Argument $_ }) -join ' '
    $out = Join-Path $env:TEMP 'avel-r6-killed.out'
    $process = Start-Process $cli $arguments -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    $null = $process.Handle
    $open = $null
    $deadline = (Get-Date).AddSeconds(120)
    while (-not $open -and -not $process.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $open = @(Get-TraceList | Where-Object { $_.state -eq 'open' -and $before -notcontains $_.id })[0]
    }
    if (-not $open) {
        Fail "round 6a: no trace became open (avel-cli $(if ($process.HasExited) { "exited with $($process.ExitCode)" } else { 'still running' }))"
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        return
    }
    # Let the installer start, so the trace has something to record.
    $installer = $null
    $deadline = (Get-Date).AddSeconds(30)
    while (-not $installer -and (Get-Date) -lt $deadline) {
        $installer = @(Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" |
            Where-Object { $_.CommandLine -like '*ContosoTraceSetup\install.ps1*' })[0]
        if (-not $installer) { Start-Sleep -Milliseconds 250 }
    }
    if ($process.HasExited) { Fail "round 6a: trace run ended (exit $($process.ExitCode)) before it could be killed"; return }
    Stop-Process -Id $process.Id -Force
    $process.WaitForExit()
    Log "  trace $($open.id) open; avel-cli killed $(if ($installer) { "while the installer (pid $($installer.ProcessId)) ran" } else { 'before the installer started' })"
    if ($installer) { Wait-Process -Id $installer.ProcessId -Timeout 120 -ErrorAction SilentlyContinue }
    $state = @(Get-TraceList | Where-Object { $_.id -eq $open.id })[0].state
    Log "  after the kill the trace is $state"
    $stop = Invoke-Trace @('stop', '--json') 'r6-killed-stop'
    if (-not $stop.json) { Fail "round 6a: trace stop printed no JSON (exit $($stop.exit))"; return }
    $json = $stop.json
    Write-TraceSummary $json
    if ($json.trace.id -ne $open.id) { Fail "round 6a: trace stop finished $($json.trace.id), not $($open.id)" }
    if ($json.trace.state -ne 'complete') { Fail "round 6a: the trace is $($json.trace.state), not complete" }
    else { Log '  OK: trace stop finished the killed run''s trace' }
    if (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso Trace') {
        if (-not @($json.changes | Where-Object { $_ -and $_.change -eq 'created' -and $_.location.path -eq $traceRoot })) {
            Fail "round 6a: the installer finished, but the trace doesn't list $traceRoot"
        } else { Log "  OK: the trace lists what the installer did after avel-cli was killed" }
    } else {
        Log '  the installer did not finish (it ended with avel-cli)'
    }
    Remove-TraceFixture
}

function Invoke-JournalReplacedRound {
    Log ''
    Log 'Round 6b: the USN journal deleted and recreated during a trace'
    Close-OpenTrace
    $start = Invoke-Trace @('start', '--description', 'journal replaced', '--json') 'r6-journal-start'
    if ($start.exit -ne 0 -or $start.json.trace.state -ne 'open') { Fail "trace start: exit $($start.exit)"; return }
    $probe = 'HKLM\SOFTWARE\Contoso Trace Probe'
    New-Item "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Contoso Trace Probe" -Force |
        New-ItemProperty -Name Marker -Value 'journal replaced' -Force | Out-Null
    $deleted = Invoke-Native 'fsutil.exe' 'usn deletejournal /d C:'
    $created = Invoke-Native 'fsutil.exe' 'usn createjournal m=33554432 a=4194304 C:'
    Log "  fsutil usn deletejournal: exit $deleted; createjournal: exit $created"
    if ($deleted -ne 0 -or $created -ne 0) { Fail 'round 6b: fsutil failed' }
    $stop = Invoke-Trace @('stop', '--json') 'r6-journal-stop'
    Remove-Item "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Contoso Trace Probe" -Recurse -Force
    if (-not $stop.json) { Fail "round 6b: trace stop printed no JSON (exit $($stop.exit))"; return }
    $json = $stop.json
    Write-TraceSummary $json
    if ($stop.exit -ne 0) { Fail "round 6b: trace stop exited with $($stop.exit)" }
    if ($json.trace.state -ne 'incomplete' -or $json.trace.complete) { Fail "round 6b: the trace is $($json.trace.state), complete $($json.trace.complete)" }
    else { Log '  OK: the trace is incomplete' }
    $volume = @($json.trace.volumes | Where-Object { $_ -and $_.root -eq 'C:\' })
    if (-not $volume -or $volume[0].status -ne 'replaced') { Fail "round 6b: C: is $($volume.status), not replaced" }
    if (-not @($json.trace.incomplete_reasons | Where-Object { $_ -and $_.volume -eq 'C:\' -and $_.status -eq 'replaced' })) {
        Fail 'round 6b: incomplete_reasons does not name C: as replaced'
    } elseif ($volume) { Log '  OK: C: is reported replaced' }
    if (-not @($json.changes | Where-Object { $_ -and $_.change -eq 'created' -and $_.location.kind -eq 'registry-key' -and $_.location.key -eq $probe })) {
        Fail "round 6b: the registry part is missing: $probe isn't reported"
    } else { Log '  OK: the registry part is still reported' }
}

# Diagnostics, raw: how the base layer's placeholders look to programs (docs/trace.md, Windows
# containers).
function Write-SystemFacts {
    foreach ($path in "$env:SystemRoot\System32\wbem", "$env:SystemRoot\System32\kernel32.dll") {
        Log "Attributes of $($path) as programs see them: $(Get-ItemFacts $path)"
    }
}

# A container's base layer (Windows Sandbox): two of Windows' sounds, which nothing here plays,
# one deleted and one renamed after takeown and icacls (as an installer replacing a system file
# would), between `trace start` and `trace stop`. The container leaves a tombstone under each old
# name, a new item programs don't see: the trace must report the deletion and the rename (the
# security changes go with them) and nothing created, and account for the oracle's diff.
function Invoke-ContainerRound {
    Log ''
    Log "Round 0b: a container's base layer - a system file deleted, another renamed"
    $sounds = @(Get-ChildItem -LiteralPath "$env:SystemRoot\Media" -Filter '*.wav' -File -ErrorAction SilentlyContinue |
        Sort-Object Name | Select-Object -First 2)
    if ($sounds.Count -lt 2) { Fail 'round 0b: there are no two sounds in the Media folder'; return }
    $gone = $sounds[0].FullName
    $old = $sounds[1].FullName
    $new = "$old.avel"
    $change = {
        $ErrorActionPreference = 'Stop'
        foreach ($path in $gone, $old) {
            $null = & takeown.exe /f $path /a
            $null = & icacls.exe $path /grant '*S-1-5-32-544:F'
        }
        Remove-Item -LiteralPath $gone -Force
        Rename-Item -LiteralPath $old -NewName (Split-Path $new -Leaf)
        0
    }.GetNewClosure()
    $record = Invoke-TracedRound 'r0b-container' "a container's base layer" -Install $change
    $null = $traced.Remove('r0b-container')  # Not an installation
    if (-not $record) { return }
    $json = $record.result
    if ($json.trace.state -ne 'complete') { Fail "round 0b: the trace is $($json.trace.state)" }
    $media = Split-Path $gone
    $all = @($record.all.changes | Where-Object { $_ -and $_.location.path -and (Split-Path $_.location.path) -eq $media })
    foreach ($change in $all) { Log "    $(Format-Change $change)" }
    $reported = @($all | Where-Object { $_.noise -eq 'none' })
    $deleted = @($reported | Where-Object { $_.change -eq 'deleted' -and $_.location.path -eq $gone })
    $renamed = @($reported | Where-Object { $_.change -eq 'renamed' -and $_.location.path -eq $new -and $_.old_path -eq $old })
    $tombstones = @($all | Where-Object { $_.change -eq 'created' -and $_.location.path -in $gone, $old })
    if ($deleted.Count -ne 1 -or $renamed.Count -ne 1 -or $reported.Count -ne 2) {
        Fail "round 0b: want exactly 'deleted $gone' and 'renamed $new <- $old' in $media, got $($reported.Count) change(s)"
    } else {
        Log "  OK: the deletion and the rename, nothing else in $media"
    }
    if ($tombstones) { Fail "round 0b: $($tombstones.Count) tombstone(s) reported as created" }
    if ($deleted.Count -ne 1 -or $renamed.Count -ne 1 -or $tombstones) {
        Write-ItemRecords @(@($gone, $old, $new) | ForEach-Object { [pscustomobject]@{ path = $_ } }) @{}
    }
    Test-Completeness $record.lines $record.all 'r0b-container' -Journal $record.journal -Inside $record.inside
    Test-Precision $json $record.lines 'round 0b' -Strict
}
try {
    Log "Avel Phase 5 trace scenario, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME ($sid), $([Environment]::ProcessorCount) processors"
    Log "avel-cli: $((& $cli version) -join ' ')"
    Write-SystemFacts
    if (Test-Path $captures) { Remove-Item $captures -Recurse -Force }

    Invoke-Round 'unit tests' { Invoke-UnitTests }
    Invoke-Round 'round 0' { Invoke-IdleRound }
    Invoke-Round 'round 0b' { Invoke-ContainerRound }
    Invoke-Round 'round 1' { Invoke-FixtureRound }
    Invoke-Round 'round 1 (5b)' { Invoke-FixtureRevertRound $traced['r1-fixture'] }
    $number = 2
    foreach ($sample in $traceSamples) {
        Invoke-Round "round $number" { Invoke-SampleRound $sample $number }
        Invoke-Round "round $number (5b)" { Invoke-UninstallByTrace $sample $number }
        $number++
    }
    Invoke-Round 'round 5' { Invoke-RunAfterRound }
    Invoke-Round 'round 6a' { Invoke-KilledRunRound }
    Invoke-Round 'round 6b' { Invoke-JournalReplacedRound }
} catch {
    Fail "scenario error: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
    Write-Report
}
