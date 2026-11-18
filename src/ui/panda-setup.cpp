// panda-setup.cpp —— 熊猫五笔安装/卸载程序
//
// 单文件安装包：所有要装的文件都以 RCDATA 资源嵌在本 exe 里（编译时由 tools\build-setup.ps1 生成），
// 运行后释放到 %ProgramFiles%\PandaWubi，注册 64/32 位 TSF，写卸载信息。
//
//   panda-setup.exe              安装（要管理员）
//   panda-setup.exe /uninstall   卸载
//   panda-setup.exe /extract DIR 只把文件释放到 DIR（不碰注册表，用于自检）
//
// 注册表只做两件事：
//   1) 调 DLL 自己的 DllRegisterServer（HKLM 的 CLSID + CTF TIP）
//   2) 补写 LanguageProfile 的 Description/IconFile/IconIndex/Enable（和 dist\register.cmd 一致）
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>

#include "panda_version.h"

#ifndef RT_MANIFEST
#define RT_MANIFEST 24
#endif

#pragma comment(lib, "comctl32.lib")

// ---- 载荷清单（资源 id 必须和 tools\build-setup.ps1 生成的一致）----
struct PayloadEntry {
    int          id;
    const wchar_t* rel;      // 相对安装目录的子路径
    bool         seedOnly;   // true = 只在目标不存在时才写（用户配置）
};

static const PayloadEntry kFiles[] = {
    { 200, L"panda_tsf.dll",                        false },
    { 201, L"panda_tsf32.dll",                      false },
    { 202, L"panda-settings.exe",                   false },
    { 203, L"panda-phrase.exe",                     false },
    { 204, L"panda.ico",                            false },
    { 205, L"cat.ico",                              false },
    { 206, L"dog.ico",                              false },
    { 207, L"crane.ico",                            false },
    { 208, L"data\\qqwubi86.dict.tsv",              false },
    { 209, L"data\\simp2trad.dat",                  false },
    { 210, L"data\\punc.ini",                       false },
    { 211, L"data\\eng_words.txt",                  false },
    { 212, L"readme.txt",                           false },
};

static const wchar_t* kClsid   = L"{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}";
static const wchar_t* kProfileGuid = L"{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}";

static std::wstring g_exeDir;      // 本 exe 所在目录
static std::wstring g_installDir;  // %ProgramFiles%\PandaWubi

// ---------------------------------------------------------------- 小工具
static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf) - 1);
    std::wstring s(buf);
    const size_t p = s.find_last_of(L"\\/");
    if (p != std::wstring::npos) s.erase(p);
    return s;
}

static bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const size_t p = dir.find_last_of(L"\\/");
    if (p != std::wstring::npos && p > 2) EnsureDir(dir.substr(0, p));
    return CreateDirectoryW(dir.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool WriteResource(int id, const std::wstring& path) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!res) return false;
    const DWORD size = SizeofResource(nullptr, res);
    HGLOBAL mem = LoadResource(nullptr, res);
    if (!mem || !size) return false;
    const void* data = LockResource(mem);
    if (!data) return false;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

static bool RunAndWait(const std::wstring& cmdline, DWORD* exitCode = nullptr) {
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void RegSetStr(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& val) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(),
                   (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

static void RegSetDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD v) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
}

static std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = { 0 };
    const DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring();
}

// ---------------------------------------------------------------- 安装 / 卸载
static bool Install32BitDll() {
    const std::wstring wow = Env(L"WINDIR") + L"\\SysWOW64\\regsvr32.exe";
    if (GetFileAttributesW(wow.c_str()) == INVALID_FILE_ATTRIBUTES) return true;   // 没有 32 位子系统就算了
    return RunAndWait(L"\"" + wow + L"\" /s \"" + g_installDir + L"\\panda_tsf32.dll\"");
}

