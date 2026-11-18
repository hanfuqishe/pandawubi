# 收拾桌面上残留的「中 简 ，。 ⚙」悬浮状态栏
#
# 背景：旧实现里 TSF 每个线程各建一条状态栏，切走应用又收不掉，于是每条栏都留在了
# 桌面上（实测一次 10 条、来自 8 个进程）。新代码会让每条栏只在宿主进程位于前台时
# 显示，但这些"历史遗留"窗口属于已经在跑的旧 DLL 副本，必须等宿主进程重启才会消失。
# 这个脚本只是把它们隐藏起来，不杀进程、不碰注册表，随时可以重开。
#
# 用法: powershell -ExecutionPolicy Bypass -File .\tools\hide-stale-bars.ps1 [-List]
param([switch]$List, [switch]$Close)

$code = @'
using System;
using System.Runtime.InteropServices;
public class BarWnd {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string win);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
Add-Type -TypeDefinition $code -ErrorAction Stop

$cls = 'PandaWubiStatusBar'
$found = 0
$hidden = 0
$rows = @()

$h = [IntPtr]::Zero
while ($true) {
    $h = [BarWnd]::FindWindowEx([IntPtr]::Zero, $h, $cls, $null)
    if ($h -eq [IntPtr]::Zero) { break }
    $found++
    $r = New-Object BarWnd+RECT
    $null = [BarWnd]::GetWindowRect($h, [ref]$r)
    $procId = 0
    $null = [BarWnd]::GetWindowThreadProcessId($h, [ref]$procId)
    $vis = [BarWnd]::IsWindowVisible($h)
    $rows += [pscustomobject]@{
        HWND   = ('0x{0:X}' -f $h.ToInt64())
        PID    = $procId
        Visible = $vis
        Pos    = "$($r.L),$($r.T)"
    }
    if (-not $List) {
        if ($vis) { $hidden++ }
        if ($Close) {
            $null = [BarWnd]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
        } else {
            $null = [BarWnd]::ShowWindow($h, 0)   # SW_HIDE
        }
    }
}

if ($rows.Count -gt 0) { $rows | Format-Table -AutoSize | Out-String | Write-Host }

if ($List) {
    "共 $found 条状态栏窗口（只列出，未改动）"
} else {
    if ($Close) {
        "共 $found 条状态栏窗口，已关闭全部（下一次激活输入法时会按新逻辑只重建 1 条）。"
    } else {
        "共 $found 条状态栏窗口，已隐藏其中 $hidden 条可见的。"
        "它们属于仍在运行的旧 DLL 副本；宿主进程重启后会跑新代码，届时只在前台应用里出现 1 条。"    }
    "-Close 是更彻底的做法（直接 WM_CLOSE 掉，自愈逻辑会在下次激活时重建一条）。"
}
