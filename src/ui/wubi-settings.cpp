// wubi-settings —— 替代输入法的设置界面
// 无参数：弹出设置窗口
// /dump        ：把当前设置打印到标准输出
// /set k=v     ：直接改一项（供脚本/自检使用），可多次
//
// 编译为 GUI 子系统（/SUBSYSTEM:WINDOWS）：
//   以前是控制台程序，被输入法 ShellExecute 时会先闪一个黑色控制台窗口——这就是"设置框后面闪过黑框"。
//   /dump、/set 仍然可用：stdout 被重定向时直接用继承的句柄，从终端手动运行时才 AttachConsole。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include "../engine/wubi_config.h"
#include <cstdio>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static const wchar_t* kTitle = L"Panda五笔 — 设置";
static wubi::Settings g_s;
static std::wstring    g_ini;

static int    g_dpi = 96;
static HFONT  g_font = nullptr;
static HFONT  g_fontTitle = nullptr;
static HBRUSH g_bg = nullptr;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static std::wstring ConfigPath() {
    // 数据目录（用户可指向网盘同步夹）；WUBI_INI 可整体覆盖，供测试/便携使用
    return wubi::GetConfigPath();
}
enum {
    ID_L1_0 = 100, ID_L1_1, ID_L1_2,
    ID_SM_0 = 110, ID_SM_1,
    ID_PUNC = 120, ID_TRAD = 121,
    ID_CAND = 130, ID_FONT = 131, ID_BG = 132, ID_INSET = 133, ID_DICT = 134,
    ID_VERT = 135,
    ID_DATA_DIR = 140, ID_DATA_BROWSE = 141, ID_DATA_DEFAULT = 142, ID_DATA_OPEN = 143,
    ID_SAVE = 200, ID_CANCEL = 201
};

// ---------------------------------------------------------------- 控件工厂
static HWND MakeStatic(HWND p, const wchar_t* t, int x, int y, int w, int hgt) {
    HWND h = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                             S(x), S(y), S(w), S(hgt), p, nullptr, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}
