// wubi-settings —— 替代输入法的设置界面
// 无参数：弹出设置窗口（先 FreeConsole，避免双击时闪黑框）
// /dump        ：把当前设置打印到标准输出
// /set k=v     ：直接改一项（供脚本/自检使用），可多次
#include <windows.h>
#include <commctrl.h>
#include "../engine/wubi_config.h"
#include <cstdio>
#include <string>

#pragma comment(lib, "comctl32.lib")

static const wchar_t* kTitle = L"Panda五笔 — 设置";
static wubi::Settings g_s;
static std::wstring    g_ini;

static std::wstring ConfigPath() {
    // 允许用环境变量覆盖（自检/便携部署用）
    wchar_t override_[MAX_PATH];
    DWORD on = GetEnvironmentVariableW(L"WUBI_INI", override_, ARRAYSIZE(override_));
    if (on > 0 && on < ARRAYSIZE(override_)) return std::wstring(override_);

    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, ARRAYSIZE(buf));
    std::wstring dir = (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring(L".");
    dir += L"\\PandaWubi";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.ini";
}

enum {
    ID_L1_0 = 100, ID_L1_1, ID_L1_2,
    ID_SM_0 = 110, ID_SM_1,
    ID_PUNC = 120, ID_TRAD = 121,
    ID_CAND = 130, ID_FONT = 131, ID_BG = 132, ID_INSET = 133, ID_DICT = 134,
    ID_SAVE = 200, ID_CANCEL = 201
};

static HWND MakeStatic(HWND p, const wchar_t* t, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE, x, y, w, h, p, nullptr, nullptr, nullptr);
}
static HWND MakeRadio(HWND p, const wchar_t* t, int x, int y, int w, int id, bool checked) {
    HWND h = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (checked ? WS_TABSTOP : 0),
                             x, y, w, 22, p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    if (checked) SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
    return h;
}
static HWND MakeCheck(HWND p, const wchar_t* t, int x, int y, int w, int id, bool checked) {
    HWND h = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             x, y, w, 22, p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    if (checked) SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
    return h;
}

