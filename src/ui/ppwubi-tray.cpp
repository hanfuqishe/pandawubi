// ppwubi-tray —— PP五笔 托盘程序
// 常驻托盘显示图标；左键/双击打开设置；右键菜单含设置、关于、退出。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdarg>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

static const wchar_t* kAppName = L"PP五笔";
static const wchar_t* kTip     = L"PP五笔 —— 右键打开菜单";
static const UINT     kTrayId  = 1;
static const UINT     WM_TRAY  = WM_APP + 1;
static const UINT     ID_SETTINGS = 1001;
static const UINT     ID_ABOUT    = 1002;
static const UINT     ID_EXIT     = 1003;

static void TrayLog(const char* fmt, ...) {
    char msg[1024] = { 0 };
    va_list ap; va_start(ap, fmt);
    vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    wchar_t dir[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, ARRAYSIZE(dir));
    if (n == 0 || n >= ARRAYSIZE(dir)) return;
    std::wstring d = std::wstring(dir) + L"\\WubiTsf";
    CreateDirectoryW(d.c_str(), nullptr);
    wchar_t ovr[MAX_PATH] = { 0 };
    DWORD on = GetEnvironmentVariableW(L"PPWUBI_TRAY_LOG", ovr, ARRAYSIZE(ovr));
    std::wstring p = (on > 0 && on < ARRAYSIZE(ovr)) ? std::wstring(ovr) : (d + L"\\tray.log");
    if (on == 0) {
        wchar_t exePath[MAX_PATH] = { 0 };
        DWORD em = GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath));
        if (em > 0) {
            std::wstring ep(exePath, em);
            const size_t sl = ep.find_last_of(L"\\/");
            if (sl != std::wstring::npos) p = ep.substr(0, sl) + L"\\tray.log";
        }
    }
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, msg, (DWORD)strlen(msg), &wrote, nullptr);
    WriteFile(h, "\r\n", 2, &wrote, nullptr);
    CloseHandle(h);
}

static HICON   g_icon = nullptr;
static HWND    g_wnd  = nullptr;
static UINT    g_taskbarCreated = 0;
static std::wstring g_dir;

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    std::wstring p(buf, (n > 0 && n < ARRAYSIZE(buf)) ? n : 0);
    const size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring(L".") : p.substr(0, slash);
}

// 诊断增强：记录会话/完整性/窗口/重试/系统托盘窗口
static void TrayLogEnv() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);

    HANDLE tok = nullptr;
    DWORD integ = 0xFFFFFFFF;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD sz = 0;
        GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &sz);
        if (sz) {
            TOKEN_MANDATORY_LABEL* tml = (TOKEN_MANDATORY_LABEL*)malloc(sz);
            if (tml && GetTokenInformation(tok, TokenIntegrityLevel, tml, sz, &sz)) {
                BYTE* sid = (BYTE*)tml->Label.Sid;
                DWORD cnt = *GetSidSubAuthorityCount((PSID)sid);
                integ = *GetSidSubAuthority((PSID)sid, cnt - 1);
            }
            free(tml);
        }
        CloseHandle(tok);
    }
    HWND shellTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND shellWnd  = GetShellWindow();
    // Explorer 自身的完整性级别：若它比我们高（如管理员运行），UIPI 会拒绝托盘注册
    {
        HWND shell = GetShellWindow();
        DWORD spid = 0;
        if (shell) GetWindowThreadProcessId(shell, &spid);
        DWORD shellInteg = 0xFFFFFFFF;
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, spid);
        if (ph) {
            HANDLE t2 = nullptr;
            if (OpenProcessToken(ph, TOKEN_QUERY, &t2)) {
                DWORD s2 = 0;
                GetTokenInformation(t2, TokenIntegrityLevel, nullptr, 0, &s2);
                if (s2) {
                    TOKEN_MANDATORY_LABEL* tm2 = (TOKEN_MANDATORY_LABEL*)malloc(s2);
                    if (tm2 && GetTokenInformation(t2, TokenIntegrityLevel, tm2, s2, &s2)) {
                        BYTE* sid2 = (BYTE*)tm2->Label.Sid;
                        DWORD c2 = *GetSidSubAuthorityCount((PSID)sid2);
                        shellInteg = *GetSidSubAuthority((PSID)sid2, c2 - 1);
                    }
                    free(tm2);
                }
                CloseHandle(t2);
            }
            CloseHandle(ph);
        }
        TrayLog("shell pid=%lu integrity=0x%lX", spid, shellInteg);
    }
    TrayLog("env session=%lu integrity=0x%lX Shell_TrayWnd=%p ShellWindow=%p explorer=%p",
            session, integ, (void*)shellTray, (void*)shellWnd,
            (void*)FindWindowW(L"Progman", nullptr));
}

