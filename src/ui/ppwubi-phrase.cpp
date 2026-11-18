// ppwubi-phrase —— 自定义短语录入窗口（独立进程，由输入法 ShellExecute 启动）
//
// 为什么做成独立进程/独立窗口，而不是在输入法里内联录：
//   输入法跑在宿主应用的 UI 线程里，弹自己的模态循环会和 TSF 的焦点/生命周期打架；
//   这个窗口是普通顶层窗口，编码和短语各有一个输入框，短语框用 RichEdit（支持 TSF），
//   所以可以直接用本输入法打中文，也能打邮箱、身份证号这类混杂内容。
//
// 保存格式与输入法一致：追加一行 `编码 <TAB> 短语 <TAB> 权重` 到 %APPDATA%\PandaWubi\user.tsv。
// 追加而不是整表重写：输入法在多个宿主进程里各有一份内存词表，整表写回会互相覆盖。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <imm.h>
#include <commctrl.h>
#include "../engine/wubi_config.h"
#include <cstdio>
#include <string>

#ifndef MSFTEDIT_CLASS
#define MSFTEDIT_CLASS L"RICHEDIT50W"
#endif

static const int ID_CODE  = 101;
static const int ID_PHRASE = 102;
static const int ID_OK    = IDOK;
static const int ID_CANCEL = IDCANCEL;

static HWND g_hwnd = nullptr;
static HWND g_code = nullptr;
static HWND g_phrase = nullptr;
static HWND g_hint = nullptr;
static HFONT g_font = nullptr;
static int   g_dpi = 96;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// 用户词文件跟输入法共用同一个数据目录（用户可指向网盘同步夹）
static std::wstring UserDictPath() {
    return wubi::GetUserWordsPath();
}

static void SetFont(HWND h, HFONT f) { SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); }

static std::wstring GetText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)GetWindowTextLengthW(h));
    return s;
}

// 短语里不允许换行/制表符（TSV 字段），顺手去掉首尾空白
static std::wstring CleanPhrase(std::wstring s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\t') continue;
        out.push_back(c);
    }
    while (!out.empty() && (out.front() == L' ')) out.erase(out.begin());
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

static std::wstring CleanCode(const std::wstring& in) {
    std::wstring out;
    for (wchar_t c : in) {
        if (out.size() >= 4) break;
        if (c >= L'A' && c <= L'Z') out.push_back((wchar_t)(c - L'A' + L'a'));
        else if (c >= L'a' && c <= L'z') out.push_back(c);
    }
    return out;
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static bool AppendUserWord(const std::string& code, const std::string& phrase) {
    const std::wstring path = UserDictPath();
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"ab") != 0 || !fp) return false;
    char line[4096];
    const int n = sprintf_s(line, sizeof(line), "%s\t%s\t2147483647\r\n", code.c_str(), phrase.c_str());
    bool ok = false;
    if (n > 0 && (size_t)n < sizeof(line)) ok = (fwrite(line, 1, (size_t)n, fp) == (size_t)n);
    fflush(fp);
    fclose(fp);
    return ok;
}

static void ShowHint(const wchar_t* text) {
    if (g_hint) SetWindowTextW(g_hint, text);
}

static void OnSave() {
    const std::wstring code = CleanCode(GetText(g_code));
    const std::wstring phrase = CleanPhrase(GetText(g_phrase));

    if (code.empty()) {
        ShowHint(L"编码不能为空：用 a–y 的字母，最多 4 位（z 是五笔保留键）");
        SetFocus(g_code);
        return;
    }
    if (phrase.empty()) {
        ShowHint(L"短语不能为空");
        SetFocus(g_phrase);
        return;
    }

    const std::string u8code = ToUtf8(code);
    const std::string u8phrase = ToUtf8(phrase);
    if (!AppendUserWord(u8code, u8phrase)) {
        ShowHint(L"写入失败：检查 %APPDATA%\\PandaWubi 是否可写");
        return;
    }
    // 让编码框显示规范化后的值（大写转小写、超长截断）
    SetWindowTextW(g_code, code.c_str());
    DestroyWindow(g_hwnd);
}