static bool Uninstall32BitDll() {
    const std::wstring wow = Env(L"WINDIR") + L"\\SysWOW64\\regsvr32.exe";
    if (GetFileAttributesW(wow.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    return RunAndWait(L"\"" + wow + L"\" /s /u \"" + g_installDir + L"\\panda_tsf32.dll\"");
}

// 调 64 位 DLL 自己的 DllRegisterServer（本进程已提升，写的是 HKLM）
static bool Register64BitDll(std::wstring& err) {
    const std::wstring dll = g_installDir + L"\\panda_tsf.dll";
    HMODULE mod = LoadLibraryW(dll.c_str());
    if (!mod) { err = L"没法加载 " + dll; return false; }
    typedef HRESULT(WINAPI* Fn)();
    Fn fn = (Fn)GetProcAddress(mod, "DllRegisterServer");
    if (!fn) { FreeLibrary(mod); err = L"DLL 里没有 DllRegisterServer"; return false; }
    const HRESULT hr = fn();
    FreeLibrary(mod);
    if (FAILED(hr)) { err = L"DllRegisterServer 失败"; return false; }
    return true;
}

static bool Unregister64BitDll() {
    const std::wstring dll = g_installDir + L"\\panda_tsf.dll";
    HMODULE mod = LoadLibraryW(dll.c_str());
    if (!mod) return true;
    typedef HRESULT(WINAPI* Fn)();
    Fn fn = (Fn)GetProcAddress(mod, "DllUnregisterServer");
    if (fn) fn();
    FreeLibrary(mod);
    return true;
}

static void WriteProfileExtras() {
    const std::wstring key =
        std::wstring(L"SOFTWARE\\Microsoft\\CTF\\TIP\\") + kClsid +
        L"\\LanguageProfile\\0x00000804\\" + kProfileGuid;
    // 图标放用户目录（实测输入指示器只认这里）
    const std::wstring appdata = Env(L"LOCALAPPDATA");
    std::wstring icon = g_installDir + L"\\panda.ico";
    if (!appdata.empty()) {
        EnsureDir(appdata + L"\\PandaWubi");
        for (const wchar_t* name : { L"panda.ico", L"cat.ico", L"dog.ico", L"crane.ico" }) {
            CopyFileW((g_installDir + L"\\" + name).c_str(),
                      (appdata + L"\\PandaWubi\\" + name).c_str(), FALSE);
        }
        const std::wstring p = appdata + L"\\PandaWubi\\panda.ico";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) icon = p;
    }
    RegSetStr(HKEY_LOCAL_MACHINE, key.c_str(), L"Description", L"熊猫五笔");
    RegSetStr(HKEY_LOCAL_MACHINE, key.c_str(), L"IconFile", icon);
    RegSetDword(HKEY_LOCAL_MACHINE, key.c_str(), L"IconIndex", 0);
    RegSetDword(HKEY_LOCAL_MACHINE, key.c_str(), L"Enable", 1);
}

static void WriteUninstallEntry() {
    const std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PandaWubi";
    const std::wstring self = g_installDir + L"\\panda-setup.exe";
    RegSetStr(HKEY_LOCAL_MACHINE, key.c_str(), L"DisplayName", L"熊猫五笔");
    RegSetStr(HKEY_LOCAL_MACHINE, key.c_str(), L"DisplayVersion", PANDA_WUBI_VERSION);
    RegSetStr(HKEY_LOCAL_MACHINE, key.c_str(), L"Publisher", L"hanfuqishe");
    RegSetStr(HKEY_LOCAL_MACHINE, key.c_str(), L"DisplayIcon", g_installDir + L"\\panda.ico");
    RegSetStr(HKEY_LOCAL_MACHINE, key.c_str(), L"InstallLocation", g_installDir);
    RegSetStr(HKEY_LOCAL_MACHINE, key.c_str(), L"UninstallString", L"\"" + self + L"\" /uninstall");
    RegSetStr(HKEY_LOCAL_MACHINE, key.c_str(), L"QuietUninstallString", L"\"" + self + L"\" /uninstall /silent");
    RegSetDword(HKEY_LOCAL_MACHINE, key.c_str(), L"NoModify", 1);
    RegSetDword(HKEY_LOCAL_MACHINE, key.c_str(), L"NoRepair", 1);
}

// 进度回调：把"正在做什么"贴到窗口上
typedef void (*StepFn)(const wchar_t* text, int percent);

static bool DoInstall(StepFn step, std::wstring& err) {
    step(L"创建安装目录…", 5);
    if (!EnsureDir(g_installDir)) { err = L"创建目录失败：" + g_installDir; return false; }

    step(L"释放程序文件…", 15);
    int done = 0;
    for (const auto& f : kFiles) {
        const std::wstring path = g_installDir + L"\\" + f.rel;
        if (f.seedOnly && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (!WriteResource(f.id, path)) { err = L"写入失败：" + path; return false; }
        done++;
        step((L"释放程序文件… " + std::wstring(f.rel)).c_str(), 15 + done * 35 / (int)(sizeof(kFiles) / sizeof(kFiles[0])));
    }
    CopyFileW((g_exeDir + L"\\panda-setup.exe").c_str(),
              (g_installDir + L"\\panda-setup.exe").c_str(), FALSE);

    step(L"注册 64 位输入法…", 60);
    if (!Register64BitDll(err)) return false;

    step(L"注册 32 位输入法…", 70);
    Install32BitDll();

    step(L"写输入法配置…", 80);
    WriteProfileExtras();

    step(L"写入初始配置…", 88);
    const std::wstring appdata = Env(L"APPDATA");
    if (!appdata.empty()) {
        EnsureDir(appdata + L"\\PandaWubi");
        const std::wstring cfg = appdata + L"\\PandaWubi\\config.ini";
        if (GetFileAttributesW(cfg.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // 让设置程序把默认值整份写出来（设置项多，手写容易漏；它没有配置文件时本来就用默认值）
            RunAndWait(L"\"" + g_installDir + L"\\panda-settings.exe\" /set CandCount=9");
        }
    }

    step(L"写卸载信息…", 95);
    WriteUninstallEntry();
    step(L"安装完成", 100);
    return true;
}

static bool DoUninstall(StepFn step, std::wstring& err) {
    step(L"注销输入法…", 10);
    Unregister64BitDll();
    Uninstall32BitDll();

    step(L"删除注册表项…", 35);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsid).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Microsoft\\CTF\\TIP\\") + kClsid).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PandaWubi");

    step(L"删除程序文件…", 60);
    WIN32_FIND_DATAW fd = {};
    const std::wstring pattern = g_installDir + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            const std::wstring p = g_installDir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // data 子目录里都是固定文件名，直接删即可
                WIN32_FIND_DATAW fd2 = {};
                HANDLE h2 = FindFirstFileW((p + L"\\*").c_str(), &fd2);
                if (h2 != INVALID_HANDLE_VALUE) {
                    do {
                        if (wcscmp(fd2.cFileName, L".") == 0 || wcscmp(fd2.cFileName, L"..") == 0) continue;
                        DeleteFileW((p + L"\\" + fd2.cFileName).c_str());
                    } while (FindNextFileW(h2, &fd2));
                    FindClose(h2);
                }
                RemoveDirectoryW(p.c_str());
            } else {
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // 本 exe 自己在安装目录里，删不掉：留个延迟删除
    const std::wstring self = g_installDir + L"\\panda-setup.exe";
    MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    RemoveDirectoryW(g_installDir.c_str());
    step(L"卸载完成（用户配置保留在 %APPDATA%\\PandaWubi）", 100);
    return true;
}

// ---------------------------------------------------------------- 界面
static UINT SystemDpi() {
    HDC dc = GetDC(nullptr);
    const UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

static HWND g_hwnd = nullptr;
static HWND g_status = nullptr;
static HWND g_prog = nullptr;
static HWND g_btn = nullptr;
static bool g_uninstall = false;
static bool g_silent = false;
static bool g_running = false;
static bool g_finished = false;
static bool g_ok = true;
static std::wstring g_err;

static void Step(const wchar_t* text, int percent) {
    if (!g_hwnd) return;
    SetWindowTextW(g_status, text);
    SendMessageW(g_prog, PBM_SETPOS, (WPARAM)percent, 0);
    UpdateWindow(g_hwnd);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void RunWorker() {
    g_running = true;
    g_ok = g_uninstall ? DoUninstall(Step, g_err) : DoInstall(Step, g_err);
    g_running = false;
    g_finished = true;
    if (g_hwnd) {
        if (!g_ok) {
            SetWindowTextW(g_status, (L"出错了：" + g_err).c_str());
            MessageBoxW(g_hwnd, (L"出错了：\n" + g_err).c_str(), L"熊猫五笔", MB_ICONERROR | MB_OK);
        } else if (!g_uninstall) {
            MessageBoxW(g_hwnd,
                L"安装完成！\n\n"
                L"还要做一步才能用：\n"
                L"  设置 > 时间和语言 > 语言和区域 > 中文(简体) > 语言选项\n"
                L"  > 添加键盘 > 熊猫五笔\n"
                L"然后在任意输入框按 Win+空格 切换过来。\n\n"
                L"输入法的设置可以用悬浮栏上的齿轮图标打开。",
                L"熊猫五笔", MB_ICONINFORMATION | MB_OK);
        }
        SetWindowTextW(g_btn, g_ok ? L"关闭" : L"退出");
        EnableWindow(g_btn, TRUE);
        PostMessageW(g_hwnd, WM_APP + 1, 0, 0);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        const UINT dpi = SystemDpi();
        auto S = [&](int v) { return MulDiv(v, (int)dpi, 96); };
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        CreateWindowExW(0, L"STATIC", g_uninstall ? L"卸载熊猫五笔" : L"安装熊猫五笔",
                        WS_CHILD | WS_VISIBLE, S(18), S(14), S(380), S(24), hwnd, nullptr, inst, nullptr);
        g_status = CreateWindowExW(0, L"STATIC", L"准备中…", WS_CHILD | WS_VISIBLE,
                                   S(18), S(44), S(380), S(40), hwnd, nullptr, inst, nullptr);
        g_prog = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE,
                                 S(18), S(92), S(380), S(16), hwnd, (HMENU)10, inst, nullptr);
        SendMessageW(g_prog, PBM_SETRANGE32, 0, 100);
        g_btn = CreateWindowExW(0, L"BUTTON", L"开始", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                S(310), S(120), S(88), S(28), hwnd, (HMENU)1, inst, nullptr);
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        for (HWND w : { g_status, g_btn }) SendMessageW(w, WM_SETFONT, (WPARAM)font, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 1) {
            if (g_finished || !g_running) {
                if (g_finished) { DestroyWindow(hwnd); return 0; }
                EnableWindow(g_btn, FALSE);
                RunWorker();
            }
        }
        return 0;
    case WM_APP + 1:
        if (g_silent) { DestroyWindow(hwnd); }
        return 0;
    case WM_CLOSE:
        if (g_running) return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int) {
    const wchar_t* cl = cmdline ? cmdline : L"";
    g_uninstall = wcsstr(cl, L"/uninstall") != nullptr || wcsstr(cl, L"-uninstall") != nullptr;
    g_silent    = wcsstr(cl, L"/silent") != nullptr || wcsstr(cl, L"-silent") != nullptr;

    // 自检模式：只把载荷释放到指定目录，不碰注册表
    if (const wchar_t* ex = wcsstr(cl, L"/extract")) {
        std::wstring dir = ex + 8;
        while (!dir.empty() && (*dir.begin() == L' ' || *dir.begin() == L'"')) dir.erase(dir.begin());
        while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"')) dir.pop_back();
        if (dir.empty()) return 2;
        g_installDir = dir;
        if (!EnsureDir(g_installDir)) return 3;
        for (const auto& f : kFiles) {
            const std::wstring path = g_installDir + L"\\" + f.rel;
            EnsureDir(path.substr(0, path.find_last_of(L'\\')));
            if (!WriteResource(f.id, path)) return 4;
        }
        return 0;
    }

    g_exeDir = ExeDir();
    std::wstring pf = Env(L"ProgramFiles");
    if (pf.empty()) pf = L"C:\\Program Files";
    g_installDir = pf + L"\\PandaWubi";

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"PandaWubiSetup";
    RegisterClassExW(&wc);

    const UINT dpi = SystemDpi();
    auto S = [&](int v) { return MulDiv(v, (int)dpi, 96); };
    g_hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName,
                             g_uninstall ? L"卸载熊猫五笔" : L"安装熊猫五笔",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(430), S(196),
                             nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, g_silent ? SW_HIDE : SW_SHOW);
    UpdateWindow(g_hwnd);

    if (g_silent) RunWorker();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (g_ok || !g_finished) ? 0 : 1;
}