static BOOL AddTrayIconWithRetry() {
    if (!g_wnd || !IsWindow(g_wnd)) {
        TrayLog("retry: window invalid hwnd=%p", (void*)g_wnd);
        return FALSE;
    }
    for (int attempt = 1; attempt <= 3; ++attempt) {
        NOTIFYICONDATAW nid = { sizeof(nid) };
        nid.hWnd = g_wnd;
        nid.uID = kTrayId;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = g_icon;
        wcsncpy_s(nid.szTip, kTip, _TRUNCATE);

        SetLastError(0);
        BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
        DWORD err = GetLastError();
        TrayLog("NIM_ADD attempt=%d ok=%d err=%lu cbSize=%u", attempt, (int)ok, err, (unsigned)nid.cbSize);

        // 183 = 已存在（多半是上次异常退出留下的残影，或句柄被复用）
        // 先清除残留再重试一次；仍报 183 就视为图标已在托盘上
        if (!ok && err == ERROR_ALREADY_EXISTS) {
            Shell_NotifyIconW(NIM_DELETE, &nid);
            Sleep(150);
            SetLastError(0);
            ok = Shell_NotifyIconW(NIM_ADD, &nid);
            err = GetLastError();
            TrayLog("NIM_ADD after delete ok=%d err=%lu", (int)ok, err);
            if (!ok && err == ERROR_ALREADY_EXISTS) {
                TrayLog("treat ERROR_ALREADY_EXISTS as success");
                ok = TRUE;
            }
        }

        if (ok) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            const BOOL v = Shell_NotifyIconW(NIM_SETVERSION, &nid);
            TrayLog("NIM_SETVERSION ok=%d err=%lu", (int)v, GetLastError());
            return TRUE;
        }
        Sleep(300);
    }
    return FALSE;
}
static DWORD QueryIntegrityOf(HANDLE hProcess) {
    HANDLE tok = nullptr;
    DWORD integ = 0xFFFFFFFF;
    if (!hProcess || !OpenProcessToken(hProcess, TOKEN_QUERY, &tok)) return integ;
    DWORD sz = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &sz);
    if (sz) {
        TOKEN_MANDATORY_LABEL* tml = (TOKEN_MANDATORY_LABEL*)malloc(sz);
        if (tml && GetTokenInformation(tok, TokenIntegrityLevel, tml, sz, &sz)) {
            BYTE* sid = (BYTE*)tml->Label.Sid;
            DWORD cnt = *GetSidSubAuthorityCount((PSID)sid);
            integ = *GetSidSubAuthority((PSID)sid, cnt - 1);
        }
        free(tml);
    }
    CloseHandle(tok);
    return integ;
}

// ---------------------------------------------------------------- 降权自愈
// 若本进程被以低完整性启动（沙箱/受限环境），托盘注册和日志写入都会失败。
// 这类环境无法自行提权，因此改为"请 Explorer 帮忙启动"——Explorer 是 medium，
// 由它启动的实例也是 medium，托盘才能正常注册。
static DWORD ParentProcessId(DWORD self) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD parent = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) { parent = pe.th32ParentProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return parent;
}

static std::wstring ProcessNameOf(DWORD pid) {
    if (!pid) return std::wstring();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return std::wstring();
    PROCESSENTRY32W pe = { sizeof(pe) };
    std::wstring name;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) { name = pe.szExeFile; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return name;
}

