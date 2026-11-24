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
#include <algorithm>
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
#include "panda_version.h"
#include "ipc.h"

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

// 缓存按资源 id 分开：熊猫(3)/狸花猫(4) 都要能各自缓存，切主题时不会拿到旧的那张
static Gdiplus::Bitmap* g_artCache[16] = { nullptr };

// 底部背景图顶行的平均颜色（= 画面的天空色）。窗口上半部分用它填，和图无缝衔接。
// 图片靠右下对齐时，窗口左边空出来的那块要跟图片"接上"。
static COLORREF            g_bandSky      = RGB(0xF7, 0xF9, 0xFC);  // 左上角那一个像素
static COLORREF            g_bandTopEdge  = RGB(0xF7, 0xF9, 0xFC);  // 图片"上边缘"平均色
static COLORREF            g_bandLeftEdge = RGB(0xF7, 0xF9, 0xFC);  // 图片"左边缘"平均色
static int                 g_bandSkyFor   = 0;                       // 已处理过的资源 id

// 图片靠右下对齐时，窗口空出来那块要跟图片接上。
// 不能只取"左上角那一个像素"：田园犬那张左上角正好是太阳的一团白光，而整片天空是蓝的 ——
// 用角上的点会把整块底色填成惨白，和图片差得很远。
// 规则：上方留白 → 取图片"上边缘"平均色；左侧留白 → 取"左边缘"平均色。
static void SampleBandSky(int id, Gdiplus::Bitmap* bmp) {
    if (!bmp || g_bandSkyFor == id) return;
    const int w = (int)bmp->GetWidth();
    const int h = (int)bmp->GetHeight();
    if (w <= 0 || h <= 0) return;

    Gdiplus::Color c0;
    if (bmp->GetPixel(0, 0, &c0) == Gdiplus::Ok) g_bandSky = RGB(c0.GetR(), c0.GetG(), c0.GetB());

    // 取"中位色"而不是平均色：图片边缘常常一边是太阳/云的高光、另一边才是真正的天色，
    // 平均会被高光拉白（田园犬那张实测均值 177,218,255，而天空其实是 149,207,255）。
    auto medianOf = [&](bool horizontal) -> COLORREF {
        struct Sample { int r, g, b, sum; };
        std::vector<Sample> v;
        v.reserve(32);
        for (int i = 0; i < 32; ++i) {
            const int x = horizontal ? (int)((long long)i * (w - 1) / 31) : 1;
            const int y = horizontal ? 1 : (int)((long long)i * (h - 1) / 31);
            Gdiplus::Color c;
            if (bmp->GetPixel(x, y, &c) == Gdiplus::Ok) {
                const int r = c.GetR(), g = c.GetG(), b = c.GetB();
                v.push_back(Sample{ r, g, b, r + g + b });
            }
        }
        if (v.empty()) return g_bandSky;
        std::sort(v.begin(), v.end(), [](const Sample& a, const Sample& b) { return a.sum < b.sum; });
        const Sample& m = v[v.size() / 2];
        return RGB(m.r, m.g, m.b);
    };
    g_bandTopEdge  = medianOf(true);
    g_bandLeftEdge = medianOf(false);
    g_bandSkyFor = id;
}

static Gdiplus::Bitmap* LoadArtFromResource(int id) {
    if (id > 0 && id < 16 && g_artCache[id]) return g_artCache[id];
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
    if (id > 0 && id < 16) g_artCache[id] = bmp;
    return bmp;
}
static LONG      g_cRef  = 0;

// ---------------------------------------------------------------- 诊断日志
// 目的: 我没有可用的激活环境，日志是唯一能从现场取证的通道。
// 位置: %TEMP%\wubi_tsf_<pid>.log ；每次写入即 flush，崩了也能留下线索。
static wchar_t      g_logPath[MAX_PATH] = { 0 };   // 当前进程的日志文件路径（attach 行里打印）
static const wchar_t* LogFilePath() { return g_logPath; }

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
    static volatile LONG seq = 0;
    const LONG n = InterlockedIncrement(&seq);
    wchar_t line[1200];
    swprintf_s(line, L"[%02d:%02d:%02d.%03d #%ld] %s\r\n",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, n, msg);

    wcscpy_s(g_logPath, path);      // 供 attach 行打印（路径长度 < MAX_PATH）
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    // 超过 16 MB 就滚一份 .old：正常的崩溃诊断只看尾部，文件太大反而不好打开
    if (sz.QuadPart > (16LL << 20)) {
        CloseHandle(h);
        std::wstring bak = std::wstring(path) + L".old";
        MoveFileExW(path, bak.c_str(), MOVEFILE_REPLACE_EXISTING);
        h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        sz.QuadPart = 0;
    }
    DWORD wrote = 0;
    if (sz.QuadPart == 0) {
        const WORD bom = 0xFEFF;
        WriteFile(h, &bom, sizeof(bom), &wrote, nullptr);
    }
    WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &wrote, nullptr);
    CloseHandle(h);
}

// 用户词文件：<数据目录>\user.tsv（自造词 + 词频自学习结果）
// 数据目录可由用户在设置里指定（可指向网盘同步夹），见 wubi::GetDataDir()
static std::wstring GetUserWordsPath() {
    return wubi::GetUserWordsPath();
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
    // 数据目录可由用户指定（设置里可选网盘同步夹）；WUBI_INI 可整体覆盖 ini 路径
    return wubi::GetConfigPath();
}void DictHolder::Load() {
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
        LogLine(L"dict load(bin): path=%s entries=%lld ok=1 userWords=%lld userPath=%s",
                binPath.c_str(), n, u, GetUserWordsPath().c_str());
        return;
    }
    n = dict_.Load(p);
    loaded_ = (n > 0);
    // 用户词（自造词 + 词频自学习结果）
    const long long u = dict_.LoadUserWords(GetUserWordsPath());
    LogLine(L"dict load(tsv): path=%s entries=%lld codes=%zu ok=%d userWords=%lld userPath=%s",
            p.c_str(), n, dict_.CodeCount(), (int)loaded_, u, GetUserWordsPath().c_str());
}

// ---------------------------------------------------------------- 候选窗
class CCandidateWindow {
public:
    ~CCandidateWindow() { Destroy(); }

