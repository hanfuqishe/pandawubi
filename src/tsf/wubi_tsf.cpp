// QQ五笔 替代输入法 —— TSF 文本服务
// 设计约束（针对 QQ五笔 在 Win11 上的两个病根）：
//   1) 惰性初始化：不在 DllMain/ImeInquire 阶段碰文件；词库首次用到才加载
//   2) 候选窗位置只用 ITfContextView::GetTextExt + 每屏 DPI，不使用 SetCursorPos/mouse_event/全局钩子
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <msctf.h>
#include <ctffunc.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <windowsx.h>
#include <olectl.h>
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <mutex>
#include <new>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

#include "../engine/wubi_engine.h"
#include "wubi_langbar.h"
#include "wubi_bar.h"
#include "../engine/wubi_config.h"
#include "../engine/wubi_composer.h"
#include "../engine/wubi_english.h"

static wubi::EnglishDict g_eng;   // 全局英文词表（进程内共享）
// 宿主进程的可执行文件名（小写），用于 [app:<exe>] 配置覆盖
static std::wstring HostAppName() {
    wchar_t path[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) return std::wstring();
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) s.erase(0, slash + 1);
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
#include "../ui/cand_layout.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// ---------------------------------------------------------------- 标识
// 与 QQ五笔 的 {6AF21238-DB8B-45DB-8567-7F7023AFC493} 刻意区分
static const CLSID CLSID_WubiTsf =
    { 0x7f3b0c21, 0x9d55, 0x4f0e, { 0xb1, 0xc2, 0x5a, 0x6d, 0x8e, 0x9f, 0x0a, 0x11 } };
static const GUID GUID_WubiProfile =
    { 0x7f3b0c21, 0x9d55, 0x4f0e, { 0xb1, 0xc2, 0x5a, 0x6d, 0x8e, 0x9f, 0x0a, 0x12 } };

static const LANGID kLangId = 0x0804;          // zh-CN
static const wchar_t* kWndClassCand = L"PandaWubiCandidateWnd";
static const wchar_t* kWndClassUI   = L"PandaWubiUIWnd";

static HINSTANCE g_hInst = nullptr;


// ---------------------------------------------------------------- 候选框装饰图（PNG + GDI+）
static ULONG_PTR g_gdiplusToken = 0;
static Gdiplus::Bitmap* g_artBitmap = nullptr;

static void EnsureGdiplus() {
    if (g_gdiplusToken) return;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &in, nullptr);
}

static Gdiplus::Bitmap* LoadArtFromResource(int id) {
    if (g_artBitmap) return g_artBitmap;
    EnsureGdiplus();
    if (!g_gdiplusToken) return nullptr;

    HRSRC res = FindResourceW(g_hInst, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!res) return nullptr;
    const DWORD size = SizeofResource(g_hInst, res);
    HGLOBAL mem = LoadResource(g_hInst, res);
    if (!mem || !size) return nullptr;
    void* data = LockResource(mem);
    if (!data) return nullptr;

    HGLOBAL buf = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!buf) return nullptr;
    void* dst = GlobalLock(buf);
    memcpy(dst, data, size);
    GlobalUnlock(buf);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(buf, TRUE, &stream)) || !stream) {
        GlobalFree(buf);
        return nullptr;
    }
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(stream);
    stream->Release();
    if (bmp && bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; bmp = nullptr; }
    g_artBitmap = bmp;
    return g_artBitmap;
}
static LONG      g_cRef  = 0;

// ---------------------------------------------------------------- 诊断日志
// 目的: 我没有可用的激活环境，日志是唯一能从现场取证的通道。
// 位置: %TEMP%\wubi_tsf_<pid>.log ；每次写入即 flush，崩了也能留下线索。
static void LogLine(const wchar_t* fmt, ...) {
    static wchar_t path[MAX_PATH] = { 0 };
    if (path[0] == 0) {
        wchar_t env[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"WUBI_LOG", env, ARRAYSIZE(env));
        if (n > 0 && n < ARRAYSIZE(env)) {
            wcscpy_s(path, env);
        } else {
            // 默认放在 %LOCALAPPDATA%\PandaWubi\（持久、用户好找）；失败再退 %TEMP%
            wchar_t base[MAX_PATH];
            DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", base, ARRAYSIZE(base));
            bool ok = false;
            if (m > 0 && m < ARRAYSIZE(base)) {
                wchar_t dir[MAX_PATH];
                swprintf_s(dir, L"%s\\PandaWubi", base);
                CreateDirectoryW(dir, nullptr);      // 已存在则忽略
                if (swprintf_s(path, L"%s\\panda_tsf_%lu.log", dir, GetCurrentProcessId()) > 0) ok = true;
            }
            if (!ok) {
                wchar_t dir[MAX_PATH];
                if (GetTempPathW(ARRAYSIZE(dir), dir) == 0) return;
                swprintf_s(path, L"%spanda_tsf_%lu.log", dir, GetCurrentProcessId());
            }
        }
    }

    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(msg, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1200];
    swprintf_s(line, L"[%02d:%02d:%02d.%03d] %s\r\n",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    DWORD wrote = 0;
    if (sz.QuadPart == 0) {
        const WORD bom = 0xFEFF;
        WriteFile(h, &bom, sizeof(bom), &wrote, nullptr);
    }
    WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &wrote, nullptr);
    CloseHandle(h);
}

// 用户词文件：%APPDATA%\PandaWubi\user.tsv（自造词 + 词频自学习结果）
static std::wstring GetUserWordsPath() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", base, ARRAYSIZE(base));
    std::wstring dir = (n > 0 && n < ARRAYSIZE(base)) ? std::wstring(base) : std::wstring(L".");
    dir += L"\\PandaWubi";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\user.tsv";
}

// ---------------------------------------------------------------- 词库单例
class DictHolder {
public:
    wubi::Dictionary& Get() {
        std::call_once(once_, [this] { Load(); });
        return dict_;
    }
    bool Loaded() const { return loaded_; }
private:
    void Load();
    wubi::Dictionary dict_;
    std::once_flag   once_;
    bool             loaded_ = false;
};

static DictHolder g_dict;

static std::wstring GetConfigPath();   // 定义在后面
static HRESULT LaunchSettingsWindow(HWND parent);   // defined later

static std::wstring GetDictPath() {
    // 1) 环境变量覆盖（开发/测试用）
    wchar_t env[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"WUBI_DICT", env, ARRAYSIZE(env));
    if (n > 0 && n < ARRAYSIZE(env)) return env;

    wchar_t mod[MAX_PATH] = { 0 };
    std::wstring dir;
    if (GetModuleFileNameW(g_hInst, mod, ARRAYSIZE(mod))) {
        dir = mod;
        const size_t p = dir.find_last_of(L"\\/");
        if (p != std::wstring::npos) dir.erase(p);
    }
    if (dir.empty()) dir = L".";

    // 2) 配置里指定的词库（data 目录下的文件名），缺失则回退默认
    const wubi::Settings cfg = wubi::LoadSettings(GetConfigPath(), HostAppName());
    const std::wstring want = dir + L"\\data\\" + cfg.DictFile;
    if (GetFileAttributesW(want.c_str()) != INVALID_FILE_ATTRIBUTES) return want;

    if (cfg.DictFile != L"qqwubi86.dict.tsv") {
        LogLine(L"dict: %ls not found, fallback to default", want.c_str());
    }

    // 3) 默认
    return dir + L"\\data\\qqwubi86.dict.tsv";
}

static std::wstring GetConfigPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, ARRAYSIZE(buf));
    std::wstring base = (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring(L".");
    const std::wstring dir  = base + L"\\PandaWubi";
    const std::wstring path = dir + L"\\config.ini";

    // 一次性迁移：老版本用的是 %APPDATA%\\WubiTsf
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::wstring oldDir = base + L"\\WubiTsf";
        const std::wstring oldIni = oldDir + L"\\config.ini";
        if (GetFileAttributesW(oldIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
            CreateDirectoryW(dir.c_str(), nullptr);
            CopyFileW(oldIni.c_str(), path.c_str(), FALSE);
            const std::wstring oldUser = oldDir + L"\\user.tsv";
            const std::wstring newUser = dir + L"\\user.tsv";
            if (GetFileAttributesW(oldUser.c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(newUser.c_str()) == INVALID_FILE_ATTRIBUTES) {
                CopyFileW(oldUser.c_str(), newUser.c_str(), FALSE);
            }
        }
    }
    return path;
}
void DictHolder::Load() {
    // 优先共享只读映射的二进制（多进程共享物理页，载入仅毫秒级）；
    // 缺失时回退 TSV（约 100ms，且每进程各占一份内存）。
    std::wstring p = GetDictPath();
    // 注意：词库文件名是 qqwubi86.dict.tsv，不能简单去掉最后一个 '.' 之后的部分，
    // 否则会得到 qqwubi86.dict.bin（不存在）。显式处理后缀。
    std::wstring binPath = p;
    const std::wstring sufDictTsv = L".dict.tsv";
    const std::wstring sufTsv = L".tsv";
    if (binPath.size() > sufDictTsv.size() &&
        binPath.compare(binPath.size() - sufDictTsv.size(), sufDictTsv.size(), sufDictTsv) == 0) {
        binPath = binPath.substr(0, binPath.size() - sufDictTsv.size()) + L".bin";
    } else if (binPath.size() > sufTsv.size() &&
               binPath.compare(binPath.size() - sufTsv.size(), sufTsv.size(), sufTsv) == 0) {
        binPath = binPath.substr(0, binPath.size() - sufTsv.size()) + L".bin";
    }

    long long n = dict_.LoadBinary(binPath);
    if (n > 0) {
        loaded_ = true;
        const long long u = dict_.LoadUserWords(GetUserWordsPath());
        LogLine(L"dict load(bin): path=%s entries=%lld ok=1 userWords=%lld", binPath.c_str(), n, u);
        return;
    }
    n = dict_.Load(p);
    loaded_ = (n > 0);
    // 用户词（自造词 + 词频自学习结果）
    const long long u = dict_.LoadUserWords(GetUserWordsPath());
    LogLine(L"dict load(tsv): path=%s entries=%lld codes=%zu ok=%d userWords=%lld",
            p.c_str(), n, dict_.CodeCount(), (int)loaded_, u);
}

// ---------------------------------------------------------------- 候选窗
class CCandidateWindow {
public:
    ~CCandidateWindow() { Destroy(); }

    void SetItems(const std::vector<std::wstring>& items, bool vertical) {
        items_ = items;
        vertical_ = vertical;
        RecalcAndResize();
    }
    void SetOnClick(std::function<void(int)> fn) { onClick_ = std::move(fn); }
    void SetLetterLabels(bool on) { if (letterLabels_ != on) { letterLabels_ = on; RecalcAndResize(); } }
    // 纯文本徽标：不显示序号（用于中/英状态提示，避免被当成候选词）
    void SetPlain(bool on) { if (plain_ != on) { plain_ = on; RecalcAndResize(); } }
    // 外观：字号(pt) / 背景色 / 内框凹陷
    void SetCode(const std::wstring& c) { code_ = c; if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE); }

    void SetStyle(int fontPt, COLORREF bg, bool inset) {
        fontPt_ = fontPt > 6 ? fontPt : 14;
        bgCol_  = bg;
        inset_  = inset;
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        if (hwnd_) {
            const UINT dpi = Dpi();
            const int h = -MulDiv(fontPt_, (int)dpi, 72);
            font_ = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!art_) art_ = (HBITMAP)LoadImageW(g_hInst, MAKEINTRESOURCEW(2), IMAGE_BITMAP,
                                              0, 0, LR_CREATEDIBSECTION);
        }
        RecalcAndResize();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
    }
    // 短暂显示后自动隐藏
    void ShowBriefly(const RECT& caret, UINT ms) {
        ShowAt(caret);
        if (hwnd_) SetTimer(hwnd_, kBriefTimerId, ms ? ms : 700, nullptr);
    }
    // rect 为屏幕坐标（光标插入点）
    void ShowAt(const RECT& caret);
    void Hide();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Ensure();
    void Destroy();
    void RecalcAndResize();
    void Paint(HDC dc);
    UINT Dpi() const;
    int  padX_ = 4, padY_ = 2;

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<std::wstring> items_;
    std::vector<std::wstring> labels_;     // 形如 "1.你好"
    wubi::CandLayout layout_;
    bool vertical_ = false;
    bool letterLabels_ = false;   // 便捷输入模式下用 a. b. c.;
    bool plain_ = false;                    // 纯文本徽标（不显示序号）
    int      fontPt_ = 14;                    // 字号（pt）
    COLORREF bgCol_  = RGB(0xF7, 0xF9, 0xFC);    // 背景色
    bool     inset_  = true;                  // 内框凹陷
    HBITMAP  art_    = nullptr;               // 右下角熊猫装饰
    std::wstring code_;                       // 顶部显示的组码
    int      headH_  = 0;                     // 顶部框高度
    int      rowH_   = 0;                     // 每行高度
    int      artW_   = 0;                     // 右下角装饰图尺寸（右侧留白）
    static const UINT kBriefTimerId = 0xB1;  // 徽标自动隐藏定时器
    std::function<void(int)> onClick_;
    int  width_ = 0, height_ = 0;
};

UINT CCandidateWindow::Dpi() const {
    if (hwnd_) {
        typedef UINT (WINAPI* FnGetDpiForWindow)(HWND);
        static FnGetDpiForWindow fn = (FnGetDpiForWindow)GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        if (fn) { UINT d = fn(hwnd_); if (d) return d; }
    }
    HDC dc = GetDC(nullptr);
    UINT d = 96;
    if (dc) { d = (UINT)GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc); }
    return d ? d : 96;
}