static HWND MakeGroup(HWND p, const wchar_t* t, int x, int y, int w, int hgt) {
    HWND h = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                             S(x), S(y), S(w), S(hgt), p, nullptr, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_fontTitle, TRUE);
    return h;
}
static HWND MakeRadio(HWND p, const wchar_t* t, int x, int y, int w, int id, bool checked, bool groupStart = false) {
    HWND h = CreateWindowExW(0, L"BUTTON", t,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON |
                                 (groupStart ? WS_GROUP : 0),
                             S(x), S(y), S(w), S(22), p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (checked) SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
    return h;
}
static HWND MakeCheck(HWND p, const wchar_t* t, int x, int y, int w, int id, bool checked, bool groupStart = false) {
    HWND h = CreateWindowExW(0, L"BUTTON", t,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | (groupStart ? WS_GROUP : 0),
                             S(x), S(y), S(w), S(22), p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (checked) SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
    return h;
}
static HWND MakeEdit(HWND p, int x, int y, int w, int id, const wchar_t* text, int limit) {
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                             S(x), S(y), S(w), S(24), p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(h, EM_SETLIMITTEXT, (WPARAM)limit, 0);
    return h;
}
static HWND MakeButton(HWND p, const wchar_t* t, int x, int y, int w, int id, bool def) {
    HWND h = CreateWindowExW(0, L"BUTTON", t,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                             S(x), S(y), S(w), S(30), p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

static void ReadUiInto(HWND hwnd) {
    g_s.Level1Mode = SendMessageW(GetDlgItem(hwnd, ID_L1_0), BM_GETCHECK, 0, 0) == BST_CHECKED ? 0
                   : SendMessageW(GetDlgItem(hwnd, ID_L1_1), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 2;
    g_s.SortMode   = SendMessageW(GetDlgItem(hwnd, ID_SM_1), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
    g_s.EnablePunction = SendMessageW(GetDlgItem(hwnd, ID_PUNC), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
    g_s.TradChinese    = SendMessageW(GetDlgItem(hwnd, ID_TRAD), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
    g_s.CandVertical   = SendMessageW(GetDlgItem(hwnd, ID_VERT), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
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

static void BuildChildren(HWND hwnd) {
    // 字体：优先雅黑；标题用同尺寸的粗体，靠"分组框"划分区块，避免一堆左对齐的散装控件
    const int pt = 9;
    g_font = CreateFontW(-S(pt * 96 / 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_fontTitle = CreateFontW(-S(pt * 96 / 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!g_fontTitle) g_fontTitle = g_font;

    const int padX = 16, groupW = 428, inX = padX + 16;
    int y = 12;

    // 一级简码
    MakeGroup(hwnd, L" 一级简码（输入多个键时如何处理）", padX, y, groupW, 96);
    MakeRadio(hwnd, L"不处理：长码候选里照常出现",              inX, y + 24, 360, ID_L1_0, g_s.Level1Mode == 0, true);
    MakeRadio(hwnd, L"隐藏：彻底不出现在候选里",                inX, y + 46, 360, ID_L1_1, g_s.Level1Mode == 1);
    MakeRadio(hwnd, L"沉底：放到最后，仍可选中（默认）",        inX, y + 68, 360, ID_L1_2, g_s.Level1Mode == 2);
    y += 96 + 8;

    // 排序与输入（合成一组，省高度；175% DPI 下窗口才放得下）
    MakeGroup(hwnd, L" 排序与输入", padX, y, groupW, 124);
    MakeRadio(hwnd, L"候选排序：码短优先（默认，与 QQ五笔 接近）", inX, y + 24, 380, ID_SM_0, g_s.SortMode == 0, true);
    MakeRadio(hwnd, L"候选排序：纯词频优先",                        inX, y + 46, 380, ID_SM_1, g_s.SortMode == 1);
    MakeCheck(hwnd, L"中文标点（Ctrl+Shift+P 可临时切换）",         inX, y + 72, 380, ID_PUNC, g_s.EnablePunction != 0, true);
    MakeCheck(hwnd, L"输出繁体（Ctrl+Shift+T 可临时切换）",         inX, y + 94, 380, ID_TRAD, g_s.TradChinese != 0);
    y += 124 + 8;

    // 候选窗外观
    MakeGroup(hwnd, L" 候选窗外观", padX, y, groupW, 140);
    MakeStatic(hwnd, L"候选个数", inX, y + 24, 90, 20);
    {
        wchar_t nb[8];
        wsprintfW(nb, L"%d", g_s.CandCount);
        MakeEdit(hwnd, inX + 96, y + 20, 52, ID_CAND, nb, 1);
    }
    MakeStatic(hwnd, L"1 – 9 个", inX + 156, y + 24, 120, 20);

    MakeStatic(hwnd, L"字号", inX, y + 54, 90, 20);
    {
        wchar_t fb[8];
        wsprintfW(fb, L"%d", g_s.CandFontPt);
        MakeEdit(hwnd, inX + 96, y + 50, 52, ID_FONT, fb, 2);
    }
    MakeStatic(hwnd, L"8 – 24 pt", inX + 156, y + 54, 120, 20);

    MakeStatic(hwnd, L"背景色", inX, y + 84, 90, 20);
    {
        wchar_t cb[16];
        swprintf_s(cb, L"#%06X", (unsigned)(g_s.CandBgColor & 0xFFFFFF));
        HWND be = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", cb,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  S(inX + 96), S(y + 80), S(90), S(24), hwnd,
                                  (HMENU)(INT_PTR)ID_BG, nullptr, nullptr);
        SendMessageW(be, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(be, EM_SETLIMITTEXT, (WPARAM)7, 0);
    }
    MakeCheck(hwnd, L"内框凹陷", inX, y + 112, 160, ID_INSET, g_s.CandInset != 0, true);
    MakeCheck(hwnd, L"候选竖排", inX + 170, y + 112, 160, ID_VERT, g_s.CandVertical != 0);
    y += 140 + 8;

    // 词库
    MakeGroup(hwnd, L" 词库（data 目录）", padX, y, groupW, 58);
    {
        HWND cb = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                  S(inX), S(y + 24), S(300), S(220), hwnd, (HMENU)(INT_PTR)ID_DICT, nullptr, nullptr);
        SendMessageW(cb, WM_SETFONT, (WPARAM)g_font, TRUE);
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
    y += 58 + 8;

    // 数据目录：配置(config.ini) + 自定义词/短语(user.tsv) 都放这里，可指向网盘同步夹
    MakeGroup(hwnd, L" 数据目录（配置与自定义词/短语都放这里）", padX, y, groupW, 100);
    {
        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wubi::GetDataDir().c_str(),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
                                  S(inX), S(y + 24), S(224), S(24), hwnd,
                                  (HMENU)(INT_PTR)ID_DATA_DIR, nullptr, nullptr);
        SendMessageW(ed, WM_SETFONT, (WPARAM)g_font, TRUE);
        MakeButton(hwnd, L"浏览…", inX + 232, y + 21, 56, ID_DATA_BROWSE, false);
        MakeButton(hwnd, L"默认",  inX + 294, y + 21, 48, ID_DATA_DEFAULT, false);
        MakeButton(hwnd, L"打开",  inX + 348, y + 21, 48, ID_DATA_OPEN, false);
        MakeStatic(hwnd, L"指向 OneDrive / 坚果云等同步目录，换机器后指回来即可恢复设置和自定义词。",
                   inX, y + 54, 380, S(32));
    }
    y += 100 + 12;
    // 底部按钮（右下角对齐）
    MakeButton(hwnd, L"保存", padX + groupW - 196, y, 90, ID_SAVE, true);
    MakeButton(hwnd, L"取消", padX + groupW - 96,  y, 90, ID_CANCEL, false);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        BuildChildren(hwnd);
        return 0;
    case WM_ERASEBKGND: {              // 自己也刷背景：避免某些抓图/重绘路径留黑底
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {          // 让分组框里的文字背景跟窗口一致，不留白块
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)g_bg;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_DATA_BROWSE) {
            // 选目录（SHBrowseForFolder：不需要 COM 初始化，够用）
            BROWSEINFOW bi = {};
            wchar_t title[128] = L"选择数据目录（可指向 OneDrive 等同步目录）";
            bi.lpszTitle = title;
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH] = { 0 };
                if (SHGetPathFromIDListW(pidl, path) && path[0]) {
                    SetWindowTextW(GetDlgItem(hwnd, ID_DATA_DIR), path);
                }
                CoTaskMemFree(pidl);
            }
            return 0;
        }
        if (LOWORD(wp) == ID_DATA_DEFAULT) {
            SetWindowTextW(GetDlgItem(hwnd, ID_DATA_DIR), wubi::DefaultDataDir().c_str());
            return 0;
        }
        if (LOWORD(wp) == ID_DATA_OPEN) {
            const std::wstring dir = wubi::GetDataDir();
            ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (LOWORD(wp) == ID_SAVE) {
            ReadUiInto(hwnd);

            // 数据目录变了：先落注册表，再把旧目录里缺的文件搬过去，最后往新目录里写设置
            wchar_t dirBuf[MAX_PATH * 2] = { 0 };
            GetWindowTextW(GetDlgItem(hwnd, ID_DATA_DIR), dirBuf, ARRAYSIZE(dirBuf) - 1);
            const std::wstring want = dirBuf;
            const std::wstring cur = wubi::GetDataDir();
            const std::wstring def = wubi::DefaultDataDir();
            if (_wcsicmp(want.c_str(), cur.c_str()) != 0) {
                if (!wubi::SetDataDir(_wcsicmp(want.c_str(), def.c_str()) == 0 ? std::wstring() : want)) {
                    MessageBoxW(hwnd, L"无法记录数据目录（写 HKCU\\Software\\PandaWubi 失败）。", kTitle,
                                MB_OK | MB_ICONERROR);
                    return 0;
                }
                const std::wstring now = wubi::GetDataDir();
                const int moved = wubi::MigrateDataFiles(cur, now);
                g_ini = wubi::GetConfigPath();
                if (moved > 0) {
                    MessageBoxW(hwnd, L"数据目录已切换，原有的设置与用户词已复制过去。\n\n已打开的程序需要重启才会用新目录。",
                                kTitle, MB_OK | MB_ICONINFORMATION);
                }
            }

            const bool ok = wubi::SaveSettings(g_ini, g_s);
            const std::wstring msg = ok
                ? (L"已保存。\n\n配置与用户词都在：\n" + wubi::GetDataDir() +
                   L"\n\n新设置对之后启动的程序生效；已打开的程序需要重启才会重新读取。")
                : std::wstring(L"保存失败：请检查数据目录是否可写。");
            MessageBoxW(hwnd, msg.c_str(), kTitle, MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
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

// 窗口客户区尺寸随 DPI 缩放；控件坐标在 BuildChildren 里也按 S() 缩放
static void MeasureClient(int& w, int& h) {
    w = S(460);
    // 与 BuildChildren 里的累加保持一致
    h = S(12 + 96 + 8 + 124 + 8 + 140 + 8 + 58 + 8 + 100 + 12 + 30 + 12);
}

static int RunDialog() {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    {
        HDC dc = GetDC(nullptr);
        g_dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ReleaseDC(nullptr, dc);
        if (g_dpi <= 0) g_dpi = 96;
    }
    g_bg = GetSysColorBrush(COLOR_BTNFACE);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"WubiSettingsWnd";
    wc.hbrBackground = g_bg;
    RegisterClassExW(&wc);

    int cw = 0, ch = 0;
    MeasureClient(cw, ch);
    RECT rc = { 0, 0, cw, ch };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;

    // 先建在屏幕外、不显示，摆好位置再 ShowWindow：避免"先闪一个位于默认位置的空窗"
    HWND hwnd = CreateWindowExW(0, L"WubiSettingsWnd", kTitle,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                -32000, -32000, ww, wh,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;

    // 标题栏用小熊猫图标（不编进 exe，和候选窗/状态栏共用 dist\panda.ico）
    {
        wchar_t self[MAX_PATH] = { 0 };
        GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
        std::wstring dir(self);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir.erase(slash);
            const std::wstring icoPath = dir + L"\\panda.ico";
            HICON hi = (HICON)LoadImageW(nullptr, icoPath.c_str(), IMAGE_ICON, 0, 0,
                                        LR_LOADFROMFILE | LR_DEFAULTSIZE);
            if (!hi) hi = (HICON)LoadImageW(nullptr, icoPath.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
            if (hi) {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hi);
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hi);
            }
        }
    }

    // 居中到工作区
    RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - ww) / 2;
    const int y = work.top + ((work.bottom - work.top) - wh) / 2;
    SetWindowPos(hwnd, HWND_TOP, x, y, ww, wh, SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (g_font) DeleteObject(g_font);
    if (g_fontTitle && g_fontTitle != g_font) DeleteObject(g_fontTitle);
    return 0;
}

// GUI 子系统下没有控制台：/dump、/set 的输出在重定向时直接用继承的句柄，
// 从终端手动运行时接上父进程的控制台。
static void EnsureStdIo() {
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD type = h ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) return;   // 已重定向
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    g_ini = ConfigPath();
    g_s = wubi::LoadSettings(g_ini);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv && argc > 1) {
        const std::wstring cmd = argv[1];
        if (cmd == L"/dump") {
            EnsureStdIo();
            std::printf("ini=%ls\n", g_ini.c_str());
            std::printf("datadir=%ls\n", wubi::GetDataDir().c_str());
            std::printf("userdict=%ls\n", wubi::GetUserWordsPath().c_str());
            std::printf("Level1Mode=%d SortMode=%d EnablePunction=%d TradChinese=%d CandCount=%d CandVertical=%d InputMode=%d\n",
                        g_s.Level1Mode, g_s.SortMode, g_s.EnablePunction, g_s.TradChinese,
                        g_s.CandCount, g_s.CandVertical, g_s.InputMode);
            LocalFree(argv);
            return 0;
        }
        if (cmd == L"/datadir" && argc > 2) {
            // 指定数据目录（default = 恢复默认），并把旧目录里缺的 config.ini/user.tsv 复制过去
            EnsureStdIo();
            const std::wstring want = argv[2];
            const std::wstring oldDir = wubi::GetDataDir();
            const std::wstring defDir = wubi::DefaultDataDir();
            const bool useDefault = (_wcsicmp(want.c_str(), L"default") == 0);
            if (!wubi::SetDataDir(useDefault ? std::wstring() : want)) {
                std::printf("FAILED: cannot record data dir\n");
                LocalFree(argv);
                return 1;
            }
            const std::wstring now = wubi::GetDataDir();
            const int copied = wubi::MigrateDataFiles(oldDir, now);
            std::printf("datadir -> %ls (copied %d file(s) from %ls)\n", now.c_str(), copied, oldDir.c_str());
            std::printf("ini=%ls\n", wubi::GetConfigPath().c_str());
            LocalFree(argv);
            return 0;
        }
        if (cmd == L"/set") {
            EnsureStdIo();
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
            if (!any) { std::printf("no known key\n"); LocalFree(argv); return 2; }

            const bool ok = wubi::SaveSettings(g_ini, g_s);
            std::printf("%s -> Level1Mode=%d SortMode=%d EnablePunction=%d TradChinese=%d CandCount=%d\n",
                        ok ? "saved" : "FAILED", g_s.Level1Mode, g_s.SortMode,
                        g_s.EnablePunction, g_s.TradChinese, g_s.CandCount);
            LocalFree(argv);
            return ok ? 0 : 1;
        }
    }
    if (argv) LocalFree(argv);
    return RunDialog();
}