static void ReadUiInto(HWND hwnd) {
    g_s.Level1Mode = SendMessageW(GetDlgItem(hwnd, ID_L1_0), BM_GETCHECK, 0, 0) == BST_CHECKED ? 0
                   : SendMessageW(GetDlgItem(hwnd, ID_L1_1), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 2;
    g_s.SortMode   = SendMessageW(GetDlgItem(hwnd, ID_SM_1), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
    g_s.EnablePunction = SendMessageW(GetDlgItem(hwnd, ID_PUNC), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
    g_s.TradChinese    = SendMessageW(GetDlgItem(hwnd, ID_TRAD), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
    wchar_t buf[16] = { 0 };
    GetWindowTextW(GetDlgItem(hwnd, ID_CAND), buf, 15);
    g_s.CandCount = _wtoi(buf);
    if (g_s.CandCount < 1) g_s.CandCount = 1;
    if (g_s.CandCount > 9) g_s.CandCount = 9;
    wchar_t fbuf[16] = { 0 };
    GetWindowTextW(GetDlgItem(hwnd, ID_FONT), fbuf, 15);
    g_s.CandFontPt = _wtoi(fbuf);
    if (g_s.CandFontPt < 8)  g_s.CandFontPt = 8;
    if (g_s.CandFontPt > 24) g_s.CandFontPt = 24;
    wchar_t cbuf[32] = { 0 };
    GetWindowTextW(GetDlgItem(hwnd, ID_BG), cbuf, 31);
    {
        const wchar_t* p = cbuf;
        if (*p == L'#') ++p;
        if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) p += 2;
        const unsigned v = (unsigned)wcstoul(p, nullptr, 16);
        if (v <= 0xFFFFFF) g_s.CandBgColor = (int)v;
    }
    g_s.CandInset = SendMessageW(GetDlgItem(hwnd, ID_INSET), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
    {
        HWND cb = GetDlgItem(hwnd, ID_DICT);
        const int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
        if (sel >= 0) {
            wchar_t name[128] = { 0 };
            SendMessageW(cb, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)name);
            if (name[0]) g_s.DictFile = name;
        }
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        int y = 12;
        MakeStatic(hwnd, L"一级简码（输入多个键时如何对待）", 16, y, 340, 20); y += 24;
        MakeRadio(hwnd, L"不处理",                         40, y, 120, ID_L1_0, g_s.Level1Mode == 0); y += 24;
        MakeRadio(hwnd, L"隐藏（彻底不出现在候选里）",      40, y, 300, ID_L1_1, g_s.Level1Mode == 1); y += 24;
        MakeRadio(hwnd, L"沉底（放最后，仍可选中）",        40, y, 300, ID_L1_2, g_s.Level1Mode == 2); y += 32;

        MakeStatic(hwnd, L"候选排序", 16, y, 340, 20); y += 24;
        MakeRadio(hwnd, L"码短优先（默认，与 QQ五笔 接近）", 40, y, 320, ID_SM_0, g_s.SortMode == 0); y += 24;
        MakeRadio(hwnd, L"纯词频优先",                       40, y, 320, ID_SM_1, g_s.SortMode == 1); y += 32;

        MakeStatic(hwnd, L"其他", 16, y, 340, 20); y += 24;
        MakeCheck(hwnd, L"中文标点（Ctrl+Shift+P 可临时切换）", 40, y, 320, ID_PUNC, g_s.EnablePunction != 0); y += 26;
        MakeCheck(hwnd, L"输出繁体（Ctrl+Shift+T 可临时切换）", 40, y, 320, ID_TRAD, g_s.TradChinese != 0); y += 30;

        MakeStatic(hwnd, L"候选个数（1-9）", 16, y + 4, 130, 20);
        HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                 150, y, 50, 24, hwnd, (HMENU)(INT_PTR)ID_CAND, nullptr, nullptr);
        wchar_t nb[8];
        wsprintfW(nb, L"%d", g_s.CandCount);
        SetWindowTextW(e, nb);
        y += 34;

        MakeStatic(hwnd, L"候选框字号（8-24）", 16, y + 4, 130, 20);
        HWND fe = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                 150, y, 50, 24, hwnd, (HMENU)(INT_PTR)ID_FONT, nullptr, nullptr);
        {
            wchar_t fb[8];
            wsprintfW(fb, L"%d", g_s.CandFontPt);
            SetWindowTextW(fe, fb);
        }
        y += 32;

        MakeStatic(hwnd, L"背景色（#RRGGBB）", 16, y + 4, 130, 20);
        HWND be = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE,
                                 150, y, 90, 24, hwnd, (HMENU)(INT_PTR)ID_BG, nullptr, nullptr);
        {
            wchar_t cb[16];
            swprintf_s(cb, L"#%06X", (unsigned)(g_s.CandBgColor & 0xFFFFFF));
            SetWindowTextW(be, cb);
        }
        y += 32;

        MakeCheck(hwnd, L"内框凹陷效果", 40, y, 200, ID_INSET, g_s.CandInset != 0);
        y += 32;

        MakeStatic(hwnd, L"词库（data 目录）", 16, y + 4, 130, 20);
        {
            HWND cb = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                      150, y, 220, 200, hwnd, (HMENU)(INT_PTR)ID_DICT, nullptr, nullptr);
            // 枚举 exe 同目录 data\*.tsv / *.bin
            wchar_t self[MAX_PATH] = { 0 };
            GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
            std::wstring dir(self);
            const size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) dir.erase(slash);
            const std::wstring pattern = dir + L"\\data\\*";
            WIN32_FIND_DATAW fd;
            HANDLE hfind = FindFirstFileW(pattern.c_str(), &fd);
            int selIdx = -1, idx = 0;
            if (hfind != INVALID_HANDLE_VALUE) {
                do {
                    const std::wstring name = fd.cFileName;
                    if (name.size() > 4 &&
                        (name.compare(name.size() - 4, 4, L".tsv") == 0 ||
                         name.compare(name.size() - 4, 4, L".bin") == 0)) {
                        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)name.c_str());
                        if (name == g_s.DictFile) selIdx = idx;
                        ++idx;
                    }
                } while (FindNextFileW(hfind, &fd));
                FindClose(hfind);
            }
            {
                wchar_t cur[128];
                wcscpy_s(cur, g_s.DictFile.c_str());
                if (selIdx < 0) { SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)cur); selIdx = idx; }
                SendMessageW(cb, CB_SETCURSEL, (WPARAM)selIdx, 0);
            }
        }
        y += 32;
        y += 40;

        CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        150, y, 90, 30, hwnd, (HMENU)(INT_PTR)ID_SAVE, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
                        250, y, 90, 30, hwnd, (HMENU)(INT_PTR)ID_CANCEL, nullptr, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SAVE) {
            ReadUiInto(hwnd);
            const bool ok = wubi::SaveSettings(g_ini, g_s);
            MessageBoxW(hwnd, ok ? L"已保存。新设置对**新启动的程序**生效；\n已打开的程序需要重启才会重新读取。"
                                 : L"保存失败，请检查 %APPDATA%\\PandaWubi 是否可写。",
                        kTitle, MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == ID_CANCEL) { DestroyWindow(hwnd); return 0; }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int RunDialog() {
    FreeConsole();   // 双击时不留黑框
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"WubiSettingsWnd";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, 420, 600 };   // 增加外观与词库设置
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    HWND hwnd = CreateWindowExW(0, L"WubiSettingsWnd", kTitle,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    g_ini = ConfigPath();
    g_s = wubi::LoadSettings(g_ini);

    if (argc > 1) {
        const std::wstring cmd = argv[1];
        if (cmd == L"/dump") {
            std::printf("ini=%ls\n", g_ini.c_str());
            std::printf("Level1Mode=%d SortMode=%d EnablePunction=%d TradChinese=%d CandCount=%d CandVertical=%d InputMode=%d\n",
                        g_s.Level1Mode, g_s.SortMode, g_s.EnablePunction, g_s.TradChinese,
                        g_s.CandCount, g_s.CandVertical, g_s.InputMode);
            return 0;
        }
        if (cmd == L"/set") {
            bool any = false;
            for (int i = 2; i < argc; ++i) {
                std::wstring kv = argv[i];
                size_t eq = kv.find(L'=');
                if (eq == std::wstring::npos) continue;
                std::wstring k = kv.substr(0, eq), v = kv.substr(eq + 1);
                int iv = _wtoi(v.c_str());
                if      (k == L"Level1Mode")     { g_s.Level1Mode = iv; any = true; }
                else if (k == L"SortMode")       { g_s.SortMode = iv; any = true; }
                else if (k == L"EnablePunction") { g_s.EnablePunction = iv; any = true; }
                else if (k == L"TradChinese")    { g_s.TradChinese = iv; any = true; }
                else if (k == L"CandCount")      { g_s.CandCount = iv; any = true; }
                else if (k == L"CandVertical")   { g_s.CandVertical = iv; any = true; }
            }
            if (!any) { std::printf("no known key\n"); return 2; }

            const bool ok = wubi::SaveSettings(g_ini, g_s);
            std::printf("%s -> Level1Mode=%d SortMode=%d EnablePunction=%d TradChinese=%d CandCount=%d\n",
                        ok ? "saved" : "FAILED", g_s.Level1Mode, g_s.SortMode,
                        g_s.EnablePunction, g_s.TradChinese, g_s.CandCount);
            return ok ? 0 : 1;
        }
    }
    return RunDialog();
}
