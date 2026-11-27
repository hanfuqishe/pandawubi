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
#include "../tsf/ipc.h"
#include <cstdio>
#include <string>
#include <vector>
#include <windowsx.h>

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

// 热键录制状态：点"热键"按钮后进入录制，按下的组合键直接写进 g_hotkeyText；
// Esc / 点别处取消。文本用 "Ctrl+J" 这种写法，和 config.ini 里的 HotkeyTrad 一致。
static std::wstring g_hotkeyText;
static bool         g_hkCapturing = false;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// 保存之后通知正在运行的输入法：把配置重读一遍（不用重启任何应用）
static void NotifyReload() {
    const UINT msg = PandaReloadMessage();
    const wchar_t* classes[] = { L"PandaWubiStatusBar", L"PandaWubiCandidateWnd" };
    for (const wchar_t* cls : classes) {
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, cls, nullptr)) != nullptr) {
            PostMessageW(h, msg, 0, 0);
        }
    }
}

static std::wstring ConfigPath() {
    // 数据目录（用户可指向网盘同步夹）；WUBI_INI 可整体覆盖，供测试/便携使用
    return wubi::GetConfigPath();
}
enum {
    ID_L1_0 = 100, ID_L1_1, ID_L1_2,
    ID_SM_0 = 110, ID_SM_1,
    ID_TRAD = 121,
    ID_HK_TRAD = 122,
    ID_CAND = 130, ID_FONT = 131, ID_BG = 132, ID_INSET = 133, ID_DICT = 134,
    ID_VERT = 135,
    ID_TXT = 136, ID_BORDER = 139, ID_THEME = 145,
    ID_DATA_DIR = 140, ID_DATA_BROWSE = 141, ID_DATA_DEFAULT = 142, ID_DATA_OPEN = 143,
    ID_FACE = 144, ID_PHRASE = 146,
    ID_SAVE = 200, ID_CANCEL = 201
};

// ---------------------------------------------------------------- 标签页
// 所有控件在窗口创建时就全部建好，切换标签只做 Show/Hide：
// 值都还在控件里，所以隐藏页上的设置保存时照样读得到。
static const int kSideW = 132;
static const wchar_t* kPageNames[] = { L"输入", L"外观", L"热键", L"词库", L"数据" };
static const int kPageCount = 5;
static int  g_page = 0;
static std::vector<HWND> g_pageCtrls[kPageCount];
static int  g_buildPage = -1;       // 建控件时当前页；-1 = 不属于任何页（底部按钮）
static HWND g_sideWnd = nullptr;

static void Track(HWND h) {
    if (g_buildPage >= 0 && g_buildPage < kPageCount) g_pageCtrls[g_buildPage].push_back(h);
}

static void ShowPage(HWND hwnd, int page) {
    (void)hwnd;
    if (page < 0 || page >= kPageCount) return;
    g_page = page;
    for (int i = 0; i < kPageCount; ++i) {
        for (HWND h : g_pageCtrls[i]) ShowWindow(h, i == page ? SW_SHOW : SW_HIDE);
    }
    if (g_sideWnd) InvalidateRect(g_sideWnd, nullptr, TRUE);
}