// 编码框只留英文字母，最多 4 位（粘贴/输入法一并清洗，规则见 wubi::FilterCode）
static void SanitizeCode() {
    wchar_t buf[64] = { 0 };
    GetWindowTextW(g_code, buf, ARRAYSIZE(buf) - 1);
    const std::wstring cur(buf);
    const std::wstring out = wubi::FilterCode(cur, 4);
    if (out != cur) {
        SetWindowTextW(g_code, out.c_str());
        SendMessageW(g_code, EM_SETSEL, (WPARAM)out.size(), (LPARAM)out.size());
    }
}

// 两个输入框的子类化：回车=保存，Esc=取消；编码框只收字母
static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    const bool isCode = (id == 1);     // 1=编码框 2=短语框

    if (msg == WM_GETDLGCODE) {
        // 多行 RichEdit 默认"要 Tab"（会插入制表符），焦点就出不去了；
        // 去掉 DLGC_WANTTAB/WANTALLKEYS，让 IsDialogMessage 用 Tab 切换焦点。
        const LRESULT base = DefSubclassProc(h, msg, wp, lp);
        if (!isCode) return (base & ~(DLGC_WANTTAB | DLGC_WANTALLKEYS)) | DLGC_WANTCHARS;
        return base;
    }
    if (msg == WM_CHAR && isCode) {
        const wchar_t c = (wchar_t)wp;
        if (c < 0x20) return DefSubclassProc(h, msg, wp, lp);        // 退格/回车等控制键放行
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'))) return 0;   // 只收英文字母
        if (GetWindowTextLengthW(h) >= 4) return 0;                  // 最多 4 位
        return DefSubclassProc(h, msg, wp, lp);
    }
    if (msg == WM_PASTE && isCode) {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        SanitizeCode();
        return r;
    }
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) { PostMessageW(g_hwnd, WM_COMMAND, ID_OK, 0); return 0; }
        if (wp == VK_ESCAPE) { PostMessageW(g_hwnd, WM_COMMAND, ID_CANCEL, 0); return 0; }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// 客户区尺寸（逻辑单位，创建时按 DPI 缩放）
static const int kClientW = 420;
static const int kClientH = 262;

static void CreateChildren(HWND hwnd) {
    const int pad = S(16);
    const int editH = S(26);
    const int labelH = S(18);
    const int w = S(kClientW) - pad * 2;

    g_font = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    int y = pad;

    // 编码
    HWND lblCode = CreateWindowExW(0, L"STATIC", L"编码",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   pad, y, S(60), labelH, hwnd, nullptr, nullptr, nullptr);
    g_code = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             pad + S(60), y - S(3), S(120), editH,
                             hwnd, (HMENU)(INT_PTR)ID_CODE, nullptr, nullptr);
    SendMessageW(g_code, EM_SETLIMITTEXT, (WPARAM)4, 0);
    // 编码框只收 a–z：把输入法从这里摘掉，直接敲字母就行，不用先按 Shift 切英文。
    // （短语框保留输入法，那边要能打中文。）
    ImmAssociateContextEx(g_code, nullptr, 0);
    HWND hintCode = CreateWindowExW(0, L"STATIC", L"a–z 字母，最多 4 位",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    pad + S(60) + S(120) + S(10), y, w - S(190), labelH,
                                    hwnd, nullptr, nullptr, nullptr);
    y += editH + S(12);

    // 短语
    HWND lblPhrase = CreateWindowExW(0, L"STATIC", L"短语",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     pad, y, S(60), labelH, hwnd, nullptr, nullptr, nullptr);
    const int phraseH = S(96);
    g_phrase = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                                   ES_AUTOVSCROLL | ES_WANTRETURN,
                               pad, y + labelH, w, phraseH,
                               hwnd, (HMENU)(INT_PTR)ID_PHRASE, nullptr, nullptr);
    if (!g_phrase) {   // 极少见：Msftedit 没起来，退回普通 EDIT（中文要靠别的输入法）
        g_phrase = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL,
                                   pad, y + labelH, w, phraseH,
                                   hwnd, (HMENU)(INT_PTR)ID_PHRASE, nullptr, nullptr);
    }
    y += labelH + phraseH + S(8);

    // 提示行（校验失败的原因写在这里）
    g_hint = CreateWindowExW(0, L"STATIC", L"中文直接打（本输入法在框里可用），邮箱/身份证号也能写；回车保存，Esc 取消。z 打头的编码与 Z 重复冲突。",
                             WS_CHILD | WS_VISIBLE | SS_LEFT,
                             pad, y, w, S(34), hwnd, nullptr, nullptr, nullptr);
    y += S(34) + S(10);

    // 底部按钮（右对齐）
    const int btnW = S(92), btnH = S(30);
    CreateWindowExW(0, L"BUTTON", L"保存",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    pad + w - btnW * 2 - S(10), y, btnW, btnH,
                    hwnd, (HMENU)(INT_PTR)ID_OK, nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    pad + w - btnW, y, btnW, btnH,
                    hwnd, (HMENU)(INT_PTR)ID_CANCEL, nullptr, nullptr);

    for (HWND h : { lblCode, hintCode, lblPhrase, g_hint }) SetFont(h, g_font);
    EnumChildWindows(hwnd, [](HWND h, LPARAM) -> BOOL { SetFont(h, g_font); return TRUE; }, 0);

    SetWindowSubclass(g_code, EditProc, 1, 0);
    SetWindowSubclass(g_phrase, EditProc, 2, 0);

    SetFocus(g_code);
}
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateChildren(hwnd);
        return 0;
    case WM_ERASEBKGND: {              // 自己刷背景：避免某些抓图/重绘路径留黑底
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CODE && HIWORD(wp) == EN_CHANGE) {
            // 不能在 EN_CHANGE 里直接 SetWindowText（重入会让编辑框把自己的文本回滚成空），
            // 延后到消息循环再清洗：粘贴/输入法/拖放都走这条路。
            PostMessageW(hwnd, WM_APP + 1, 0, 0);
            return 0;
        }
        switch (LOWORD(wp)) {
        case ID_OK:     OnSave(); return 0;
        case ID_CANCEL: DestroyWindow(hwnd); return 0;
        }
        return 0;
    case WM_APP + 1:                   // 编码框清洗（延后执行，避免重入）
        SanitizeCode();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int BarDpi() {
    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
}

