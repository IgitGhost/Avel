# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario trace). Phase 5a acceptance test for
# Avel Trace, the install tracker (docs/trace.md), checked against an independent oracle:
# avel_capture (tests/helpers/capture.cpp) captures every file on C: and the registry roots the
# tracker watches right before and right after each traced installation, and the diff of the two
# captures is what really changed. In rounds 0, 0b and 1 both captures are taken while the trace is
# open, so the trace must account for every line of the diff, unless its item demonstrably
# changed again inside the trace but outside the captures (Get-AgainReason); around `trace run`
# (rounds 2-4) they can't be, and a line is put down to the moments between a capture and the
# trace's own window (from its trace.json) only with evidence: its creation, last write or key's
# last write time lies there, or a watch of the volume's names saw it go away (or a created one
# appear) then. A created
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
#   2-4. 7-Zip (MSI), Notepad++ (NSIS) and Inno Setup through `trace run`: it must end by itself
#      (installer-done) and be complete against the oracle; precision exceptions are logged;
#   5. the fixture through `trace run`, starting its app at the end: done within 45 s, the app
#      listed as left running and still running;
#   6. a `trace run` killed after its snapshot is finished by `trace stop`; a USN journal deleted
#      and recreated during a trace makes it incomplete (C: "replaced"), the registry still reported.
# Rounds 1-5 keep what they traced in $traced (trace id, oracle captures, results), for later rounds
# (5b's trace-based removal and undo) to compare against.
# Writes results\trace.json (avel-cli's output), results\trace.log, results\trace-oracle-<round>.txt
# (each round's oracle diff, every line with what accounted for it) and, last, results\trace.txt.

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$installers = 'C:\Avel\sandbox\installers'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$scenario = 'trace'
. "$PSScriptRoot\common.ps1"
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
$traceSamples = @(
    @{ label = 'r2-7zip'; name = '7-Zip (MSI)'; command = @("$installers\7z-x64.msi", '/qn', '/norestart') },
    @{ label = 'r3-notepadpp'; name = 'Notepad++ (NSIS)'; command = @("$installers\npp-installer-x64.exe", '/S') },
    @{ label = 'r4-innosetup'; name = 'Inno Setup'
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
# Nothing tells when something was deleted.
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
# time) then, by itself or with a folder above it (see Find-MovedFolder).
function Get-GapReason($line, $gaps) {
    if (-not $gaps) { return $null }
    $file = $line.kind -in 'F', 'D'
    if ($line.sign -eq '-') {
        if (-not $file) { return $null }
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
    if (-not $id) { Write-Held "    task $($task): no Id in TaskCache\Tree, its entries by id can't be named" }
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
        if ($item.location.kind -in 'service', 'driver', 'task', 'firewall-rule') {
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
    Log "Avel Phase 5a trace scenario, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME ($sid), $([Environment]::ProcessorCount) processors"
    Log "avel-cli: $((& $cli version) -join ' ')"
    Write-SystemFacts
    if (Test-Path $captures) { Remove-Item $captures -Recurse -Force }

    Invoke-Round 'unit tests' { Invoke-UnitTests }
    Invoke-Round 'round 0' { Invoke-IdleRound }
    Invoke-Round 'round 0b' { Invoke-ContainerRound }
    Invoke-Round 'round 1' { Invoke-FixtureRound }
    $number = 2
    foreach ($sample in $traceSamples) {
        Invoke-Round "round $number" { Invoke-SampleRound $sample $number }
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