void CCandidateWindow::Ensure() {
    if (hwnd_) return;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = CCandidateWindow::WndProc;
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = kWndClassCand;
    wc.style         = CS_DROPSHADOW;   // 系统柔和阴影
    RegisterClassExW(&wc);   // 已注册时忽略失败

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kWndClassCand, L"", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, g_hInst, this);
    if (!hwnd_) return;

    UINT dpi = Dpi();
    int h = -MulDiv(fontPt_, (int)dpi, 72);
    font_ = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void CCandidateWindow::Destroy() {
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void CCandidateWindow::RecalcAndResize() {
    Ensure();
    if (!hwnd_ || items_.empty()) { layout_ = wubi::CandLayout(); width_ = height_ = 0; return; }

    const UINT dpi = Dpi();
    padX_ = MulDiv(7, (int)dpi, 96);
    padY_ = MulDiv(4, (int)dpi, 96);
    rowH_ = MulDiv(20, (int)dpi, 96);
    headH_ = code_.empty() ? 0 : MulDiv(24, (int)dpi, 96);

    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    labels_.clear();
    int maxW = MulDiv(90, (int)dpi, 96);
    for (size_t i = 0; i < items_.size(); ++i) {
        std::wstring t;
        if (plain_) t = items_[i];
        else if (letterLabels_) t = std::wstring(1, (wchar_t)(L'a' + i)) + L". " + items_[i];
        else t = std::to_wstring(i + 1) + L". " + items_[i];
        SIZE sz;
        GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
        if (sz.cx > maxW) maxW = sz.cx;
        labels_.push_back(std::move(t));
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);

    artW_ = MulDiv(74, (int)dpi, 96);   // 装饰图占一列，避免压住文字

    layout_ = wubi::CandLayout();   // 自绘，不再用外部布局
    if (vertical_ || plain_) {
        width_ = maxW + padX_ * 2 + artW_;
        height_ = headH_ + (int)items_.size() * rowH_ + padY_ * 2;
    } else {
        width_ = (int)items_.size() * (maxW + padX_) + padX_ + artW_;
        height_ = headH_ + rowH_ + padY_ * 2;
    }
    if (width_ < MulDiv(120, (int)dpi, 96)) width_ = MulDiv(120, (int)dpi, 96);
    height_ += MulDiv(4, (int)dpi, 96);
    height_ += MulDiv(18, (int)dpi, 96);          // 给装饰图留出高度
    layout_.width = width_;
    layout_.height = height_;
}
void CCandidateWindow::Paint(HDC dc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W = rc.right, H = rc.bottom;
    const UINT dpi = Dpi();

    // ---- 背景：淡绿 -> 白的竖向渐变 ----
    for (int y = 0; y < H; ++y) {
        const int t = H > 1 ? (y * 255 / (H - 1)) : 0;
        const int r = 238 + (255 - 238) * t / 255;
        const int g = 249 + (255 - 249) * t / 255;
        const int b = 236 + (255 - 236) * t / 255;
        HBRUSH br = CreateSolidBrush(RGB(r, g, b));
        RECT line = { 0, y, W, y + 1 };
        FillRect(dc, &line, br);
        DeleteObject(br);
    }

    // ---- 右下角：熊猫吃竹子（GDI+，PNG 带 alpha）----
    if (Gdiplus::Bitmap* art = LoadArtFromResource(3)) {
        const int artSize = artW_ > 0 ? artW_ - MulDiv(6, (int)dpi, 96) : MulDiv(44, (int)dpi, 96);
        Gdiplus::Graphics gfx(dc);
        gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        gfx.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        // 作为背景水印：降低不透明度，保证文字清晰
        Gdiplus::ImageAttributes ia;
        Gdiplus::ColorMatrix cm = {};
        cm.m[0][0] = 1.0f; cm.m[1][1] = 1.0f; cm.m[2][2] = 1.0f;
        cm.m[3][3] = 0.72f;   // 整体不透明度（细节图需要更实一些）
        ia.SetColorMatrix(&cm);
        gfx.DrawImage(art, Gdiplus::RectF((Gdiplus::REAL)(W - artSize - MulDiv(2, (int)dpi, 96)),
                                        (Gdiplus::REAL)(H - artSize - MulDiv(2, (int)dpi, 96)),
                                        (Gdiplus::REAL)artSize, (Gdiplus::REAL)artSize),
                      0.0f, 0.0f, (Gdiplus::REAL)art->GetWidth(), (Gdiplus::REAL)art->GetHeight(),
                      Gdiplus::UnitPixel, &ia);
    }

    // ---- 外框（淡绿灰）----
    HPEN border = CreatePen(PS_SOLID, 1, RGB(168, 205, 168));
    HGDIOBJ oldPen = SelectObject(dc, border);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int radius = MulDiv(6, (int)dpi, 96);
    RoundRect(dc, 0, 0, W, H, radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, font_);

    int y = padY_;

    // ---- 顶部：组码 + 翻页箭头 ----
    if (headH_ > 0) {
        RECT hb = { padX_ / 2, y, W - padX_ / 2, y + headH_ - MulDiv(2, (int)dpi, 96) };
        HBRUSH hbg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &hb, hbg);
        DeleteObject(hbg);
        HPEN hp = CreatePen(PS_SOLID, 1, RGB(150, 196, 150));
        HGDIOBJ op = SelectObject(dc, hp);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, hb.left, hb.top, hb.right, hb.bottom, radius, radius);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(hp);

        SetTextColor(dc, RGB(30, 90, 40));
        RECT tr = hb;
        tr.left += MulDiv(6, (int)dpi, 96);
        DrawTextW(dc, code_.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SetTextColor(dc, RGB(120, 160, 120));
        RECT ar = hb;
        ar.right -= MulDiv(6, (int)dpi, 96);
        DrawTextW(dc, L"\u25C0 \u25B6", -1, &ar, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        y += headH_;
    }

    // ---- 候选行 ----
    const size_t n = labels_.size();
    if (vertical_ || plain_) {
        for (size_t i = 0; i < n; ++i) {
            RECT r = { padX_, y + (int)i * rowH_, W - padX_ - artW_ / 3, y + (int)(i + 1) * rowH_ };
            const std::wstring& full = labels_[i];
            const size_t dot = full.find(L'.');
            const std::wstring num = (dot == std::wstring::npos) ? std::wstring() : full.substr(0, dot + 1);
            const std::wstring word = (dot == std::wstring::npos) ? full : full.substr(dot + 1);
            SetTextColor(dc, i == 0 ? RGB(52, 140, 66) : RGB(120, 156, 120));
            RECT nr = r;
            DrawTextW(dc, num.c_str(), -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SIZE sn = { 0, 0 };
            GetTextExtentPoint32W(dc, num.c_str(), (int)num.size(), &sn);
            SetTextColor(dc, i == 0 ? RGB(20, 40, 24) : RGB(48, 56, 50));
            RECT wr = r;
            wr.left += sn.cx;
            DrawTextW(dc, word.c_str(), -1, &wr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    } else {
        int x = padX_;
        for (size_t i = 0; i < n; ++i) {
            RECT r = { x, y, x + MulDiv(110, (int)dpi, 96), y + rowH_ };
            SetTextColor(dc, i == 0 ? RGB(20, 40, 24) : RGB(48, 56, 50));
            DrawTextW(dc, labels_[i].c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            x += MulDiv(110, (int)dpi, 96);
        }
    }


    SelectObject(dc, oldFont);
}
LRESULT CALLBACK CCandidateWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CCandidateWindow* self = (CCandidateWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        LPCREATESTRUCT cs = (LPCREATESTRUCT)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_MOUSEACTIVATE:
    case WM_TIMER:
        if (wp == CCandidateWindow::kBriefTimerId) {
            KillTimer(hwnd, CCandidateWindow::kBriefTimerId);
            if (self) self->Hide();
        }
        return 0;
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (self) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int idx = self->layout_.HitTest(pt.x, pt.y);
            if (idx >= 0 && self->onClick_) {
                auto fn = self->onClick_;   // 拷贝：回调可能销毁窗口状态
                fn(idx);
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self) self->Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CCandidateWindow::ShowAt(const RECT& caret) {
    Ensure();
    if (!hwnd_ || items_.empty()) { Hide(); return; }
    if (width_ <= 0 || height_ <= 0) RecalcAndResize();

    int x = caret.left;
    int y = caret.bottom + 2;

    // 限制在该显示器工作区内（多显示器 + 混 DPI 场景）
    MONITORINFO mi = { sizeof(mi) };
    HMONITOR mon = MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        if (x + width_ > mi.rcWork.right)  x = mi.rcWork.right - width_;
        if (x < mi.rcWork.left)            x = mi.rcWork.left;
        if (y + height_ > mi.rcWork.bottom) y = caret.top - height_ - 2;
        if (y < mi.rcWork.top)             y = mi.rcWork.top;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void CCandidateWindow::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

// ---------------------------------------------------------------- 编辑会话封装
class CEditSession : public ITfEditSession {
public:
    CEditSession(std::function<HRESULT(TfEditCookie)> fn) : fn_(std::move(fn)), ref_(1) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&ref_);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP DoEditSession(TfEditCookie ec) override { return fn_(ec); }
private:
    std::function<HRESULT(TfEditCookie)> fn_;
    LONG ref_;
};

// ---------------------------------------------------------------- 悬浮状态栏归属
// TSF 为每个线程各建一个 CTextService 实例（同一进程里也常有多个：实测某进程挂了 3 条栏）。
// 以前每个实例各自 Create 一条 StatusBar，桌面于是攒出 8~10 条重叠的「中 简 ，。 ⚙」，
// 而且切走应用时谁也收不掉（切应用不会触发 Deactivate）。
// 现在：进程内只建一个窗口（SharedBar），实例计数决定它的生死；只有「拥有者」实例
// 能往这条栏里写状态，点击也回到拥有者。显隐由线程焦点回调 + 500ms 前台自检收敛。
class CTextService;

static wubi::StatusBar& SharedBar();
static CTextService* g_barOwner = nullptr;   // 当前拥有这条栏的实例（最近获得焦点的那个）
static int           g_barUsers = 0;         // 已激活的实例数

// 每个 TSF 实例属于各自的线程，激活/退出可能并发：建窗与引用计数必须串行
struct BarLock {
    CRITICAL_SECTION cs;
    BarLock()  { InitializeCriticalSection(&cs); }
    ~BarLock() { DeleteCriticalSection(&cs); }
};
static BarLock& BarGuard() { static BarLock g; return g; }

struct BarGuardScope {
    BarGuardScope()  { EnterCriticalSection(&BarGuard().cs); }
    ~BarGuardScope() { LeaveCriticalSection(&BarGuard().cs); }
};

// ---------------------------------------------------------------- 文本服务
class CTextService : public ITfTextInputProcessorEx,
                     public ITfThreadMgrEventSink,
                     public ITfKeyEventSink,
                     public ITfCompositionSink,
                     public ITfThreadFocusSink {
public:
    CTextService() { InterlockedIncrement(&g_cRef); }
    ~CTextService() { InterlockedDecrement(&g_cRef); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&ref_);
        if (c == 0) delete this;
        return c;
    }

    // ITfTextInputProcessor
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override {
        return ActivateEx(ptim, tid, 0);
    }
    STDMETHODIMP Deactivate() override;

    // ITfTextInputProcessorEx
    STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnSetFocus(ITfDocumentMgr*, ITfDocumentMgr*) override;
    STDMETHODIMP OnPushContext(ITfContext*) override { return S_OK; }
    STDMETHODIMP OnPopContext(ITfContext*) override { return S_OK; }

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL) override { return S_OK; }
    // 关键：宿主（RichEdit / 记事本 / 多数 TSF 应用）先问 OnTestKeyDown「这个键你要不要」，
    // 说不想要就直接自己消化掉了，OnKeyDown 永远不会被调用。以前这里一律回 FALSE，
    // 于是实测（tsf-host.exe -send）按键根本进不了输入法，打出来的是原样字母。
    // 现在只声明真正会被吃掉的键，口径与 OnKeyDown 保持一致。
    STDMETHODIMP OnTestKeyDown(ITfContext*, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override {
        if (pfEaten) *pfEaten = WantsKey((UINT)wParam, lParam) ? TRUE : FALSE;
        return S_OK;
    }
    STDMETHODIMP OnKeyDown(ITfContext*, WPARAM, LPARAM, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* pfEaten) override {
        if (pfEaten) *pfEaten = FALSE;
        return S_OK;
    }
    STDMETHODIMP OnKeyUp(ITfContext*, WPARAM wParam, LPARAM, BOOL* pfEaten) override {
        if (pfEaten) *pfEaten = FALSE;
        LogLine(L"OnKeyUp vk=0x%02X shiftDown=%d shiftUsed=%d", (unsigned)wParam, (int)m_shiftDown, (int)m_shiftUsed);
        // 单击 Shift（按下期间没按别的键）切换中英文
        const UINT vk = (UINT)wParam;
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
            if (m_shiftDown && !m_shiftUsed) {
                m_english = !m_english;
                if (m_comp && m_context) CancelComposition(m_context);
                LogLine(L"toggle english=%d", (int)m_english);
                SetInputModeCompartments(!m_english);
                FlashStatus(m_english ? L"\u82f1" : L"\u4e2d");
                BarSync();
            }
            m_shiftDown = false;
            m_shiftUsed = false;
        }
        return S_OK;
    }
    STDMETHODIMP OnPreservedKey(ITfContext*, REFGUID, BOOL* pfEaten) override {
        if (pfEaten) *pfEaten = FALSE;
        return S_OK;
    }

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie, ITfComposition* pComposition) override;

    // ITfThreadFocusSink：切走应用的一瞬间就把悬浮栏收起来（计时器兜底）
    STDMETHODIMP OnSetThreadFocus() override;
    STDMETHODIMP OnKillThreadFocus() override;

private:
    HRESULT DoEdit(ITfContext* pic, std::function<HRESULT(TfEditCookie)> fn);
    HRESULT StartComposition(ITfContext* pic, const std::wstring& text);
    void    ApplyInputAttribute(TfEditCookie ec, ITfRange* pRange);
    HRESULT UpdateCompositionText(ITfContext* pic);
    HRESULT CommitText(ITfContext* pic, const std::wstring& text,
                       const std::string& learnCode = std::string(), bool resetComposer = true);
    void    EnsureComposer();
    void    SyncCodeFromComposer();
    HRESULT CommitString(ITfContext* pic, const std::wstring& text);
    void    LoadDataOnce();
    HRESULT CancelComposition(ITfContext* pic);
    void    RefreshCandidates(ITfContext* pic);
    void    Reset();
    void    OnBarClick(int idx);   // 悬浮栏点击（回调进入拥有者实例）
    void    BarSync();             // 只在拥有者实例上生效
    bool    WantsKey(UINT vk, LPARAM lp);   // OnTestKeyDown 的口径（与 OnKeyDown 一致）
    static bool IsCodeKey(UINT vk, wchar_t* out);

    LONG ref_ = 1;
    ITfThreadMgr* m_tim = nullptr;
    TfClientId    m_tid = 0;
    DWORD         m_adviseTim = TF_INVALID_COOKIE;
    DWORD         m_adviseFocus = TF_INVALID_COOKIE;
    bool          m_keyAdvised = false;

    ITfComposition*        m_comp = nullptr;
    ITfContextComposition* m_compHost = nullptr;
    ITfContext*            m_context = nullptr;
    std::wstring    m_code;
    wubi::Composer  m_composer;        // 组码/候选状态机（唯一实现）
    bool            m_composerReady = false;
    int             m_commitCount = 0;
    std::wstring    m_lastCommit;   // 上一次上屏的内容（Z 键重复用）
    bool            m_repeatPending = false;   // Z 按下后等待空格上屏
    std::wstring    m_repeatText;
    bool            m_quickMode = false;       // 分号进入的"便捷输入"
    std::string     m_quickDigits;
    #define m_eng g_eng
    std::wstring    m_engTyped;                // 英文补全已输入的字母（保留实际大小写）
    bool            m_engActive = false;      // 英文单词补全模式（区别于自由英文 m_english）
    std::vector<std::wstring> m_quickForms;
    bool            m_letterLabels = false;   // 便捷输入模式下候选用 a. b. c.
    ITfLangBarItem* m_langBar = nullptr;   // 语言栏按钮（显示自定义图标）
    wubi::DigitPunctGate m_gate;   // 数字后单次半角逗号/句号

    // ---- 自定义词组 / 自定义短语（Ctrl+= / Ctrl+Shift+=）----
    enum class CustomMode { None, Word, Phrase };
    CustomMode           m_custom = CustomMode::None;
    int                  m_customPhase = 0;    // 短语：0=输编码 1=录内容
    std::string          m_customCode;         // 短语编码（a..y，最多 4）
    std::string          m_customWord;         // 词组：逐字拼起来的 UTF-8
    std::vector<std::wstring> m_customChars;   // 词组：字序列（← 取字、Del 删首）
    std::wstring         m_customText;         // 短语：已录内容
    bool                 m_customAscii = false; // 短语录入：false=中文（五笔） true=英文（按原样）
    std::deque<std::wstring> m_recentChars;    // 最近上屏的单字（← 依次回取）
    void BeginCustomWord(ITfContext* pic);
    void BeginCustomPhrase(ITfContext* pic);
    void ExitCustom(ITfContext* pic, bool cancel);
    bool CustomKey(ITfContext* pic, UINT vk, LPARAM lp, bool shift, BOOL* pfEaten);
    void RefreshCustomPrompt();
    void SaveCustomWord(ITfContext* pic);
    void SaveCustomPhrase(ITfContext* pic);
    void RememberCommitted(const std::wstring& text);
    bool CaptureCustomText(const std::wstring& text);   // 短语录制：把上屏内容收进缓冲

    // 中英文切换（单击 Shift）与状态提示
    bool m_english   = false;
    bool m_shiftDown = false;
    bool m_shiftUsed = false;
    bool m_statusShown = false;
    RECT m_lastCaret = {};
    bool m_hasCaret  = false;
    void FlashStatus(const wchar_t* text);
    void SetInputModeCompartments(bool chinese);
    // 英文补全
    void SetEnglish(bool on);
    bool QueryCaretRect(RECT& out);
    std::string EngPrefix() const;            // 查词用的全小写前缀
    void EngUpdate(ITfContext* pic);
    void EngType(ITfContext* pic, wchar_t ch);   // ch 保留真实大小写
    void EngCommit(ITfContext* pic, int idx, bool withSpace);
    void EngCancel(ITfContext* pic);
    void ShowCandidateList(const std::vector<std::wstring>& display);
    HRESULT UpdateCompositionTextWith(ITfContext* pic, const std::wstring& text);
    void BeginRepeat();
    void BeginQuick();
    void RefreshQuickCandidates(ITfContext* pic);
    void ExitQuick(ITfContext* pic, bool cancel);
    TfGuidAtom      m_attrAtom = 0;        // 组码显示属性 atom
    bool            m_attrTried = false;
    CCandidateWindow m_candWnd;
    wubi::Punctuation     m_punc;
    wubi::Transliteration m_trad;
    bool  m_tradEnabled = false;
    bool  m_puncEnabled = true;
    int   m_candCount   = 9;
    wubi::Settings m_settings;
    bool  m_dataLoaded  = false;
};

static CTextService* g_service = nullptr;   // 仅用于 DllCanUnloadNow 之外的调试；不持有引用

// 进程内唯一的状态栏对象。DLL 卸载时它的析构会兜底 Destroy，避免留下指向已卸载代码的窗口。
static wubi::StatusBar& SharedBar() {
    static wubi::StatusBar bar;
    return bar;
}

void CTextService::BarSync() {
    if (g_barOwner == this)
        SharedBar().SetState(m_english, m_tradEnabled, m_puncEnabled);
}

void CTextService::OnBarClick(int idx) {
    switch (idx) {
    case 0: m_english = !m_english; SetInputModeCompartments(!m_english); break;   // 中/英
    case 1: m_tradEnabled = !m_tradEnabled; m_settings.TradChinese = m_tradEnabled ? 1 : 0;
            wubi::SaveSettings(GetConfigPath(), m_settings); break;               // 简/繁
    case 2: m_puncEnabled = !m_puncEnabled; m_settings.EnablePunction = m_puncEnabled ? 1 : 0;
            wubi::SaveSettings(GetConfigPath(), m_settings); break;               // 标点
    case 4: {
        // 关于
        wchar_t msg[512];
        swprintf_s(msg,
            L"Panda五笔\n\n构建: %hs %hs\n词库: %ls\n词条: %lld\n\n中/英: 单击 Shift 或点状态栏\n简繁: Ctrl+Shift+T\n标点: Ctrl+Shift+P",
            __DATE__, __TIME__,
            m_settings.DictFile.c_str(), (long long)g_dict.Get().EntryCount());
        MessageBoxW(nullptr, msg, L"关于 Panda五笔", MB_OK | MB_ICONINFORMATION);
        break;
    }
    default: LaunchSettingsWindow(nullptr); break;                              // 设置
    }
    BarSync();
    LogLine(L"bar click idx=%d en=%d trad=%d punc=%d", idx,
            (int)m_english, (int)m_tradEnabled, (int)m_puncEnabled);
}

STDMETHODIMP CTextService::OnSetThreadFocus() {
    {
        BarGuardScope guard;
        g_barOwner = this;
        BarSync();
        SharedBar().Show(true);
    }
    LogLine(L"OnSetThreadFocus: tid=%u visible=%d", m_tid, (int)SharedBar().Visible());
    return S_OK;
}

STDMETHODIMP CTextService::OnKillThreadFocus() {
    // 只临时收起来（不清“要栏”意愿）：回到前台时 Show(true)/计时器会把它放回来。
    // 同进程里别的线程刚接手这条栏时（owner 已易主）不能收。
    bool owned = false;
    {
        BarGuardScope guard;
        owned = (g_barOwner == this);
        if (owned) SharedBar().HideNow();
    }
    LogLine(L"OnKillThreadFocus: tid=%u owned=%d", m_tid, (int)owned);
    return S_OK;
}

bool CTextService::IsCodeKey(UINT vk, wchar_t* out) {
    if (vk >= 'A' && vk <= 'Z') { if (vk - 'A' <= ('y' - 'a')) { *out = (wchar_t)(vk - 'A' + 'a'); return true; } }
    return false;
}

STDMETHODIMP CTextService::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfTextInputProcessor) ||
        IsEqualIID(riid, IID_ITfTextInputProcessorEx)) {
        *ppv = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink)) {
        *ppv = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfKeyEventSink)) {
        *ppv = static_cast<ITfKeyEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfCompositionSink)) {
        *ppv = static_cast<ITfCompositionSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfThreadFocusSink)) {
        *ppv = static_cast<ITfThreadFocusSink*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP CTextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD) {
    if (!ptim) return E_INVALIDARG;
    m_tim = ptim;
    m_tim->AddRef();
    m_tid = tid;

    ITfSource* src = nullptr;
    if (SUCCEEDED(m_tim->QueryInterface(IID_ITfSource, (void**)&src)) && src) {
        src->AdviseSink(IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this),
                        &m_adviseTim);
        src->AdviseSink(IID_ITfThreadFocusSink, static_cast<ITfThreadFocusSink*>(this),
                        &m_adviseFocus);
        src->Release();
    }

    ITfKeystrokeMgr* ksm = nullptr;
    if (SUCCEEDED(m_tim->QueryInterface(IID_ITfKeystrokeMgr, (void**)&ksm)) && ksm) {
        const HRESULT hAdv = ksm->AdviseKeyEventSink(m_tid, static_cast<ITfKeyEventSink*>(this), TRUE);
        m_keyAdvised = SUCCEEDED(hAdv);
        LogLine(L"AdviseKeyEventSink tid=%u hr=0x%08lX advised=%d",
                m_tid, (unsigned long)hAdv, (int)m_keyAdvised);
        ksm->Release();
    } else {
        LogLine(L"AdviseKeyEventSink: no ITfKeystrokeMgr");
    }
    // 配置：%APPDATA%\PandaWubi\config.ini（键名与 QQ五笔 Config.ini 一致，可直接迁移）
    m_settings   = wubi::LoadSettings(GetConfigPath(), HostAppName());
    // 候选窗外观众（字号/背景色/凹陷）
    m_candWnd.SetStyle(m_settings.CandFontPt,
                       (COLORREF)(m_settings.CandBgColor & 0xFFFFFF),
                       m_settings.CandInset != 0);
    m_puncEnabled = m_settings.EnablePunction != 0;
    m_tradEnabled = m_settings.TradChinese != 0;
    m_candCount   = m_settings.CandCount;

    // 激活时就准备好词库（而不是等第一次按键），并把状态一次记清：
    // mmap 模式下约 0.1ms，不会拖慢激活。
    EnsureComposer();
    {
        auto probe = g_dict.Get().Lookup("wqvb", 1);
        // 词条是 UTF-8；日志是宽字符，必须显式转换（%hs 会按 C locale 变成乱码）
        std::wstring probeW = L"<none>";
        if (!probe.empty()) {
            int n = MultiByteToWideChar(CP_UTF8, 0, probe[0].word.c_str(),
                                        (int)probe[0].word.size(), nullptr, 0);
            if (n > 0) {
                probeW.assign((size_t)n, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, probe[0].word.c_str(),
                                    (int)probe[0].word.size(), &probeW[0], n);
            }
        }
        LogLine(L"ready: bin=%d entries=%zu codes=%zu probe_wqvb=%s",
                (int)g_dict.Get().UsingBinary(), g_dict.Get().EntryCount(),
                g_dict.Get().CodeCount(), probeW.c_str());
    }

    LogLine(L"build: %hs %hs", __DATE__, __TIME__);   // 便于判断进程里跑的是哪一版

    // 英文词表（data\eng_words.txt，与 DLL 同目录）
    {
        static bool engLoaded = false;
        if (!engLoaded) {
            wchar_t modPath[MAX_PATH] = { 0 };
            GetModuleFileNameW(g_hInst, modPath, ARRAYSIZE(modPath));
            std::wstring dir(modPath);
            const size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                dir.erase(slash);
                const long long n = g_eng.Load(dir + L"\\data\\eng_words.txt");
                LogLine(L"eng load: words=%lld", n);
                if (n > 0) engLoaded = true;
            }
        }
    }
    // 注册语言栏按钮：Windows 的输入指示器只从这里取图标
    if (m_tim && !m_langBar) {
        ITfLangBarItemMgr* lbm = nullptr;
        if (SUCCEEDED(m_tim->QueryInterface(IID_ITfLangBarItemMgr, (void**)&lbm)) && lbm) {
            static const GUID kLangBarItemGuid = { 0x8c4f2a31, 0x6b7d, 0x4e29, { 0x9a, 0x3c, 0x1d, 0x5e, 0x7f, 0x9b, 0x0c, 0x21 } };
            IUnknown* unk = CreateLangBarItem(CLSID_WubiTsf, kLangBarItemGuid, g_hInst, 1);
            if (unk) {
                ITfLangBarItem* item = nullptr;
                if (SUCCEEDED(unk->QueryInterface(IID_ITfLangBarItem, (void**)&item)) && item) {
                    const HRESULT hAdd = lbm->AddItem(item);
                    if (SUCCEEDED(hAdd)) { m_langBar = item; LogLine(L"langbar item added hr=0x%08lX", (unsigned long)hAdd); }
                    else { item->Release(); LogLine(L"langbar AddItem failed hr=0x%08lX", (unsigned long)hAdd); }
                }
                unk->Release();
            }
            lbm->Release();
        }
    }
    LogLine(L"app config: exe=%ls trad=%d punc=%d vertical=%d",
            HostAppName().c_str(), (int)m_tradEnabled, (int)m_puncEnabled, m_settings.CandVertical);
    // 自绘悬浮状态栏：进程内唯一（见上方 SharedBar 注释），点击转发给当前拥有者
    {
        BarGuardScope guard;
        ++g_barUsers;
        if (g_barUsers == 1) {
            SharedBar().Create(g_hInst, GetConfigPath(), [](int idx) {
                if (g_barOwner) g_barOwner->OnBarClick(idx);
            });
        }
        g_barOwner = this;
        BarSync();
        SharedBar().Show(true);
    }
    LogLine(L"bar: users=%d owner=%p visible=%d", g_barUsers, (void*)this,
            (int)SharedBar().Visible());
    LogLine(L"ActivateEx: tid=%u punc=%d trad=%d candCount=%d",
            m_tid, (int)m_puncEnabled, (int)m_tradEnabled, m_candCount);
    g_service = this;
    return S_OK;
}

