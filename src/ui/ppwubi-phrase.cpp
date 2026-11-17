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
#include <commctrl.h>
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

static std::wstring UserDictPath() {
    wchar_t base[MAX_PATH] = { 0 };
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", base, ARRAYSIZE(base));
    std::wstring dir = (n > 0 && n < ARRAYSIZE(base)) ? std::wstring(base) : std::wstring(L".");
    dir += L"\\PandaWubi";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\user.tsv";
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

// 两个输入框的子类化：回车=保存，Esc=取消
static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) { PostMessageW(g_hwnd, WM_COMMAND, ID_OK, 0); return 0; }
        if (wp == VK_ESCAPE) { PostMessageW(g_hwnd, WM_COMMAND, ID_CANCEL, 0); return 0; }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void CreateChildren(HWND hwnd) {
    const int pad = S(14), rowH = S(24), editH = S(26);

    g_font = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    CreateWindowExW(0, L"STATIC", L"编码（a–y，最多 4 位）：",
                    WS_CHILD | WS_VISIBLE, pad, pad, S(260), S(18), hwnd, nullptr, nullptr, nullptr);
    g_code = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             pad, pad + S(20), S(140), editH, hwnd, (HMENU)(INT_PTR)ID_CODE, nullptr, nullptr);

    CreateWindowExW(0, L"STATIC", L"短语（中文直接打；也可写邮箱、身份证号）：",
                    WS_CHILD | WS_VISIBLE, pad, pad + S(20) + editH + S(14), S(380), S(18),
                    hwnd, nullptr, nullptr, nullptr);
    g_phrase = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               pad, pad + S(20) + editH + S(36), S(380), editH, hwnd, (HMENU)(INT_PTR)ID_PHRASE,
                               nullptr, nullptr);
    if (!g_phrase) {   // 极少见：Msftedit 没起来，退回普通 EDIT（中文要靠别的输入法）
        g_phrase = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   pad, pad + S(20) + editH + S(36), S(380), editH, hwnd,
                                   (HMENU)(INT_PTR)ID_PHRASE, nullptr, nullptr);
    }

    g_hint = CreateWindowExW(0, L"STATIC", L"",
                             WS_CHILD | WS_VISIBLE | SS_LEFT,
                             pad, pad + S(20) + editH + S(36) + editH + S(10), S(380), S(34),
                             hwnd, nullptr, nullptr, nullptr);

    CreateWindowExW(0, L"BUTTON", L"保存(&S)",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    S(256), pad + S(20) + editH + S(36), S(64), S(28), hwnd, (HMENU)(INT_PTR)ID_OK, nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    S(328), pad + S(20) + editH + S(36), S(64), S(28), hwnd, (HMENU)(INT_PTR)ID_CANCEL, nullptr, nullptr);

    for (HWND h : { g_code, g_phrase, g_hint }) SetFont(h, g_font);
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
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_OK:     OnSave(); return 0;
        case ID_CANCEL: DestroyWindow(hwnd); return 0;
        }
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

    const int w = S(410), h = S(210);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST, L"PandaWubiPhraseWindow", L"自定义短语 —— Panda五笔",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                             nullptr, nullptr, hSelf, nullptr);
    if (!g_hwnd) return 1;

    // 居中
    RECT rc = { 0, 0, w, h };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST);
    const int ww = rc.right - rc.left, hh = rc.bottom - rc.top;
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(g_hwnd, HWND_TOP, (sw - ww) / 2, (sh - hh) / 2, ww, hh, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_hwnd, &msg)) continue;   // Tab 切换、按钮默认响应
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