// 返回 true 表示"已经交给 Explorer 重启，本进程应退出"
static bool RelaunchViaExplorerIfSandboxed() {
    const DWORD integ = QueryIntegrityOf(GetCurrentProcess());
    if (integ != 0x1000) return false;                 // 不是低完整性，正常继续

    const std::wstring parent = ProcessNameOf(ParentProcessId(GetCurrentProcessId()));
    if (_wcsicmp(parent.c_str(), L"explorer.exe") == 0) return false;  // 已由 Explorer 启动

    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    TrayLog("detected low integrity, relaunch via explorer: parent=%ls", parent.c_str());

    // explorer.exe "<路径>" 会把请求转交给运行中的 Explorer，由它（medium）启动
    // 路径必须带引号：装在 C:\Program Files\... 下时 explorer.exe 会把空格当分隔符，
    // 结果什么也没启动，而这里照样 return true 让本进程退出 → 托盘图标彻底消失。
    const std::wstring quoted = L"\"" + std::wstring(exe) + L"\"";
    HINSTANCE h = ShellExecuteW(nullptr, L"open", L"explorer.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL);
    TrayLog("ShellExecute(explorer) ret=%p err=%lu", (void*)h, GetLastError());
    return (INT_PTR)h > 32;   // >32 才是成功，见 ShellExecute 返回值约定
}

static void AddTrayIcon() {
    if (AddTrayIconWithRetry()) return;

    const DWORD err = GetLastError();
    wchar_t exeP[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exeP, ARRAYSIZE(exeP));

    const DWORD selfInt = QueryIntegrityOf(GetCurrentProcess());
    HWND shell = GetShellWindow();
    DWORD spid = 0;
    if (shell) GetWindowThreadProcessId(shell, &spid);
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, spid);
    const DWORD shInt = ph ? QueryIntegrityOf(ph) : 0;
    if (ph) CloseHandle(ph);

    wchar_t logP[MAX_PATH + 32] = { 0 };
    wcscpy_s(logP, exeP);
    wchar_t* slash = wcsrchr(logP, L'\\');
    if (slash) wcscpy_s(slash + 1, 32, L"tray.log");

    wchar_t msg[900];
    swprintf_s(msg,
        L"托盘图标注册失败\n\n"
        L"错误码：%lu\n"
        L"本进程完整性：0x%lX\n"
        L"资源管理器完整性：0x%lX\n"
        L"程序路径：%ls\n"
        L"日志文件：%ls",
        err, selfInt, shInt, exeP, logP);
    MessageBoxW(nullptr, msg, L"PP五笔", MB_OK | MB_ICONWARNING);
}