STDMETHODIMP CTextService::Deactivate() {
    // 归还这条栏：最后一个实例走人时才真正销毁窗口
    int usersLeft = 0;
    {
        BarGuardScope guard;
        if (g_barOwner == this) g_barOwner = nullptr;
        if (g_barUsers > 0) --g_barUsers;
        usersLeft = g_barUsers;
        if (g_barUsers == 0) {
            SharedBar().Show(false);
            SharedBar().Destroy();
        }
    }
    LogLine(L"Deactivate: tid=%u barUsers=%d", m_tid, usersLeft);
    if (m_langBar) {
        ITfLangBarItemMgr* lbm = nullptr;
        if (m_tim && SUCCEEDED(m_tim->QueryInterface(IID_ITfLangBarItemMgr, (void**)&lbm)) && lbm) {
            lbm->RemoveItem(m_langBar);
            lbm->Release();
        }
        m_langBar->Release();
        m_langBar = nullptr;
    }
    if (m_commitCount > 0) {
        const bool ok = g_dict.Get().SaveUserWords(GetUserWordsPath());
        LogLine(L"user words flush on deactivate: ok=%d total=%zu", (int)ok, g_dict.Get().UserWordCount());
    }
    Reset();
    if (m_tim) {
        if (m_keyAdvised) {
            ITfKeystrokeMgr* ksm = nullptr;
            if (SUCCEEDED(m_tim->QueryInterface(IID_ITfKeystrokeMgr, (void**)&ksm)) && ksm) {
                ksm->UnadviseKeyEventSink(m_tid);
                ksm->Release();
            }
            m_keyAdvised = false;
        }
        if (m_adviseTim != TF_INVALID_COOKIE) {
            ITfSource* src = nullptr;
            if (SUCCEEDED(m_tim->QueryInterface(IID_ITfSource, (void**)&src)) && src) {
                src->UnadviseSink(m_adviseTim);
                if (m_adviseFocus != TF_INVALID_COOKIE) src->UnadviseSink(m_adviseFocus);
                src->Release();
            }
            m_adviseTim = TF_INVALID_COOKIE;
            m_adviseFocus = TF_INVALID_COOKIE;
        }
        m_tim->Release();
        m_tim = nullptr;
    }
    g_service = nullptr;
    return S_OK;
}