// ---------------------------------------------------------------- 控件工厂
static HWND MakeStatic(HWND p, const wchar_t* t, int x, int y, int w, int hgt) {
    HWND h = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                             S(x), S(y), S(w), S(hgt), p, nullptr, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    Track(h);
    return h;
}
static HWND MakeGroup(HWND p, const wchar_t* t, int x, int y, int w, int hgt) {
    HWND h = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                             S(x), S(y), S(w), S(hgt), p, nullptr, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_fontTitle, TRUE);
    Track(h);
    return h;
}
static HWND MakeRadio(HWND p, const wchar_t* t, int x, int y, int w, int id, bool checked, bool groupStart = false) {
    HWND h = CreateWindowExW(0, L"BUTTON", t,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON |
                                 (groupStart ? WS_GROUP : 0),
                             S(x), S(y), S(w), S(22), p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (checked) SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
    Track(h);
    return h;
}
static HWND MakeCheck(HWND p, const wchar_t* t, int x, int y, int w, int id, bool checked, bool groupStart = false) {
    HWND h = CreateWindowExW(0, L"BUTTON", t,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | (groupStart ? WS_GROUP : 0),
                             S(x), S(y), S(w), S(22), p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (checked) SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
    Track(h);
    return h;
}
static HWND MakeEdit(HWND p, int x, int y, int w, int id, const wchar_t* text, int limit) {
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                             S(x), S(y), S(w), S(24), p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(h, EM_SETLIMITTEXT, (WPARAM)limit, 0);
    Track(h);
    return h;
}
static HWND MakeButton(HWND p, const wchar_t* t, int x, int y, int w, int id, bool def) {
    HWND h = CreateWindowExW(0, L"BUTTON", t,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                             S(x), S(y), S(w), S(30), p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    Track(h);
    return h;
}

static void EndCapture(HWND h) {
    g_hkCapturing = false;
    SetWindowTextW(h, g_hotkeyText.c_str());
    SetFocus(h);   // 焦点留在按钮上，方便再按一次重录
}

// 热键按钮的子类化：录制期间把按键吃下来，翻译成 "Ctrl+J" 这种文本。
// 为什么要子类化：按键消息只会发给有焦点的控件，主窗口过程收不到。
static LRESULT CALLBACK HotkeyBtnProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_GETDLGCODE:
        // 录制期间让 Tab/Enter/Esc 也送到这里，别被 IsDialogMessage 拿去翻焦点/点默认按钮
        if (g_hkCapturing) return DLGC_WANTALLKEYS;
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (!g_hkCapturing) break;
        const UINT vk = (UINT)wp;
        if (vk == VK_ESCAPE) { EndCapture(h); return 0; }
        const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool win   = ((GetKeyState(VK_LWIN) & 0x8000) != 0) ||
                           ((GetKeyState(VK_RWIN) & 0x8000) != 0);
        auto mods = [&]() {
            std::wstring s;
            if (ctrl)  s += L"Ctrl+";
            if (alt)   s += L"Alt+";
            if (shift) s += L"Shift+";
            if (win)   s += L"Win+";
            return s;
        };
        if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
            vk == VK_LWIN || vk == VK_RWIN) {
            SetWindowTextW(h, (mods() + L"…").c_str());   // 还在按修饰键：先显示进度
            return 0;
        }
        wubi::Hotkey hk;
        hk.ctrl = ctrl; hk.alt = alt; hk.shift = shift; hk.win = win; hk.vk = vk;
        if (!hk.ctrl && !hk.alt && !hk.win) {
            SetWindowTextW(h, L"要带 Ctrl / Alt / Win");
            return 0;
        }
        // 规范化 + 校验：Format 再 Parse 得回来的才是输入法认得的键
        wubi::Hotkey check;
        const std::wstring text = wubi::FormatHotkey(hk);
        if (text.empty() || !wubi::ParseHotkey(text, check)) {
            SetWindowTextW(h, L"这个键不支持，换一个");
            return 0;
        }
        g_hotkeyText = text;
        g_hkCapturing = false;
        SetWindowTextW(h, g_hotkeyText.c_str());
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (g_hkCapturing) return 0;       // 录制期间按键抬起不外传（免得触发按钮/菜单）
        break;
    case WM_KILLFOCUS:
        if (g_hkCapturing) EndCapture(h);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static HWND MakeHotkeyButton(HWND p, int x, int y, int w, int id, const std::wstring& text) {
    HWND h = MakeButton(p, text.c_str(), x, y, w, id, false);
    SetWindowSubclass(h, HotkeyBtnProc, 1, 0);
    return h;
}

static void ReadUiInto(HWND hwnd) {
    g_s.Level1Mode = SendMessageW(GetDlgItem(hwnd, ID_L1_0), BM_GETCHECK, 0, 0) == BST_CHECKED ? 0
                   : SendMessageW(GetDlgItem(hwnd, ID_L1_1), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 2;
    g_s.SortMode   = SendMessageW(GetDlgItem(hwnd, ID_SM_1), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
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
    auto readColor = [&](int id, int& out) {
        wchar_t buf[32] = { 0 };
        GetWindowTextW(GetDlgItem(hwnd, id), buf, 31);
        const wchar_t* p = buf;
        if (*p == L'#') ++p;
        if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) p += 2;
        const unsigned v = (unsigned)wcstoul(p, nullptr, 16);
        if (v <= 0xFFFFFF) out = (int)v;
    };
    readColor(ID_BG, g_s.CandBgColor);
    readColor(ID_TXT, g_s.CandTextColor);
    readColor(ID_BORDER, g_s.CandBorderColor);
    {
        HWND cb = GetDlgItem(hwnd, ID_THEME);
        const int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
        g_s.Theme = (sel == 3) ? L"crane" : ((sel == 2) ? L"dog" : ((sel == 1) ? L"cat" : L"panda"));
    }
    {
        HWND cb = GetDlgItem(hwnd, ID_FACE);
        wchar_t face[128] = { 0 };
        GetWindowTextW(cb, face, ARRAYSIZE(face) - 1);
        g_s.CandFontFace = face[0] ? face : L"Microsoft YaHei UI";
    }
    g_s.CandInset = SendMessageW(GetDlgItem(hwnd, ID_INSET), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
    {
        // 热键文本来自录制按钮；万一状态不对（比如录制到一半直接点保存），退回原值
        wubi::Hotkey hk;
        if (!g_hotkeyText.empty() && wubi::ParseHotkey(g_hotkeyText, hk)) {
            g_s.HotkeyTrad = wubi::FormatHotkey(hk);
        }
    }
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

// ---------------------------------------------------------------- 侧栏（Logo + 标签）
static HBRUSH g_sideBg = nullptr, g_sideSel = nullptr, g_sideHover = nullptr, g_accent = nullptr;
static HFONT  g_fontSideTitle = nullptr, g_fontTab = nullptr, g_fontTiny = nullptr;
static HICON  g_logo = nullptr;
static int    g_hoverTab = -1;

static int TabTopPx()    { return S(120); }
static int TabHeightPx() { return S(34); }
static int TabIndexAt(int y) {
    const int t = y - TabTopPx();
    if (t < 0) return -1;
    const int i = t / TabHeightPx();
    return (i >= 0 && i < kPageCount) ? i : -1;
}

// 侧栏单独一个子窗口：自己画、自己收鼠标，主窗口那堆控件重绘时不会把它擦花
static LRESULT CALLBACK SideProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g_sideBg);
        SetBkMode(dc, TRANSPARENT);

        if (g_logo) {
            const int sz = S(56);
            DrawIconEx(dc, (rc.right - sz) / 2, S(16), g_logo, sz, sz, 0, nullptr, DI_NORMAL);
        }
        HFONT old = (HFONT)SelectObject(dc, g_fontSideTitle);
        SetTextColor(dc, RGB(0xF2, 0xF4, 0xF8));
        RECT t1 = { 0, S(78), rc.right, S(100) };
        DrawTextW(dc, L"熊猫五笔", -1, &t1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, g_fontTiny);
        SetTextColor(dc, RGB(0x93, 0x9D, 0xAB));
        RECT t2 = { 0, S(98), rc.right, S(114) };
        DrawTextW(dc, L"Panda Wubi", -1, &t2, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        for (int i = 0; i < kPageCount; ++i) {
            RECT ir = { 0, TabTopPx() + i * TabHeightPx(), rc.right, TabTopPx() + (i + 1) * TabHeightPx() };
            if (i == g_page)          FillRect(dc, &ir, g_sideSel);
            else if (i == g_hoverTab) FillRect(dc, &ir, g_sideHover);
            if (i == g_page) {
                RECT ar = { 0, ir.top + S(8), S(3), ir.bottom - S(8) };
                FillRect(dc, &ar, g_accent);
            }
            SelectObject(dc, g_fontTab);
            SetTextColor(dc, i == g_page ? RGB(0xF2, 0xF4, 0xF8) : RGB(0xA6, 0xB0, 0xBE));
            RECT tx = { S(26), ir.top, rc.right - S(6), ir.bottom };
            DrawTextW(dc, kPageNames[i], -1, &tx, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int i = TabIndexAt(GET_Y_LPARAM(lp));
        if (i != g_hoverTab) {
            g_hoverTab = i;
            InvalidateRect(h, nullptr, TRUE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
        }
        SetCursor(LoadCursorW(nullptr, i >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hoverTab = -1;
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    case WM_LBUTTONDOWN: {
        const int i = TabIndexAt(GET_Y_LPARAM(lp));
        if (i >= 0 && i != g_page) PostMessageW(GetParent(h), WM_APP + 1, (WPARAM)i, 0);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void BuildChildren(HWND hwnd) {
    // 正文字体用雅黑；分组框标题用同尺寸的半粗体，靠"分组框"划分区块，避免一堆左对齐的散装控件
    const int pt = 9;
    auto mk = [](int points, int weight) {
        return CreateFontW(-S(points * 96 / 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    };
    g_font      = mk(pt, FW_NORMAL);
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_fontTitle = mk(pt, FW_SEMIBOLD);
    if (!g_fontTitle) g_fontTitle = g_font;
    g_fontSideTitle = mk(12, FW_SEMIBOLD);
    g_fontTab       = mk(10, FW_NORMAL);
    g_fontTiny      = mk(8, FW_NORMAL);

    g_sideBg    = CreateSolidBrush(RGB(0x2B, 0x30, 0x39));
    g_sideSel   = CreateSolidBrush(RGB(0x3A, 0x42, 0x50));
    g_sideHover = CreateSolidBrush(RGB(0x33, 0x3A, 0x45));
    g_accent    = CreateSolidBrush(RGB(0x6B, 0xBF, 0x59));

    // 侧栏 Logo 用主题图标（dist\panda.ico / cat.ico / dog.ico / crane.ico）
    {
        wchar_t self[MAX_PATH] = { 0 };
        GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
        std::wstring dir(self);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir.erase(slash);
            const std::wstring icoFile = (g_s.Theme == L"crane") ? L"crane.ico"
                                   : (g_s.Theme == L"dog")   ? L"dog.ico"
                                   : (g_s.Theme == L"cat")   ? L"cat.ico" : L"panda.ico";
            g_logo = (HICON)LoadImageW(nullptr, (dir + L"\\" + icoFile).c_str(), IMAGE_ICON,
                                       S(56), S(56), LR_LOADFROMFILE);
        }
    }

    RECT crc = { 0, 0, S(556), S(300) };
    GetClientRect(hwnd, &crc);
    g_sideWnd = CreateWindowExW(0, L"PandaSettingsSidebar", L"", WS_CHILD | WS_VISIBLE,
                                0, 0, S(kSideW), crc.bottom, hwnd, nullptr, nullptr, nullptr);
    if (g_sideWnd) SetWindowPos(g_sideWnd, HWND_TOP, 0, 0, S(kSideW), crc.bottom, SWP_NOACTIVATE);

    const int contentX = kSideW + 16;   // 148
    const int groupW   = 392;
    const int inX      = contentX + 14; // 162
    const int top      = 16;

    // ------------------------------------------------ 页 0：输入
    g_buildPage = 0;
    {
        MakeGroup(hwnd, L" 一级简码（输入多个键时如何处理）", contentX, top, groupW, 96);
        MakeRadio(hwnd, L"不处理：长码候选里照常出现",         inX, top + 24, 340, ID_L1_0, g_s.Level1Mode == 0, true);
        MakeRadio(hwnd, L"隐藏：彻底不出现在候选里",           inX, top + 46, 340, ID_L1_1, g_s.Level1Mode == 1);
        MakeRadio(hwnd, L"沉底：放到最后，仍可选中（默认）",   inX, top + 68, 340, ID_L1_2, g_s.Level1Mode == 2);

        const int y2 = top + 96 + 8;
        MakeGroup(hwnd, L" 候选排序", contentX, y2, groupW, 100);
        MakeRadio(hwnd, L"码短优先（默认，与 QQ五笔 接近）", inX, y2 + 24, 340, ID_SM_0, g_s.SortMode == 0, true);
        MakeRadio(hwnd, L"纯词频优先",                       inX, y2 + 46, 340, ID_SM_1, g_s.SortMode == 1);
        MakeCheck(hwnd, (L"输出繁体（" + g_hotkeyText + L" 可临时切换）").c_str(),
                  inX, y2 + 70, 340, ID_TRAD, g_s.TradChinese != 0, true);
    }

    // ------------------------------------------------ 页 1：外观
    g_buildPage = 1;
    {
        MakeGroup(hwnd, L" 候选窗外观", contentX, top, groupW, 184);
        MakeStatic(hwnd, L"候选个数", inX, top + 28, 64, 20);
        {
            wchar_t nb[8];
            wsprintfW(nb, L"%d", g_s.CandCount);
            MakeEdit(hwnd, inX + 64, top + 24, 52, ID_CAND, nb, 1);
        }
        MakeStatic(hwnd, L"字号", inX + 148, top + 28, 44, 20);
        {
            wchar_t fb[8];
            wsprintfW(fb, L"%d", g_s.CandFontPt);
            MakeEdit(hwnd, inX + 192, top + 24, 48, ID_FONT, fb, 2);
        }
        MakeStatic(hwnd, L"pt（8–24）", inX + 246, top + 28, 100, 20);

        // 字体（可输入，也可从下拉里选系统字体）
        MakeStatic(hwnd, L"字体", inX, top + 60, 44, 20);
        {
            HWND cb = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", g_s.CandFontFace.c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                                      S(inX + 44), S(top + 56), S(150), S(240), hwnd,
                                      (HMENU)(INT_PTR)ID_FACE, nullptr, nullptr);
            SendMessageW(cb, WM_SETFONT, (WPARAM)g_font, TRUE);
            Track(cb);
            HDC dc = GetDC(hwnd);
            LOGFONTW lf = {};
            lf.lfCharSet = DEFAULT_CHARSET;
            EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW* lpelf, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
                HWND combo = (HWND)p;
                if (lpelf->lfFaceName[0] != L'@') {   // 跳过竖排字体
                    if (SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)lpelf->lfFaceName) == CB_ERR) {
                        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)lpelf->lfFaceName);
                    }
                }
                return 1;
            }, (LPARAM)cb, 0);
            ReleaseDC(hwnd, dc);
            SendMessageW(cb, CB_SETCURSEL,
                         (WPARAM)SendMessageW(cb, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)g_s.CandFontFace.c_str()), 0);
        }
        // 主题：影响悬浮栏 Logo、候选窗右下角装饰图、输入指示器图标
        MakeStatic(hwnd, L"主题", inX + 202, top + 60, 44, 20);
        {
            HWND cb = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                      S(inX + 246), S(top + 56), S(116), S(160), hwnd,
                                      (HMENU)(INT_PTR)ID_THEME, nullptr, nullptr);
            SendMessageW(cb, WM_SETFONT, (WPARAM)g_font, TRUE);
            Track(cb);
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"熊猫（默认）");
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"狸花猫");
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"中华田园犬");
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"丹顶鹤");
            SendMessageW(cb, CB_SETCURSEL,
                         (WPARAM)((g_s.Theme == L"crane") ? 3 : (g_s.Theme == L"dog") ? 2
                                : (g_s.Theme == L"cat") ? 1 : 0), 0);
        }

        auto makeColor = [&](int x, int yy, const wchar_t* label, int id, int color, int limit) {
            MakeStatic(hwnd, label, x, yy + 4, 64, 20);
            wchar_t cb[16];
            swprintf_s(cb, L"#%06X", (unsigned)(color & 0xFFFFFF));
            HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", cb,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     S(x + 64), S(yy), S(76), S(24), hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(e, WM_SETFONT, (WPARAM)g_font, TRUE);
            SendMessageW(e, EM_SETLIMITTEXT, (WPARAM)limit, 0);
            Track(e);
        };
        makeColor(inX,       top + 98, L"背景色", ID_BG,     g_s.CandBgColor,     7);
        makeColor(inX + 164, top + 98, L"文字色", ID_TXT,    g_s.CandTextColor,   7);
        makeColor(inX,       top + 128, L"边框色", ID_BORDER, g_s.CandBorderColor, 7);
        MakeCheck(hwnd, L"内框凹陷", inX + 164, top + 126, 96,  ID_INSET, g_s.CandInset != 0, true);
        MakeCheck(hwnd, L"候选竖排", inX + 262, top + 126, 100, ID_VERT,  g_s.CandVertical != 0);
    }

    // ------------------------------------------------ 页 2：热键
    g_buildPage = 2;
    {
        MakeGroup(hwnd, L" 简繁切换热键", contentX, top, groupW, 96);
        MakeStatic(hwnd, L"简繁切换", inX, top + 28, 64, 20);
        MakeHotkeyButton(hwnd, inX + 64, top + 24, 140, ID_HK_TRAD, g_hotkeyText);
        MakeStatic(hwnd, L"点一下按钮，再按下想要的组合键（Esc 取消）。", inX, top + 68, 340, 20);
        MakeStatic(hwnd, L"左右 Shift 都切中/英；这个热键只管简/繁（须带 Ctrl / Alt / Win）。",
                   inX, top + 108, 340, S(22));
    }

    // ------------------------------------------------ 页 3：词库
    g_buildPage = 3;
    {
        MakeGroup(hwnd, L" 词库（data 目录）", contentX, top, groupW, 72);
        {
            HWND cb = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                      S(inX), S(top + 28), S(300), S(220), hwnd,
                                      (HMENU)(INT_PTR)ID_DICT, nullptr, nullptr);
            SendMessageW(cb, WM_SETFONT, (WPARAM)g_font, TRUE);
            Track(cb);
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
            if (selIdx < 0) {
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)g_s.DictFile.c_str());
                selIdx = idx;
            }
            SendMessageW(cb, CB_SETCURSEL, (WPARAM)selIdx, 0);
        }
        MakeStatic(hwnd, L"列出 data 目录里的 *.tsv / *.bin，选一个即用。", inX, top + 88, 340, 20);
        MakeButton(hwnd, L"自定义词 / 短语…", inX, top + 116, 150, ID_PHRASE, false);
        MakeStatic(hwnd, L"自定义词和短语存在数据目录的 user.tsv 里。", inX, top + 154, 340, 20);
    }

    // ------------------------------------------------ 页 4：数据
    g_buildPage = 4;
    {
        MakeGroup(hwnd, L" 数据目录（配置与自定义词/短语都放这里）", contentX, top, groupW, 164);
        {
            HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wubi::GetDataDir().c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
                                      S(inX), S(top + 28), S(340), S(24), hwnd,
                                      (HMENU)(INT_PTR)ID_DATA_DIR, nullptr, nullptr);
            SendMessageW(ed, WM_SETFONT, (WPARAM)g_font, TRUE);
            Track(ed);
        }
        MakeButton(hwnd, L"浏览…", inX,       top + 62, 76, ID_DATA_BROWSE,  false);
        MakeButton(hwnd, L"默认",  inX + 82,  top + 62, 60, ID_DATA_DEFAULT, false);
        MakeButton(hwnd, L"打开",  inX + 148, top + 62, 60, ID_DATA_OPEN,    false);
        MakeStatic(hwnd, L"指向 OneDrive / 坚果云等同步目录，换机器后指回来即可恢复设置和自定义词。",
                   inX, top + 104, 340, S(40));
    }

    // ------------------------------------------------ 底部按钮（不随页面切换）
    g_buildPage = -1;
    MakeButton(hwnd, L"保存", contentX + groupW - 196, 256, 90, ID_SAVE, true);
    MakeButton(hwnd, L"取消", contentX + groupW - 96,  256, 90, ID_CANCEL, false);

    ShowPage(hwnd, 0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        BuildChildren(hwnd);
        return 0;
    case WM_SIZE:                      // 侧栏跟着窗口高度走（全高的一条）
        if (g_sideWnd) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            SetWindowPos(g_sideWnd, HWND_TOP, 0, 0, S(kSideW), rc.bottom, SWP_NOACTIVATE);
        }
        return 0;
    case WM_APP + 1:                   // 侧栏点了某个标签
        ShowPage(hwnd, (int)wp);
        SetFocus(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_ERASEBKGND: {              // 自己也刷背景：避免某些抓图/重绘路径留黑底
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:            // 让分组框里的文字背景跟窗口一致，不留白块
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)g_bg;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_HK_TRAD) {
            // 进入热键录制：焦点给按钮，按键由 HotkeyBtnProc 收
            g_hkCapturing = true;
            SetWindowTextW(GetDlgItem(hwnd, ID_HK_TRAD), L"请按组合键…");
            SetFocus(GetDlgItem(hwnd, ID_HK_TRAD));
            return 0;
        }
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
        if (LOWORD(wp) == ID_PHRASE) {
            // 自定义词 / 短语窗口是独立进程（panda-phrase.exe），和输入法里 Ctrl+Shift+= 打开的是同一个
            wchar_t self[MAX_PATH] = { 0 };
            GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
            std::wstring dir(self);
            const size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) dir.erase(slash);
            const std::wstring exe = dir + L"\\panda-phrase.exe";
            if (ShellExecuteW(hwnd, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= (HINSTANCE)32) {
                MessageBoxW(hwnd, (L"没找到 " + exe).c_str(), kTitle, MB_OK | MB_ICONWARNING);
            }
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
                    MessageBoxW(hwnd, L"数据目录已切换，原有的设置与用户词已复制过去，并已通知正在运行的输入法。",
                                kTitle, MB_OK | MB_ICONINFORMATION);
                }
            }

            const bool ok = wubi::SaveSettings(g_ini, g_s);
            const std::wstring msg = ok
                ? (L"已保存，并已通知正在运行的输入法立即生效（不需要重启任何程序）。\n\n配置与用户词都在：\n" +
                   wubi::GetDataDir() +
                   L"\n\n若某个程序还在用旧版本的输入法 DLL，需要重启那个程序一次。")
                : std::wstring(L"保存失败：请检查数据目录是否可写。");
            if (ok) NotifyReload();
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
// 分页之后高度只按"最高的一页"算：侧栏 Logo + 5 个标签，右侧内容 + 底部按钮
static void MeasureClient(int& w, int& h) {
    w = S(556);
    h = S(300);
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

    WNDCLASSEXW side = { sizeof(side) };
    side.lpfnWndProc = SideProc;
    side.hInstance = GetModuleHandleW(nullptr);
    side.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    side.lpszClassName = L"PandaSettingsSidebar";
    RegisterClassExW(&side);

    int cw = 0, ch = 0;
    MeasureClient(cw, ch);
    RECT rc = { 0, 0, cw, ch };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;

    // 先建在屏幕外、不显示，摆好位置再 ShowWindow：避免"先闪一个位于默认位置的空窗"
    HWND hwnd = CreateWindowExW(0, L"WubiSettingsWnd", kTitle,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
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
            const std::wstring icoFile = (g_s.Theme == L"crane") ? L"crane.ico"
                                   : (g_s.Theme == L"dog") ? L"dog.ico"
                                   : (g_s.Theme == L"cat") ? L"cat.ico" : L"panda.ico";
            const std::wstring icoPath = dir + L"\\" + icoFile;
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
        // Esc 关窗：要在 IsDialogMessageW 之前判，否则会被它当"取消"吞掉
        // （热键录制中除外——那会儿 Esc 是"取消录制"，由按钮自己吃）
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && !g_hkCapturing) break;
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_font) DeleteObject(g_font);
    if (g_fontTitle && g_fontTitle != g_font) DeleteObject(g_fontTitle);
    if (g_fontSideTitle) DeleteObject(g_fontSideTitle);
    if (g_fontTab) DeleteObject(g_fontTab);
    if (g_fontTiny) DeleteObject(g_fontTiny);
    if (g_sideBg) DeleteObject(g_sideBg);
    if (g_sideSel) DeleteObject(g_sideSel);
    if (g_sideHover) DeleteObject(g_sideHover);
    if (g_accent) DeleteObject(g_accent);
    if (g_logo) DestroyIcon(g_logo);
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
    {
        // 热键文本以"规范化后"的形式进界面（config 里手写成 ctrl+j 也会显示成 Ctrl+J）
        wubi::Hotkey hk;
        if (wubi::ParseHotkey(g_s.HotkeyTrad, hk)) {
            g_hotkeyText = wubi::FormatHotkey(hk);
            g_s.HotkeyTrad = g_hotkeyText;
        } else {
            g_hotkeyText = L"Ctrl+J";
            g_s.HotkeyTrad = g_hotkeyText;
        }
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv && argc > 1) {
        const std::wstring cmd = argv[1];
        if (cmd == L"/dump") {
            EnsureStdIo();
            std::printf("ini=%ls\n", g_ini.c_str());
            std::printf("datadir=%ls\n", wubi::GetDataDir().c_str());
            std::printf("userdict=%ls\n", wubi::GetUserWordsPath().c_str());
            std::printf("Level1Mode=%d SortMode=%d TradChinese=%d CandCount=%d CandVertical=%d InputMode=%d\n",
                        g_s.Level1Mode, g_s.SortMode, g_s.TradChinese,
                        g_s.CandCount, g_s.CandVertical, g_s.InputMode);
            std::printf("Theme=%ls CandFontFace=%ls CandFont=%d CandBgColor=#%06X CandTextColor=#%06X CandBorderColor=#%06X CandInset=%d\n",
                        g_s.Theme.c_str(), g_s.CandFontFace.c_str(), g_s.CandFontPt,
                        g_s.CandBgColor & 0xFFFFFF, g_s.CandTextColor & 0xFFFFFF,
                        g_s.CandBorderColor & 0xFFFFFF, g_s.CandInset);
            std::printf("HotkeyTrad=%ls\n", g_s.HotkeyTrad.c_str());
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
                else if (k == L"TradChinese")    { g_s.TradChinese = iv; any = true; }
                else if (k == L"CandCount")      { g_s.CandCount = iv; any = true; }
                else if (k == L"CandVertical")   { g_s.CandVertical = iv; any = true; }
                else if (k == L"Theme")          { g_s.Theme = (v == L"cat") ? L"cat" : ((v == L"dog") ? L"dog" : ((v == L"crane") ? L"crane" : L"panda")); any = true; }
                else if (k == L"HotkeyTrad") {
                    wubi::Hotkey hk;
                    if (!wubi::ParseHotkey(v, hk)) { std::printf("bad hotkey: %ls\n", v.c_str()); LocalFree(argv); return 2; }
                    g_s.HotkeyTrad = wubi::FormatHotkey(hk);
                    any = true;
                }
            }
            if (!any) { std::printf("no known key\n"); LocalFree(argv); return 2; }

            const bool ok = wubi::SaveSettings(g_ini, g_s);
            if (ok) NotifyReload();       // 让正在运行的输入法立刻用上新设置
            std::printf("%s -> Level1Mode=%d SortMode=%d TradChinese=%d CandCount=%d\n",
                        ok ? "saved" : "FAILED", g_s.Level1Mode, g_s.SortMode,
                        g_s.TradChinese, g_s.CandCount);
            LocalFree(argv);
            return ok ? 0 : 1;
        }
    }
    if (argv) LocalFree(argv);
    return RunDialog();
}
