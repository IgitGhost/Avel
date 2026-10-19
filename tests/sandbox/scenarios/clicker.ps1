# Clicks through uninstaller dialogs the way a user would, until -StopFile appears. Started by
# uninstall.ps1 as its own PowerShell process (background jobs don't see the desktop's windows).
#
# Presses "Uninstall", "Yes" or "Next" first; "Finish", "Close" or "OK" only when nothing else is
# offered, so a maintenance page's "Close" isn't hit before its "Uninstall". The sandbox runs in the
# host's Windows language, so labels are matched in English and Russian (built from code points to
# keep this file ASCII), and the standard Yes button (control ID 6) is recognized by its ID.
#
# Classic buttons (msiexec, NSIS, Inno, WiX Burn) are found as child windows and pressed by posting
# WM_COMMAND to their parent: managed UI Automation reports them as panes, not buttons, and has no
# way to press them. Buttons without a window of their own (task dialogs, DirectUI) are found and
# invoked through UI Automation.
#
# -WindowTitle limits the clicker to windows whose title matches the regex, so it can be tried on a
# dev machine without pressing buttons in unrelated windows; it also lifts the process ignore list.
param(
    [Parameter(Mandatory)][string]$StopFile,
    [Parameter(Mandatory)][string]$LogFile,
    [string]$WindowTitle
)

function Write-Log([string]$text) { Add-Content $LogFile ('{0:HH:mm:ss} {1}' -f (Get-Date), $text) -Encoding utf8 }

function From-CodePoints([int[]]$codes) { -join ($codes | ForEach-Object { [char]$_ }) }
$ru = @{
    yes = From-CodePoints 0x414, 0x430
    uninstall = From-CodePoints 0x423, 0x434, 0x430, 0x43B, 0x438, 0x442, 0x44C
    next = From-CodePoints 0x414, 0x430, 0x43B, 0x435, 0x435
    deinstall = From-CodePoints 0x414, 0x435, 0x438, 0x43D, 0x441, 0x442, 0x430, 0x43B, 0x43B, 0x438, 0x440, 0x43E, 0x432, 0x430, 0x442, 0x44C
    done = From-CodePoints 0x413, 0x43E, 0x442, 0x43E, 0x432, 0x43E
    close = From-CodePoints 0x417, 0x430, 0x43A, 0x440, 0x44B, 0x442, 0x44C
    ok = From-CodePoints 0x41E, 0x41A
}
$forwardLabels = "^(Uninstall|Yes|Next.*|Remove|$($ru.yes)|$($ru.uninstall)|$($ru.next).*|$($ru.deinstall))$"
$finishLabels = "^(Finish|Close|OK|$($ru.done)|$($ru.close)|$($ru.ok))$"
$idYes = 6

try {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [System.Windows.Automation.AutomationElement]
    $isButton = New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class AvelClicker {
    public class Button {
        public IntPtr Handle;
        public int Id;
        public string Text;
    }

    delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr hwnd);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr hwnd);
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr hwnd, int index);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint msg, IntPtr wParam, StringBuilder lParam,
        uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);

    const int GWL_STYLE = -16;
    const uint WM_GETTEXT = 0x000D, WM_COMMAND = 0x0111, SMTO_ABORTIFHUNG = 0x0002;

    public static List<IntPtr> TopLevelWindows() {
        var windows = new List<IntPtr>();
        EnumWindows((hwnd, lParam) => { if (IsWindowVisible(hwnd)) windows.Add(hwnd); return true; }, IntPtr.Zero);
        return windows;
    }

    public static uint ProcessId(IntPtr hwnd) {
        uint processId;
        GetWindowThreadProcessId(hwnd, out processId);
        return processId;
    }

    public static string Title(IntPtr hwnd) {
        var text = new StringBuilder(512);
        GetWindowText(hwnd, text, text.Capacity);
        return text.ToString();
    }

    // Visible, enabled push buttons: the standard BUTTON class and VCL's subclasses of it (Inno's
    // TNewButton), without check boxes, radio buttons and group boxes, which share the class.
    public static List<Button> Buttons(IntPtr window) {
        var buttons = new List<Button>();
        EnumChildWindows(window, (hwnd, lParam) => {
            var className = new StringBuilder(256);
            GetClassName(hwnd, className, className.Capacity);
            if (!className.ToString().EndsWith("Button", StringComparison.OrdinalIgnoreCase)) return true;
            if (!IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd)) return true;
            int type = GetWindowLong(hwnd, GWL_STYLE) & 0xF;
            bool push = type == 0 || type == 1 || type >= 0xC;  // (DEF)PUSHBUTTON, (DEF)SPLITBUTTON, (DEF)COMMANDLINK
            if (!push) return true;
            var text = new StringBuilder(256);
            IntPtr ignored;
            SendMessageTimeout(hwnd, WM_GETTEXT, (IntPtr)text.Capacity, text, SMTO_ABORTIFHUNG, 500, out ignored);
            buttons.Add(new Button { Handle = hwnd, Id = GetDlgCtrlID(hwnd), Text = text.ToString().Replace("&", "").Trim() });
            return true;
        }, IntPtr.Zero);
        return buttons;
    }

    // Posted, not sent: a button that opens a modal loop would otherwise block the clicker.
    public static bool Click(Button button) {
        IntPtr wParam = (IntPtr)(button.Id & 0xFFFF);  // BN_CLICKED (0) in the high word
        return PostMessage(GetParent(button.Handle), WM_COMMAND, wParam, button.Handle);
    }
}
'@
} catch {
    Write-Log "clicker failed to start: $($_.Exception.Message)"
    exit 1
}
$integrity = (whoami /groups | Select-String 'Mandatory Label') -replace '^Mandatory Label\\(\S+).*', '$1'
Write-Log "clicker started (integrity: $integrity)"