STDMETHODIMP CTextService::OnSetFocus(ITfDocumentMgr* pimdFocus, ITfDocumentMgr* pimdPrev) {
    // 曾经的实现无条件 Reset()，会在候选窗或无关焦点事件出现时打断组码。
    // 现在只有“焦点离开所有文档”或“换到另一个文档”才重置。
    bool shouldReset = false;
    if (pimdFocus == nullptr) {
        shouldReset = true;
    } else if (m_context) {
        ITfDocumentMgr* cur = nullptr;
        if (SUCCEEDED(m_context->GetDocumentMgr(&cur)) && cur) {
            shouldReset = (cur != pimdFocus);
            cur->Release();
        }
    }
    LogLine(L"OnSetFocus: focus=%p prev=%p comp=%p reset=%d",
            (void*)pimdFocus, (void*)pimdPrev, (void*)m_comp, (int)shouldReset);
    if (shouldReset) Reset();

    // 焦点变化时把中/英隔间状态同步到新文档：系统指示器通常在焦点变化时重新查询
    if (pimdFocus) {
        ITfContext* ctx = nullptr;
        if (SUCCEEDED(pimdFocus->GetTop(&ctx)) && ctx) {
            ITfContext* saved = m_context;
            m_context = ctx;
            SetInputModeCompartments(!m_english);
            m_context = saved;
            ctx->Release();
        }
    }
    return S_OK;
}

void CTextService::Reset() {
    LogLine(L"Reset: comp=%p code=%hs", (void*)m_comp, m_composer.Code().c_str());
    m_code.clear();
    m_composer.Reset();
    m_candWnd.Hide();
    // 焦点丢失时把自定义模式也收干净，避免下次输入还停在"等回车保存"的状态
    if (m_custom != CustomMode::None) {
        m_custom = CustomMode::None;
        m_customPhase = 0;
        m_customCode.clear();
        m_customText.clear();
        m_customChars.clear();
        m_customWord.clear();
        m_customAscii = false;
        LogLine(L"Reset: custom mode cleared");
    }
    if (m_compHost) { m_compHost->Release(); m_compHost = nullptr; }
    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_comp) { m_comp->Release(); m_comp = nullptr; }
}

STDMETHODIMP CTextService::OnCompositionTerminated(TfEditCookie, ITfComposition*) {
    LogLine(L"OnCompositionTerminated: code=%hs comp=%p", m_composer.Code().c_str(), (void*)m_comp);
    if (m_comp) { m_comp->Release(); m_comp = nullptr; }
    m_code.clear();
    m_composer.Reset();
    m_candWnd.Hide();
    return S_OK;
}

HRESULT CTextService::DoEdit(ITfContext* pic, std::function<HRESULT(TfEditCookie)> fn) {
    CEditSession* s = new CEditSession(std::move(fn));
    HRESULT hrSession = E_FAIL;
    HRESULT hr = pic->RequestEditSession(m_tid, s, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
    s->Release();
    if (FAILED(hr)) return hr;
    return hrSession;
}

// 关键：StartComposition 与 SetText 必须在**同一个编辑会话**内完成。
// 若分成两个会话，部分应用/TSF 会在两个会话之间终止这个空组合串，
// 表现为“每按一个键都重新开始组码”（实测日志里 comp 反复重建）。
HRESULT CTextService::StartComposition(ITfContext* pic, const std::wstring& text) {
    if (m_comp) return S_OK;
    if (m_compHost) { m_compHost->Release(); m_compHost = nullptr; }
    if (m_context) { m_context->Release(); m_context = nullptr; }
    pic->AddRef();
    m_context = pic;
    if (FAILED(pic->QueryInterface(IID_ITfContextComposition, (void**)&m_compHost)) || !m_compHost) {
        m_context->Release(); m_context = nullptr;
        return E_FAIL;
    }

    CTextService* self = this;
    HRESULT hr = DoEdit(pic, [self, &text](TfEditCookie ec) -> HRESULT {
        ITfInsertAtSelection* ias = nullptr;
        HRESULT hr2 = self->m_context->QueryInterface(IID_ITfInsertAtSelection, (void**)&ias);
        if (FAILED(hr2) || !ias) return E_FAIL;
        ITfRange* pRange = nullptr;
        hr2 = ias->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &pRange);
        ias->Release();
        if (FAILED(hr2) || !pRange) return E_FAIL;

        hr2 = self->m_compHost->StartComposition(
            ec, pRange, static_cast<ITfCompositionSink*>(self), &self->m_comp);
        if (SUCCEEDED(hr2) && self->m_comp) {
            hr2 = pRange->SetText(ec, 0, text.c_str(), (LONG)text.size());
            if (SUCCEEDED(hr2)) {
                self->ApplyInputAttribute(ec, pRange);
                ITfRange* pEnd = nullptr;
                if (SUCCEEDED(pRange->Clone(&pEnd)) && pEnd) {
                    pEnd->Collapse(ec, TF_ANCHOR_END);
                    TF_SELECTION sel;
                    sel.range = pEnd;
                    sel.style.ase = TF_AE_NONE;
                    sel.style.fInterimChar = FALSE;
                    self->m_context->SetSelection(ec, 1, &sel);
                    pEnd->Release();
                }
            }
        }
        LogLine(L"StartComposition: start=0x%08X text=0x%08X comp=%p",
                (unsigned)hr2, (unsigned)(self->m_comp ? 0u : 1u), (void*)self->m_comp);
        pRange->Release();
        return hr2;
    });
    return hr;
}

// 组码按 TSF 的“输入中”显示属性渲染（通常是虚线下划线）。
// GUID_ATTR_INPUT 在 Windows SDK 里没有 C 符号，因此运行时枚举系统已注册的
// 显示属性并取第一个；失败则跳过（仅外观，不影响输入）。
void CTextService::ApplyInputAttribute(TfEditCookie ec, ITfRange* pRange) {
    if (m_attrAtom == 0 && !m_attrTried) {
        m_attrTried = true;
        ITfDisplayAttributeMgr* dam = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TF_DisplayAttributeMgr, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_ITfDisplayAttributeMgr, (void**)&dam)) && dam) {
            IEnumTfDisplayAttributeInfo* en = nullptr;
            if (SUCCEEDED(dam->EnumDisplayAttributeInfo(&en)) && en) {
                ITfDisplayAttributeInfo* info = nullptr;
                ULONG fetched = 0;
                if (en->Next(1, &info, &fetched) == S_OK && fetched == 1 && info) {
                    GUID g = {};
                    if (SUCCEEDED(info->GetGUID(&g))) {
                        ITfCategoryMgr* cat = nullptr;
                        if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                                       IID_ITfCategoryMgr, (void**)&cat)) && cat) {
                            cat->RegisterGUID(g, &m_attrAtom);
                            cat->Release();
                        }
                        LogLine(L"display attribute: { %08lX-%04X-%04X } atom=%u",
                                (unsigned long)g.Data1, g.Data2, g.Data3, (unsigned)m_attrAtom);
                    }
                    info->Release();
                }
                en->Release();
            }
            dam->Release();
        }
    }
    if (m_attrAtom == 0 || !pRange) return;
    ITfProperty* prop = nullptr;
    if (SUCCEEDED(m_context->GetProperty(GUID_PROP_ATTRIBUTE, &prop)) && prop) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = (LONG)m_attrAtom;
        prop->SetValue(ec, pRange, &v);
        VariantClear(&v);
        prop->Release();
    }
}
HRESULT CTextService::UpdateCompositionText(ITfContext* pic) {
    return UpdateCompositionTextWith(pic, m_code);
}

HRESULT CTextService::UpdateCompositionTextWith(ITfContext* pic, const std::wstring& text) {
    if (!m_comp) return E_FAIL;
    CTextService* self = this;
    return DoEdit(pic, [self, &text](TfEditCookie ec) -> HRESULT {
        ITfRange* pRange = nullptr;
        HRESULT hr = self->m_comp->GetRange(&pRange);
        if (FAILED(hr) || !pRange) return E_FAIL;
        hr = pRange->SetText(ec, 0, text.c_str(), (LONG)text.size());
        if (SUCCEEDED(hr)) {
            self->ApplyInputAttribute(ec, pRange);
            ITfRange* pEnd = nullptr;
            if (SUCCEEDED(pRange->Clone(&pEnd)) && pEnd) {
                pEnd->Collapse(ec, TF_ANCHOR_END);
                TF_SELECTION sel;
                sel.range = pEnd;
                sel.style.ase = TF_AE_NONE;
                sel.style.fInterimChar = FALSE;
                self->m_context->SetSelection(ec, 1, &sel);
                pEnd->Release();
            }
        }
        pRange->Release();
        return hr;
    });
}
void CTextService::LoadDataOnce() {
    if (m_dataLoaded) return;
    m_dataLoaded = true;
    wchar_t path[MAX_PATH];
    std::wstring dir = L".";
    if (GetModuleFileNameW(g_hInst, path, ARRAYSIZE(path))) {
        std::wstring s(path);
        size_t p = s.find_last_of(L"\\/");
        if (p != std::wstring::npos) dir = s.substr(0, p);
    }
    m_punc.Load(dir + L"\\data\\punc.ini");
    m_trad.Load(dir + L"\\data\\simp2trad.dat");
}

// 直接插入一段文本（标点、简繁后上屏等），不走组码/候选流程
HRESULT CTextService::CommitString(ITfContext* pic, const std::wstring& text) {
    if (text.empty()) return S_OK;
    // 短语录制中：不写文档，先收进缓冲
    if (CaptureCustomText(text)) return S_OK;
    ITfContext* ctx = pic;
    ctx->AddRef();
    HRESULT hr = DoEdit(ctx, [ctx, &text](TfEditCookie ec) -> HRESULT {
        ITfInsertAtSelection* ias = nullptr;
        if (FAILED(ctx->QueryInterface(IID_ITfInsertAtSelection, (void**)&ias)) || !ias) return E_FAIL;
        ITfRange* r = nullptr;
        HRESULT h = ias->InsertTextAtSelection(ec, 0,
                                               text.c_str(), (LONG)text.size(), &r);
        ias->Release();
        // 关键：InsertTextAtSelection(flags=0) 在很多应用里不会把插入点移到文本之后，
        // 结果光标停在符号左边。这里显式把选区收拢到插入范围末尾。
        if (SUCCEEDED(h) && r) {
            ITfRange* after = nullptr;
            if (SUCCEEDED(r->Clone(&after)) && after) {
                if (SUCCEEDED(after->Collapse(ec, TF_ANCHOR_END))) {
                    TF_SELECTION sel = {};
                    sel.range = after;
                    sel.style.ase = TF_AE_NONE;
                    sel.style.fInterimChar = FALSE;
                    if (FAILED(ctx->SetSelection(ec, 1, &sel))) {
                        LogLine(L"CommitString: SetSelection failed");
                    }
                }
                after->Release();
            }
        }
        if (r) r->Release();
        return h;
    });
    ctx->Release();
    if (SUCCEEDED(hr)) RememberCommitted(text);   // 供"自定义词组"用 ← 回取
    return hr;
}

HRESULT CTextService::CommitText(ITfContext* pic, const std::wstring& textIn,
                                 const std::string& learnCode, bool resetComposer) {
    LogLine(L"CommitText: comp=%p textlen=%zu text=%s", (void*)m_comp, textIn.size(), textIn.c_str());
    if (!m_comp) return E_FAIL;
    std::wstring w = textIn;
    LoadDataOnce();
    // 短语录制中：候选照样能选，但结果收进缓冲，不写文档
    if (CaptureCustomText(w)) {
        if (resetComposer) m_composer.Reset();   // 组码必须收干净，否则下一个字会接在旧码后面
        m_code.clear();
        CancelComposition(pic);
        return S_OK;
    }
    if (m_tradEnabled && m_trad.Loaded()) w = m_trad.ToTraditional(w);

    // 词频自学习：记住这次实际选中的「码 -> 词」，下次同码时它会提前
    {
        const std::string code = learnCode.empty() ? m_composer.Code() : learnCode;
        // 只有四码上屏才参与词频学习：一/二/三级简码是固定约定，不该被频次改写
        if (code.size() == 4) {
            int n = WideCharToMultiByte(CP_UTF8, 0, textIn.c_str(), (int)textIn.size(),
                                        nullptr, 0, nullptr, nullptr);
            if (n > 0) {
                std::string u8((size_t)n, '\0');
                WideCharToMultiByte(CP_UTF8, 0, textIn.c_str(), (int)textIn.size(),
                                    &u8[0], n, nullptr, nullptr);
                g_dict.Get().BumpUserWord(code, u8);
                ++m_commitCount;
                // 每 8 次上屏落盘一次，减少多进程写同一文件的冲突
                if (m_commitCount % 8 == 0) {
                    const bool ok = g_dict.Get().SaveUserWords(GetUserWordsPath());
                    LogLine(L"user words saved: commits=%d ok=%d total=%zu",
                            m_commitCount, (int)ok, g_dict.Get().UserWordCount());
                }
            }
        }
    }

    CTextService* self = this;
    HRESULT hr = DoEdit(pic, [self, &w](TfEditCookie ec) -> HRESULT {
        ITfRange* pRange = nullptr;
        HRESULT hr2 = self->m_comp->GetRange(&pRange);
        if (FAILED(hr2) || !pRange) return E_FAIL;
        hr2 = pRange->SetText(ec, 0, w.c_str(), (LONG)w.size());
        if (SUCCEEDED(hr2)) {
            ITfRange* pEnd = nullptr;
            if (SUCCEEDED(pRange->Clone(&pEnd)) && pEnd) {
                pEnd->Collapse(ec, TF_ANCHOR_END);
                TF_SELECTION sel;
                sel.range = pEnd;
                sel.style.ase = TF_AE_NONE;
                sel.style.fInterimChar = FALSE;
                self->m_context->SetSelection(ec, 1, &sel);
                pEnd->Release();
            }
            hr2 = self->m_comp->EndComposition(ec);
        }
        pRange->Release();
        return hr2;
    });

    if (SUCCEEDED(hr) && !w.empty()) m_lastCommit = w;   // 记住上一次上屏的内容
    if (SUCCEEDED(hr)) RememberCommitted(w);             // 供"自定义词组"用 ← 回取
    if (resetComposer) m_composer.Reset();
    m_code.clear();
    m_candWnd.Hide();
    if (m_comp) { m_comp->Release(); m_comp = nullptr; }
    return hr;
}