int WINAPI wWinMain(HINSTANCE hSelf, HINSTANCE, LPWSTR, int) {
    // 一般不会重复开：已经有一个窗口时就把它提到前面
    CreateMutexW(nullptr, FALSE, L"Local\\PandaWubiPhraseWindow");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND old = FindWindowW(L"PandaWubiPhraseWindow", nullptr);
        if (old) { ShowWindow(old, SW_SHOWNORMAL); SetForegroundWindow(old); }
        return 0;
    }

    // 先声明 DPI 感知：否则 175% 缩放下窗口会被系统位图拉伸，控件也按 96 排版
    {
        typedef BOOL (WINAPI* SetCtx)(HANDLE);
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        auto setCtx = u32 ? (SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : nullptr;
        BOOL ok = FALSE;
        if (setCtx) ok = setCtx((HANDLE)-4);        // PER_MONITOR_AWARE_V2
        if (!ok) SetProcessDPIAware();
    }
    g_dpi = BarDpi();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    LoadLibraryW(L"Msftedit.dll");

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hSelf;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"PandaWubiPhraseWindow";
    RegisterClassExW(&wc);

    const int w = S(kClientW), h = S(kClientH);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST, L"PandaWubiPhraseWindow", L"自定义短语 —— Panda五笔",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             -32000, -32000, w, h,
                             nullptr, nullptr, hSelf, nullptr);
    if (!g_hwnd) return 1;

    // 标题栏图标（和状态栏/设置窗共用 dist\panda.ico）
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
                SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hi);
                SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hi);
            }
        }
    }

    // 居中到工作区，摆好位置再显示（先建在屏幕外，避免闪一下空窗）
    RECT rc = { 0, 0, w, h };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST);
    const int ww = rc.right - rc.left, hh = rc.bottom - rc.top;
    RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - ww) / 2;
    const int y = work.top + ((work.bottom - work.top) - hh) / 2;
    SetWindowPos(g_hwnd, HWND_TOP, x, y, ww, hh, SWP_NOACTIVATE);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_hwnd, &msg)) continue;   // Tab 切换、按钮默认响应
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