# Buttons drawn without a window of their own, read through UI Automation.
function Get-WindowlessButtons([IntPtr]$hwnd) {
    $window = $A::FromHandle($hwnd)
    @($window.FindAll('Descendants', $isButton) | Where-Object {
        $_.Current.IsEnabled -and $_.Current.NativeWindowHandle -eq 0
    } | ForEach-Object {
        [pscustomobject]@{ Element = $_; Id = 0; Text = $_.Current.Name.Replace('&', '').Trim() }
    })
}

$ignored = 'explorer', 'powershell', 'conhost', 'ShellExperienceHost', 'SearchHost', 'StartMenuExperienceHost',
    'TextInputHost', 'ApplicationFrameHost', 'SystemSettings', 'avel-cli', 'avel', 'sihost', 'ctfmon',
    'OpenConsole', 'WindowsTerminal'
$seen = @{}  # Windows already reported, to log each new one once
while (-not (Test-Path $StopFile)) {
    foreach ($hwnd in [AvelClicker]::TopLevelWindows()) {
        try {
            $title = [AvelClicker]::Title($hwnd)
            if ($WindowTitle) {
                if ($title -notmatch $WindowTitle) { continue }
            } elseif (-not $title) {
                continue
            }
            $name = (Get-Process -Id ([AvelClicker]::ProcessId($hwnd))).ProcessName
            if (-not $WindowTitle -and $ignored -contains $name) { continue }

            $buttons = @([AvelClicker]::Buttons($hwnd))
            $via = 'win32'
            if (-not $buttons) {
                $buttons = Get-WindowlessButtons $hwnd
                $via = 'uia'
            }
            $key = "$name|$title|$(($buttons | ForEach-Object { $_.Text }) -join ',')"
            if (-not $seen.ContainsKey($key)) {
                $seen[$key] = $true
                Write-Log ('window {0} "{1}", buttons ({2}): {3}' -f $name, $title, $via,
                    (($buttons | ForEach-Object { '{0}[{1}]' -f $_.Text, $_.Id }) -join ', '))
            }
            $target = $buttons | Where-Object { $_.Text -match $forwardLabels -or $_.Id -eq $idYes } |
                Select-Object -First 1
            if (-not $target) {
                $target = $buttons | Where-Object { $_.Text -match $finishLabels } | Select-Object -First 1
            }
            if ($target) {
                Write-Log ('click {0} "{1}": {2}' -f $name, $title, $target.Text)
                if ($target.Element) {
                    $target.Element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                } elseif (-not [AvelClicker]::Click($target)) {
                    Write-Log "  posting the click failed"
                }
                Start-Sleep -Milliseconds 1500
            }
        } catch {
            # Windows come and go while they're inspected.
        }
    }
    Start-Sleep -Milliseconds 700
}
Write-Log 'clicker stopped'