void CTextService::EnsureComposer() {
    // 注意：必须是实例状态。多个线程各有一个 CTextService，
    // 用 static 会导致只有第一个实例拿到词典。
    if (!m_composerReady) {
        m_composer.SetDictionary(&g_dict.Get());
        // 特殊码：date / time / week 直出日期时间
        m_composer.SetSpecialProvider([](const std::string& code, std::vector<wubi::Candidate>& out) -> bool {
            int kind = -1;
            if (code == "date")      kind = 0;
            else if (code == "time") kind = 1;
            else if (code == "week") kind = 2;
            if (kind < 0) return false;
            SYSTEMTIME st = {};
            GetLocalTime(&st);
            std::vector<std::string> words;
            wubi::DateTimeCandidates(kind, st.wYear, st.wMonth, st.wDay,
                                     st.wHour, st.wMinute, st.wSecond, st.wDayOfWeek, words);
            if (words.empty()) return false;
            for (auto& w : words) out.push_back(wubi::Candidate{ w, code, 0, false });
            LogLine(L"special code %hs -> %zu candidates", code.c_str(), out.size());
            return true;
        });
        m_composerReady = true;
        // 候选窗鼠标点选：命中第 idx 项 -> 上屏
        m_candWnd.SetOnClick([this](int idx) {
            auto o = m_composer.SelectDigit(idx + 1);
            if (o.act == wubi::Composer::Act::Committed && m_context) {
                CommitText(m_context, o.commit, o.commitCode, true);
            }
        });
    }
    m_composer.SetLimit((size_t)m_candCount);
}

void CTextService::SyncCodeFromComposer() {
    m_code.clear();
    for (char c : m_composer.Code()) m_code.push_back((wchar_t)c);
}
HRESULT CTextService::CancelComposition(ITfContext* pic) {
    LogLine(L"CancelComposition: comp=%p", (void*)m_comp);
    if (!m_comp) return S_OK;
    CTextService* self = this;
    HRESULT hr = DoEdit(pic, [self](TfEditCookie ec) -> HRESULT {
        ITfRange* pRange = nullptr;
        HRESULT hr2 = self->m_comp->GetRange(&pRange);
        if (SUCCEEDED(hr2) && pRange) {
            pRange->SetText(ec, 0, L"", 0);   // 清空组码
            pRange->Release();
        }
        return self->m_comp->EndComposition(ec);
    });
    m_code.clear();
    m_composer.Reset();
    m_candWnd.Hide();
    if (m_comp) { m_comp->Release(); m_comp = nullptr; }
    return hr;
}

// ---------------------------------------------------------------- Z 重复 / 便捷输入
static std::wstring AsciiToWide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) w.push_back((wchar_t)(unsigned char)c);
    return w;
}

void CTextService::BeginRepeat() {
    if (m_lastCommit.empty()) return;
    m_repeatText = m_lastCommit;
    m_repeatPending = true;
}

void CTextService::BeginQuick() {
    m_quickMode = true;
    m_quickDigits.clear();
    m_quickForms.clear();
}

void CTextService::RefreshQuickCandidates(ITfContext* pic) {
    (void)pic;
    m_letterLabels = true;                 // 便捷输入用 a. b. c. 选词
    wubi::NumberForms(m_quickDigits, m_quickForms);
    ShowCandidateList(m_quickForms);
}

void CTextService::ExitQuick(ITfContext* pic, bool cancel) {
    m_quickMode = false;
    m_letterLabels = false;
    m_quickDigits.clear();
    m_quickForms.clear();
    if (cancel) {
        if (m_comp) CancelComposition(pic);
        m_candWnd.Hide();
    }
}

void CTextService::ShowCandidateList(const std::vector<std::wstring>& display) {
    m_candWnd.Hide();
    if (display.empty()) return;

    m_candWnd.SetLetterLabels(m_letterLabels);
    m_candWnd.SetItems(display, m_settings.CandVertical != 0);

    // 光标矩形（只读会话内取，避免影响输入）
    CTextService* self = this;
    RECT caret = { 0, 0, 0, 0 };
    bool haveCaret = false;
    CEditSession* s = new CEditSession([self, &caret, &haveCaret](TfEditCookie ec) -> HRESULT {
        if (!self->m_context) return E_FAIL;      // 没在组码时（提示/自定义模式）没有 context
        ITfContextView* view = nullptr;
        if (FAILED(self->m_context->GetActiveView(&view)) || !view) return E_FAIL;
        ITfRange* pRange = nullptr;
        if (self->m_comp && SUCCEEDED(self->m_comp->GetRange(&pRange)) && pRange) {
            RECT rc = { 0, 0, 0, 0 };
            BOOL clipped = FALSE;
            if (SUCCEEDED(view->GetTextExt(ec, pRange, &rc, &clipped))) {
                caret = rc;
                haveCaret = true;
            }
            pRange->Release();
        }
        view->Release();
        if (!haveCaret) {
            // 兜底：取本线程焦点窗口的插入符位置（不使用全局钩子/鼠标坐标）
            GUITHREADINFO gti = { sizeof(gti) };
            if (GetGUIThreadInfo(GetCurrentThreadId(), &gti) && gti.hwndCaret) {
                POINT pt = { gti.rcCaret.left, gti.rcCaret.bottom };
                if (ClientToScreen(gti.hwndCaret, &pt)) {
                    caret.left = caret.right = pt.x;
                    caret.top = caret.bottom = pt.y;
                    haveCaret = true;
                }
            }
        }
        return S_OK;
    });
    HRESULT hrSession = E_FAIL;
    if (m_context) m_context->RequestEditSession(m_tid, s, TF_ES_SYNC | TF_ES_READ, &hrSession);
    s->Release();

    if (haveCaret) {
        m_lastCaret = caret;
        m_hasCaret = true;
        m_candWnd.ShowAt(caret);
    }
}

// ---------------------------------------------------------------- 中/英 状态上报
// Windows 的语言指示器读的是 TSF 隔间，而不是我们自己内部的 bool。
//   GUID_COMPARTMENT_KEYBOARD_OPENCLOSE             1=打开(中文) 0=关闭(英文)
//   GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION  TF_CONVERSIONMODE_NATIVE / ALPHANUMERIC
// ---------------------------------------------------------------- 英文字母补全
// g_eng 定义见文件前部
// 数据：data\eng_words.txt（48k 词，取自 QQ五笔 engcore.dat 的 NEPQ 词表）
// 数据：data\eng_words.txt（48k 词，取自 QQ五笔 engcore.dat 的 NEPQ 词表）

bool CTextService::QueryCaretRect(RECT& out) {
    GUITHREADINFO gti = { sizeof(gti) };
    if (GetGUIThreadInfo(GetCurrentThreadId(), &gti) && gti.hwndCaret) {
        POINT pt = { gti.rcCaret.left, gti.rcCaret.bottom };
        if (ClientToScreen(gti.hwndCaret, &pt)) {
            out.left = out.right = pt.x;
            out.top = out.bottom = pt.y;
            return true;
        }
    }
    if (m_hasCaret) { out = m_lastCaret; return true; }
    return false;
}
void CTextService::SetEnglish(bool on) {
    m_english = on;
    SetInputModeCompartments(!on);
    FlashStatus(on ? L"\u82f1" : L"\u4e2d");
    LogLine(L"SetEnglish=%d", (int)on);
}

// 查词只认小写前缀；显示与上屏用 m_engTyped 里的真实大小写（见 wubi::ApplyCase）
std::string CTextService::EngPrefix() const {
    std::string s;
    s.reserve(m_engTyped.size());
    for (wchar_t c : m_engTyped) {
        s.push_back((c >= L'A' && c <= L'Z') ? (char)(c - L'A' + 'a') : (char)c);
    }
    return s;
}

void CTextService::EngUpdate(ITfContext* pic) {
    if (m_engTyped.empty()) { EngCancel(pic); return; }
    // 组码预览就显示用户实际打出来的大小写，不再一律小写
    if (m_comp) UpdateCompositionTextWith(pic, m_engTyped);
    else        StartComposition(pic, m_engTyped);

    const auto words = m_eng.Suggest(EngPrefix(), (size_t)m_candCount);
    std::vector<std::wstring> disp;
    disp.reserve(words.size());
    for (const auto& w : words) disp.push_back(wubi::ApplyCase(AsciiToWide(w), m_engTyped));

    m_candWnd.SetPlain(false);
    if (!disp.empty()) {
    m_candWnd.SetItems(disp, m_settings.CandVertical != 0);   // 跟随"候选竖排"设置
        if (QueryCaretRect(m_lastCaret)) m_hasCaret = true;   // 没记录时当场取，避免跑到屏幕左上角
        m_candWnd.ShowAt(m_lastCaret);
    } else {
        m_candWnd.Hide();
    }
    LogLine(L"eng type %s -> %zu cands", m_engTyped.c_str(), disp.size());
}

void CTextService::EngType(ITfContext* pic, wchar_t ch) {
    if (m_engTyped.size() >= 32) EngCommit(pic, 0, false);
    m_engTyped.push_back(ch);
    EngUpdate(pic);
}

void CTextService::EngCommit(ITfContext* pic, int idx, bool withSpace) {
    const auto words = m_eng.Suggest(EngPrefix(), (size_t)m_candCount);
    std::wstring out;
    if (idx >= 0 && idx < (int)words.size()) out = wubi::ApplyCase(AsciiToWide(words[(size_t)idx]), m_engTyped);
    else                                     out = m_engTyped;   // 没候选：原样上屏，大小写照抄

    m_engTyped.clear();
    m_engActive = false;   // 上屏后回到中文模式
    if (m_comp) CancelComposition(pic);
    m_candWnd.Hide();
    if (withSpace) out += L' ';
    CommitString(pic, out);
    LogLine(L"eng commit idx=%d len=%zu out=%s", idx, out.size(), out.c_str());
}

void CTextService::EngCancel(ITfContext* pic) {
    m_engTyped.clear();
    m_engActive = false;
    if (m_comp) CancelComposition(pic);
    m_candWnd.Hide();
}

// ---------------------------------------------------------------- 自定义词组 / 自定义短语
static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring TruncForPrompt(const std::wstring& s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    return s.substr(0, maxChars) + L"…";
}

// 追加一行到用户词文件。
// 为什么不用 SaveUserWords 整表重写：输入法在每个宿主进程里各有一份内存词表，
// explorer/微信这些进程里的旧副本随时会在 Deactivate 时整表落盘，把刚建好的词冲掉
// （实测到过：新建的短语在别的进程落盘后消失）。追加只会加行，不会删别人的东西。
static bool AppendUserWordLine(const std::wstring& path, const std::string& code,
                               const std::string& word, long long weight) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"ab") != 0 || !fp) return false;
    char line[2048];
    const int n = sprintf_s(line, sizeof(line), "%s\t%s\t%lld\r\n",
                            code.c_str(), word.c_str(), weight);
    bool ok = false;
    if (n > 0 && (size_t)n < sizeof(line)) ok = (fwrite(line, 1, (size_t)n, fp) == (size_t)n);
    fflush(fp);
    fclose(fp);
    return ok;
}

// 记住刚上屏的字，供"自定义词组"用 ← 逐个回取
void CTextService::RememberCommitted(const std::wstring& text) {
    for (wchar_t c : text) {
        if (c < 0x20) continue;                      // 空格/换行不进词组素材
        m_recentChars.push_back(std::wstring(1, c));
        if (m_recentChars.size() > 64) m_recentChars.pop_front();
    }
}

// 短语录制期间：本该写到文档里的内容改为收进缓冲，回车时才真正入库
bool CTextService::CaptureCustomText(const std::wstring& text) {
    if (m_custom != CustomMode::Phrase || m_customPhase != 1) return false;
    if (text.empty()) return true;
    if (m_customText.size() + text.size() > 512) return true;   // 上限保护
    m_customText += text;
    RefreshCustomPrompt();
    return true;
}

void CTextService::BeginCustomWord(ITfContext* pic) {
    m_custom = CustomMode::Word;
    m_customPhase = 0;
    m_customCode.clear();
    m_customText.clear();
    m_customChars.clear();
    m_customWord.clear();
    m_customAscii = false;
    m_engActive = false;
    m_quickMode = false;
    m_repeatPending = false;
    m_letterLabels = false;
    if (m_comp) CancelComposition(pic);
    m_candWnd.Hide();
    RefreshCustomPrompt();
    LogLine(L"custom word: begin, recent chars=%zu", m_recentChars.size());
}

void CTextService::BeginCustomPhrase(ITfContext* pic) {
    m_custom = CustomMode::Phrase;
    m_customPhase = 0;
    m_customCode.clear();
    m_customText.clear();
    m_customChars.clear();
    m_customWord.clear();
    m_customAscii = false;
    m_engActive = false;
    m_quickMode = false;
    m_repeatPending = false;
    m_letterLabels = false;
    if (m_comp) CancelComposition(pic);
    m_candWnd.Hide();
    RefreshCustomPrompt();
    LogLine(L"custom phrase: begin");
}

void CTextService::ExitCustom(ITfContext* pic, bool cancel) {
    const bool wasWord = (m_custom == CustomMode::Word);
    m_custom = CustomMode::None;
    m_customPhase = 0;
    m_customCode.clear();
    m_customText.clear();
    m_customChars.clear();
    m_customWord.clear();
    m_customAscii = false;
    m_letterLabels = false;
    if (cancel) {
        if (m_comp) CancelComposition(pic);
        m_candWnd.Hide();
    }
    LogLine(L"custom: exit cancel=%d word=%d", (int)cancel, (int)wasWord);
}

void CTextService::RefreshCustomPrompt() {
    std::vector<std::wstring> lines;
    if (m_custom == CustomMode::Word) {
        std::wstring shown;
        for (const auto& c : m_customChars) shown += c;
        lines.push_back(L"自定义词组：" + (shown.empty() ? std::wstring(L"（按 ← 取字）") : TruncForPrompt(shown, 16)));
        lines.push_back(L"← 加入前一字 · Del 删首 · 回车保存 · Esc 取消");
    } else if (m_custom == CustomMode::Phrase && m_customPhase == 0) {
        lines.push_back(L"自定义短语 · 编码：" +
                        (m_customCode.empty() ? std::wstring(L"（输入字母）") : Utf8ToWide(m_customCode)));
        lines.push_back(L"a–y 字母作编码，回车确认 · Esc 取消");
    } else if (m_custom == CustomMode::Phrase) {
        lines.push_back(L"编码 " + Utf8ToWide(m_customCode) + L"【" +
                        ((m_english || m_customAscii) ? L"英文" : L"中文") + L"】内容：" +
                        (m_customText.empty() ? std::wstring(L"（请输入短语）") : TruncForPrompt(m_customText, 20)));
        lines.push_back(L"回车保存 · F2 切中英 · Backspace 删除 · Esc 取消");
    }
    if (lines.empty()) { m_candWnd.Hide(); return; }
    m_letterLabels = false;
    ShowCandidateList(lines);
}