    void SetItems(const std::vector<std::wstring>& items, bool vertical) {
        items_ = items;
        vertical_ = vertical;
        // 取消"徽标短暂显示"的定时器：否则中/英状态提示的 700ms 定时器会在组码过程中把窗口藏掉
        if (hwnd_) KillTimer(hwnd_, kBriefTimerId);
        RecalcAndResize();
    }
    // 与 items_ 一一对应：1 = 人工锁定（自造词/自定义短语），词尾加个小淡蓝星号
    void SetPinned(const std::vector<char>& v) {
        if (pinned_ == v) return;
        pinned_ = v;
        RecalcAndResize();
    }
    void SetOnClick(std::function<void(int)> fn) { onClick_ = std::move(fn); }
    // 右键菜单（合并后状态栏的菜单挂到这里）
    void SetOnMenu(std::function<void(POINT)> fn) { onMenu_ = std::move(fn); }
    HWND Hwnd() { Ensure(); return hwnd_; }
    // 状态模式：空闲时就显示在光标附近，只放"中/英·简/繁"这类基本信息
    // 状态条内容：熊猫图标 + 中/英 + 简/繁 + 设置齿轮（和原来的悬浮栏一致）
    void SetStatusInfo(bool english, bool trad, const std::wstring& iconPath);
    void SetOnStatusClick(std::function<void(int)> fn) { onStatusClick_ = std::move(fn); }
    void SetStatusMode(bool on) {
        if (statusMode_ == on) return;
        statusMode_ = on;
    }
    bool StatusMode() const { return statusMode_; }
    void SetLetterLabels(bool on) { if (letterLabels_ != on) { letterLabels_ = on; RecalcAndResize(); } }
    // 纯文本徽标：不显示序号（用于中/英状态提示，避免被当成候选词）
    void SetPlain(bool on) { if (plain_ != on) { plain_ = on; RecalcAndResize(); } }
    // 主题「丹顶鹤」：候选窗最下方铺一条整幅风景（不抠图），右对齐 + 底对齐
    //   id = RCDATA 资源号；capDip = 条带最大高度（像素为宽时按比例算，超过就裁掉上半部分）
    void SetBottomBand(int id, int capDip) {
        if (bandId_ == id && bandCapDip_ == capDip) return;
        bandId_ = id; bandCapDip_ = capDip;
        if (hwnd_) RecalcAndResize();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 主题：候选窗右下角装饰图用哪个资源（3=熊猫吃竹子，4=狸花猫）
    void SetArtId(int id) {
        if (artId_ == id) return;
        artId_ = id;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    // 顶部组码显示
    void SetCode(const std::wstring& c) { code_ = c; if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    bool HasCode() const { return !code_.empty(); }
    // 外观：字体名 / 字号(pt) / 背景色 / 文字色 / 选中文字色 / 选中底色 / 边框色 / 内镶凹陷
    void SetStyle(int fontPt, COLORREF bg, bool inset,
                  const std::wstring& face = L"Microsoft YaHei UI",
                  COLORREF text = RGB(0x20, 0x20, 0x28),
                  COLORREF border = RGB(0xA8, 0xCD, 0xA8)) {
        fontPt_   = fontPt > 6 ? fontPt : 14;
        bgCol_    = bg;
        inset_    = inset;
        face_     = face.empty() ? std::wstring(L"Microsoft YaHei UI") : face;
        textCol_  = text;
        borderCol_ = border;
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        if (starFont_) { DeleteObject(starFont_); starFont_ = nullptr; }
        if (hwnd_) {
            const UINT dpi = Dpi();
            const int h = -MulDiv(fontPt_, (int)dpi, 72);
            font_ = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face_.c_str());
            int starPt = fontPt_ * 3 / 5;
            if (starPt < 8) starPt = 8;
            starFont_ = CreateFontW(-MulDiv(starPt, (int)dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face_.c_str());
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
    void PaintStatus(HDC dc);
    void PaintPinnedStar(HDC dc, int x, const RECT& row, UINT dpi);
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
    std::wstring face_ = L"Microsoft YaHei UI";   // 字体名
    std::vector<char> pinned_;                    // 1 = 人工锁定，词尾加小星号
    int      artId_ = 3;                          // 右下角装饰图资源 id（3=熊猫 4=狸花猫 0=不画）
    int      bandId_ = 0;                         // 底部风景条资源 id（0=不画）
    int      bandCapDip_ = 120;                   // 底部风景条最大高度（逻辑像素）
    int      bandH_ = 0;                          // 本次布局算出的条带高度（物理像素）
    HFONT    starFont_ = nullptr;                 // 星号用的小字号字体
    COLORREF textCol_   = RGB(0x20, 0x20, 0x28);  // 普通候选字

    COLORREF borderCol_ = RGB(0xA8, 0xCD, 0xA8);  // 边框
    HBITMAP  art_    = nullptr;               // 右下角熊猫装饰
    std::wstring code_;                       // 顶部显示的组码
    int      headH_  = 0;                     // 顶部框高度
    int      rowH_   = 0;                     // 每行高度
    int      artW_   = 0;                     // 右下角装饰图尺寸（右侧留白）
    static const UINT kBriefTimerId = 0xB1;  // 徽标自动隐藏定时器
    std::function<void(int)> onClick_;
    std::function<void(POINT)> onMenu_;
    std::function<void(int)> onStatusClick_;
    bool statusMode_ = false;
    bool stEnglish_ = false;      // 状态条：当前中/英
    bool stTrad_ = false;         // 状态条：当前简/繁
    HICON statusIcon_ = nullptr;  // 状态条左侧的产品图标
    std::wstring statusIconPath_;
    std::vector<RECT> statusZones_;   // 0=图标 1=中/英 2=简/繁 3=齿轮
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
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face_.c_str());
    {
        int starPt = fontPt_ * 3 / 5;
        if (starPt < 8) starPt = 8;
        starFont_ = CreateFontW(-MulDiv(starPt, (int)dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face_.c_str());
    }
    LoadArtFromResource(artId_);   // 预热：PNG 解码 + GDI+ 初始化放到窗口创建时，不占第一次按键的时间
}

void CCandidateWindow::Destroy() {
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    if (starFont_) { DeleteObject(starFont_); starFont_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void CCandidateWindow::RecalcAndResize() {
    Ensure();
    // 允许"只有顶部小框、没有候选行"的状态：英文模式没匹配到单词时也要显示输入内容
    if (!hwnd_ || (items_.empty() && code_.empty() && !statusMode_)) {
        layout_ = wubi::CandLayout();
        width_ = height_ = 0;
        return;
    }
    // 状态条（空闲时的小窗）：图标 + 中/英 + 简/繁 + 齿轮
    if (statusMode_) {
        const UINT dpiS = Dpi();
        padX_ = MulDiv(6, (int)dpiS, 96);
        padY_ = MulDiv(3, (int)dpiS, 96);
        HDC dcS = GetDC(hwnd_);
        HGDIOBJ oldS = SelectObject(dcS, font_);
        SIZE sCn = {0,0}, sTr = {0,0}, sGear = {0,0};
        GetTextExtentPoint32W(dcS, L"\u4e2d", 1, &sCn);
        GetTextExtentPoint32W(dcS, L"\u7b80", 1, &sTr);
        GetTextExtentPoint32W(dcS, L"\u2699", 1, &sGear);
        SelectObject(dcS, oldS);
        ReleaseDC(hwnd_, dcS);
        const int iconSz = MulDiv(16, (int)dpiS, 96);
        const int gap = padX_ / 2;
        width_ = padX_ * 2 + iconSz + padX_ + sCn.cx + padX_ + sTr.cx + padX_ + sGear.cx;
        height_ = ((sCn.cy > iconSz) ? sCn.cy : iconSz) + padY_ * 2;
        (void)gap;
        layout_ = wubi::CandLayout();
        layout_.width = width_;
        layout_.height = height_;
        return;
    }

    const UINT dpi = Dpi();
    padX_ = MulDiv(7, (int)dpi, 96);
    padY_ = MulDiv(4, (int)dpi, 96);
    rowH_ = MulDiv(20, (int)dpi, 96);
    headH_ = code_.empty() ? 0 : MulDiv(24, (int)dpi, 96);

    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    // 行高要跟着字号走：以前写死 20px，字号调大后文字会压在下一行上
    {
        TEXTMETRICW tm = {};
        if (GetTextMetricsW(dc, &tm)) {
            const int need = (int)tm.tmHeight + MulDiv(8, (int)dpi, 96);
            if (rowH_ < need) rowH_ = need;
            const int needHead = (int)tm.tmHeight + MulDiv(10, (int)dpi, 96);
            if (headH_ > 0 && headH_ < needHead) headH_ = needHead;
        }
    }
    labels_.clear();
    int maxW = MulDiv(90, (int)dpi, 96);
    int starW = 0;                       // 星号宽（人工锁定词尾的小淡蓝星）
    if (starFont_ && pinned_.size() == items_.size()) {
        HGDIOBJ sf = SelectObject(dc, starFont_);
        SIZE ss = { 0, 0 };
        GetTextExtentPoint32W(dc, L"*", 1, &ss);
        starW = ss.cx + MulDiv(3, (int)dpi, 96);   // 星号 + 一点间距
        SelectObject(dc, sf);
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        std::wstring t;
        if (plain_) t = items_[i];
        else if (letterLabels_) t = std::wstring(1, (wchar_t)(L'a' + i)) + L". " + items_[i];
        else t = std::to_wstring(i + 1) + L". " + items_[i];
        SIZE sz;
        GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
        int w = sz.cx;
        if (!plain_ && i < pinned_.size() && pinned_[i]) w += starW;
        if (w > maxW) maxW = w;
        labels_.push_back(std::move(t));
    }
    // 只有顶部小框（没有候选）：宽度按当前输入的文本算，不画装饰图
    if (items_.empty()) {
        SIZE cs = { 0, 0 };
        GetTextExtentPoint32W(dc, code_.c_str(), (int)code_.size(), &cs);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        artW_ = 0;
        bandH_ = 0;          // 没有候选行时不画底部风景条（也不用留高度）
        width_ = cs.cx + padX_ * 2 + MulDiv(10, (int)dpi, 96);
        if (width_ < MulDiv(120, (int)dpi, 96)) width_ = MulDiv(120, (int)dpi, 96);
        height_ = headH_ + padY_ * 2 + MulDiv(6, (int)dpi, 96);
        layout_ = wubi::CandLayout();
        layout_.width = width_;
        layout_.height = height_;
        return;
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

    // 整窗背景图：不额外占高度，图片直接铺满整个候选窗（文字压在上面）
    bandH_ = 0;
    layout_.width = width_;
    layout_.height = height_;
}
// 人工锁定的小星号：淡蓝色、比正文小一号，画在词尾
// 状态条：图标 + 中/英 + 简/繁 + 齿轮（原悬浮栏的外观，搬到这个窗口里）
void CCandidateWindow::PaintStatus(HDC dc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const UINT dpi = Dpi();
    HBRUSH bg = CreateSolidBrush(bgCol_);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    HBRUSH bd = CreateSolidBrush(borderCol_);
    FrameRect(dc, &rc, bd);
    DeleteObject(bd);

    statusZones_.clear();
    const int padX = MulDiv(6, (int)dpi, 96);
    const int iconSz = MulDiv(16, (int)dpi, 96);
    int x = padX;
    int y = (rc.bottom - iconSz) / 2;

    // 图标区
    RECT zIcon = { x - MulDiv(2,(int)dpi,96), 0, x + iconSz + MulDiv(2,(int)dpi,96), rc.bottom };
    if (statusIcon_) DrawIconEx(dc, x, y, statusIcon_, iconSz, iconSz, 0, nullptr, DI_NORMAL);
    statusZones_.push_back(zIcon);
    x += iconSz + padX;

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    const wchar_t* texts[3] = { stEnglish_ ? L"\u82f1" : L"\u4e2d", stTrad_ ? L"\u7e41" : L"\u7b80", L"\u2699" };
    const bool act[3] = { stEnglish_, stTrad_, false };
    for (int i = 0; i < 3; ++i) {
        SIZE sz = { 0, 0 };
        GetTextExtentPoint32W(dc, texts[i], 1, &sz);
        RECT z = { x - padX / 2, 0, x + sz.cx + padX / 2, rc.bottom };
        RECT tr = { x, 0, x + sz.cx, rc.bottom };
        SetTextColor(dc, act[i] ? RGB(200, 70, 40) : textCol_);
        DrawTextW(dc, texts[i], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        statusZones_.push_back(z);
        x += sz.cx + padX;
    }
    SelectObject(dc, oldFont);
}

void CCandidateWindow::SetStatusInfo(bool english, bool trad, const std::wstring& iconPath) {
    const bool changed = (stEnglish_ != english) || (stTrad_ != trad) || (statusIconPath_ != iconPath);
    stEnglish_ = english;
    stTrad_ = trad;
    if (statusIconPath_ != iconPath) {
        statusIconPath_ = iconPath;
        if (statusIcon_) { DestroyIcon(statusIcon_); statusIcon_ = nullptr; }
        if (!iconPath.empty()) {
            statusIcon_ = (HICON)LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                           MulDiv(16, (int)Dpi(), 96), MulDiv(16, (int)Dpi(), 96),
                                           LR_LOADFROMFILE | LR_DEFAULTCOLOR);
        }
    }
    if (changed) {
        RecalcAndResize();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void CCandidateWindow::PaintPinnedStar(HDC dc, int x, const RECT& row, UINT dpi) {
    if (!starFont_) return;
    HGDIOBJ oldFont = SelectObject(dc, starFont_);
    SetTextColor(dc, RGB(0x6F, 0xA8, 0xD8));   // 淡淡的蓝
    RECT sr = { x + MulDiv(2, (int)dpi, 96), row.top + MulDiv(6, (int)dpi, 96),
                row.right, row.bottom };
    DrawTextW(dc, L"*", -1, &sr, DT_LEFT | DT_TOP | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

void CCandidateWindow::Paint(HDC dc) {
    if (statusMode_) { PaintStatus(dc); return; }
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W = rc.right, H = rc.bottom;
    const UINT dpi = Dpi();

    // ---- 主题底图：先算好几何，再决定"空出来那块"填什么颜色 ----
    Gdiplus::Bitmap* band = nullptr;
    Gdiplus::REAL bLeft = 0, bTop = 0, bDw = 0, bDh = 0;
    COLORREF bandFill = bgCol_;
    if (bandId_ != 0 && !items_.empty()) {
        band = LoadArtFromResource(bandId_);
        if (band) SampleBandSky(bandId_, band);
        if (band && band->GetWidth() > 0 && band->GetHeight() > 0) {
            const Gdiplus::REAL iw = (Gdiplus::REAL)band->GetWidth();
            const Gdiplus::REAL ih = (Gdiplus::REAL)band->GetHeight();
            Gdiplus::REAL scale = (Gdiplus::REAL)W / iw;
            if (ih * scale > (Gdiplus::REAL)H) scale = (Gdiplus::REAL)H / ih;
            bDw = iw * scale; bDh = ih * scale;
            bLeft = (Gdiplus::REAL)W - bDw;
            bTop  = (Gdiplus::REAL)H - bDh;
            // 上方留白 → 用图片"上边缘"的平均色；左侧留白 → 用"左边缘"的平均色
            bandFill = (bTop > 0.5f) ? g_bandTopEdge : g_bandLeftEdge;
        } else {
            band = nullptr;
        }
    }

    // ---- 背景填充 ----
    {
        HBRUSH br = CreateSolidBrush(band ? bandFill : bgCol_);
        RECT all = { 0, 0, W, H };
        FillRect(dc, &all, br);
        DeleteObject(br);
    }

    // ---- 主题底图：整幅不抠，等比缩放到贴住窗口，右对齐 + 底对齐 ----
    if (band) {
        Gdiplus::Graphics gfx(dc);
        gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        gfx.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        const Gdiplus::REAL iw = (Gdiplus::REAL)band->GetWidth();
        const Gdiplus::REAL ih = (Gdiplus::REAL)band->GetHeight();
        const Gdiplus::REAL dw = bDw, dh = bDh, left = bLeft, top = bTop;
        if (iw > 0 && ih > 0) {
            gfx.DrawImage(band, Gdiplus::RectF(left, top, dw, dh),
                          0.0f, 0.0f, iw, ih, Gdiplus::UnitPixel, nullptr);

            const BYTE sr = GetRValue(bandFill), sg = GetGValue(bandFill), sb = GetBValue(bandFill);

            // 图片左沿：从"左边那个纯色"淡入到图片，宽 ~22dp。
            // 左边是纯色，所以淡出的终点正好等于左边底色，接缝自然消失。
            if (left > 0) {
                const Gdiplus::REAL fadeW = (Gdiplus::REAL)MulDiv(22, (int)dpi, 96);
                const Gdiplus::REAL w2 = fadeW < dw ? fadeW : dw;
                if (w2 > 0) {
                    Gdiplus::RectF fs(left, top, w2, dh);
                    Gdiplus::LinearGradientBrush fg(fs, Gdiplus::Color(255, sr, sg, sb),
                                                    Gdiplus::Color(0, sr, sg, sb),
                                                    Gdiplus::LinearGradientModeHorizontal);
                    gfx.FillRectangle(&fg, fs);
                }
            }

            // 上沿：只有"图片上方还有空白（宽图贴在下方时）"才需要糊一下
            const Gdiplus::REAL fadeH = (Gdiplus::REAL)MulDiv(22, (int)dpi, 96);
            if (top > 0 && fadeH > 0 && top + fadeH < (Gdiplus::REAL)H) {
                Gdiplus::RectF strip(left, top, dw, fadeH);
                Gdiplus::LinearGradientBrush lg(strip, Gdiplus::Color(255, sr, sg, sb),
                                                Gdiplus::Color(0, sr, sg, sb),
                                                Gdiplus::LinearGradientModeVertical);
                gfx.FillRectangle(&lg, strip);
            }

            // 整窗一层很淡的白纱：文字压在画面上（比如熊猫的黑手臂）也能看清；
            // 盖整窗而不是只盖图片，拉伸列与图片被压淡的程度一致，接缝不会因此露出来。
            Gdiplus::SolidBrush veil(Gdiplus::Color(66, 255, 255, 255));
            gfx.FillRectangle(&veil, Gdiplus::RectF(0.0f, 0.0f, (Gdiplus::REAL)W, (Gdiplus::REAL)H));
        }
    }

    // ---- 右下角：熊猫吃竹子（GDI+，PNG 带 alpha）；只有候选行时才画 ----
    if (!items_.empty()) if (Gdiplus::Bitmap* art = LoadArtFromResource(artId_)) {
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
    HPEN border = CreatePen(PS_SOLID, 1, borderCol_);
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
        HBRUSH hbg = CreateSolidBrush(bgCol_);
        FillRect(dc, &hb, hbg);
        DeleteObject(hbg);
        HPEN hp = CreatePen(PS_SOLID, 1, borderCol_);
        HGDIOBJ op = SelectObject(dc, hp);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, hb.left, hb.top, hb.right, hb.bottom, radius, radius);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(hp);

        SetTextColor(dc, textCol_);
        RECT tr = hb;
        tr.left += MulDiv(6, (int)dpi, 96);
        DrawTextW(dc, code_.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (!items_.empty()) {   // 没有候选时不画翻页箭头（英文模式只显示输入内容）
            SetTextColor(dc, RGB(GetRValue(textCol_) / 2 + 0x60, GetGValue(textCol_) / 2 + 0x60, GetBValue(textCol_) / 2 + 0x60));
            RECT ar = hb;
            ar.right -= MulDiv(6, (int)dpi, 96);
            DrawTextW(dc, L"\u25C0 \u25B6", -1, &ar, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
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
            SetTextColor(dc, textCol_);   // 不再给 1 号候选加底色，序号 1 已经说明了一切
            RECT nr = r;
            DrawTextW(dc, num.c_str(), -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SIZE sn = { 0, 0 };
            GetTextExtentPoint32W(dc, num.c_str(), (int)num.size(), &sn);
            SetTextColor(dc, textCol_);
            RECT wr = r;
            wr.left += sn.cx;
            DrawTextW(dc, word.c_str(), -1, &wr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            // 人工锁定的词：词尾一个小淡蓝星号（不抢眼，只是提示"这是我手动加的"）
            if (i < pinned_.size() && pinned_[i]) {
                SIZE sw = { 0, 0 };
                GetTextExtentPoint32W(dc, word.c_str(), (int)word.size(), &sw);
                PaintPinnedStar(dc, wr.left + sw.cx, r, dpi);
            }
        }
    } else {
        int x = padX_;
        for (size_t i = 0; i < n; ++i) {
            RECT r = { x, y, x + MulDiv(110, (int)dpi, 96), y + rowH_ };
            SetTextColor(dc, textCol_);
            DrawTextW(dc, labels_[i].c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            if (i < pinned_.size() && pinned_[i]) {
                SIZE sw = { 0, 0 };
                GetTextExtentPoint32W(dc, labels_[i].c_str(), (int)labels_[i].size(), &sw);
                PaintPinnedStar(dc, r.left + sw.cx, r, dpi);
            }
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
    case WM_RBUTTONUP: {
        if (self && self->onMenu_) {
            POINT pt;
            GetCursorPos(&pt);
            auto fn = self->onMenu_;
            fn(pt);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (self) {
            if (self->statusMode_) {
                POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                for (size_t i = 0; i < self->statusZones_.size(); ++i) {
                    const RECT& z = self->statusZones_[i];
                    if (pt.x >= z.left && pt.x < z.right && pt.y >= z.top && pt.y < z.bottom) {
                        auto fn = self->onStatusClick_;
                        if (fn) fn((int)i);
                        break;
                    }
                }
                return 0;
            }
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
        if (self) {
            // 双缓冲：先画到内存位图再整体贴上，避免逐条画文字/贴图时闪
            RECT rc = { 0, 0, 0, 0 };
            GetClientRect(hwnd, &rc);
            const int w = rc.right, h = rc.bottom;
            if (w > 0 && h > 0) {
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
                if (mem && bmp) {
                    HGDIOBJ oldBmp = SelectObject(mem, bmp);
                    self->Paint(mem);
                    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, oldBmp);
                }
                if (bmp) DeleteObject(bmp);
                if (mem) DeleteDC(mem);
            } else {
                self->Paint(dc);
            }
        }
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
    if (!hwnd_ || (items_.empty() && code_.empty())) { Hide(); return; }
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
    // 已经显示在同一位置同一尺寸时，只重画内容——否则每按一键都 SetWindowPos 一次，
    // 窗口会闪一下（尺寸变化尤其明显）。
    RECT cur = { 0, 0, 0, 0 };
    const bool visible = IsWindowVisible(hwnd_) != FALSE;
    GetWindowRect(hwnd_, &cur);
    const bool sameGeom = (cur.left == x && cur.top == y &&
                           cur.right - cur.left == width_ && cur.bottom - cur.top == height_);
    if (sameGeom) {
        if (!visible) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);   // 自己画背景，不用系统擦除
        return;
    }
    // 尺寸/位置变了：一次 SetWindowPos 完成，已可见时不重复 show（减少闪烁）
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_,
                 SWP_NOACTIVATE | (visible ? 0u : (UINT)SWP_SHOWWINDOW));
    InvalidateRect(hwnd_, nullptr, FALSE);
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
// ---------------------------------------------------------------- 拼音反查表
// data\pinyin.tsv：拼音<TAB>字<TAB>五笔全码（tools\build-pinyin.ps1 从 Rime 的
// luna_pinyin.dict.yaml 生成）。按 Z 之后的字母走这张表，候选里带上五笔编码。
struct PyEntry {
    std::string  py;      // 拼音（小写，无调）
    std::wstring ch;      // 汉字
    std::wstring code;    // 该字的五笔全码
};

static std::vector<PyEntry>& PinyinTable() {
    static std::vector<PyEntry> table;
    return table;
}

static bool LoadPinyinTableOnce() {
    static bool loaded = false;
    static bool tried = false;
    if (tried) return loaded;
    tried = true;

    wchar_t mod[MAX_PATH] = { 0 };
    if (!GetModuleFileNameW(g_hInst, mod, ARRAYSIZE(mod))) return false;
    std::wstring dir(mod);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    dir.erase(slash);
    const std::wstring path = dir + L"\\data\\pinyin.tsv";

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) {
        LogLine(L"pinyin table: missing %s", path.c_str());
        return false;
    }
    auto& tbl = PinyinTable();
    char buf[1024];
    bool first = true;
    while (fgets(buf, sizeof(buf), fp)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        if (first) { first = false; if (line.rfind("pinyin", 0) == 0) continue; }
        const size_t tab1 = line.find('\t');
        if (tab1 == std::string::npos) continue;
        const size_t tab2 = line.find('\t', tab1 + 1);
        PyEntry e;
        e.py = line.substr(0, tab1);
        const std::string chUtf8 = line.substr(tab1 + 1,
            (tab2 == std::string::npos) ? std::string::npos : tab2 - tab1 - 1);
        const std::string code = (tab2 == std::string::npos) ? std::string() : line.substr(tab2 + 1);
        if (e.py.empty() || chUtf8.empty()) continue;
        const int n = MultiByteToWideChar(CP_UTF8, 0, chUtf8.c_str(), (int)chUtf8.size(), nullptr, 0);
        if (n <= 0) continue;
        e.ch.resize((size_t)n);
        MultiByteToWideChar(CP_UTF8, 0, chUtf8.c_str(), (int)chUtf8.size(), e.ch.data(), n);
        e.code.assign(code.begin(), code.end());
        tbl.push_back(std::move(e));
    }
    fclose(fp);
    // 表按"拼音"升序排好（生成脚本已排序；这里再排一次兜底），后面的前缀查询才能二分
    std::stable_sort(tbl.begin(), tbl.end(),
                     [](const PyEntry& a, const PyEntry& b) { return a.py < b.py; });
    loaded = true;
    LogLine(L"pinyin table: %zu entries from %s (sorted by pinyin)", tbl.size(), path.c_str());
    return loaded;
}

class CTextService;

static wubi::StatusBar& SharedBar();
static CTextService* g_barOwner = nullptr;   // 当前拥有这条栏的实例（最近获得焦点的那个）
static int           g_barUsers = 0;         // 已激活的实例数
static volatile LONG g_docFocusCount = 0;    // 正处在"有文档焦点"状态的实例数（0 = 焦点不在任何输入框里）
static volatile LONG g_docFocusEvents = 0;   // 收到过多少次焦点变化（0 = 这个宿主从不汇报，那就别拿它当依据）

// 焦点是否落在某个能输入的控件上：决定悬浮栏要不要显示（定义在 CTextService 之后）
static void UpdateDocFocusForBar(CTextService* owner);

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
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM, BOOL* pfEaten) override {
        if (pfEaten) *pfEaten = FALSE;
        LogLine(L"OnKeyUp vk=0x%02X shiftDown=%d shiftUsed=%d comp=%p code=%hs",
                (unsigned)wParam, (int)m_shiftDown, (int)m_shiftUsed, (void*)m_comp,
                m_composer.Code().c_str());
        // 单击 Shift（按下期间没按别的键）：
        //   左 Shift → 中/英切换（组码中按：先把编码原样上屏，再切英文）；
        //   右 Shift → 简/繁切换（组码中按：候选框里的字立刻跟着换）；
        //   两个 Shift 一起按 → 两个动作都执行。
        const UINT vk = (UINT)wParam;
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
            // 抬起时 GetKeyState 可能已经复位，所以以按下时记录的 side 为准
            const int side = (vk == VK_LSHIFT) ? 1 : (vk == VK_RSHIFT) ? 2 : m_shiftSide;
            // 另一侧是否还按着：按下时记录（m_shiftBoth）+ 此刻的实时状态，两者取或，
            // 这样即便只收到一次 KeyDown（或只有 KeyUp），也能判断出"两个 Shift 一起按"。
            const bool otherDown = (side == 1) ? ((GetKeyState(VK_RSHIFT) & 0x8000) != 0)
                                             : ((GetKeyState(VK_LSHIFT) & 0x8000) != 0);
            LogLine(L"shift tap: vk=0x%02X side=%d both=%d other=%d",
                    (unsigned)vk, side, (int)m_shiftBoth, (int)otherDown);
            if (m_shiftDown && !m_shiftUsed) {
                const bool both = m_shiftBoth || otherDown;
                if (both) {
                    ToggleTrad(pic);
                    ToggleEnglish(pic);
                    std::wstring s = m_english ? L"\u82f1" : L"\u4e2d";
                    s += m_tradEnabled ? L"\u7e41" : L"\u7b80";
                    FlashStatus(s.c_str());
                    LogLine(L"both shift: english=%d trad=%d", (int)m_english, (int)m_tradEnabled);
                } else if (side == 2) {
                    ToggleTrad(pic);
                    FlashStatus(m_tradEnabled ? L"\u7e41" : L"\u7b80");
                } else {
                    ToggleEnglish(pic);
                }
                m_shiftBoth = false;
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

    // 有正在进行的组码时，即使焦点报告离开也不能把栏收掉
    bool HasComposition() const { return m_comp != nullptr; }
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
    std::wstring    m_lastCommit;                  // 上一次上屏的内容
    std::vector<std::wstring> m_recentCommits;     // 最近 4 次上屏内容（Z 键候选用，旧的在前）
    bool            m_zPending = false;            // Z 按下后正在等用户选一项
    std::vector<std::wstring> m_zItems;            // Z 弹出的候选项（前 4 个历史 + 版本号）
    // Z 之后的拼音反查（拼音 -> 汉字 + 五笔编码）
    bool                      m_pyLookup = false;
    std::string               m_pyTyped;           // 已输入的拼音
    std::vector<std::wstring> m_pyChars;           // 当前页的候选（汉字/词）
    std::vector<std::wstring> m_pyCodes;           // 当前页对应的五笔全码
    std::vector<std::wstring> m_pyAll;             // 全部匹配（翻页用）
    std::vector<std::wstring> m_pyAllCodes;
    int                       m_pyPage = 0;
    bool            m_quickMode = false;       // 分号进入的"便捷输入"
    std::string     m_quickDigits;
    // 便捷输入里按"其他可打印键"进入：原样记录，不限长，回车整段半角输出
    bool            m_rawMode = false;
    std::wstring    m_rawText;
    // 会话级"自动学到的词"：从连续上屏的单字里拼出来（2~4 字），只存在于本进程，
    // 候选里排在**最后**；只有用户真的选了它才写进 user.tsv，否则进程结束就没了。
    struct SessionWord { std::string code; std::wstring word; };
    std::vector<SessionWord> m_sessionWords;
    wchar_t         m_lastChar1 = 0;    // 最近上屏的单字
    wchar_t         m_lastChar2 = 0;    // 再往前一个
    wchar_t         m_lastChar3 = 0;    // 再往前一个（拼 4 字词用）
    size_t          m_candSessionStart = 0;   // display 里从这一项起是会话词
    std::vector<std::wstring> m_lastLearned;  // 最近一次上屏学到的词（退格时撤销）
    #define m_eng g_eng
    std::wstring    m_engTyped;                // 英文补全已输入的字母（保留实际大小写）
    bool            m_engActive = false;      // 英文单词补全模式（区别于自由英文 m_english）
    std::vector<std::wstring> m_quickForms;
    std::vector<char> m_candPinned;   // 与候选一一对应：1 = 人工锁定
    bool            m_letterLabels = false;   // 便捷输入模式下候选用 a. b. c.
    ITfLangBarItem* m_langBar = nullptr;   // 语言栏按钮（显示自定义图标）
    wubi::DigitPunctGate m_gate;   // 数字后单次半角逗号/句号

    // ---- 自定义词组（Ctrl+=）；自定义短语走独立窗口 panda-phrase.exe ----
    bool                 m_customWordMode = false;
    std::string          m_customWord;         // 词组：逐字拼起来的 UTF-8
    std::vector<std::wstring> m_customChars;   // 词组：字序列（← 取字、Del 删首）
    std::vector<std::wstring> m_recentChars;    // 最近上屏的单字（← 依次回取）
    void BeginCustomWord(ITfContext* pic);
    void ExitCustom(ITfContext* pic, bool cancel);
    bool CustomKey(ITfContext* pic, UINT vk, LPARAM lp, bool shift, BOOL* pfEaten);
    void RefreshCustomPrompt();
    void SaveCustomWord(ITfContext* pic);
    void RememberCommitted(const std::wstring& text);
    void TrimRecentCharsByDocument(ITfContext* pic);   // 剔除已被删掉的字（宿主能报光标位置时）
    void LaunchPhraseWindow();                 // 启动独立窗口录自定义短语
    void MaybeReloadUserWords();               // 外部新增的词（窗口写的）热加载
    ULONGLONG m_userWordsStamp = 0;
    ULONGLONG m_userWordsChecked = 0;

    void ToggleTrad(ITfContext* pic);      // 简/繁切换（Ctrl+Shift+T 与单击右 Shift 共用）
    void ToggleEnglish(ITfContext* pic);   // 中/英切换（单击左 Shift）
    void ShowIdleStatus(ITfContext* pic);  // 空闲时把"中/英·简/繁"贴在光标附近（合并后的悬浮窗）
    void ShowStatusMenu(POINT pt);         // 合并窗口的右键菜单（原来悬浮栏那套）
    // 中英文切换（单击左 Shift）与状态提示
    bool m_english   = false;
    bool m_docFocused = false;   // 当前是否有"文档焦点"（TSF 的 OnSetFocus 非空）
    // 顶字上屏（四码满了再打第五个键）时，旧的组合串会在稍后才回调 OnCompositionTerminated；
    // 那次回调要保留 composer 里的新码、不要清状态，否则第五个键看起来被吞掉了
    bool m_keepCodeOnTerminate = false;
    bool m_shiftDown = false;
    bool m_shiftUsed = false;
    std::wstring m_iconPath;   // 当前主题图标路径（合并窗状态条左侧那个）
    int  m_shiftSide = 1;      // 1 = 左 Shift，2 = 右 Shift（按下时记录）
    bool m_shiftBoth = false;  // 本次单击期间两个 Shift 都按下了
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
    void LearnEnglishWord(const std::wstring& w, bool hadTrailingSpace);   // 自动学习英文单词
    void NoteCommittedForLearning(const std::wstring& text);   // 记连续上屏的单字，拼会话词
    bool AddSessionWord(const std::wstring& w);
    bool PickSessionCandidate(ITfContext* pic, int displayIdx); // 选了会话词 -> 正式入词库
    bool HasSessionWordForCode(const std::string& code) const;
    void UndoLastLearned();   // 退格/删除：把刚学的词撤掉，并把字符退出学习链
    void EngCancel(ITfContext* pic);
    void ShowCandidateList(const std::vector<std::wstring>& display);
    HRESULT UpdateCompositionTextWith(ITfContext* pic, const std::wstring& text);
    void BeginRepeat();
    void PyLookupRefresh(ITfContext* pic, bool resetPage = true);   // 按 m_pyTyped 刷新候选
    bool PyLookupFlipPage(ITfContext* pic, int delta);              // 翻页
    void PyLookupEnd(ITfContext* pic);              // 结束反查（清状态、收候选窗）
    void RememberRecentCommit(const std::wstring& text);   // 记入"最近 4 次上屏"
    void ApplySettings();                                   // 把 m_settings 套到候选窗/词库/选项上
public:
    void ReloadSettingsNow();                               // 重新读 config.ini 并立刻生效
private:
    void BeginQuick();
    void RefreshQuickCandidates(ITfContext* pic);
    void ExitQuick(ITfContext* pic, bool cancel);
    void RefreshRawHint(ITfContext* pic);       // 原样记录模式的提示
    void ExitRaw(ITfContext* pic, bool commit); // 结束原样记录（commit=回车输出）
    TfGuidAtom      m_attrAtom = 0;        // 组码显示属性 atom
    bool            m_attrTried = false;
    CCandidateWindow m_candWnd;
    wubi::Punctuation     m_punc;
    wubi::Transliteration m_trad;
    bool  m_tradEnabled = false;

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

// 设置窗口保存后发消息过来：这里把"下次按键时重载"标脏，
// 并立刻通知当前拥有状态栏的那个实例（如果在本进程）马上生效。
static volatile LONG g_reloadPending = 0;
static void RequestSettingsReload();   // 定义见下（需要 CTextService 完整类型）
static void BroadcastSettingsReload(); // 通知本机所有输入法窗口重读配置

// 空闲状态：把"中/英 + 简/繁"贴在光标下方（合并后的悬浮窗；原来是一条独立的悬浮栏）
void CTextService::ShowIdleStatus(ITfContext* pic) {
    (void)pic;
    if (m_comp && !m_composer.Code().empty()) return;   // 正在组码：窗口归候选列表用
    RECT caret = {};
    bool hasCaret = QueryCaretRect(caret);
    if (!hasCaret && m_hasCaret) { caret = m_lastCaret; hasCaret = true; }
    if (!hasCaret) { if (!m_statusShown) m_candWnd.Hide(); return; }
    m_hasCaret = true;
    m_lastCaret = caret;

    m_candWnd.SetStatusMode(true);
    m_candWnd.SetPlain(true);
    m_candWnd.SetLetterLabels(false);
    m_candWnd.SetCode(std::wstring());
    m_candWnd.SetItems({ L"\u4e2d\u6587" }, false);   // 占位：实际绘制在 PaintStatus 里
    m_candWnd.SetStatusInfo(m_english, m_tradEnabled, m_iconPath);
    m_candWnd.SetOnStatusClick([this](int zone) {
        if (zone == 3)      OnBarClick(2);          // ⚙ 设置
        else if (zone == 1) OnBarClick(0);          // 中 / 英
        else if (zone == 2) OnBarClick(1);          // 简 / 繁
        else { POINT p; GetCursorPos(&p); ShowStatusMenu(p); }   // 点图标 = 弹菜单
    });
    m_candWnd.ShowAt(caret);
    m_statusShown = false;
    LogLine(L"idle status: en=%d trad=%d at %ld,%ld", (int)m_english, (int)m_tradEnabled, (long)caret.left, (long)caret.bottom);
}

// 合并窗口的右键菜单：内容与原来的悬浮栏一致（中/英、简/繁、主题、设置、版本）
void CTextService::ShowStatusMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (m_english ? 0 : MF_CHECKED), 1, L"\u4e2d\u6587\u8f93\u5165");
    AppendMenuW(menu, MF_STRING | (m_tradEnabled ? MF_CHECKED : 0), 2, L"\u7b80\u7e41\u8f93\u51fa");
    HMENU themeMenu = CreatePopupMenu();
    static const wchar_t* kThemeNames[4] = { L"\u718a\u732b", L"\u72f8\u82b1\u732b", L"\u4e2d\u534e\u7530\u56ed\u72ac", L"\u4e39\u9876\u9e64" };
    const bool isCrane = (m_settings.Theme == L"crane");
    const bool isDog   = (m_settings.Theme == L"dog");
    const bool isCat   = (m_settings.Theme == L"cat");
    const int curTheme = isCrane ? 3 : (isDog ? 2 : (isCat ? 1 : 0));
    for (int i = 0; i < 4; ++i) {
        AppendMenuW(themeMenu, MF_STRING | (curTheme == i ? MF_CHECKED : 0), 100 + i, kThemeNames[i]);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)themeMenu, L"\u4e3b\u9898");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"\u8bbe\u7f6e...");
    AppendMenuW(menu, MF_STRING, 4, PANDA_WUBI_VERSION_FULL);

    HWND owner = m_candWnd.Hwnd();
    HWND prevFg = GetForegroundWindow();
    const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(menu);
    if (prevFg) SetForegroundWindow(prevFg);
    if (cmd >= 1 && cmd <= 4) OnBarClick(cmd - 1);
    else if (cmd >= 100 && cmd <= 103) OnBarClick(cmd);
}

void CTextService::BarSync() {
    if (g_barOwner == this)
        SharedBar().SetState(m_english, m_tradEnabled);
}

void CTextService::OnBarClick(int idx) {
    // 右键菜单里的"主题"子菜单：命令 id 100+n（顺序：熊猫/狸花猫/田园犬/丹顶鹤）
    if (idx >= 100) {
        static const wchar_t* kThemes[4] = { L"panda", L"cat", L"dog", L"crane" };
        const int t = idx - 100;
        if (t >= 0 && t < 4) {
            m_settings.Theme = kThemes[t];
            wubi::SaveSettings(GetConfigPath(), m_settings);
            ApplySettings();                       // 本进程立刻换图标/装饰图
            BroadcastSettingsReload();             // 通知其它进程的悬浮栏/候选窗也跟上
            LogLine(L"theme switched from bar menu: %s", m_settings.Theme.c_str());
        }
        return;
    }
    switch (idx) {
    case 0: m_english = !m_english; SetInputModeCompartments(!m_english); break;   // 中/英
    case 1: m_tradEnabled = !m_tradEnabled; m_settings.TradChinese = m_tradEnabled ? 1 : 0;
            wubi::SaveSettings(GetConfigPath(), m_settings);
            // 组字中/反查中从栏上切换简繁：候选框里的字也要立刻换（和 Ctrl+Shift+T 一致）
            if (m_pyLookup) PyLookupRefresh(nullptr, false);
            else if (!m_composer.Code().empty()) RefreshCandidates(nullptr);
            break;                                                                // 简/繁
    case 2: LaunchSettingsWindow(nullptr); break;                                 // 设置
    case 3: {
        // 关于
        wchar_t msg[512];
        swprintf_s(msg,
            L"熊猫五笔 v" PANDA_WUBI_VERSION L"\n\n构建: %hs %hs\n词库: %ls\n词条: %lld\n\n中/英: 单击左 Shift 或点状态栏\n简繁: 单击右 Shift（或 Ctrl+Shift+T）",
            __DATE__, __TIME__,
            m_settings.DictFile.c_str(), (long long)g_dict.Get().EntryCount());
        MessageBoxW(nullptr, msg, L"关于 Panda五笔", MB_OK | MB_ICONINFORMATION);
        break;
    }
    default: LaunchSettingsWindow(nullptr); break;                              // 设置
    }
    BarSync();
    LogLine(L"bar click idx=%d en=%d trad=%d", idx,
            (int)m_english, (int)m_tradEnabled);
}

STDMETHODIMP CTextService::OnSetThreadFocus() {
    {
        BarGuardScope guard;
        g_barOwner = this;
        BarSync();
        SharedBar().Show(false);      // 合并后不再显示独立悬浮栏
        ShowIdleStatus(nullptr);      // 状态信息贴到光标附近（同一个窗口）
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
        m_candWnd.Hide();      // 合并窗（状态徽标）也一起收起
    }
    LogLine(L"OnKillThreadFocus: tid=%u owned=%d", m_tid, (int)owned);
    return S_OK;
}

bool CTextService::IsCodeKey(UINT vk, wchar_t* out) {
    // 'z' 也收：自定义短语的编码常见 dz / sfz 这类拼音缩写（以 z 打头仍归"重复上次上屏"，那才是已知限制）
    if (vk >= 'A' && vk <= 'Z') { *out = (wchar_t)(vk - 'A' + 'a'); return true; }
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
    // 候选窗外观众（字体/字号/背景色/文字色/选中色/边框/凹陷）
    // 注意：设置里存的是 0xRRGGBB（网页习惯），COLORREF 是 0x00BBGGRR，必须换序，
    // 否则红蓝会颠倒（以前只设了个近白的背景，所以没暴露出来）。
    ApplySettings();

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
    // 用户自己"打进去过"的英文词（自动学习），存在数据目录里
    const long long nu = g_eng.Load(wubi::GetDataDir() + L"\\eng_user.txt");
    LogLine(L"eng user words: %lld", nu > 0 ? nu : 0);
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
            IUnknown* unk = CreateLangBarItem(CLSID_WubiTsf, kLangBarItemGuid, g_hInst,
                                             (m_settings.Theme == L"crane") ? 9 :
                                              (m_settings.Theme == L"dog") ? 7 :
                                              ((m_settings.Theme == L"cat") ? 5 : 1));   // 1=熊猫 5=狸花猫 7=田园犬 9=丹顶鹤
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
    LogLine(L"app config: exe=%ls trad=%d vertical=%d",
            HostAppName().c_str(), (int)m_tradEnabled, m_settings.CandVertical);
    LogLine(L"cand style: cfg=%s face=%s pt=%d bg=#%06X text=#%06X border=#%06X",
            GetConfigPath().c_str(), m_settings.CandFontFace.c_str(), m_settings.CandFontPt,
            m_settings.CandBgColor & 0xFFFFFF, m_settings.CandTextColor & 0xFFFFFF,
            m_settings.CandBorderColor & 0xFFFFFF);
    // 自绘悬浮状态栏：进程内唯一（见上方 SharedBar 注释），点击转发给当前拥有者
    {
        BarGuardScope guard;
        ++g_barUsers;
        if (g_barUsers == 1) {
            SharedBar().Create(g_hInst, GetConfigPath(), [](int idx) {
                if (g_barOwner) g_barOwner->OnBarClick(idx);
            }, [] { RequestSettingsReload(); });
        }
        g_barOwner = this;
        BarSync();
        SharedBar().Show(false);      // 合并后不再显示独立悬浮栏
        ShowIdleStatus(nullptr);      // 状态信息贴到光标附近（同一个窗口）
    }
    LogLine(L"bar: users=%d owner=%p visible=%d", g_barUsers, (void*)this,
            (int)SharedBar().Visible());
    LogLine(L"ActivateEx: tid=%u trad=%d candCount=%d",
            m_tid, (int)m_tradEnabled, m_candCount);
    g_service = this;
    return S_OK;
}

STDMETHODIMP CTextService::Deactivate() {
    // 归还这条栏：最后一个实例走人时才真正销毁窗口
    int usersLeft = 0;
    {
        BarGuardScope guard;
        if (g_barOwner == this) g_barOwner = nullptr;
        if (m_docFocused) {
        m_docFocused = false;
        InterlockedIncrement(&g_docFocusEvents);
        if (g_docFocusCount > 0) InterlockedDecrement(&g_docFocusCount);
    }
    if (g_barUsers > 0) --g_barUsers;
        usersLeft = g_barUsers;
        if (g_barUsers == 0) {
            // 只隐藏，不销毁窗口：Deactivate 很可能跑在另一个线程上，那儿 DestroyWindow 会失败，
            // 旧代码把 hwnd_ 置空 + 把 USERDATA 清零，结果窗口既没销毁、也不再响应计时器，
            // 变成永久的"幽灵栏"（用户看到好几个悬浮窗就是这么来的）。
            // 窗口留着还有好处：它的 500ms 计时器会继续在前台自检，切到后台就自动藏起来。
            SharedBar().Show(false);
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

// 焦点是否落在某个能输入的控件上：决定悬浮栏要不要显示
static void UpdateDocFocusForBar(CTextService* owner) {
    // 宿主一次焦点事件都没报过：保持"显示"的默认行为，别把栏藏没了
    if (g_docFocusEvents == 0) return;
    const bool focused = (g_docFocusCount > 0) || (owner && owner->HasComposition());
    SharedBar().SetDocFocus(focused);
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

    // 文档焦点进出：维护进程级计数，让悬浮栏"焦点不在输入框上就收起"
    {
        const bool now = (pimdFocus != nullptr);
        if (now != m_docFocused) {
            m_docFocused = now;
            InterlockedIncrement(&g_docFocusEvents);
            if (now) InterlockedIncrement(&g_docFocusCount);
            else     InterlockedDecrement(&g_docFocusCount);
            UpdateDocFocusForBar(this);
            LogLine(L"doc focus: %d (count=%ld)", (int)now, (long)g_docFocusCount);
        }
    }
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
    m_keepCodeOnTerminate = false;
    m_code.clear();
    m_composer.Reset();
    m_candWnd.Hide();
    // 焦点丢失时把自定义词组模式收干净，避免下次输入还停在"等回车保存"的状态
    if (m_customWordMode) {
        m_customWordMode = false;
        m_customChars.clear();
        m_customWord.clear();
        LogLine(L"Reset: custom word mode cleared");
    }
    if (m_rawMode) {
        m_rawMode = false;
        m_rawText.clear();
    }
    if (m_pyLookup) {
        m_pyLookup = false;
        m_pyTyped.clear();
        m_pyChars.clear();
        m_pyCodes.clear();
    }
    if (m_compHost) { m_compHost->Release(); m_compHost = nullptr; }
    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_comp) { m_comp->Release(); m_comp = nullptr; }
}

STDMETHODIMP CTextService::OnCompositionTerminated(TfEditCookie, ITfComposition* pComposition) {
    LogLine(L"OnCompositionTerminated: code=%hs comp=%p terminated=%p",
            m_composer.Code().c_str(), (void*)m_comp, (void*)pComposition);
    // 顶字上屏（四码满了又打第五个键）会"先提交旧的组合串、再开一个新的"。
    // 旧串的终止回调是**晚一步**才到的，那时 m_comp 已经指向新串了 ——
    // 老代码不分青红皂白把 m_comp 释放掉，等于把刚开的那个新组合串掐死，
    // 于是第五个键看起来"被吞掉了"（候选窗也不出来）。
    if (m_comp) { m_comp->Release(); m_comp = nullptr; }
    if (m_keepCodeOnTerminate) {
        // 这是"顶字上屏"结束旧组合串的回调：composer 里已经装好了新码，
        // 不要 Reset、不要把候选窗收掉——等用户打下一键再开新组合串。
        m_keepCodeOnTerminate = false;
        LogLine(L"OnCompositionTerminated: keep code=%hs after auto-commit", m_composer.Code().c_str());
        return S_OK;
    }
    m_code.clear();
    m_composer.Reset();
    m_candWnd.Hide();
    return S_OK;
}

HRESULT CTextService::DoEdit(ITfContext* pic, std::function<HRESULT(TfEditCookie)> fn) {
    CEditSession* s = new CEditSession(std::move(fn));
    // 打一行"进入编辑会话"再调用：宿主卡死（用户只能强杀进程）时，日志会停在这一行，
    // 一眼就能看出"卡在 RequestEditSession（宿主没响应编辑请求）"，而不是我们自己的逻辑。
    LARGE_INTEGER freq = {}, t0 = {}, t1 = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    LogLine(L"DoEdit: begin (RequestEditSession SYNC)");
    HRESULT hrSession = E_FAIL;
    HRESULT hr = pic->RequestEditSession(m_tid, s, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
    QueryPerformanceCounter(&t1);
    s->Release();
    const double ms = (freq.QuadPart > 0)
        ? (t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;
    if (FAILED(hr) || FAILED(hrSession) || ms >= 300.0) {
        LogLine(L"DoEdit: done hr=0x%08lX session=0x%08lX %.1f ms", 
                (unsigned long)hr, (unsigned long)hrSession, ms);
    }
    if (FAILED(hr)) return hr;
    return hrSession;
}

// 关键：StartComposition 与 SetText 必须在**同一个编辑会话**内完成。
// 若分成两个会话，部分应用/TSF 会在两个会话之间终止这个空组合串，
// 表现为“每按一个键都重新开始组码”（实测日志里 comp 反复重建）。
HRESULT CTextService::StartComposition(ITfContext* pic, const std::wstring& text) {
    if (m_comp) return S_OK;
    if (m_pyLookup) {
        m_pyLookup = false;
        m_pyTyped.clear();
        m_pyChars.clear();
        m_pyCodes.clear();
    }
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
    if (FAILED(hr)) LogLine(L"StartComposition failed hr=0x%08lX", (unsigned long)hr);

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
    // 组码字母不上屏：组合串始终留空，编码只在候选窗顶部小框里显示。
    // 这样上屏前文档里不会先出现 "wqvb" 再被汉字替换。
    return UpdateCompositionTextWith(pic, std::wstring());
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
    if (SUCCEEDED(hr)) {
        RememberCommitted(text);        // 供"自定义词组"用 ← 回取
        RememberRecentCommit(text);     // 供 Z 键候选用
    }
    if (FAILED(hr)) LogLine(L"CommitString failed hr=0x%08lX len=%zu", (unsigned long)hr, text.size());
    return hr;
}

HRESULT CTextService::CommitText(ITfContext* pic, const std::wstring& textIn,
                                 const std::string& learnCode, bool resetComposer) {
    LogLine(L"CommitText: comp=%p textlen=%zu text=%s", (void*)m_comp, textIn.size(), textIn.c_str());
    if (!m_comp) return E_FAIL;
    std::wstring w = textIn;
    LoadDataOnce();
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

    if (SUCCEEDED(hr)) RememberRecentCommit(w);   // 记住最近几次上屏（Z 键候选）
    if (SUCCEEDED(hr)) RememberCommitted(w);             // 供"自定义词组"用 ← 回取
    if (SUCCEEDED(hr)) NoteCommittedForLearning(w);        // 连续单字 -> 会话级自动学词
    if (resetComposer) m_composer.Reset();
    m_code.clear();
    m_candWnd.Hide();
    if (m_comp) { m_comp->Release(); m_comp = nullptr; }
    ShowIdleStatus(nullptr);   // 上屏完回到"只显示状态"的小窗
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
            if (m_context && idx >= (int)m_composer.Candidates().size() && PickSessionCandidate(m_context, idx)) return;
            auto o = m_composer.SelectDigit(idx + 1);
            if (o.act == wubi::Composer::Act::Committed && m_context) {
                CommitText(m_context, o.commit, o.commitCode, true);
            }
        });
    }
    m_composer.SetLimit((size_t)m_candCount);
}

// 把当前 m_settings 套用到候选窗外观、简繁开关、候选个数、词库排序等。
// 激活时调用一次；config.ini 变过的话，MaybeReloadUserWords() 里会再调一次（即时生效）。
// 系统输入指示器的图标来自注册表里的 IconFile：改主题后写一次（HKCU 优先，不需要管理员）。
// 说明：shell 只在加载语言配置时读这个值，所以通常要重启 explorer 或重新登录才看到变化。
// 通知本机所有输入法窗口重读配置（和设置窗口保存时做的事一样）
static void BroadcastSettingsReload() {
    const UINT msg = PandaReloadMessage();
    const wchar_t* classes[] = { L"PandaWubiStatusBar", L"PandaWubiCandidateWnd" };
    for (const wchar_t* cls : classes) {
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, cls, nullptr)) != nullptr) {
            PostMessageW(h, msg, 0, 0);
        }
    }
}

static void UpdateIndicatorIconFile(const std::wstring& icoName) {
    wchar_t dll[MAX_PATH] = { 0 };
    GetModuleFileNameW(g_hInst, dll, ARRAYSIZE(dll) - 1);
    std::wstring dir(dll);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring dllIco = dir.substr(0, slash) + L"\\" + icoName;

    // 图标必须放在**用户目录**里，输入指示器才认（实测：放在 Program Files 下会显示成汉字）。
    // 装到 Program Files 之后 DLL 目录就是只读的程序目录，所以这里优先用
    // %LOCALAPPDATA%\PandaWubi\<ico>，没有就从 DLL 目录复制一份过去。
    std::wstring path;
    wchar_t base[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, ARRAYSIZE(base)) > 0) {
        const std::wstring userDir = std::wstring(base) + L"\\PandaWubi";
        const std::wstring userIco = userDir + L"\\" + icoName;
        if (GetFileAttributesW(userIco.c_str()) == INVALID_FILE_ATTRIBUTES) {
            CreateDirectoryW(userDir.c_str(), nullptr);
            if (GetFileAttributesW(dllIco.c_str()) != INVALID_FILE_ATTRIBUTES) {
                // 目标可能是只读的（以前装过/被别的工具改成只读），先清属性再拷，否则静默失败
                SetFileAttributesW(userIco.c_str(), FILE_ATTRIBUTE_NORMAL);
                CopyFileW(dllIco.c_str(), userIco.c_str(), FALSE);
            }
        }
        if (GetFileAttributesW(userIco.c_str()) != INVALID_FILE_ATTRIBUTES) path = userIco;
    }
    if (path.empty()) path = dllIco;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    const wchar_t* kProfile =
        L"Software\\Microsoft\\CTF\\TIP\\{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}"
        L"\\LanguageProfile\\0x00000804\\{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}";
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kProfile, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"IconFile", 0, REG_SZ, (const BYTE*)path.c_str(),
                   (DWORD)((path.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

void CTextService::ApplySettings() {
    // 设置里存的是 0xRRGGBB（网页习惯），COLORREF 是 0x00BBGGRR，必须换序
    auto toRef = [](int v) { return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); };
    const bool themeCrane = (m_settings.Theme == L"crane");

    // 丹顶鹤主题：窗口色调跟着那张风景图走（淡薄荷天 + 墨绿字 + 青灰边）。
    // 只有在用户没改过颜色（还是出厂默认）时才接管，改过就完全尊重用户设置。
    const bool themeCat = (m_settings.Theme == L"cat");
    const bool colorsAreDefault = (m_settings.CandBgColor == 0xF7F9FC &&
                                   m_settings.CandTextColor == 0x202028 &&
                                   m_settings.CandBorderColor == 0xA8CDA8);
    const bool themePanda = (m_settings.Theme != L"cat" && m_settings.Theme != L"dog" && m_settings.Theme != L"crane");
    const bool themeDog = (m_settings.Theme == L"dog");
    const bool useThemeColors = colorsAreDefault && (themeCrane || themeCat || themePanda || themeDog);
    COLORREF bgRef = toRef(m_settings.CandBgColor);
    COLORREF txRef = toRef(m_settings.CandTextColor);
    COLORREF bdRef = toRef(m_settings.CandBorderColor);
    if (useThemeColors) {
        if (themeCrane)      { bgRef = RGB(0xD3, 0xE9, 0xE6); txRef = RGB(0x24, 0x36, 0x32); bdRef = RGB(0x8F, 0xB8, 0xB0); }
        else if (themeCat)   { bgRef = RGB(0xE8, 0xF3, 0xFC); txRef = RGB(0x25, 0x30, 0x29); bdRef = RGB(0xA5, 0xC3, 0x9E); }
        else if (themeDog)   { bgRef = RGB(0xE9, 0xF6, 0xFF); txRef = RGB(0x2A, 0x33, 0x24); bdRef = RGB(0xA8, 0xC0, 0x8E); }
        else                 { bgRef = RGB(0xF2, 0xF6, 0xEC); txRef = RGB(0x24, 0x33, 0x26); bdRef = RGB(0xA8, 0xC4, 0x9A); }
    }
    m_candWnd.SetStyle(m_settings.CandFontPt, bgRef,
                       m_settings.CandInset != 0,
                       m_settings.CandFontFace, txRef, bdRef);

    m_tradEnabled = m_settings.TradChinese != 0;
    m_candCount   = m_settings.CandCount;
    m_composer.SetLimit((size_t)m_candCount);

    // 主题：panda（默认）/ cat（狸花猫）/ dog（中华田园犬）/ crane（丹顶鹤）
    const bool cat   = (m_settings.Theme == L"cat");
    const bool dog   = (m_settings.Theme == L"dog");
    const bool crane = (m_settings.Theme == L"crane");
    const wchar_t* themeIcon = crane ? L"crane.ico"
                             : dog   ? L"dog.ico"
                             : cat   ? L"cat.ico" : L"panda.ico";
    // 四套主题现在都用"整幅图当窗口背景"（不抠图），不再画角落小图：
    //   RCDATA 10=鹤 11=猫扑蝴蝶 12=熊猫 13=田园犬
    const int themeBandId = crane ? 10 : (cat ? 11 : (dog ? 13 : 12));
    m_candWnd.SetBottomBand(themeBandId, 130);
    m_candWnd.SetArtId(0);                                      // 不再用角落小图
    SharedBar().SetIconFile(themeIcon);                         // 悬浮栏最左边的 Logo
        // 合并窗状态条左侧图标：优先 %LOCALAPPDATA%\PandaWubi\<主题>.ico，退路是 DLL 同目录
        {
            wchar_t base[MAX_PATH] = { 0 };
            std::wstring cand;
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, ARRAYSIZE(base)) > 0) {
                cand = std::wstring(base) + L"\\PandaWubi\\" + themeIcon;
            }
            if (cand.empty() || GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) {
                wchar_t mod[MAX_PATH] = { 0 };
                if (GetModuleFileNameW(g_hInst, mod, ARRAYSIZE(mod)) > 0) {
                    std::wstring d(mod);
                    const size_t sl = d.find_last_of(L"\\/");
                    if (sl != std::wstring::npos) { d.erase(sl + 1); cand = d + themeIcon; }
                }
            }
            m_iconPath = cand;
            LogLine(L"status icon: %s (exists=%d)", m_iconPath.c_str(),
                    (int)(GetFileAttributesW(m_iconPath.c_str()) != INVALID_FILE_ATTRIBUTES));
        }
    SharedBar().SetThemeIndex(crane ? 3 : (dog ? 2 : (cat ? 1 : 0)));   // 右键菜单里打勾
    LogLine(L"theme: %s (band=%d, icon=%s)", m_settings.Theme.c_str(), themeBandId, themeIcon);
    UpdateIndicatorIconFile(themeIcon);   // 系统输入指示器的图标（需重新登录/重启 explorer 才刷新）
    BarSync();
}

void CTextService::ReloadSettingsNow() {
    m_settings = wubi::LoadSettings(GetConfigPath(), HostAppName());
    ApplySettings();
    if (m_comp && m_context) RefreshCandidates(m_context);   // 正在组码就顺手刷新候选
    LogLine(L"settings reloaded: trad=%d candCount=%d vertical=%d font=%s %dpt",
            (int)m_tradEnabled, m_candCount, m_settings.CandVertical,
            m_settings.CandFontFace.c_str(), m_settings.CandFontPt);
}

void RequestSettingsReload() {
    InterlockedExchange(&g_reloadPending, 1);
    BarGuardScope guard;
    if (g_barOwner) g_barOwner->ReloadSettingsNow();   // 本进程当前活跃的实例立刻生效
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

// 最近 4 次上屏的内容（Z 键候选用；连续重复的只记一次）
// 是不是标点/符号（Z 键重复和自定义词组都不算标点，而且词组素材不跨标点）
static bool IsPunctChar(wchar_t c) {
    if (c < 0x20) return false;
    if (c >= 0x21 && c <= 0x2F) return true;   // ! " # $ % & ' ( ) * + , - . /
    if (c >= 0x3A && c <= 0x40) return true;   // : ; < = > ? @
    if (c >= 0x5B && c <= 0x60) return true;   // [ \ ] ^ _ `
    if (c >= 0x7B && c <= 0x7E) return true;   // { | } ~
    if (c >= 0x00A1 && c <= 0x00BF) return true;   // ¡ … ¿
    if (c >= 0x2010 && c <= 0x2027) return true;   // ‐ – — ‘ ’ “ ” … ‧
    if (c >= 0x3000 && c <= 0x303F) return true;   // 　、。〈〉《》「」『』【】etc.
    if (c >= 0xFE10 && c <= 0xFE1F) return true;   // 竖排标点
    if (c >= 0xFE30 && c <= 0xFE4F) return true;   // 兼容形式
    if (c >= 0xFF01 && c <= 0xFF20) return true;   // ！＂＃…＠
    if (c >= 0xFF3B && c <= 0xFF40) return true;   // ［＼］＾＿｀
    if (c >= 0xFF5B && c <= 0xFF65) return true;   // ｛｜｝～｟｠｡｢…･
    return false;
}

static bool IsPunctText(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s) if (!IsPunctChar(c)) return false;
    return true;
}

void CTextService::RememberRecentCommit(const std::wstring& text) {
    if (text.empty()) return;
    // 纯标点不进"最近上屏"（Z 键重复只应该重复词/字，不该是「，」「。」）
    if (IsPunctText(text)) return;
    m_lastCommit = text;
    if (!m_recentCommits.empty() && m_recentCommits.back() == text) return;
    m_recentCommits.push_back(text);
    if (m_recentCommits.size() > 4) m_recentCommits.erase(m_recentCommits.begin());
}

// —— Z 反查：拼音 -> 汉字/词（候选里带五笔编码），支持翻页 ——
void CTextService::PyLookupRefresh(ITfContext* pic, bool resetPage) {
    if (resetPage) m_pyPage = 0;
    m_pyChars.clear();
    m_pyCodes.clear();
    if (!LoadPinyinTableOnce() || m_pyTyped.empty()) {
        m_candWnd.SetItems({}, true);
        m_candWnd.Hide();
        return;
    }
    LARGE_INTEGER t0 = {}, t1 = {}, freq = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    // 只在换拼音时重建全量匹配；翻页时复用
    if (resetPage || m_pyAll.empty()) {
        m_pyAll.clear();
        m_pyAllCodes.clear();
        const auto& tbl = PinyinTable();
        const size_t kMaxMatch = 400;
        // 表按拼音升序：二分定位前缀区间，只扫这一段（以前是 9.5 万行全表线性扫描）
        auto lower = std::lower_bound(tbl.begin(), tbl.end(), m_pyTyped,
                                      [](const PyEntry& e, const std::string& v) { return e.py < v; });
        for (auto it = lower; it != tbl.end() && m_pyAll.size() < kMaxMatch; ++it) {
            if (it->py.size() < m_pyTyped.size()) continue;
            if (it->py.compare(0, m_pyTyped.size(), m_pyTyped) != 0) break;   // 升序，后面都不是这个前缀
            std::wstring ch = it->ch;
            // 组字/反查时切了简繁：显示和上屏都用同一份转换结果
            if (m_tradEnabled && m_trad.Loaded()) ch = m_trad.ToTraditional(ch);
            m_pyAll.push_back(ch);
            m_pyAllCodes.push_back(it->code);
        }
    }

    const size_t pageSize = (size_t)(m_candCount > 0 ? m_candCount : 9);
    const size_t pages = (m_pyAll.size() + pageSize - 1) / pageSize;
    if (pages == 0) m_pyPage = 0;
    else if (m_pyPage >= (int)pages) m_pyPage = (int)pages - 1;

    std::vector<std::wstring> display;
    const size_t from = (size_t)m_pyPage * pageSize;
    for (size_t i = from; i < m_pyAll.size() && i < from + pageSize; ++i) {
        m_pyChars.push_back(m_pyAll[i]);
        m_pyCodes.push_back(m_pyAllCodes[i]);
        std::wstring text = m_pyAll[i];
        if (!m_pyAllCodes[i].empty()) { text += L' '; text += m_pyAllCodes[i]; }
        display.push_back(std::move(text));
    }

    m_candWnd.SetPlain(false);
    std::wstring head = AsciiToWide(m_pyTyped);
    if (pages > 1) {
        head += L"  ";
        head += std::to_wstring(m_pyPage + 1);
        head += L"/";
        head += std::to_wstring(pages);
    }
    m_candWnd.SetCode(head);
    m_candWnd.SetItems(display, m_settings.CandVertical != 0);
    if (QueryCaretRect(m_lastCaret)) m_hasCaret = true;
    m_candWnd.ShowAt(m_lastCaret);
    QueryPerformanceCounter(&t1);
    const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    LogLine(L"pinyin lookup: '%hs' -> %zu hit(s), page %d/%zu, %.2f ms (table %zu rows)",
            m_pyTyped.c_str(), m_pyAll.size(), m_pyPage + 1, pages > 0 ? pages : 1,
            ms, PinyinTable().size());
}

bool CTextService::PyLookupFlipPage(ITfContext* pic, int delta) {
    if (!m_pyLookup || m_pyAll.empty()) return false;
    const size_t pageSize = (size_t)(m_candCount > 0 ? m_candCount : 9);
    const size_t pages = (m_pyAll.size() + pageSize - 1) / pageSize;
    const int want = m_pyPage + delta;
    if (want < 0 || (size_t)want >= pages) return false;
    m_pyPage = want;
    PyLookupRefresh(pic, false);
    return true;
}
void CTextService::PyLookupEnd(ITfContext*) {
    m_pyLookup = false;
    m_pyTyped.clear();
    m_pyChars.clear();
    m_pyCodes.clear();
    m_pyAll.clear();
    m_pyAllCodes.clear();
    m_pyPage = 0;
    m_candWnd.SetCode(std::wstring());
    m_candWnd.Hide();
}
void CTextService::BeginRepeat() {
    if (m_lastCommit.empty()) return;
    m_zItems.clear();
    for (size_t i = 0; i < m_recentCommits.size(); ++i) {
        m_zItems.push_back(m_recentCommits[m_recentCommits.size() - 1 - i]);
    }
    m_zItems.push_back(PANDA_WUBI_VERSION_FULL);
    m_zPending = true;
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
    // 便捷输入有自己的组合串（数字），顶部小框不要再挂着上一个汉字的编码
    m_candWnd.SetCode(std::wstring());
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

// 全角 -> 半角（"回车后完整半角输出"）：FF01–FF5E 直接减 0xFEE0，再补几个常用标点
static std::wstring ToHalfWidth(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t c : in) {
        if (c >= 0xFF01 && c <= 0xFF5E) { out.push_back((wchar_t)(c - 0xFEE0)); continue; }
        switch (c) {
        case 0x3000: out.push_back(L' '); break;        // 全角空格
        case 0x3001: out += L","; break;                // 、
        case 0x3002: out += L"."; break;                // 。
        case 0x300A: out += L"<"; break;                // 《
        case 0x300B: out += L">"; break;                // 》
        case 0x300C: out += L"["; break;                // 「
        case 0x300D: out += L"]"; break;                // 」
        case 0x2018: case 0x2019: out += L"'"; break;   // ‘ ’
        case 0x201C: case 0x201D: out += L"\""; break;   // “ ”
        case 0x2026: out += L"..."; break;              // …
        case 0x2014: out += L"-"; break;                // —
        case 0x00B7: out += L"."; break;                // ·
        default: out.push_back(c); break;
        }
    }
    return out;
}

void CTextService::RefreshRawHint(ITfContext* pic) {
    (void)pic;
    // 提示里只显示前 48 个字符，但 m_rawText 本身不限长（提交的是完整内容）
    std::wstring shown = m_rawText;
    bool truncated = false;
    if (shown.size() > 48) { shown = shown.substr(0, 48); truncated = true; }
    std::vector<std::wstring> lines;
    lines.push_back(L"原样记录：" + shown + (truncated ? std::wstring(L"…") : std::wstring()));
    lines.push_back(L"继续输入任意字符（不限长）· 回车整段半角输出 · Backspace 删一个 · Esc 取消");
    m_letterLabels = false;
    ShowCandidateList(lines);
    LogLine(L"raw: %zu char(s)", m_rawText.size());
}

void CTextService::ExitRaw(ITfContext* pic, bool commit) {
    const std::wstring text = ToHalfWidth(m_rawText);
    m_rawMode = false;
    m_rawText.clear();
    m_candWnd.Hide();
    if (!commit || text.empty()) return;
    CommitString(pic, text);
    RememberRecentCommit(text);
    LogLine(L"raw commit: %zu char(s)", text.size());
}

void CTextService::ShowCandidateList(const std::vector<std::wstring>& display) {
    // 位置兜底：拿不到插入点坐标时退到鼠标位置。
    // 否则提示窗会跑到屏幕左上角(0,2)，用户看着像"根本没弹出来"，
    // 可输入法其实已经在等按键了（自定义词组 / 便捷输入都踩过这个坑）。
    {
        RECT probe = {};
        const bool hasProbe = QueryCaretRect(probe);
        if (!hasProbe || (probe.left == 0 && probe.top == 0)) {
            POINT pt = { 0, 0 };
            if (GetCursorPos(&pt) && (pt.x != 0 || pt.y != 0)) {
                probe.left = probe.right = pt.x;
                probe.top = probe.bottom = pt.y;
            }
        }
        if (probe.left != 0 || probe.top != 0) {
            m_lastCaret = probe;
            m_hasCaret = true;
        }
    }
    // 非组码的提示（便捷输入 / Z 重复 / 自定义词组 / 状态徽标）不该沿用上一次的组码，
    // 否则按分号进便捷输入时，框里还留着刚才那个字的编码。
    if (!m_comp) {
        if (m_candWnd.HasCode()) LogLine(L"hint list: cleared stale code");
        m_candWnd.SetCode(std::wstring());
    }
    // 注意：这里不能先 Hide()。组码阶段每按一键都会走这里，hide -> show 会让候选框闪一下，
    // 直接就地更新内容即可（需要隐藏时 ShowAt 自己会处理）。
    if (display.empty()) {
        // 还在组码但一个候选都没匹配到：也要留下顶部编码框，
        // 否则屏幕上一片空白、用户不知道输入法是否在工作。
        // （编码为空时 ShowAt 会自己隐藏，所以这里安全）
        m_candWnd.SetPlain(false);   // FlashStatus 会把"纯文本"打开（那里不画序号），别带到这里
        m_candWnd.SetLetterLabels(m_letterLabels);
        m_candWnd.SetItems(display, m_settings.CandVertical != 0);
        if (QueryCaretRect(m_lastCaret)) m_hasCaret = true;
        m_candWnd.ShowAt(m_lastCaret);
        return;
    }

    m_candWnd.SetPlain(false);   // 同上：候选列表一律带序号
    m_candWnd.SetLetterLabels(m_letterLabels);
    LogLine(L"show list: items=%zu caret=%ld,%ld hasCaret=%d",
            display.size(), m_lastCaret.left, m_lastCaret.top, (int)m_hasCaret);
    m_candWnd.SetItems(display, m_settings.CandVertical != 0);
    m_candWnd.SetPinned(m_candPinned);

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
    } else if (m_hasCaret) {
        // 没有 m_context（自定义词组 / Z 重复 / 便捷输入这些"非组码提示"就是这样），
        // 编辑会话拿不到光标；以前这里直接不 ShowAt —— 于是提示框不出现，
        // 可输入法已经进了那个模式，按键全被吞，表现特别诡异。
        // 用前面探测到的位置（光标 -> 鼠标）兜底显示。
        LogLine(L"show list: no context caret, fallback to %ld,%ld", m_lastCaret.left, m_lastCaret.top);
        m_candWnd.ShowAt(m_lastCaret);
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

static double NowMs() {
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return freq.QuadPart ? (double)c.QuadPart * 1000.0 / (double)freq.QuadPart : 0.0;
}

void CTextService::EngUpdate(ITfContext* pic) {
    if (m_engTyped.empty()) { EngCancel(pic); return; }
    const double t0 = NowMs();
    // 组码预览就显示用户实际打出来的大小写，不再一律小写
    if (m_comp) UpdateCompositionTextWith(pic, m_engTyped);
    else        StartComposition(pic, m_engTyped);
    const double t1 = NowMs();

    const auto words = m_eng.Suggest(EngPrefix(), (size_t)m_candCount);
    std::vector<std::wstring> disp;
    disp.reserve(words.size());
    for (const auto& w : words) disp.push_back(wubi::ApplyCase(AsciiToWide(w), m_engTyped));

    const double t2 = NowMs();
    m_candWnd.SetPlain(false);
    m_candWnd.SetCode(m_engTyped);   // 候选窗顶部小框显示"实际打出来的样子"（含大小写）
    m_candWnd.SetItems(disp, m_settings.CandVertical != 0);       // 跟随"候选竖排"设置（可能是空）
    if (QueryCaretRect(m_lastCaret)) m_hasCaret = true;           // 没记录时当场取，避免跑到屏幕左上角
    m_candWnd.ShowAt(m_lastCaret);   // 没匹配到单词也照样显示，只是只有顶部小框
    const double t3 = NowMs();
    LogLine(L"eng type %s -> %zu cands (comp=%.1fms suggest=%.1fms show=%.1fms)",
            m_engTyped.c_str(), disp.size(), t1 - t0, t2 - t1, t3 - t2);
}

void CTextService::EngType(ITfContext* pic, wchar_t ch) {
    if (m_engTyped.size() >= 32) EngCommit(pic, 0, false);
    m_engTyped.push_back(ch);
    EngUpdate(pic);
}

// 英文单词自动学习：用户打出来、词表里没有的串，落地到 <数据目录>\eng_user.txt，
// 下次就能补全。只收"看起来像词"的串：纯字母（允许 - '），长度 3..32。
void CTextService::LearnEnglishWord(const std::wstring& w, bool hadTrailingSpace) {
    std::wstring s = w;
    if (hadTrailingSpace && !s.empty() && s.back() == L' ') s.pop_back();
    if (s.size() < 3 || s.size() > 32) return;
    for (wchar_t c : s) {
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-' || c == L'\'')) return;
    }
    std::string word;
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return;
        word.resize((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &word[0], n, nullptr, nullptr);
    }
    if (word.empty() || g_eng.Has(word)) return;
    if (!g_eng.Add(word)) return;

    const std::wstring path = wubi::GetDataDir() + L"\\eng_user.txt";
    CreateDirectoryW(wubi::GetDataDir().c_str(), nullptr);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"ab") == 0 && fp) {
        std::string line = word;
        line += "\r\n";
        fwrite(line.data(), 1, line.size(), fp);
        fclose(fp);
    }
    LogLine(L"eng learn: %hs (total %zu)", word.c_str(), g_eng.Count());
}

void CTextService::EngCommit(ITfContext* pic, int idx, bool withSpace) {
    const auto words = m_eng.Suggest(EngPrefix(), (size_t)m_candCount);
    std::wstring out;
    const bool usedCandidate = (idx >= 0 && idx < (int)words.size());
    if (usedCandidate) out = wubi::ApplyCase(AsciiToWide(words[(size_t)idx]), m_engTyped);
    else               out = m_engTyped;   // 没候选：原样上屏，大小写照抄

    m_engTyped.clear();
    m_engActive = false;   // 上屏后回到中文模式
    if (m_comp) CancelComposition(pic);
    m_candWnd.SetCode(std::wstring());   // 清掉顶部小框，别让下次复用时残留
    m_candWnd.Hide();
    if (withSpace) out += L' ';
    CommitString(pic, out);
    LogLine(L"eng commit idx=%d len=%zu out=%s", idx, out.size(), out.c_str());
    // 学一个"词表里没有的英文串"（idx<0 表示没有候选、原样上屏那条路径）
    if (!usedCandidate) LearnEnglishWord(out, withSpace);   // 词表里没有 -> 学下来
}

void CTextService::EngCancel(ITfContext* pic) {
    m_engTyped.clear();
    m_engActive = false;
    if (m_comp) CancelComposition(pic);
    m_candWnd.SetCode(std::wstring());
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
    bool broke = false;
    for (wchar_t c : text) {
        if (c < 0x20) continue;                      // 空格/换行不进词组素材
        if (IsPunctChar(c)) {                        // 标点：不进素材，而且把素材切断
            if (!broke) { m_recentChars.clear(); broke = true; }
            continue;
        }
        m_recentChars.push_back(std::wstring(1, c));
        if (m_recentChars.size() > 64) m_recentChars.erase(m_recentChars.begin());
    }
}

void CTextService::LaunchPhraseWindow() {
    // 自定义短语在独立窗口里录（编码 + 短语两个输入框），不走输入法内联状态：
    // 输入法跑在宿主 UI 线程里，弹模态循环会跟 TSF 的焦点/生命周期打架。
    wchar_t mod[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(g_hInst, mod, ARRAYSIZE(mod)) == 0) { FlashStatus(L"找不到 panda-phrase.exe"); return; }
    std::wstring dir(mod);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    dir.erase(slash);
    const std::wstring exe = dir + L"\\panda-phrase.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FlashStatus(L"缺少 panda-phrase.exe（先构建一次）");
        LogLine(L"phrase window: missing %ls", exe.c_str());
        return;
    }
    const HINSTANCE h = ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    const bool ok = (INT_PTR)h > 32;
    LogLine(L"phrase window: launch %ls -> %s", exe.c_str(), ok ? L"ok" : L"failed");
    if (ok) FlashStatus(L"自定义短语：在弹出的窗口里填编码和短语");
}

// 外部（panda-phrase.exe 或别的宿主进程）往 user.tsv 追加了词，这里按修改时间热加载。
// 节流到 1 秒一次，避免每次按键都查文件。
void CTextService::MaybeReloadUserWords() {
    const ULONGLONG now = GetTickCount64();
    if (now - m_userWordsChecked < 1000) return;
    m_userWordsChecked = now;

    // 设置窗口保存后会给各个输入法窗口 PostMessage(PandaReloadMessage())：
    // 收到通知的实例已经就地套用过了，这里只是消费"待重载"标志，
    // 让没拿到通知的实例（别的线程/别的宿主进程）在下次按键时也跟上。
    if (InterlockedExchange(&g_reloadPending, 0)) ReloadSettingsNow();

    const std::wstring path = GetUserWordsPath();
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return;
    const ULONGLONG stamp = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
    if (stamp == m_userWordsStamp) return;
    m_userWordsStamp = stamp;

    const long long n = g_dict.Get().LoadUserWords(path);
    LogLine(L"user words reloaded: %lld line(s), total=%zu", n, g_dict.Get().UserWordCount());
}

// 用文档里"光标之前实际还留着的字"清洗最近上屏队列：
// 打了「工」「人」「了」，发现「了」打错按退格删掉，再加词组时队列里还留着「了」。
// 这里读光标前最多 64 个字，从后往前按子序列匹配——对得上的保留，对不上的就是被删掉的。
// 注意：宿主必须能把光标位置报出来（ITfRangeACP）；报 0 的宿主（例如 RichEdit/记事本）
// 拿不到可靠位置，就放弃清洗，让用户在词组模式里用 Del/Backspace 手动修。
void CTextService::TrimRecentCharsByDocument(ITfContext* pic) {
    if (!pic || m_recentChars.empty()) return;

    const LONG kMax = 64;
    std::wstring tail;
    CEditSession* s = new CEditSession([pic, &tail, kMax](TfEditCookie ec) -> HRESULT {
        TF_SELECTION sel = {};
        ULONG n = 0;
        if (FAILED(pic->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &n)) || n == 0 || !sel.range) return E_FAIL;
        ITfRangeACP* acp = nullptr;
        HRESULT hr = sel.range->QueryInterface(IID_ITfRangeACP, (void**)&acp);
        if (FAILED(hr) || !acp) { sel.range->Release(); return E_FAIL; }

        LONG start = 0, len = 0;
        hr = acp->GetExtent(&start, &len);
        const LONG caret = start + len;
        if (SUCCEEDED(hr) && caret > 0) {
            const LONG from = (caret > kMax) ? (caret - kMax) : 0;
            if (SUCCEEDED(acp->SetExtent(from, caret - from))) {
                wchar_t buf[128] = { 0 };
                ULONG got = 0;
                if (SUCCEEDED(sel.range->GetText(ec, 0, buf, (ULONG)kMax, &got))) tail.assign(buf, got);
            }
        }
        acp->Release();
        sel.range->Release();
        return S_OK;
    });
    HRESULT hrSession = E_FAIL;
    const HRESULT hrReq = pic->RequestEditSession(m_tid, s, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
    s->Release();
    if (FAILED(hrReq) || FAILED(hrSession)) {
        LogLine(L"recent chars: session req=0x%08lX session=0x%08lX", (unsigned long)hrReq, (unsigned long)hrSession);
        return;
    }
    if (tail.empty()) {
        LogLine(L"recent chars: no usable caret, skip trim (%zu kept)", m_recentChars.size());
        return;
    }

    std::vector<std::wstring> history = m_recentChars;
    const size_t dropped = wubi::TrimCharsByText(history, tail);
    if (dropped == 0) return;
    if (history.empty()) {   // 一个字都对不上：可能刚换了文档，保守不清空
        LogLine(L"recent chars: no match, keep %zu", m_recentChars.size());
        return;
    }
    m_recentChars.swap(history);
    LogLine(L"recent chars: trimmed %zu deleted char(s), %zu left", dropped, m_recentChars.size());
}

void CTextService::BeginCustomWord(ITfContext* pic) {
    TrimRecentCharsByDocument(pic);
    m_customWordMode = true;
    m_customChars.clear();
    m_customWord.clear();
    // 默认就把最近输入的两个字带进来（取法同 ←：从队尾取、插到最前，字序才是输入顺序），
    // 常见情形直接回车即可；要更长的词组继续按 ← 往前取。
    for (int i = 0; i < 2 && !m_recentChars.empty(); ++i) {
        m_customChars.insert(m_customChars.begin(), m_recentChars.back());
        m_recentChars.pop_back();
    }
    for (const auto& c : m_customChars) m_customWord += WideToUtf8(c);
    m_engActive = false;
    m_quickMode = false;
    m_zPending = false;
    m_zItems.clear();
    m_letterLabels = false;
    if (m_comp) CancelComposition(pic);
    m_candWnd.Hide();
    LogLine(L"custom word: begin, auto-took %zu char(s), %zu left in history",
            m_customChars.size(), m_recentChars.size());
    // 缓冲区空 = 没有可取的"最近上屏的字"：直接退出，别留一个既没提示框、回车也退不掉的状态
    if (m_customChars.empty()) {
        m_customWordMode = false;
        m_customWord.clear();
        FlashStatus(L"没有可取的最近输入：先打几个字再按 Ctrl+=");
        LogLine(L"custom word: empty buffer, exit immediately");
        return;
    }
    RefreshCustomPrompt();
}

// 中/英切换：单击左 Shift（组码中按 → 先把编码原样上屏，再切英文）
void CTextService::ToggleEnglish(ITfContext* pic) {
    const bool composing = (m_comp != nullptr) && (m_context != nullptr) &&
                           !m_composer.Code().empty();
    if (composing && pic) {
        // 用户在半路按 Shift，多半是发现"这一串本来就该是英文"，
        // 所以上屏的是**编码本身**（wqvb 就是 wqvb），而不是它对应的汉字。
        const std::string code = m_composer.Code();      // 先留住：取消组合会清空 composer
        const std::wstring raw(code.begin(), code.end());
        if (m_comp && m_context) CancelComposition(m_context);
        if (!raw.empty()) CommitString(pic, raw);
        // 切到"英文直通"：不能把编码留在补全缓冲里 —— 否则下一个字母会进英文补全
        // （吃掉按键、弹候选），而不是直接落到文档里。这里全部清干净。
        m_engTyped.clear();
        m_engActive = false;
        m_quickMode = false;
        m_zPending = false;
        LogLine(L"shift during compose: committed raw code '%ls' (%zu chars), switch to english (pass-through)",
                raw.c_str(), raw.size());
    }
    if (m_comp && m_context) CancelComposition(m_context);
    if (composing) {
        m_english = true;                 // 组码阶段按 Shift：切到英文
        m_candWnd.Hide();
    } else {
        m_english = !m_english;           // 平时：切换中/英
    }
    LogLine(L"switch english=%d (composing=%d)", (int)m_english, (int)composing);
    SetInputModeCompartments(!m_english);
    BarSync();
    // 注意：这里不能调 FlashStatus —— 它会挂 700ms 定时器把窗口藏掉（用户反馈"按 Shift 后窗口 1 秒就消失"）
    if (!composing) ShowIdleStatus(pic);
}

// 简/繁切换：Ctrl+Shift+T 与"单击右 Shift"共用
void CTextService::ToggleTrad(ITfContext* pic) {
    LoadDataOnce();
    m_tradEnabled = !m_tradEnabled;
    m_settings.TradChinese = m_tradEnabled ? 1 : 0;
    const bool saved = wubi::SaveSettings(GetConfigPath(), m_settings);
    LogLine(L"toggle trad -> %d saved=%d", (int)m_tradEnabled, (int)saved);
    BarSync();
    // 组字中/反查中切换：候选框里的字要立刻跟着换，否则显示与上屏不一致
    if (m_pyLookup) PyLookupRefresh(pic, false);
    else if (!m_composer.Code().empty()) RefreshCandidates(pic);
    else ShowIdleStatus(pic);      // 不在组码：刷新状态条上的简/繁
}

void CTextService::ExitCustom(ITfContext* pic, bool cancel) {
    const bool wasWord = m_customWordMode;
    m_customWordMode = false;
    m_customChars.clear();
    m_customWord.clear();
    m_letterLabels = false;
    if (cancel) {
        if (m_comp) CancelComposition(pic);
        m_candWnd.Hide();
    }
    LogLine(L"custom: exit cancel=%d word=%d", (int)cancel, (int)wasWord);
}

void CTextService::RefreshCustomPrompt() {
    if (!m_customWordMode) { m_candWnd.Hide(); return; }
    std::wstring shown;
    for (const auto& c : m_customChars) shown += c;
    std::vector<std::wstring> lines;
    lines.push_back(L"自定义词组：" + (shown.empty() ? std::wstring(L"（按 ← 取字）") : TruncForPrompt(shown, 16)));
    lines.push_back(L"← 添一个字（加到最前）· → 缩一个 · Del 删首 · Backspace 删尾 · 回车保存 · Esc 取消");
    m_letterLabels = false;
    ShowCandidateList(lines);
}

void CTextService::SaveCustomWord(ITfContext* pic) {
    if (m_customChars.empty()) {   // 空缓冲区按回车：直接退出，不留在加词状态
        ExitCustom(pic, true);
        LogLine(L"custom word: enter on empty buffer -> exit");
        return;
    }
    if (m_customChars.size() < 2) {
        const size_t had = m_customChars.size();
        ExitCustom(pic, true);
        FlashStatus(L"至少两个字，加词取消");
        LogLine(L"custom word: too short (%zu char), aborted", had);
        return;
    }
    const std::string code = wubi::PhraseCode(m_customWord, g_dict.Get());
    if (code.empty()) { FlashStatus(L"取码失败：有字查不到全码"); return; }

    g_dict.Get().AddUserWord(code, m_customWord, 0x7FFFFFFF);
    const bool ok = AppendUserWordLine(GetUserWordsPath(), code, m_customWord, 0x7FFFFFFF);
    const std::wstring text = Utf8ToWide(m_customWord);
    const std::wstring flash = L"已加入词组 " + Utf8ToWide(code) + L" → " + TruncForPrompt(text, 12);
    const std::string savedWord = m_customWord;
    ExitCustom(pic, false);
    FlashStatus(flash.c_str());
    LogLine(L"custom word saved: code=%hs word=%s ok=%d total=%zu",
            code.c_str(), Utf8ToWide(savedWord).c_str(), (int)ok, g_dict.Get().UserWordCount());
}

// 自定义词组模式下的按键。返回 true 表示已处理（键要吞掉）
bool CTextService::CustomKey(ITfContext* pic, UINT vk, LPARAM lp, bool shift, BOOL* pfEaten) {
    (void)lp; (void)shift;
    if (!m_customWordMode) return false;
    // Ctrl/Alt 组合键照常交给应用（复制/粘贴等），别在自定义模式里被吃掉
    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return false;

    switch (vk) {
    case VK_LEFT:   // ← 把上一个上屏的字加到最前面：先取回的是更近的字，这样字序才是输入顺序
        if (!m_recentChars.empty()) {
            m_customChars.insert(m_customChars.begin(), m_recentChars.back());
            m_recentChars.pop_back();
            m_customWord.clear();
            for (const auto& c : m_customChars) m_customWord += WideToUtf8(c);
        }
        RefreshCustomPrompt();
        *pfEaten = TRUE;
        return true;
    case VK_RIGHT:   // → 缩一个：把最近取回来的那个字（在最前面）退回上屏队列，← 可以再取
        if (!m_customChars.empty()) {
        if (m_customChars.size() <= 2) { FlashStatus(L"至少两个字，不能再删（Esc 取消，回车保存）"); *pfEaten = TRUE; return true; }
            m_recentChars.push_back(m_customChars.front());
            m_customChars.erase(m_customChars.begin());
            m_customWord.clear();
            for (const auto& c : m_customChars) m_customWord += WideToUtf8(c);
        }
        if (m_customChars.empty()) { ExitCustom(pic, true); *pfEaten = TRUE; return true; }  // 候选汉字删到 0 就退出
        RefreshCustomPrompt();
        *pfEaten = TRUE;
        return true;
    case VK_DELETE:  // Del 删掉最前面那个字（真正丢弃，不回队列）
        if (!m_customChars.empty()) {
        if (m_customChars.size() <= 2) { FlashStatus(L"至少两个字，不能再删（Esc 取消，回车保存）"); *pfEaten = TRUE; return true; }
            m_customChars.erase(m_customChars.begin());
            m_customWord.clear();
            for (const auto& c : m_customChars) m_customWord += WideToUtf8(c);
        }
        if (m_customChars.empty()) { ExitCustom(pic, true); *pfEaten = TRUE; return true; }  // 候选汉字删到 0 就退出
        RefreshCustomPrompt();
        *pfEaten = TRUE;
        return true;
    case VK_BACK:    // Backspace 删掉最后面那个字（最容易打错的通常是最近这个）
        if (m_customChars.size() <= 2) { FlashStatus(L"至少两个字，不能再删（Esc 取消，回车保存）"); *pfEaten = TRUE; return true; }
        if (!m_customChars.empty()) {
            m_customChars.pop_back();
            m_customWord.clear();
            for (const auto& c : m_customChars) m_customWord += WideToUtf8(c);
        }
        if (m_customChars.empty()) { ExitCustom(pic, true); *pfEaten = TRUE; return true; }  // 候选汉字删到 0 就退出
        RefreshCustomPrompt();
        *pfEaten = TRUE;
        return true;
    case VK_RETURN: SaveCustomWord(pic); *pfEaten = TRUE; return true;
    case VK_ESCAPE: ExitCustom(pic, true); *pfEaten = TRUE; return true;
    default: *pfEaten = TRUE; return true;   // 其它键不落到文档里
    }
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
// —— 会话级自动学词：连续上屏的单字拼成 2~4 字词，只在本进程里当"最后的候选" ——
static std::string W2U8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

void CTextService::NoteCommittedForLearning(const std::wstring& text) {
    // 只把"单个汉字"接进链：多字词/标点会打断链，避免把词的内部和词边界混起来
    m_lastLearned.clear();      // 只有"最近一次上屏"学到的词可以被退格撤销
    if (text.size() != 1) { m_lastChar1 = m_lastChar2 = m_lastChar3 = 0; return; }
    const wchar_t c = text[0];
    if (c < 0x3400 || c > 0x9FFF) { m_lastChar1 = m_lastChar2 = m_lastChar3 = 0; return; }
    // 2 字
    if (m_lastChar1) {
        std::wstring w; w += m_lastChar1; w += c;
        if (AddSessionWord(w)) m_lastLearned.push_back(w);
    }
    // 3 字
    if (m_lastChar1 && m_lastChar2) {
        std::wstring w; w += m_lastChar2; w += m_lastChar1; w += c;
        if (AddSessionWord(w)) m_lastLearned.push_back(w);
    }
    // 4 字
    if (m_lastChar1 && m_lastChar2 && m_lastChar3) {
        std::wstring w; w += m_lastChar3; w += m_lastChar2; w += m_lastChar1; w += c;
        if (AddSessionWord(w)) m_lastLearned.push_back(w);
    }
    m_lastChar3 = m_lastChar2;
    m_lastChar2 = m_lastChar1;
    m_lastChar1 = c;
}

// 退格/删除：用户其实不想要刚打的那个字 -> 把这一步学到的会话词撤掉，字符也退出学习链
void CTextService::UndoLastLearned() {
    if (m_lastLearned.empty() && !m_lastChar1) return;
    for (const auto& w : m_lastLearned) {
        for (size_t i = 0; i < m_sessionWords.size(); ++i) {
            if (m_sessionWords[i].word == w) {
                m_sessionWords.erase(m_sessionWords.begin() + (std::ptrdiff_t)i);
                LogLine(L"session undo: %s", w.c_str());
                break;
            }
        }
    }
    m_lastLearned.clear();
    // 字符退出链：下一个字不会再和"被删掉的字"组词
    m_lastChar1 = m_lastChar2;
    m_lastChar2 = m_lastChar3;
    m_lastChar3 = 0;
}

bool CTextService::AddSessionWord(const std::wstring& w) {
    if (w.size() < 2 || w.size() > 4) return false;                // 2~4 字
    const std::string w8 = W2U8(w);
    if (w8.empty()) return false;
    const std::string code = wubi::PhraseCode(w8, g_dict.Get());
    if (code.empty() || code.size() != 4) return false;            // 取不出四码就不学
    for (const auto& c : g_dict.Get().LookupExact(code)) {
        if (c.word == w8) return false;                            // 词库里已有
    }
    for (const auto& sw : m_sessionWords) {
        if (sw.code == code && sw.word == w) return false;         // 已经学过一次
    }
    if (m_sessionWords.size() >= 64) m_sessionWords.erase(m_sessionWords.begin());
    m_sessionWords.push_back(SessionWord{ code, w });
    LogLine(L"session word: %s (code=%hs, total %zu)", w.c_str(), code.c_str(), m_sessionWords.size());
    return true;
}

bool CTextService::HasSessionWordForCode(const std::string& code) const {
    for (const auto& sw : m_sessionWords) if (sw.code == code) return true;
    return false;
}

bool CTextService::PickSessionCandidate(ITfContext* pic, int displayIdx) {
    const int normalCount = (int)m_composer.Candidates().size();
    if (displayIdx < normalCount) return false;
    const std::string code = m_composer.Code();
    int seen = 0;
    for (size_t i = 0; i < m_sessionWords.size(); ++i) {
        if (m_sessionWords[i].code != code) continue;
        if (seen++ != displayIdx - normalCount) continue;
        const std::wstring word = m_sessionWords[i].word;
        const std::string w8 = W2U8(word);
        // 用户真选了它 -> 正式记入用户词库（和自造词同一套存储）
        g_dict.Get().AddUserWord(code, w8, 0x7FFFFFFF);
        const bool saved = g_dict.Get().SaveUserWords(GetUserWordsPath());
        m_sessionWords.erase(m_sessionWords.begin() + (std::ptrdiff_t)i);
        if (m_comp) CancelComposition(pic);
        CommitString(pic, word);
        RememberRecentCommit(word);
        m_candWnd.Hide();
        LogLine(L"session word committed: %s (code=%hs saved=%d)", word.c_str(), code.c_str(), (int)saved);
        return true;
    }
    return false;
}

void CTextService::RefreshCandidates(ITfContext* pic) {
    m_candWnd.SetStatusMode(false);    // 组码时窗口归候选列表用
    (void)pic;
    m_letterLabels = false;
    const auto& cands = m_composer.Candidates();
    LogLine(L"cands: code=%hs n=%zu comp=%p", m_composer.Code().c_str(), cands.size(), (void*)m_comp);
    std::vector<std::wstring> display;
    display.reserve(cands.size());
    // 只有"人工强制指定"（自造词/自定义短语）才在词尾加个小淡蓝星号；
    // 词频自学习把某个词顶到前面时，不加任何记号。
    m_candPinned.assign(cands.size(), 0);
    for (size_t i = 0; i < cands.size(); ++i) {
        const auto& e = cands[i];
        int n = MultiByteToWideChar(CP_UTF8, 0, e.word.c_str(), (int)e.word.size(), nullptr, 0);
        std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, e.word.c_str(), (int)e.word.size(), &w[0], n);
        m_candPinned[i] = e.IsPinned() ? 1 : 0;
        if (m_tradEnabled && m_trad.Loaded()) w = m_trad.ToTraditional(w);   // 候选显示与上屏保持一致的简繁
        display.push_back(w);
    }
    // 会话级自动学的词：码相同才出现，而且**排在最后**，不影响正常候选顺序
    m_candSessionStart = display.size();
    {
        const std::string cc = m_composer.Code();
        for (const auto& sw : m_sessionWords) {
            if (sw.code != cc) continue;
            std::wstring w = sw.word;
            if (m_tradEnabled && m_trad.Loaded()) w = m_trad.ToTraditional(w);
            display.push_back(w);
            m_candPinned.push_back(0);
        }
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
    // 状态提示（中/英、简繁…）跟组码无关：顶部编码框要清掉，否则会挂着上一个字的编码
    m_candWnd.SetCode(std::wstring());
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
    // 原样记录模式：所有键都由输入法处理（回车整段输出、退格删、Esc 取消）
    if (m_rawMode) {
        return vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU &&
               vk != VK_LWIN && vk != VK_RWIN && vk != VK_CAPITAL;
    }
    // Z 拼音反查期间：字母、退格、空格、回车、Esc、数字都由输入法处理
    if (m_pyLookup) {
        return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') ||
               vk == VK_BACK || vk == VK_SPACE || vk == VK_RETURN || vk == VK_ESCAPE ||
               vk == VK_OEM_MINUS || vk == VK_OEM_PLUS || vk == VK_PRIOR || vk == VK_NEXT;
    }
    // 自定义词组模式：几乎所有键都由输入法处理，先声明下来，否则宿主直接自己消化
    if (m_customWordMode) {
        return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_DELETE || vk == VK_RETURN || vk == VK_ESCAPE ||
               vk == VK_BACK || vk == VK_SPACE || (vk >= '0' && vk <= '9') ||
               IsPrintableVk(vk) || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
    }
    // 注意：退格**不声明**。声明了（TestKeyDown=TRUE）而 OnKeyDown 又不吞，
    // 有些宿主会直接把这一键丢掉、不再删字（实测 RichEdit 就是如此），属于会砸功能的做法。
    // 代价是多数宿主不会把退格转给输入法，所以"删掉的字还在最近上屏队列里"这件事
    // 只能靠 OnKeyDown 里的记账（能在部分宿主生效），见那里的注释。
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt  = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (alt) return false;
    if (ctrl) {
        // Ctrl 组合默认交给应用，但自己的热键要先声明，否则宿主根本不会把 KeyDown 转到输入法：
        //   Ctrl+= 自定义词组 · Ctrl+Shift+= 自定义短语 · Ctrl+Shift+T 简繁
        // 注意：这些热键要在**英文模式判断之前**声明 —— 英文模式只表示"字母键不打字"，
        // 不等于输入法的快捷键失效（之前放在后面，英文模式下一按 Ctrl+= 就进不了加词，是个 bug）。
        if (vk == VK_OEM_PLUS) return true;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift && (vk == 'T' || vk == 'P')) return true;
        return false;
    }
    // 大写锁定开着：用户在敲命令/全大写，输入法整体让路（Ctrl 热键在上面已单独声明过）
    if ((GetKeyState(VK_CAPITAL) & 1) != 0 && !m_comp && !m_quickMode && !m_engActive &&
        !m_zPending && !m_customWordMode) {
        return false;
    }

    if (m_english) return false;                       // 自由英文模式：字母键完全不参与

    // 退格/删除：平时不声索（交给应用删字），但"刚学过词、可能要撤销"时声索一次，
    // 撤销完立刻恢复不声索，避免影响宿主正常删字。
    if ((vk == VK_BACK || vk == VK_DELETE) && !m_comp && !m_engActive && !m_quickMode &&
        !m_zPending && !m_customWordMode && (!m_lastLearned.empty() || m_lastChar1)) {
        return true;
    }

    if (vk >= 'A' && vk <= 'Z') {
        // 'z' 现在也是编码字符（自定义短语的 dz / sfz 要用），所以一律声明。
        // 没有"上次上屏"内容时不会走 Z 重复分支，z 会当作组码首字母。
        return true;
    }

    const bool busy = m_comp || m_engActive || m_quickMode || m_zPending ||
                      !m_composer.Code().empty();
    if (busy) {
        // 组码/补全/便捷输入期间这些键都由状态机处理
        return vk == VK_SPACE || vk == VK_RETURN || vk == VK_BACK || vk == VK_ESCAPE ||
               (vk >= '0' && vk <= '9') || vk == VK_OEM_1 || vk == VK_OEM_7 ||
               vk == VK_OEM_MINUS || vk == VK_OEM_PLUS || IsPrintableVk(vk);
    }
    if (vk == VK_OEM_1 && !(GetKeyState(VK_SHIFT) & 0x8000)) return true;   // 分号：便捷输入

    {                                                 // 中文标点接管：确实能映射才声明
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
    MaybeReloadUserWords();   // 短语窗口/别的进程刚追加的词，按键时就地热加载
    // 状态提示显示到下一次按键为止
    if (m_statusShown) { m_candWnd.Hide(); m_statusShown = false; }

    // Shift 单击检测：按下期间若按了别的键，就不算单击。
    // 同时记下按的是哪一侧：左 Shift = 中/英，右 Shift = 简/繁。
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        m_shiftDown = true;
        m_shiftUsed = false;
        m_shiftSide = (vk == VK_LSHIFT) ? 1 : (vk == VK_RSHIFT) ? 2
                    : (((GetKeyState(VK_RSHIFT) & 0x8000) != 0) ? 2 : 1);
        // 两个 Shift 同时按着：松开时两个动作都执行（先简繁、再中英）
        m_shiftBoth = ((vk == VK_LSHIFT) || ((GetKeyState(VK_LSHIFT) & 0x8000) != 0)) &&
                      ((vk == VK_RSHIFT) || ((GetKeyState(VK_RSHIFT) & 0x8000) != 0));
        return S_OK;      // Shift 本身不拦截
    }
    if (m_shiftDown) m_shiftUsed = true;

    const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool caps  = (GetKeyState(VK_CAPITAL) & 1) != 0;
    LogLine(L"OnKeyDown vk=0x%02X scan=0x%02X ctrl=%d alt=%d comp=%p code=%hs",
            vk, (unsigned)((lParam >> 16) & 0xFF), (int)ctrl, (int)alt, (void*)m_comp, m_composer.Code().c_str());

    // Ctrl+Shift+T 切换简繁（组码中也生效）；单击右 Shift 走同一个函数
    if (ctrl && shift && vk == 'T') {
        ToggleTrad(pic);
        *pfEaten = TRUE;
        return S_OK;
    }
    // 旧的实现留在下面这行里（已抽成 ToggleTrad，见下方函数体）

    // Ctrl+= 进入自定义词组；Ctrl+Shift+= 打开"自定义短语"窗口（独立进程，编码+短语两个输入框）。
    // 用 VK_OEM_PLUS 判断，所以美式键盘上 Ctrl+= 与 Ctrl+Shift+= 都能触发，靠 Shift 区分。
    if (ctrl && !alt && vk == VK_OEM_PLUS) {
        if (shift) {
            if (m_customWordMode) ExitCustom(pic, true);
            LaunchPhraseWindow();
        } else {
            if (m_customWordMode) ExitCustom(pic, true);   // 再按一次先退出，避免状态叠加
            BeginCustomWord(pic);
        }
        *pfEaten = TRUE;
        return S_OK;
    }
    // 自定义模式里的键优先处理；返回 false 表示交给下面的常规流程（组码/选词）
    if (CustomKey(pic, vk, lParam, shift, pfEaten)) return S_OK;

    // 退格：不在组码/补全里时这一键是给应用删字的，同时把它从"最近上屏的字"里剔掉——
    // 否则打错一个字退格删掉后，做自定义词组时它还会被 ← 取回来（字序/内容全错）。
    // 退格（非组码）：撤销刚学的会话词，把字符退出学习链。
    // 能确认"光标前一个字就是我们刚上屏的那个字"时，自己把它删掉并吞掉这个键（各宿主一致）；
    // 确认不了就只撤销学习、把键交给应用去删（标准 TSF 约定：声索但 Eaten=FALSE）。
    if (vk == VK_BACK && !m_comp && !m_engActive && !m_quickMode && !m_zPending &&
        !m_customWordMode && !m_english && !ctrl && !alt && m_lastChar1) {
        const wchar_t expect = m_lastChar1;
        CTextService* self = this;
        bool deleted = false;
        DoEdit(pic, [self, expect, &deleted](TfEditCookie ec) -> HRESULT {
            if (!self->m_context) return E_FAIL;
            TF_SELECTION sel = {};
            ULONG got = 0;
            if (FAILED(self->m_context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &got)) ||
                got == 0 || !sel.range) return E_FAIL;
            BOOL empty = FALSE;
            sel.range->IsEmpty(ec, &empty);
            if (!empty) { sel.range->Release(); return E_FAIL; }   // 有选中内容：交给应用
            ITfRange* r = nullptr;
            HRESULT hr = sel.range->Clone(&r);
            sel.range->Release();
            if (FAILED(hr) || !r) return E_FAIL;
            hr = r->Collapse(ec, TF_ANCHOR_START);
            LONG shifted = 0;
            if (SUCCEEDED(hr)) hr = r->ShiftStart(ec, -1, &shifted, nullptr);
            wchar_t buf[8] = { 0 };
            ULONG n = 0;
            if (SUCCEEDED(hr) && shifted != 0) hr = r->GetText(ec, 0, buf, ARRAYSIZE(buf) - 1, &n);
            if (SUCCEEDED(hr) && n == 1 && buf[0] == expect) {
                if (SUCCEEDED(r->SetText(ec, 0, L"", 0))) deleted = true;
            }
            r->Release();
            return S_OK;
        });
        if (!m_recentChars.empty()) m_recentChars.pop_back();
        UndoLastLearned();
        LogLine(L"backspace: undo learning (selfDeleted=%d)", (int)deleted);
        if (deleted) { *pfEaten = TRUE; return S_OK; }   // 已经自己删掉了
        // 否则让应用去删（*pfEaten 保持 FALSE）
    }

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

    // —— 便捷输入里"原样记录"模式：任意字符照记，回车整段半角输出 ——
    if (m_rawMode) {
        if (vk == VK_RETURN || vk == VK_SPACE) { ExitRaw(pic, true); *pfEaten = TRUE; return S_OK; }   // 空格/回车都上屏
        if (vk == VK_ESCAPE) { ExitRaw(pic, false); *pfEaten = TRUE; return S_OK; }
        if (vk == VK_BACK) {
            if (!m_rawText.empty()) m_rawText.pop_back();
            if (m_rawText.empty()) { ExitRaw(pic, false); }
            else { RefreshRawHint(pic); }
            *pfEaten = TRUE;
            return S_OK;
        }
        // 其它一律原样记录（字母/数字/标点/空格，不限长）
        BYTE ks[256] = { 0 };
        GetKeyboardState(ks);
        wchar_t ch = 0;
        if (ToUnicode(vk, (UINT)lParam, ks, &ch, 1, 0) != 1) ch = 0;
        if (ch >= 0x20 && ch != 0x7F) {
            m_rawText.push_back(ch);
            RefreshRawHint(pic);
        }
        *pfEaten = TRUE;      // 原样记录模式下所有键都由输入法处理，别漏给应用
        return S_OK;
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
            // 空格：选第 1 个候选补全并补一个空格；回车：不补全，原样上屏输入框里的内容
            if (vk == VK_SPACE) { EngCommit(pic, 0, true); *pfEaten = TRUE; return S_OK; }
            if (vk == VK_RETURN) { EngCommit(pic, -1, false); *pfEaten = TRUE; return S_OK; }
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
    // 大写锁定开着：全键直通（与"单击 Shift 进自由英文"等价），输入法不组码、不接管标点
    if (!ctrl && !alt && caps && !m_comp && !m_quickMode && !m_engActive && !m_rawMode &&
        !m_pyLookup && !m_zPending && !m_customWordMode) {
        return S_OK;
    }

    if (m_english) return S_OK;

    // —— Z 待定：列出"最近 4 次上屏（倒序）+ 版本号"，选一项就上屏 ——
    if (m_zPending) {
        const int n = (int)m_zItems.size();
        int idx = -1;
        if (vk == VK_SPACE || vk == VK_RETURN)            idx = 0;
        else if (vk >= '1' && vk <= '9' && (int)(vk - '1') < n) idx = (int)(vk - '1');
        if (vk == VK_ESCAPE) {
            m_zPending = false;
            m_zItems.clear();
            if (m_comp) CancelComposition(pic);
            m_candWnd.Hide();
            *pfEaten = TRUE;
            return S_OK;
        }
        if (idx >= 0) {
            const std::wstring text = m_zItems[(size_t)idx];
            m_zPending = false;
            m_zItems.clear();
            if (m_comp) CancelComposition(pic);
            CommitString(pic, text);
            RememberRecentCommit(text);       // 选出来的内容也算"刚上屏"，下次 Z 能取到
            m_candWnd.Hide();
            LogLine(L"z pick idx=%d len=%zu", idx, text.size());
            *pfEaten = TRUE;
            return S_OK;
        }
        // 字母：不选历史，改为进入"拼音反查"（Z 后面打拼音，找汉字 + 五笔编码）
        if (vk >= 'A' && vk <= 'Z' && !ctrl && !alt) {
            m_zPending = false;
            m_zItems.clear();
            m_candWnd.Hide();
            m_pyLookup = true;
            m_pyTyped.assign(1, (char)((vk >= 'A' && vk <= 'Z') ? (vk - 'A' + 'a') : vk));
            PyLookupRefresh(pic);
            *pfEaten = TRUE;
            return S_OK;
        }
        // 其它键：放弃这次选择，继续按正常流程处理该键
        m_zPending = false;
        m_zItems.clear();
        m_candWnd.Hide();
    }

    // —— Z 反查：拼音 -> 汉字（候选里带五笔编码） ——
    if (m_pyLookup) {
        if (vk >= 'A' && vk <= 'Z') {
            if (m_pyTyped.size() < 24) m_pyTyped.push_back((char)(vk - 'A' + 'a'));
            PyLookupRefresh(pic);
            *pfEaten = TRUE;
            return S_OK;
        }
        if (vk == VK_BACK) {
            if (m_pyTyped.empty()) { PyLookupEnd(pic); }
            else { m_pyTyped.pop_back(); PyLookupRefresh(pic); }
            *pfEaten = TRUE;
            return S_OK;
        }
        if (vk == VK_ESCAPE) { PyLookupEnd(pic); *pfEaten = TRUE; return S_OK; }
        // 翻页：- / = （和组码时一致），PageUp / PageDown 也行
        if ((vk == VK_OEM_MINUS && !shift) || vk == VK_PRIOR) {
            PyLookupFlipPage(pic, -1);
            *pfEaten = TRUE;
            return S_OK;
        }
        if ((vk == VK_OEM_PLUS && !shift) || vk == VK_NEXT) {
            PyLookupFlipPage(pic, +1);
            *pfEaten = TRUE;
            return S_OK;
        }
        // 选第 N 个（当前页内）：空格/回车 = 本页第 1 个
        int pick = -1;
        if (vk == VK_SPACE || vk == VK_RETURN) pick = 0;
        else if (vk >= '1' && vk <= '9')      pick = (int)(vk - '1');
        if (pick >= 0) {
            if ((size_t)pick < m_pyChars.size()) {
                const std::wstring ch = m_pyChars[(size_t)pick];
                const std::wstring code = m_pyCodes[(size_t)pick];
                PyLookupEnd(pic);
                CommitString(pic, ch);
                RememberRecentCommit(ch);
                LogLine(L"pinyin pick '%ls' code=%ls", ch.c_str(), code.c_str());
            } else {
                PyLookupEnd(pic);
            }
            *pfEaten = TRUE;
            return S_OK;
        }
        // 其它键：退出反查，按键继续按正常流程走
        PyLookupEnd(pic);
    }

    // —— 分号便捷输入：数字转三种中文形式 ——

    if (m_quickMode) {
        // 先算出这个键实际产生什么字符：Shift+数字 是 # @ ! 之类，不能当数字
        BYTE ksQuick[256] = { 0 };
        GetKeyboardState(ksQuick);
        wchar_t produced = 0;
        if (ToUnicode(vk, (UINT)lParam, ksQuick, &produced, 1, 0) != 1) produced = 0;

        // 数字（按实际字符判断）：累积，之后空格/回车选三种中文形式（既有规则）
        if (produced >= L'0' && produced <= L'9') {
            const char digit = (char)produced;
            if (m_quickDigits.size() < 12) m_quickDigits.push_back(digit);
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
        // 字母：转入英文单词补全（既有规则）
        if (vk >= 'A' && vk <= 'Z') {
            ExitQuick(pic, true);
            m_engActive = true;
            EngType(pic, LetterWithCase(vk, shift, caps));
            *pfEaten = TRUE;
            return S_OK;
        }
        // 空格/回车：有数字先选形式；没数字时"空格 = 输出分号、回车 = 退出"
        if (vk == VK_SPACE || vk == VK_RETURN) {
            if (!m_quickDigits.empty()) {
                wubi::NumberForms(m_quickDigits, m_quickForms);
                if (!m_quickForms.empty()) {
                    const std::wstring out = m_quickForms[0];
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
            if (vk == VK_SPACE) {
                // 分号 + 空格 = 其实只想要一个分号
                ExitQuick(pic, true);
                const std::wstring semi(L"\uFF1B");
                CommitString(pic, semi);
                RememberRecentCommit(semi);
                LogLine(L"quick: semicolon + space -> %s", semi.c_str());
                *pfEaten = TRUE;
                return S_OK;
            }
            ExitQuick(pic, true);
            *pfEaten = TRUE;
            return S_OK;
        }
        // 任意标点（含分号本身）-> 进入原样转录模式：之后所有键照收，空格/回车上屏
        {
            if (produced >= 0x20 && produced != 0x7F) {
                ExitQuick(pic, true);
                m_rawMode = true;
                m_rawText.assign(1, produced);
                RefreshRawHint(pic);
                *pfEaten = TRUE;
                return S_OK;
            }
        }
        // 其它键：退出便捷输入，继续按正常流程处理
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
    // 顶字上屏之后：composer 里已经有新码，但组合串要等下一键才开。
    // 这里补一手——凡是 composer 里有码却没有组合串，就先把它开起来，
    // 否则空格/数字这些"选字键"会走非组码分支，看起来像"空格不上屏"。
    if (!m_comp && !m_composer.Code().empty() && !m_customWordMode) {
        if (SUCCEEDED(StartComposition(pic, std::wstring()))) {
            LogLine(L"ensure composition for pending code=%hs", m_composer.Code().c_str());
        }
    }

    if (m_comp) {
        auto handle = [&](wubi::Composer::Outcome o) {
            switch (o.act) {
            case wubi::Composer::Act::Updated:
                SyncCodeFromComposer();
                if (!m_comp) StartComposition(pic, std::wstring());   // 空码被丢弃后重新开组合串
                else         UpdateCompositionText(pic);
                RefreshCandidates(pic);
                *pfEaten = TRUE;
                break;
            case wubi::Composer::Act::Committed:
                CommitText(pic, o.commit, o.commitCode, true);
                *pfEaten = TRUE;
                break;
            case wubi::Composer::Act::CommitThenContinue:
                // 顶字上屏：把第 1 候选落盘（结束当前组合串），新码留在 composer 里。
                // 这里**不能**马上 StartComposition：旧组合串的终止回调晚一步才到，
                // 会把刚开的那个新组合串掐掉（第五个键就"被吞掉"了）。
                LogLine(L"auto-commit %zu chars, continue with code=%hs", o.commit.size(), o.commitCode.c_str());
                m_keepCodeOnTerminate = true;
                CommitText(pic, o.commit, o.commitCode, /*resetComposer=*/false);
                SyncCodeFromComposer();
                RefreshCandidates(pic);      // 候选窗照常显示新码的候选（组合串等下一键再开）
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
        // —— 空码（一个候选都没有）不要卡住 ——
        // 回车/空格：把空码直接清掉（什么都不上屏）；继续打字母：丢掉空码、从这一键重新开始。
        // 空码判定要看"这个码在词库里是否存在"，而不是"当前有没有可显示的候选"：
        // 不足四码时词会被过滤掉，xky 这种只有词的码会一个候选都不剩，但它并不是空码。
        // 只有会话词、没有正常候选时，这个码**不是空码**（空格/数字要能选会话词）
        const bool deadCode = m_composer.Candidates().empty()
                              && !HasSessionWordForCode(m_composer.Code())
                              && !g_dict.Get().HasPrefix(m_composer.Code());
        if (deadCode && (vk == VK_RETURN || vk == VK_SPACE)) {
            LogLine(L"dead code cleared by Enter/Space (code=%hs)", m_composer.Code().c_str());
            CancelComposition(pic);
            *pfEaten = TRUE;
            return S_OK;
        }
        wchar_t ch = 0;
        if (IsCodeKey(vk, &ch)) {
            if (deadCode) {                  // 空码 + 继续输入：扔掉旧码，用这一键重开
                LogLine(L"dead code dropped, restart with %c (was %hs)", (wchar_t)ch, m_composer.Code().c_str());
                m_composer.Reset();          // 只重置状态机，组合串留着（本来就是空的）
            }
            LogLine(L"key(composing) %c", (wchar_t)ch);
            handle(m_composer.TypeCode((char)ch));
            return S_OK;
        }
        if (vk == VK_SPACE || vk == VK_RETURN) {
            // 没有正常候选、只有会话词（它排在第 1 位）时，空格/回车也选它
            if (m_composer.Candidates().empty() && PickSessionCandidate(pic, 0)) {
                *pfEaten = TRUE;
                return S_OK;
            }
            handle(m_composer.SelectBest());
            return S_OK;
        }
        // 下面这些是"按住 Shift 就变成另一个符号"的键，必须区分有没有 Shift：
        //   ; -> :   ' -> "   - -> _   = -> +   1 -> !
        // 否则 Shift+; 会被当成"选第 2 候选"，冒号怎么都打不出来。
        if (!shift && vk >= '1' && vk <= '9') {
            // 数字落在"会话词"那一段就选会话词（选完自动进用户词库）；
            // 注意这里必须 *pfEaten = TRUE，否则数字会继续交给应用、跟着上屏。
            const int d = (int)(vk - '0');
            const int normalCount = (int)m_composer.Candidates().size();
            if (d - 1 >= normalCount && PickSessionCandidate(pic, d - 1)) {
                *pfEaten = TRUE;
                return S_OK;
            }
            handle(m_composer.SelectDigit(d));
            return S_OK;
        }
        if (!shift && vk == VK_OEM_1) {   // 分号 = 第 2 个候选（也要能选到会话词）
            const int normal2 = (int)m_composer.Candidates().size();
            if (1 >= normal2 && PickSessionCandidate(pic, 1)) { *pfEaten = TRUE; return S_OK; }
            handle(m_composer.SelectDigit(2));
            return S_OK;
        }
        if (!shift && vk == VK_OEM_7) {   // 单引号 = 第 3 个候选
            const int normal3 = (int)m_composer.Candidates().size();
            if (2 >= normal3 && PickSessionCandidate(pic, 2)) { *pfEaten = TRUE; return S_OK; }
            handle(m_composer.SelectDigit(3));
            return S_OK;
        }
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
                if (deadCode) CancelComposition(pic);   // 空码：先清掉，只输出这个符号
                else          handle(m_composer.SelectBest());   // 提交 #1 并结束组合串
                LoadDataOnce();
                std::wstring outSym;
                if (m_punc.Map((char)symChar, wubi::PuncMode::ChineseHalf, outSym)) {
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

    // Z 键：列"最近 4 次上屏（倒序）" + 版本号，共最多 5 项；选哪项就上屏哪项
    if (vk == 'Z' && !shift) {
        // 开一个空组合串：选词期间宿主产生的 WM_CHAR 会被组合串吸收，
        // 不会在菜单关掉后漏一个数字进文档（实测 tsf-host 里确实会漏）。
        if (!m_comp) StartComposition(pic, std::wstring());
        m_zItems.clear();
        for (size_t i = 0; i < m_recentCommits.size(); ++i) {
            m_zItems.push_back(m_recentCommits[m_recentCommits.size() - 1 - i]);   // 最近的在最前
        }
        m_zItems.push_back(PANDA_WUBI_VERSION_FULL);
        m_zPending = true;
        ShowCandidateList(m_zItems);
        LogLine(L"z list: %zu item(s) (history=%zu)", m_zItems.size(), m_recentCommits.size());
        *pfEaten = TRUE;
        return S_OK;
    }


    // 中文标点接管（ASCII 标点 -> 中文标点，含引号成对）
    {
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
            if (FAILED(StartComposition(pic, std::wstring()))) { m_composer.Reset(); return S_OK; }   // 组码不上屏
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
        {
            // 把宿主 exe 名也记下来：出问题时按进程 pid / exe 名找日志
            wchar_t exe[MAX_PATH] = { 0 };
            GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe) - 1);
            const wchar_t* base = wcsrchr(exe, L'\\');
            LogLine(L"DllMain: attach pid=%lu exe=%s", GetCurrentProcessId(), base ? base + 1 : exe);
            LogLine(L"DllMain: log file = %s", LogFilePath());
        }
    }
    if (reason == DLL_PROCESS_DETACH) {
        // 正常退出（进程结束/FreeLibrary）会写这一行；日志里最后没有它 = 宿主异常终止
        LogLine(L"DllMain: detach pid=%lu (clean exit)", GetCurrentProcessId());
    }

    return TRUE;
}
