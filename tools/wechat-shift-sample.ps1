# 微信 Shift 采样：把微信拉到前台，只发 Shift（不按任何字母），然后从
# %LOCALAPPDATA%\PandaWubi\panda_tsf_<pid>.log 里读采样结果。
#
# 背景：微信（Weixin.exe）只把字母/标点转给 TSF，Shift 的 OnTestKeyDown /
# OnKeyDown / OnKeyUp 都没有，所以输入法改用 20ms 轮询采样（见 wubi_tsf.cpp
# 的 ShiftSampleTick）。本脚本就是给这条采样通路造真实输入的。
#
# 用法：powershell -ExecutionPolicy Bypass -File tools\wechat-shift-sample.ps1 [-NoLaunch]
param([switch]$NoLaunch)

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class WxKey {
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow_();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    public static void Tap(byte vk, int holdMs) {
        keybd_event(vk, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(holdMs);
        keybd_event(vk, 0, 2, UIntPtr.Zero);
    }
}
"@

$exe = 'C:\Program Files\Tencent\Weixin\Weixin.exe'
function Get-WxWindow {
    Get-Process -Name Weixin -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } |
        ForEach-Object { [pscustomobject]@{ Pid = $_.Id; Hwnd = $_.MainWindowHandle; Title = $_.MainWindowTitle } } |
        Select-Object -First 1
}

$wx = Get-WxWindow
if (-not $wx -and -not $NoLaunch) {
    Write-Host '[1] 启动微信…'
    Start-Process -FilePath $exe | Out-Null
    for ($i = 0; $i -lt 60 -and -not $wx; $i++) { Start-Sleep -Milliseconds 500; $wx = Get-WxWindow }
}
if (-not $wx) { Write-Host '[!] 没找到微信窗口'; exit 2 }
Write-Host ("[2] 微信 pid={0} hwnd=0x{1:X} title={2}" -f $wx.Pid, $wx.Hwnd, $wx.Title)

# 强制前台（和 tsf-host 一样用 AttachThreadInput，不模拟 ALT，不惊动 shell）
$fg = [WxKey]::GetForegroundWindow()
$fgThread = 0; $fgPid = 0
if ($fg -ne [IntPtr]::Zero) { $fgThread = [WxKey]::GetWindowThreadProcessId($fg, [ref]$fgPid) }
$my = [WxKey]::GetCurrentThreadId()
if ($fgThread -and $fgThread -ne $my) { [WxKey]::AttachThreadInput($fgThread, $my, $true) | Out-Null }
[WxKey]::BringWindowToTop($wx.Hwnd) | Out-Null
[WxKey]::SetForegroundWindow($wx.Hwnd) | Out-Null
if ($fgThread -and $fgThread -ne $my) { [WxKey]::AttachThreadInput($fgThread, $my, $false) | Out-Null }
Start-Sleep -Milliseconds 1200

$log = Join-Path $env:LOCALAPPDATA ("PandaWubi\panda_tsf_{0}.log" -f $wx.Pid)
if (-not (Test-Path $log)) { Write-Host "[!] 没有日志：$log（微信进程里还没加载输入法？）"; exit 3 }
$before = (Get-Content $log).Count
Write-Host "[3] 日志 $log（已有 $before 行）"

# 只发 Shift：左、右、两个一起。每次按住 150ms（20ms 采样一定能看见）。
Write-Host '[4] 左 Shift …'; [WxKey]::Tap(0xA0, 150); Start-Sleep -Milliseconds 800
Write-Host '[5] 右 Shift …'; [WxKey]::Tap(0xA1, 150); Start-Sleep -Milliseconds 800
Write-Host '[6] 左右一起 …'
[WxKey]::keybd_event(0xA0, 0, 0, [UIntPtr]::Zero)
[WxKey]::keybd_event(0xA1, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 150
[WxKey]::keybd_event(0xA1, 0, 2, [UIntPtr]::Zero)
[WxKey]::keybd_event(0xA0, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 1500

Write-Host '[7] 新增日志（Shift 相关）:'
Get-Content $log | Select-Object -Skip $before |
    Where-Object { $_ -match 'shift|testkey|english|trad|doc focus|OnKey' } |
    ForEach-Object { Write-Host "    $_" }
Write-Host '[8] 完成。'