void CTextService::SaveCustomWord(ITfContext* pic) {
    if (m_customChars.size() < 2) { FlashStatus(L"至少两个字：按 ← 从刚输入的字里取"); return; }
    const std::string code = wubi::PhraseCode(m_customWord, g_dict.Get());
    if (code.empty()) { FlashStatus(L"取码失败：有字查不到全码"); return; }

    g_dict.Get().AddUserWord(code, m_customWord, 0x7FFFFFFF);
    const bool ok = AppendUserWordLine(GetUserWordsPath(), code, m_customWord, 0x7FFFFFFF);
    const std::wstring text = Utf8ToWide(m_customWord);
    const std::wstring flash = L"已加入词组 " + Utf8ToWide(code) + L" → " + TruncForPrompt(text, 12);
    ExitCustom(pic, false);
    FlashStatus(flash.c_str());
    LogLine(L"custom word saved: code=%hs word=%hs ok=%d total=%zu",
            code.c_str(), m_customWord.c_str(), (int)ok, g_dict.Get().UserWordCount());
}

void CTextService::SaveCustomPhrase(ITfContext* pic) {
    if (m_customCode.empty()) { FlashStatus(L"请先输入编码"); return; }
    if (m_customText.empty()) { FlashStatus(L"内容为空：先输入短语再回车"); return; }

    const std::string u8 = WideToUtf8(m_customText);
    g_dict.Get().AddUserWord(m_customCode, u8, 0x7FFFFFFF);
    const bool ok = AppendUserWordLine(GetUserWordsPath(), m_customCode, u8, 0x7FFFFFFF);
    const std::wstring flash = L"已保存短语 " + Utf8ToWide(m_customCode) + L" → " + TruncForPrompt(m_customText, 12);
    ExitCustom(pic, false);
    FlashStatus(flash.c_str());
    LogLine(L"custom phrase saved: code=%hs len=%zu ok=%d total=%zu",
            m_customCode.c_str(), m_customText.size(), (int)ok, g_dict.Get().UserWordCount());
}

// 自定义模式下的按键。返回 true 表示已处理（键要吞掉）
bool CTextService::CustomKey(ITfContext* pic, UINT vk, LPARAM lp, bool shift, BOOL* pfEaten) {
    if (m_custom == CustomMode::None) return false;
    // Ctrl/Alt 组合键照常交给应用（复制/粘贴等），别在自定义模式里被吃掉
    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return false;

    if (m_custom == CustomMode::Word) {
        switch (vk) {
        case VK_LEFT:   // ← 把上一个上屏的字加进来
            if (!m_recentChars.empty()) {
                m_customChars.push_back(m_recentChars.back());
                m_recentChars.pop_back();
                for (char c : WideToUtf8(m_customChars.back())) m_customWord.push_back(c);
            }
            RefreshCustomPrompt();
            *pfEaten = TRUE;
            return true;
        case VK_DELETE:  // Del 删掉最前面那个字（跳过输错的字）
            if (!m_customChars.empty()) {
                m_customChars.erase(m_customChars.begin());
                m_customWord.clear();
                for (const auto& c : m_customChars) m_customWord += WideToUtf8(c);
            }
            RefreshCustomPrompt();
            *pfEaten = TRUE;
            return true;
        case VK_RETURN: SaveCustomWord(pic); *pfEaten = TRUE; return true;
        case VK_ESCAPE: ExitCustom(pic, true); *pfEaten = TRUE; return true;
        default: *pfEaten = TRUE; return true;   // 其它键不落到文档里
        }
    }

    // ---- 自定义短语 ----
    if (m_customPhase == 0) {   // 第一步：指定编码
        if (vk >= 'A' && vk <= 'Y') {           // 只用五笔键位 a..y
            if (m_customCode.size() < 4) m_customCode.push_back((char)(vk - 'A' + 'a'));
            RefreshCustomPrompt();
            *pfEaten = TRUE;
            return true;
        }
        if (vk == VK_BACK) {
            if (!m_customCode.empty()) m_customCode.pop_back();
            RefreshCustomPrompt();
            *pfEaten = TRUE;
            return true;
        }
        if (vk == VK_RETURN) {
            if (m_customCode.empty()) { FlashStatus(L"编码不能为空"); *pfEaten = TRUE; return true; }
            m_customPhase = 1;
            RefreshCustomPrompt();
            *pfEaten = TRUE;
            return true;
        }
        if (vk == VK_ESCAPE) { ExitCustom(pic, true); *pfEaten = TRUE; return true; }
        if (vk == 'Z') FlashStatus(L"z 是五笔保留键，不能作编码");   // 别静默吞掉，给个提示
        *pfEaten = TRUE;
        return true;
    }

    // 第二步：录内容。组码/选词交给正常流程，上屏的文字会被 CaptureCustomText 收进缓冲
    if (vk == VK_F2) {   // 录入中英切换：英文时按原样收（邮箱/网址），中文时走五笔（地址）
        m_customAscii = !m_customAscii;
        RefreshCustomPrompt();
        *pfEaten = TRUE;
        return true;
    }
    const bool composing = m_comp || m_engActive || m_quickMode || m_repeatPending;
    if (composing) return false;

    if (vk == VK_RETURN) { SaveCustomPhrase(pic); *pfEaten = TRUE; return true; }
    if (vk == VK_ESCAPE) { ExitCustom(pic, true); *pfEaten = TRUE; return true; }
    if (vk == VK_BACK) {   // 缓冲里删一个字
        if (!m_customText.empty()) {
            size_t i = m_customText.size() - 1;
            if (i > 0 && m_customText[i] >= 0xDC00 && m_customText[i] <= 0xDFFF) --i;   // 代理对
            m_customText.resize(i);
        }
        RefreshCustomPrompt();
        *pfEaten = TRUE;
        return true;
    }

    BYTE ks[256];
    GetKeyboardState(ks);
    wchar_t buf[8] = { 0 };

    if (m_english || m_customAscii) {
        // 英文录入：所有可打印键按实际键位收进缓冲（大小写、@ . _ 都照原样）
        if (ToUnicode(vk, (UINT)lp, ks, buf, 8, 0) == 1 && buf[0] >= 0x20) {
            CaptureCustomText(std::wstring(1, buf[0]));
        }
        *pfEaten = TRUE;
        return true;
    }
    // 中文态：数字与空格按原样收进缓冲（身份证号、地址里的空格）
    if ((vk >= '0' && vk <= '9') || vk == VK_SPACE) {
        if (ToUnicode(vk, (UINT)lp, ks, buf, 8, 0) == 1) CaptureCustomText(std::wstring(1, buf[0]));
        *pfEaten = TRUE;
        return true;
    }
    (void)shift;
    return false;   // 字母/标点继续走正常流程
}

void CTextService::SetInputModeCompartments(bool chinese) {
    if (!m_context) { LogLine(L"compartments: no context"); return; }

    ITfCompartmentMgr* cm = nullptr;
    if (FAILED(m_context->QueryInterface(IID_ITfCompartmentMgr, (void**)&cm)) || !cm) {
        LogLine(L"compartments: no ITfCompartmentMgr");
        return;
    }

    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = chinese ? 1 : 0;
    ITfCompartment* comp = nullptr;
    if (SUCCEEDED(cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &comp)) && comp) {
        const HRESULT hr = comp->SetValue(m_tid, &v);
        LogLine(L"OPENCLOSE=%d hr=0x%08lX", (int)chinese, (unsigned long)hr);
        comp->Release();
    } else {
        LogLine(L"compartments: GetCompartment(OPENCLOSE) failed");
    }

    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = chinese ? TF_CONVERSIONMODE_NATIVE : TF_CONVERSIONMODE_ALPHANUMERIC;
    comp = nullptr;
    if (SUCCEEDED(cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &comp)) && comp) {
        const HRESULT hr = comp->SetValue(m_tid, &v);
        LogLine(L"CONVERSION=%d hr=0x%08lX", (int)chinese, (unsigned long)hr);
        comp->Release();
    }

    cm->Release();
}
// 组码候选：把 Composer 的候选转成宽字符后交给 ShowCandidateList
void CTextService::RefreshCandidates(ITfContext* pic) {
    (void)pic;
    m_letterLabels = false;
    const auto& cands = m_composer.Candidates();
    std::vector<std::wstring> display;
    display.reserve(cands.size());
    for (const auto& e : cands) {
        int n = MultiByteToWideChar(CP_UTF8, 0, e.word.c_str(), (int)e.word.size(), nullptr, 0);
        std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, e.word.c_str(), (int)e.word.size(), &w[0], n);
        if (e.user) w = L"★" + w;   // 用户词/被 Ctrl+数字 锁定的候选加标记
        if (m_tradEnabled && m_trad.Loaded()) w = m_trad.ToTraditional(w);   // 候选显示与上屏保持一致的简繁
        display.push_back(w);
    }
    {
        const std::string cc = m_composer.Code();
        std::wstring wc(cc.begin(), cc.end());
        m_candWnd.SetCode(wc);
    }
    ShowCandidateList(display);
}
// 把中/英状态通知给托盘程序（ppwubi-tray.exe），并写状态文件供其启动时读取
// ---------------------------------------------------------------- 指示器图标切换
// 语言指示器读的是注册表里那个固定路径的 .ico（dist\ppwubi.ico）。
// 路径不能改（要管理员），但文件内容可以换——所以切换中/英时把源图标复制过去。

// 中/英状态提示：复用候选窗，显示到下一次按键为止
void CTextService::FlashStatus(const wchar_t* text) {
    // 没有记录过光标位置时（例如没在组码），当场向系统要一次
    if (!m_hasCaret) {
        GUITHREADINFO gti = { sizeof(gti) };
        if (GetGUIThreadInfo(GetCurrentThreadId(), &gti) && gti.hwndCaret) {
            POINT pt = { gti.rcCaret.left, gti.rcCaret.bottom };
            if (ClientToScreen(gti.hwndCaret, &pt)) {
                m_lastCaret.left = m_lastCaret.right = pt.x;
                m_lastCaret.top = m_lastCaret.bottom = pt.y;
                m_hasCaret = true;
            }
        }
    }
    if (!m_hasCaret) { LogLine(L"FlashStatus: no caret, skipped"); return; }
    m_candWnd.SetPlain(true);          // 不显示序号，避免被当成候选词
    m_candWnd.SetItems({ text }, false);
    m_candWnd.ShowBriefly(m_lastCaret, 700);
    m_statusShown = true;
    LogLine(L"FlashStatus: %ls", text);
}

// 字母键的真实大小写：Shift 与 CapsLock 相异即为大写（CapsLock 开 + Shift = 小写，与系统一致）
static wchar_t LetterWithCase(UINT vk, bool shift, bool caps) {
    const bool upper = (shift != caps);
    return upper ? (wchar_t)vk : (wchar_t)(vk - 'A' + 'a');
}

// 可以被输入法接管的"可打印键"（不含 F1-F12、方向键、PrintScreen 等）
static bool IsPrintableVk(UINT vk) {
    if (vk >= '0' && vk <= '9') return true;
    if (vk >= 'A' && vk <= 'Z') return true;
    if (vk == VK_SPACE || vk == VK_OEM_1 || vk == VK_OEM_PLUS || vk == VK_OEM_COMMA ||
        vk == VK_OEM_MINUS || vk == VK_OEM_PERIOD || vk == VK_OEM_2 || vk == VK_OEM_3 ||
        vk == VK_OEM_4 || vk == VK_OEM_5 || vk == VK_OEM_6 || vk == VK_OEM_7)
        return true;
    return false;
}

// 与 OnKeyDown 同一口径：只在"这个键一定会被吃掉"时才向宿主声明。
bool CTextService::WantsKey(UINT vk, LPARAM lp) {
    // 自定义词组/短语模式：几乎所有键都由输入法处理，先声明下来，否则宿主直接自己消化
    if (m_custom != CustomMode::None) {
        return vk == VK_LEFT || vk == VK_DELETE || vk == VK_RETURN || vk == VK_ESCAPE ||
               vk == VK_BACK || vk == VK_SPACE || vk == VK_F2 || (vk >= '0' && vk <= '9') ||
               IsPrintableVk(vk) || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
    }
    if (m_english) return false;                       // 自由英文模式：输入法完全不参与
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt  = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (alt) return false;
    if (ctrl) {
        // Ctrl 组合默认交给应用，但自己的热键要先声明，否则宿主根本不会把 KeyDown 转到输入法：
        //   Ctrl+= 自定义词组 · Ctrl+Shift+= 自定义短语 · Ctrl+Shift+T/P 简繁/标点
        if (vk == VK_OEM_PLUS) return true;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift && (vk == 'T' || vk == 'P')) return true;
        return false;
    }

    if (vk >= 'A' && vk <= 'Z') {
        // 'z' 在没有"上次上屏"时不由输入法处理（OnKeyDown 里同样不吞），别声明
        if (vk == 'Z' && m_lastCommit.empty()) return false;
        return true;                                   // 组码 / 英文补全都会吃字母
    }

    const bool busy = m_comp || m_engActive || m_quickMode || m_repeatPending ||
                      !m_composer.Code().empty();
    if (busy) {
        // 组码/补全/便捷输入期间这些键都由状态机处理
        return vk == VK_SPACE || vk == VK_RETURN || vk == VK_BACK || vk == VK_ESCAPE ||
               (vk >= '0' && vk <= '9') || vk == VK_OEM_1 || vk == VK_OEM_7 ||
               vk == VK_OEM_MINUS || vk == VK_OEM_PLUS || IsPrintableVk(vk);
    }
    if (vk == VK_OEM_1 && !(GetKeyState(VK_SHIFT) & 0x8000)) return true;   // 分号：便捷输入

    if (m_puncEnabled) {                               // 中文标点接管：确实能映射才声明
        LoadDataOnce();
        BYTE ks[256];
        GetKeyboardState(ks);
        wchar_t buf[8] = { 0 };
        if (ToUnicode(vk, (UINT)lp, ks, buf, 8, 0) == 1 && buf[0] > 0x20 && buf[0] < 0x7F) {
            std::wstring out;
            return m_punc.Map((char)buf[0], wubi::PuncMode::ChineseHalf, out);
        }
    }
    return false;
}