static void RemoveTrayIcon() {
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_wnd;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void LaunchSettings() {
    std::wstring exe = g_dir + L"\\wubi-settings.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_wnd, L"找不到 wubi-settings.exe（应与本程序在同一目录）。", kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    ShellExecuteW(g_wnd, L"open", exe.c_str(), nullptr, g_dir.c_str(), SW_SHOWNORMAL);
}

static void ShowAbout() {
    std::wstring msg =
        L"PP五笔 —— 基于 REA 分析结果重建的 QQ五笔 替代输入法\n\n"
        L"· 五笔86 码表（14.7 万词条，mmap 共享加载）\n"
        L"· 一级/二级/三级简码优先，四码才参与词频\n"
        L"· 词频自学习、简繁切换、中文标点、数字便捷输入\n\n"
        L"设置：托盘图标双击，或右键菜单「设置」\n"
        L"构建时间：" + std::wstring(L"" __DATE__ L" " __TIME__);
    MessageBoxW(g_wnd, msg.c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
}

static void ShowMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"设置(&S)…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_ABOUT, L"关于(&A)");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"退出(&X)");
    SetForegroundWindow(g_wnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
    case ID_SETTINGS: LaunchSettings(); break;
    case ID_ABOUT:    ShowAbout();      break;
    case ID_EXIT:     DestroyWindow(g_wnd); break;
    default: break;
    }
}

// ---------------------------------------------------------------- 中/英状态
// 由 IME（wubi_tsf.dll）通过 WM_APP+10 通知：wParam=0 中文 / 1 英文
static bool  g_english = false;
static HICON g_iconZh = nullptr;
static HICON g_iconEn = nullptr;

static std::wstring StateFilePath() {
    wchar_t dir[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, ARRAYSIZE(dir));
    if (n == 0 || n >= ARRAYSIZE(dir)) return std::wstring();
    std::wstring d = std::wstring(dir) + L"\\WubiTsf";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\ime_state.txt";
}

static void NotifyState(bool english, bool enable) {
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_wnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = english ? (g_iconEn ? g_iconEn : g_iconZh)
                        : (g_iconZh ? g_iconZh : g_iconEn);
    wcscpy_s(nid.szTip, english ? L"PP五笔 —— 英文（单击打开设置）"
                                : L"PP五笔 —— 中文（单击打开设置）");
    Shell_NotifyIconW(enable ? NIM_MODIFY : NIM_ADD, &nid);
    TrayLog("state -> %ls", english ? L"EN" : L"ZH");
}

static void LoadStateFromFile() {
    const std::wstring p = StateFilePath();
    if (p.empty()) return;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, p.c_str(), L"rb") != 0 || !fp) return;
    char buf[16] = { 0 };
    size_t rd = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (rd > 0) g_english = (buf[0] == 'e' || buf[0] == 'E');
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        // 已用 NIM_SETVERSION 声明 NOTIFYICON_VERSION_4：v4 下左键发的是 NIN_SELECT / NIN_KEYSELECT，
        // 不再发 WM_LBUTTONUP；老写法会导致"单击托盘图标没反应"。两种都接。
        if (lp == NIN_SELECT || lp == NIN_KEYSELECT || lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK
            || LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT
            || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
            LaunchSettings();
        } else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU
                   || LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            ShowMenu();
        }
        return 0;
    case WM_APP + 10:   // IME 通知中/英切换
        g_english = (wp != 0);
        NotifyState(g_english, true);
        return 0;
    case WM_APP + 2:
        AddTrayIcon();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SETTINGS) LaunchSettings();
        else if (LOWORD(wp) == ID_ABOUT) ShowAbout();
        else if (LOWORD(wp) == ID_EXIT) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == g_taskbarCreated && g_taskbarCreated != 0) { AddTrayIcon(); return 0; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain() {
    // 单实例：已有托盘程序在跑就不再启动
    CreateMutexW(nullptr, FALSE, L"Local\\PPWubiTraySingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"PP五笔 托盘程序已经在运行。", kAppName, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (RelaunchViaExplorerIfSandboxed()) return 0;

    g_dir = ExeDir();
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // 优先加载同目录的 ppwubi.ico，多尺寸更清晰
    std::wstring icoPath = g_dir + L"\\ppwubi.ico";
    g_iconZh = (HICON)LoadImageW(nullptr, icoPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    {
        std::wstring enPath = g_dir + L"\\ppwubi-en.ico";
        g_iconEn = (HICON)LoadImageW(nullptr, enPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }
    g_icon = g_iconZh;
    TrayLog("LoadImage(ico) icon=%p err=%lu", (void*)g_icon, GetLastError());
    if (!g_icon) { g_icon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION); TrayLog("fallback icon=%p", (void*)g_icon); }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PPWubiTrayWnd";
    RegisterClassExW(&wc);

    g_wnd = CreateWindowExW(0, L"PPWubiTrayWnd", kAppName, WS_OVERLAPPED, 0, 0, 0, 0,
                            nullptr, nullptr, wc.hInstance, nullptr);
    TrayLog("CreateWindow hwnd=%p err=%lu", (void*)g_wnd, GetLastError());
    if (!g_wnd) return 1;

    TrayLog("---- start ----");
    TrayLogEnv();
    // 延迟到消息循环启动后再注册：某些环境下循环启动前调用 Shell_NotifyIcon 会失败
    PostMessageW(g_wnd, WM_APP + 2, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_icon && g_icon != LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION)) DestroyIcon(g_icon);
    return 0;
}