STDMETHODIMP CTextService::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    const UINT vk = (UINT)wParam;
    EnsureComposer();
    // 状态提示显示到下一次按键为止
    if (m_statusShown) { m_candWnd.Hide(); m_statusShown = false; }

    // Shift 单击检测：按下期间若按了别的键，就不算单击
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        m_shiftDown = true;
        m_shiftUsed = false;
        return S_OK;      // Shift 本身不拦截
    }
    if (m_shiftDown) m_shiftUsed = true;

    const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool caps  = (GetKeyState(VK_CAPITAL) & 1) != 0;
    LogLine(L"OnKeyDown vk=0x%02X scan=0x%02X ctrl=%d alt=%d comp=%p code=%hs",
            vk, (unsigned)((lParam >> 16) & 0xFF), (int)ctrl, (int)alt, (void*)m_comp, m_composer.Code().c_str());

    // Ctrl+Shift+T 切换简繁；Ctrl+Shift+P 切换中文标点（组码中也生效）
    if (ctrl && shift && (vk == 'T' || vk == 'P')) {
        LoadDataOnce();
        if (vk == 'T') { m_tradEnabled = !m_tradEnabled; m_settings.TradChinese = m_tradEnabled ? 1 : 0; }
        else           { m_puncEnabled = !m_puncEnabled; m_settings.EnablePunction = m_puncEnabled ? 1 : 0; }
        const bool saved = wubi::SaveSettings(GetConfigPath(), m_settings);
        LogLine(L"toggle %c -> trad=%d punc=%d saved=%d",
                (wchar_t)vk, (int)m_tradEnabled, (int)m_puncEnabled, (int)saved);
        *pfEaten = TRUE;
        return S_OK;
    }

    // Ctrl+= 进入自定义词组；Ctrl+Shift+= （也就是带 Shift 的那个 + 键）进入自定义短语。
    // 用 VK_OEM_PLUS 判断，所以美式键盘上 Ctrl+= 与 Ctrl+Shift+= 都能触发，靠 Shift 区分。
    if (ctrl && !alt && vk == VK_OEM_PLUS) {
        if (m_custom != CustomMode::None) ExitCustom(pic, true);   // 再按一次先退出，避免状态叠加
        if (shift) BeginCustomPhrase(pic);
        else       BeginCustomWord(pic);
        *pfEaten = TRUE;
        return S_OK;
    }
    // 自定义模式里的键优先处理；返回 false 表示交给下面的常规流程（组码/选词）
    if (CustomKey(pic, vk, lParam, shift, pfEaten)) return S_OK;

    // 其它 Ctrl/Alt 组合键一律交还应用：Ctrl+C 复制、Ctrl+V 粘贴、Alt 菜单等。
    // 之前这里不判断修饰键，导致按下 Ctrl+C 时被当成组码 'c' 吞掉。
    // Ctrl+数字：把第 N 个候选提到首位（仅四码时生效），并写入用户词固化
    if (ctrl && !alt && !shift && vk >= '1' && vk <= '9' &&
        m_comp && m_composer.Code().size() == 4) {
        auto o = m_composer.Promote((int)(vk - '0'));
        if (o.act == wubi::Composer::Act::Updated) {
            SyncCodeFromComposer();
            UpdateCompositionText(pic);
            RefreshCandidates(pic);
            g_dict.Get().SaveUserWords(GetUserWordsPath());
            LogLine(L"promote candidate %d for code %hs", (int)(vk - '0'), m_composer.Code().c_str());
        }
        *pfEaten = TRUE;
        return S_OK;
    }

    if (ctrl || alt) {
        LogLine(L"pass-through: ctrl=%d alt=%d vk=0x%02X", (int)ctrl, (int)alt, vk);
        return S_OK;   // *pfEaten 保持 FALSE，按键继续交给应用
    }
    // Shift+字母（或 CapsLock）-> 进入"英文单词补全"模式（不是自由英文模式）
    if (vk >= 'A' && vk <= 'Z' && !m_engActive && (shift || caps)) {
        if (m_comp) CancelComposition(pic);   // 放弃正在进行的中文组码
        m_shiftDown = false;                  // 本次 Shift 不参与"单击"判定
        m_shiftUsed = true;
        m_engActive = true;                   // 只开补全，不改 m_english
        EngType(pic, LetterWithCase(vk, shift, caps));   // 首字母保留实际大小写
        *pfEaten = TRUE;
        return S_OK;
    }

    // ① 英文单词补全（Shift+字母 或 便捷输入里打字母进入）
    if (m_engActive || !m_engTyped.empty()) {
        if (vk >= 'A' && vk <= 'Z') {
            m_engActive = true;
            EngType(pic, LetterWithCase(vk, shift, caps));   // 后续字母同样照抄大小写
            *pfEaten = TRUE;
            return S_OK;
        }
        if (!m_engTyped.empty()) {
            if (vk == VK_SPACE || vk == VK_RETURN) { EngCommit(pic, 0, vk == VK_SPACE); *pfEaten = TRUE; return S_OK; }
            if (vk >= '1' && vk <= '9')        { EngCommit(pic, (int)(vk - '1'), true);  *pfEaten = TRUE; return S_OK; }  // 数字选择也接空格
            if (vk == VK_BACK) {
                m_engTyped.pop_back();
                if (m_engTyped.empty()) EngCancel(pic); else EngUpdate(pic);
                *pfEaten = TRUE;
                return S_OK;
            }
            if (vk == VK_ESCAPE) { EngCancel(pic); *pfEaten = TRUE; return S_OK; }
            EngCommit(pic, 0, false);   // 其它键：先上屏当前词，再放行
            return S_OK;
        }
        m_engActive = false;            // 空缓冲且非字母：退出补全状态
        return S_OK;
    }

    // ② 自由输入英文（单击 Shift）：输入法完全不参与，全部直通
    if (m_english) return S_OK;

    // —— Z 待定：此时不往文档里写任何文字，只提示"按空格重复" ——
    if (m_repeatPending) {
        if (vk == VK_SPACE || vk == VK_RETURN) {
            m_repeatPending = false;
            const std::wstring text = m_repeatText;
            m_repeatText.clear();
            CommitString(pic, text);          // 此刻才真正写入文档
            *pfEaten = TRUE;
            m_candWnd.Hide();                 // CommitString 不会自动关候选窗
            return S_OK;
        }
        if (vk == VK_ESCAPE) {
            m_repeatPending = false;
            m_repeatText.clear();
            m_candWnd.Hide();
            *pfEaten = TRUE;
            return S_OK;
        }
        // 其它键：放弃这次重复，继续按正常流程处理该键
        m_repeatPending = false;
        m_repeatText.clear();
        m_candWnd.Hide();
    }

    // —— 分号便捷输入：数字转三种中文形式 ——
    if (m_quickMode) {
        if (vk >= '0' && vk <= '9') {
            if (m_quickDigits.size() < 12) m_quickDigits.push_back((char)vk);
            const std::wstring shown = AsciiToWide(m_quickDigits);
            if (m_comp) UpdateCompositionTextWith(pic, shown);
            else        StartComposition(pic, shown);
            RefreshQuickCandidates(pic);
            *pfEaten = TRUE;
            return S_OK;
        }
        if (vk == VK_BACK) {
            if (m_quickDigits.empty()) { ExitQuick(pic, true); *pfEaten = TRUE; return S_OK; }
            m_quickDigits.pop_back();
            if (m_quickDigits.empty()) { ExitQuick(pic, true); *pfEaten = TRUE; return S_OK; }
            UpdateCompositionTextWith(pic, AsciiToWide(m_quickDigits));
            RefreshQuickCandidates(pic);
            *pfEaten = TRUE;
            return S_OK;
        }
        if (vk == VK_ESCAPE) { ExitQuick(pic, true); *pfEaten = TRUE; return S_OK; }
        if (vk >= 'A' && vk <= 'Z') {   // 便捷输入里按字母 -> 转入英文单词补全（大小写照抄）
            ExitQuick(pic, true);
            m_engActive = true;
            EngType(pic, LetterWithCase(vk, shift, caps));
            *pfEaten = TRUE;
            return S_OK;
        }
        if (vk == VK_SPACE || vk == VK_RETURN || vk == VK_OEM_1 || vk == VK_OEM_7 ||
            false) {
            size_t idx = 0;
            if (vk == 'B' || vk == VK_OEM_1)      idx = 1;
            else if (vk == 'C' || vk == VK_OEM_7) idx = 2;
            wubi::NumberForms(m_quickDigits, m_quickForms);
            if (idx < m_quickForms.size()) {
                const std::wstring out = m_quickForms[idx];
                const bool hadComp = (m_comp != nullptr);
                m_quickMode = false;
                m_letterLabels = false;
                m_quickDigits.clear();
                m_quickForms.clear();
                if (hadComp) CommitText(pic, out);
                else       { CommitString(pic, out); m_candWnd.Hide(); }
            } else {
                ExitQuick(pic, true);
            }
            *pfEaten = TRUE;
            return S_OK;
        }
        // 其它键：退出便捷输入，继续按正常流程处理这个键
        ExitQuick(pic, true);
    }


    // —— 数字之后的下一个逗号/句号输出半角（单次）——
    // 注意：不能只看虚拟键。不同键盘/布局下逗号可能不是 VK_OEM_COMMA，
    // 若按 vk 判断会先被当成"普通键"清掉待定，随后才走标点映射 -> 输出全角。
    // 因此这里统一看"这个键实际会产生什么字符"。
    BYTE ksNow[256];
    GetKeyboardState(ksNow);
    wchar_t produced = 0;
    if (ToUnicode(vk, (UINT)lParam, ksNow, &produced, 1, 0) != 1) produced = 0;

    const bool isDigitKey = (produced >= L'0' && produced <= L'9');
    const bool isCommaKey = (produced == L',');
    const bool isPeriodKey = (produced == L'.');
    LogLine(L"gate: vk=0x%02X produced=0x%04X pending=%d", vk, (unsigned)produced, (int)m_gate.pending);

    // 待定状态下按 Esc：取消该状态，并拦截 Esc（不发给应用）
    if (vk == VK_ESCAPE && m_gate.TakeEsc()) {
        LogLine(L"half-width pending cancelled by Esc (eaten)");
        *pfEaten = TRUE;
        return S_OK;
    }
    // 待定状态下按 , / . ：本次输出半角，随后回到正常（全角/中文标点）
    if (!m_comp && m_gate.pending && (isCommaKey || isPeriodKey)) {
        m_gate.TakeHalfWidth();
        LogLine(L"half-width punct: vk=0x%02X", vk);
        CommitString(pic, isCommaKey ? L"," : L".");
        *pfEaten = TRUE;
        return S_OK;
    }
    // 非组码时更新待定状态：数字进入待定，其它键（除逗号句号）退出
    if (!m_comp) m_gate.NoteKey(isDigitKey, isCommaKey || isPeriodKey);

    // 组码进行中：所有键交给状态机
    if (m_comp) {
        auto handle = [&](wubi::Composer::Outcome o) {
            switch (o.act) {
            case wubi::Composer::Act::Updated:
                SyncCodeFromComposer();
                UpdateCompositionText(pic);
                RefreshCandidates(pic);
                *pfEaten = TRUE;
                break;
            case wubi::Composer::Act::Committed:
                CommitText(pic, o.commit, o.commitCode, true);
                *pfEaten = TRUE;
                break;
            case wubi::Composer::Act::CommitThenContinue:
                // 顶字上屏：先把第 1 候选落盘（结束当前组合串），再用新码开新组合串
                LogLine(L"auto-commit %zu chars, continue with code=%hs", o.commit.size(), o.commitCode.c_str());
                CommitText(pic, o.commit, o.commitCode, /*resetComposer=*/false);
                SyncCodeFromComposer();
                if (SUCCEEDED(StartComposition(pic, m_code))) RefreshCandidates(pic);
                *pfEaten = TRUE;
                break;
            case wubi::Composer::Act::Cancelled:
                CancelComposition(pic);
                *pfEaten = TRUE;
                break;
            default:
                // Pass：状态机不处理，但键仍属输入法（如翻页到边界的 , .），必须吞掉
                *pfEaten = TRUE;
                break;
            }
        };
        wchar_t ch = 0;
        if (IsCodeKey(vk, &ch)) {
            LogLine(L"key(composing) %c", (wchar_t)ch);
            handle(m_composer.TypeCode((char)ch));
            return S_OK;
        }
        if (vk == VK_SPACE || vk == VK_RETURN) { handle(m_composer.SelectBest());             return S_OK; }
        // 下面这些是"按住 Shift 就变成另一个符号"的键，必须区分有没有 Shift：
        //   ; -> :   ' -> "   - -> _   = -> +   1 -> !
        // 否则 Shift+; 会被当成"选第 2 候选"，冒号怎么都打不出来。
        if (!shift && vk >= '1' && vk <= '9') { handle(m_composer.SelectDigit((int)(vk - '0'))); return S_OK; }
        if (!shift && vk == VK_OEM_1)         { handle(m_composer.SelectDigit(2));                return S_OK; }  // 分号 = 第2候选
        if (!shift && vk == VK_OEM_7)         { handle(m_composer.SelectDigit(3));                return S_OK; }  // 单引号 = 第3候选
        if (!shift && vk == VK_OEM_MINUS)     { handle(m_composer.PrevPage());                    return S_OK; }  // - 上一页
        if (!shift && vk == VK_OEM_PLUS)      { handle(m_composer.NextPage());                    return S_OK; }  // = 下一页
        if (vk == VK_ESCAPE)               { handle(m_composer.Escape());                      return S_OK; }
        if (vk == VK_BACK)                 { handle(m_composer.Backspace());                   return S_OK; }
        // 其它符号（非 ; ' , . 与数字）：先把第 1 候选上屏，再输出这个符号。
        // 这样"打码后直接打标点"能得到"候选+标点"，而不是让标点漏给应用把组合串杀掉。
        {
            BYTE ksSym[256];
            GetKeyboardState(ksSym);
            wchar_t symChar = 0;
            if (ToUnicode(vk, (UINT)lParam, ksSym, &symChar, 1, 0) == 1 &&
                symChar > 0x20 && symChar < 0x7F) {
                handle(m_composer.SelectBest());   // 提交 #1 并结束组合串
                LoadDataOnce();
                std::wstring outSym;
                if (m_puncEnabled && m_punc.Map((char)symChar, wubi::PuncMode::ChineseHalf, outSym)) {
                    CommitString(pic, outSym);
                } else {
                    CommitString(pic, std::wstring(1, symChar));
                }
                *pfEaten = TRUE;
                return S_OK;
            }
        }

        m_gate.pending = false;   // 组码期间的数字是选词，不是输入数字
        return S_OK;
    }

    // 无组码（修饰键已在函数开头处理完毕）

    // 分号：进入便捷输入（不是直接出分号）。Shift+; 是冒号，交给下面的标点接管
    if (vk == VK_OEM_1 && !shift) {
        BeginQuick();
        // 立刻显示一个提示框，让用户知道进入了便捷输入模式
        ShowCandidateList(std::vector<std::wstring>{ L"便捷输入：请输入数字" });
        LogLine(L"quick input: on");
        *pfEaten = TRUE;
        return S_OK;
    }

    // Z 键：进入"重复上次上屏"待定状态（只提示，不写文档；按空格才上屏）
    if (vk == 'Z' && !shift && !m_lastCommit.empty()) {
        m_repeatPending = true;
        m_repeatText = m_lastCommit;
        ShowCandidateList(std::vector<std::wstring>{ L"空格重复：" + m_lastCommit });
        LogLine(L"repeat pending: %zu chars (waiting for space)", m_repeatText.size());
        *pfEaten = TRUE;
        return S_OK;
    }


    // 中文标点接管（ASCII 标点 -> 中文标点，含引号成对）
    if (m_puncEnabled) {
        BYTE ks[256];
        GetKeyboardState(ks);
        wchar_t buf[8] = { 0 };
        int nn = ToUnicode(vk, (UINT)lParam, ks, buf, 8, 0);
        if (nn == 1 && buf[0] > 0x20 && buf[0] < 0x7F) {
            LoadDataOnce();
            std::wstring out;
            if (m_punc.Map((char)buf[0], wubi::PuncMode::ChineseHalf, out)) {
                CommitString(pic, out);
                *pfEaten = TRUE;
                return S_OK;
            }
        }
    }

    wchar_t ch = 0;
    if (IsCodeKey(vk, &ch)) {
        m_gate.pending = false;
        auto o = m_composer.TypeCode((char)ch);
        LogLine(L"key %c -> code=%hs act=%d", (wchar_t)ch, m_composer.Code().c_str(), (int)o.act);
        if (o.act == wubi::Composer::Act::Updated) {
            SyncCodeFromComposer();
            // 开始组码与首帧文本在同一个编辑会话内完成
            if (FAILED(StartComposition(pic, m_code))) { m_composer.Reset(); return S_OK; }
            RefreshCandidates(pic);
            *pfEaten = TRUE;
        }
    }
    return S_OK;
}
// ---------------------------------------------------------------- 设置入口（ITfFnConfigure）
// 文档：Text Services 控制面板 / 输入法菜单会 CoCreateInstance(本 TIP 的 CLSID, IID_ITfFnConfigure)，
// 拿到这个对象后显示「设置」并调用 Show()。
static HRESULT LaunchSettingsWindow(HWND parent) {
    wchar_t mod[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(g_hInst, mod, ARRAYSIZE(mod)) == 0) return E_FAIL;

    std::wstring dir(mod);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return E_FAIL;
    dir.erase(slash);

    const std::wstring exe = dir + L"\\panda-settings.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogLine(L"configure: settings exe missing: %ls", exe.c_str());
        return E_FAIL;
    }
    const HINSTANCE h = ShellExecuteW(parent, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    const bool ok = (INT_PTR)h > 32;
    LogLine(L"configure: launch %ls -> %s", exe.c_str(), ok ? L"ok" : L"failed");
    return ok ? S_OK : E_FAIL;
}

class CConfigureFn : public ITfFnConfigure {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfFnConfigure)) {
            *ppv = static_cast<ITfFnConfigure*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG c = InterlockedDecrement(&ref_);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP GetDisplayName(BSTR* name) override {
        if (!name) return E_INVALIDARG;
        *name = SysAllocString(L"Panda五笔 设置");
        return *name ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP Show(HWND wndParent, LANGID, REFGUID) override {
        return LaunchSettingsWindow(wndParent);
    }

private:
    LONG ref_ = 1;
};

// ---------------------------------------------------------------- 类工厂
class CClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&ref_);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (ppv) *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        CTextService* svc = new (std::nothrow) CTextService();
        if (!svc) return E_OUTOFMEMORY;
        HRESULT hr = svc->QueryInterface(riid, ppv);
        svc->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_cRef); else InterlockedDecrement(&g_cRef);
        return S_OK;
    }
private:
    LONG ref_ = 1;
};

// ---------------------------------------------------------------- 导出
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_WubiTsf)) return CLASS_E_CLASSNOTAVAILABLE;
    // 系统（文本服务控制面板 / 输入法菜单）会用它请求设置入口
    if (IsEqualIID(riid, IID_ITfFnConfigure)) {
        CConfigureFn* fn = new (std::nothrow) CConfigureFn();
        if (!fn) return E_OUTOFMEMORY;
        const HRESULT hrc = fn->QueryInterface(riid, ppv);
        fn->Release();
        LogLine(L"DllGetClassObject: ITfFnConfigure hr=0x%08lX", (unsigned long)hrc);
        return hrc;
    }
    CClassFactory* cf = new (std::nothrow) CClassFactory();
    if (!cf) return E_OUTOFMEMORY;
    HRESULT hr = cf->QueryInterface(riid, ppv);
    cf->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return (g_cRef == 0) ? S_OK : S_FALSE; }

// 注册根：优先 HKEY_LOCAL_MACHINE（TSF profile 目录是机器级的），否则回落 HKEY_CURRENT_USER
static bool g_machineReg = true;

static HKEY CreateReg(HKEY root, const wchar_t* subkey) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) return k;
    return nullptr;
}

// 先试 HKLM，失败则 HKCU；g_machineReg 记录本次走的是哪边
static HKEY CreateRegBest(const wchar_t* subkey) {
    HKEY k = CreateReg(HKEY_LOCAL_MACHINE, subkey);
    if (k) { g_machineReg = true; return k; }
    g_machineReg = false;
    return CreateReg(HKEY_CURRENT_USER, subkey);
}

static void RegisterTipKeys(bool install) {
    const wchar_t* kClsid = L"Software\\Classes\\CLSID\\{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}";
    const wchar_t* kTip   = L"Software\\Microsoft\\CTF\\TIP\\{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}";
    if (!install) {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsid);
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, kTip);
        RegDeleteTreeW(HKEY_CURRENT_USER, kClsid);
        RegDeleteTreeW(HKEY_CURRENT_USER, kTip);
        return;
    }
    wchar_t dll[MAX_PATH];
    GetModuleFileNameW(g_hInst, dll, ARRAYSIZE(dll));

    HKEY k = CreateRegBest(kClsid);
    if (k) {
        const wchar_t* name = L"Panda五笔";
        RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)name, (DWORD)((wcslen(name) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    std::wstring inproc = std::wstring(kClsid) + L"\\InprocServer32";
    k = CreateRegBest(inproc.c_str());
    if (k) {
        RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)dll, (DWORD)((wcslen(dll) + 1) * sizeof(wchar_t)));
        RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, (const BYTE*)L"Apartment", sizeof(L"Apartment"));
        RegCloseKey(k);
    }

    std::wstring kProfile = std::wstring(kTip) + L"\\LanguageProfile\\0x00000804\\{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}";
    k = CreateRegBest(kProfile.c_str());
    if (k) {
        const wchar_t* desc = L"Panda五笔";
        RegSetValueExW(k, L"Description", 0, REG_SZ, (const BYTE*)desc, (DWORD)((wcslen(desc) + 1) * sizeof(wchar_t)));
        // 图标路径优先级（实测结论）：
        //   %LOCALAPPDATA%\PandaWubi\panda.ico  ← 已验证在输入指示器上能正常显示
        //   其次 DLL 同目录的 panda.ico
        //   最后退回 DLL 自身（IconIndex 0）
        std::wstring iconPath = dll;
        {
            wchar_t envBase[MAX_PATH] = { 0 };
            DWORD en = GetEnvironmentVariableW(L"LOCALAPPDATA", envBase, ARRAYSIZE(envBase));
            if (en > 0 && en < ARRAYSIZE(envBase)) {
                const std::wstring p1 = std::wstring(envBase) + L"\\PandaWubi\\panda.ico";
                if (GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES) iconPath = p1;
            }
            if (iconPath == dll) {
                std::wstring d2 = dll;
                const size_t sl = d2.find_last_of(L"\\\\/");
                if (sl != std::wstring::npos) {
                    const std::wstring p2 = d2.substr(0, sl) + L"\\panda.ico";
                    if (GetFileAttributesW(p2.c_str()) != INVALID_FILE_ATTRIBUTES) iconPath = p2;
                }
            }
        }
        RegSetValueExW(k, L"IconFile", 0, REG_SZ, (const BYTE*)iconPath.c_str(),
                       (DWORD)((iconPath.size() + 1) * sizeof(wchar_t)));
        DWORD zero = 0, one = 1;
        RegSetValueExW(k, L"IconIndex", 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
        RegSetValueExW(k, L"Enable", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegCloseKey(k);
    }

    std::wstring kCat = std::wstring(kTip) +
        L"\\Category\\Category\\{34745C63-B2F0-4784-8B67-5E12C8701A31}\\{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}";
    k = CreateRegBest(kCat.c_str()); if (k) RegCloseKey(k);

    std::wstring kItem = std::wstring(kTip) +
        L"\\Category\\Item\\{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}\\{34745C63-B2F0-4784-8B67-5E12C8701A31}";
    k = CreateRegBest(kItem.c_str()); if (k) RegCloseKey(k);
}
static HRESULT EnableProfile(bool enable) {
    ITfInputProcessorProfiles* p = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITfInputProcessorProfiles, (void**)&p)) || !p) return E_FAIL;
    if (enable) {
        const wchar_t* desc = L"Panda五笔";
        wchar_t modPath[MAX_PATH] = { 0 };
        GetModuleFileNameW(g_hInst, modPath, ARRAYSIZE(modPath));
        p->AddLanguageProfile(CLSID_WubiTsf, kLangId, GUID_WubiProfile,
                              desc, (ULONG)wcslen(desc), modPath, 0, 0);
        // 用 ITfInputProcessorProfileMgr::RegisterProfile 正式登记（带图标）。
        // 依据 msime-windows 的实践：图标要用固定路径的 .ico；任务栏取不到图标会缓存「简体」文字，
        // 直到资源管理器重启，所以不能让路径随 DLL 版本变化。
        {
            ITfInputProcessorProfileMgr* pm = nullptr;
            if (SUCCEEDED(p->QueryInterface(IID_ITfInputProcessorProfileMgr, (void**)&pm)) && pm) {
                std::wstring iconFile = modPath;
                {
                    std::wstring dir(iconFile);
                    const size_t slash = dir.find_last_of(L"\\/");
                    if (slash != std::wstring::npos) {
                        const std::wstring icoPath = dir.substr(0, slash) + L"\\ppwubi.ico";
                        if (GetFileAttributesW(icoPath.c_str()) != INVALID_FILE_ATTRIBUTES) iconFile = icoPath;
                    }
                }
                LogLine(L"RegisterProfile icon=%ls", iconFile.c_str());
                const HRESULT hReg = pm->RegisterProfile(
                    CLSID_WubiTsf, kLangId, GUID_WubiProfile,
                    desc, (ULONG)wcslen(desc),
                    iconFile.c_str(), (ULONG)iconFile.size(), 0,
                    nullptr, 0, TRUE, 0);
                LogLine(L"RegisterProfile(icon) hr=0x%08lX", (unsigned long)hReg);
                pm->Release();
            }
        }
        p->EnableLanguageProfile(CLSID_WubiTsf, kLangId, GUID_WubiProfile, TRUE);
        p->EnableLanguageProfileByDefault(CLSID_WubiTsf, kLangId, GUID_WubiProfile, TRUE);
    } else {
        p->EnableLanguageProfile(CLSID_WubiTsf, kLangId, GUID_WubiProfile, FALSE);
    }
    p->Release();
    return S_OK;
}

STDAPI DllRegisterServer() {
    LogLine(L"DllRegisterServer: begin");
    RegisterTipKeys(true);
    // TSF 的 profile 目录是机器级的：没有管理员权限时注册一定失败，必须显式报错，
    // 否则 regsvr32 /s 会静默"假成功"，用户以为装好了却看不到输入法。
    if (!g_machineReg) { LogLine(L"DllRegisterServer: HKLM denied (need admin)"); return E_ACCESSDENIED; }

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT hrProfile = EnableProfile(true);

    // 按 msime-windows 的做法补注册 TIP 能力类别：
    //   IMMERSIVESUPPORT  -> UWP/沉浸式应用可用
    //   SYSTRAYSUPPORT    -> 允许在系统区域（输入指示器）显示
    // 这两个类别直接影响任务栏/输入指示器是否愿意显示我们的图标。
    {
        ITfCategoryMgr* cm = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_ITfCategoryMgr, (void**)&cm)) && cm) {
            // 类别注册（对照 QQ五笔 的 8 项）：
            //   TIP_KEYBOARD                —— 键盘类 TIP（必需）
            //   TIPCAP_IMMERSIVESUPPORT     —— 支持 UWP/沉浸式
            //   TIPCAP_SYSTRAYSUPPORT       —— 支持系统区域显示
            //   TIPCAP_INPUTMODECOMPARTMENT —— 支持输入模式隔间，指示器才会显示 中/英
            //   CATEGORY_OF_TIP->KEYBOARD   —— 声明类别归属
            //   TIPCAP_COMLESS              —— 支持无 COM 激活
            const HRESULT h1 = cm->RegisterCategory(CLSID_WubiTsf, GUID_TFCAT_TIP_KEYBOARD, CLSID_WubiTsf);
            const HRESULT h2 = cm->RegisterCategory(CLSID_WubiTsf, GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT, CLSID_WubiTsf);
            const HRESULT h3 = cm->RegisterCategory(CLSID_WubiTsf, GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT, CLSID_WubiTsf);
            const HRESULT h4 = cm->RegisterCategory(CLSID_WubiTsf, GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT, CLSID_WubiTsf);
            const HRESULT h5 = cm->RegisterCategory(CLSID_WubiTsf, GUID_TFCAT_CATEGORY_OF_TIP, GUID_TFCAT_TIP_KEYBOARD);
            const HRESULT h6 = cm->RegisterCategory(CLSID_WubiTsf, GUID_TFCAT_TIPCAP_COMLESS, CLSID_WubiTsf);
            LogLine(L"RegisterCategory: keyboard=0x%08lX immersive=0x%08lX systray=0x%08lX inputmode=0x%08lX catof=0x%08lX comless=0x%08lX",
                    (unsigned long)h1, (unsigned long)h2, (unsigned long)h3,
                    (unsigned long)h4, (unsigned long)h5, (unsigned long)h6);
            cm->Release();
        }
    }
    LogLine(L"DllRegisterServer: EnableProfile hr=0x%08X", (unsigned)hrProfile);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return hrProfile;
}

STDAPI DllUnregisterServer() {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    EnableProfile(false);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    RegisterTipKeys(false);
    return S_OK;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hModule;
        DisableThreadLibraryCalls(hModule);
        LogLine(L"DllMain: attach pid=%lu", GetCurrentProcessId());
    }
    return TRUE;
}
