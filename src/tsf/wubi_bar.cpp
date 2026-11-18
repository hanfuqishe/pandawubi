#include "wubi_bar.h"
#include "ipc.h"
#include "panda_version.h"
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <cstdio>

namespace wubi {

static const wchar_t* kBarClass = L"PandaWubiStatusBar";
static const UINT_PTR kBarTimer = 1;
static const UINT     kBarTickMs = 500;

// 悬浮栏只应该出现在“正在被操作的那个进程”里。TSF 会为每个宿主进程各加载一份本 DLL，
// 每份都会建一条栏；单靠 Deactivate 收不掉（切到别的应用并不会 Deactivate），
// 于是桌面上会攒出好几条。这里用“本进程是否前台”作为唯一判据。
// 桌面 / 任务栏 / 开始菜单这些"没有文字输入焦点"的窗口：即使它们属于本进程（explorer 里也装了 DLL），
// 也不该冒出悬浮栏 —— 用户回到桌面时那条栏没有任何意义。
static bool ForegroundIsSelf() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return false;

    wchar_t cls[64] = { 0 };
    GetClassNameW(fg, cls, ARRAYSIZE(cls) - 1);
    static const wchar_t* kNoTyping[] = {
        L"Progman",                     // 桌面
        L"WorkerW",                     // 桌面（壁纸层）
        L"Shell_TrayWnd",               // 任务栏
        L"Shell_SecondaryTrayWnd",      // 副屏任务栏
        L"NotifyIconOverflowWindow",    // 托盘溢出面板
        L"ForegroundStaging",           // 前台切换的过渡窗口
        L"MultitaskingViewFrame",       // 任务视图
        L"XamlExplorerHostIslandWindow",// 小组件面板
        L"TaskListThumbnailWnd",        // 任务栏缩略图
    };
    for (const wchar_t* c : kNoTyping) {
        if (wcscmp(cls, c) == 0) return false;
    }
    return true;
}

static UINT BarDpi() {
    HDC dc = GetDC(nullptr);
    const UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

// 同进程里把"同类但不是我"的状态栏窗口都收掉。
// 历史原因（旧代码在非建窗线程里 Destroy 失败、把 hwnd_ 置空）会让一个进程里堆出好几条栏，
// 用户就会看到桌面上同时挂着一排「中 简 ，。 ⚙」。这里每次建窗前先自愈一次。
static void CloseStrayBarsInProcess(HWND keep) {
    const DWORD self = GetCurrentProcessId();
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, L"PandaWubiStatusBar", nullptr)) != nullptr) {
        if (w == keep) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != self) continue;
        PostMessageW(w, WM_CLOSE, 0, 0);   // 默认处理会 DestroyWindow
    }
}

void StatusBar::Create(HINSTANCE inst, const std::wstring& iniPath, ClickFn onClick, VoidFn onReload) {
    inst_ = inst;
    if (hwnd_ && IsWindow(hwnd_)) return;   // 已创建：Activate 多次调用时不重复建窗
    hwnd_ = nullptr;                        // 句柄已失效（被外部关掉/窗口随线程销毁）
    CloseStrayBarsInProcess(nullptr);       // 先把同进程里的残留栏收干净
    iniPath_ = iniPath;
    onClick_ = std::move(onClick);
    if (onReload) onReload_ = std::move(onReload);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &StatusBar::WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kBarClass;
    RegisterClassExW(&wc);   // 已注册时忽略失败

    dpi_ = (int)BarDpi();
    LoadIcon_();      // 产品 Logo（和输入法指示器同一个 panda.ico）
    LoadPos();
    Layout();

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kBarClass, L"", WS_POPUP,
        0, 0, width_, height_, nullptr, nullptr, inst, this);
    CloseStrayBarsInProcess(hwnd_);   // 万一刚才又冒出一个，一并收掉
}

void StatusBar::Destroy() {
    if (!hwnd_) { want_ = false; return; }
    // DestroyWindow 只能在建窗的那个线程调用。不是本线程时不要动它：
    // 既不清 USERDATA 也不把 hwnd_ 置空，否则窗口会变成既不销毁也不自检的幽灵。
    DWORD ownerTid = GetWindowThreadProcessId(hwnd_, nullptr);
    if (ownerTid != GetCurrentThreadId()) {
        want_ = false;
        ShowWindow(hwnd_, SW_HIDE);
        return;                 // 交给拥有线程/进程退出时销毁
    }
    KillTimer(hwnd_, kBarTimer);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    want_ = false;
}

static int S(int v, int dpi);   // 定义在后（DPI 缩放）

// 主题顺序：必须与 TSF 里 OnBarClick 的表、以及 Settings::Theme 的取值顺序一致
static const wchar_t* kThemeNames[4] = { L"熊猫（默认）", L"狸花猫", L"中华田园犬", L"丹顶鹤" };

// 产品 Logo：和输入法指示器用同一个 panda.ico。
// 查找顺序与 TSF 那边保持一致：DLL 同目录 panda.ico -> %LOCALAPPDATA%\PandaWubi\panda.ico -> DLL 自带图标。
void StatusBar::LoadIcon_() {
    if (icon_) return;
    if (dpi_ <= 0) dpi_ = (int)BarDpi();   // 可能在 Create 之前就被调用（设置主题时）
    const int want = S(18, dpi_);
    wchar_t dll[MAX_PATH] = { 0 };
    GetModuleFileNameW(inst_, dll, ARRAYSIZE(dll) - 1);
    std::wstring dir(dll);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.erase(slash);

    const std::wstring candidates[] = {
        dir + L"\\" + iconName_,
        dir + L"\\panda.ico",      // 主题图标没找到就退回默认熊猫
        std::wstring(),
    };
    for (const auto& path : candidates) {
        if (path.empty()) break;
        HICON h = (HICON)LoadImageW(nullptr, path.c_str(), IMAGE_ICON, want, want, LR_LOADFROMFILE);
        if (h) { icon_ = h; return; }
    }
    wchar_t base[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, ARRAYSIZE(base)) > 0) {
        const std::wstring p = std::wstring(base) + L"\\PandaWubi\\panda.ico";
        HICON h = (HICON)LoadImageW(nullptr, p.c_str(), IMAGE_ICON, want, want, LR_LOADFROMFILE);
        if (h) { icon_ = h; return; }
    }
    // 最后退回 DLL 自带图标（与 TSF 指示器的兜底一致）
    icon_ = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(1), IMAGE_ICON, want, want, LR_DEFAULTCOLOR);
}

void StatusBar::LoadPos() {
    if (iniPath_.empty()) return;
    posX_ = (int)GetPrivateProfileIntW(L"Bar", L"X", -1, iniPath_.c_str());
    posY_ = (int)GetPrivateProfileIntW(L"Bar", L"Y", -1, iniPath_.c_str());
}

void StatusBar::SavePos() {
    if (iniPath_.empty()) return;
    wchar_t buf[32];
    swprintf_s(buf, L"%d", posX_);
    WritePrivateProfileStringW(L"Bar", L"X", buf, iniPath_.c_str());
    swprintf_s(buf, L"%d", posY_);
    WritePrivateProfileStringW(L"Bar", L"Y", buf, iniPath_.c_str());
}

static int S(int v, int dpi) { return MulDiv(v, dpi, 96); }

void StatusBar::Layout() {
    // 段：拖动手柄 | 中英 | 简繁 | 设置
    // （原来的"全角/半角（中文标点）"段已去掉：需要半角直接切英文模式，这个开关没有意义）
    const int pad  = S(5, dpi_);
    const int grip = S(24, dpi_);   // 最左边放产品 Logo（也是拖动区）
    const int segEN   = S(30, dpi_);
    const int segTrad = S(30, dpi_);
    const int segSet  = S(26, dpi_);
    height_ = S(26, dpi_);
    width_  = pad + grip + segEN + segTrad + segSet + pad;

    zones_.clear();
    int x = pad + grip;
    zones_.push_back(RECT{ x, 0, x + segEN, height_ });   x += segEN;
    zones_.push_back(RECT{ x, 0, x + segTrad, height_ }); x += segTrad;
    zones_.push_back(RECT{ x, 0, x + segSet, height_ });
}

void StatusBar::Reposition(int x, int y) {
    posX_ = x;
    posY_ = y;
    if (hwnd_) SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, SWP_NOACTIVATE);
}

void StatusBar::EnsureCreated() {
    if (hwnd_) return;
    Create(inst_, iniPath_, onClick_);
}

void StatusBar::Show(bool on) {
    want_ = on;
    if (!on) {
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    EnsureCreated();
    if (!hwnd_) return;

    // 位置：优先用存下来的坐标，但必须落在某块屏幕的工作区内。
    // config.ini 里的旧坐标可能来自别的分辨率/已经拔掉的显示器（实测存过 6136,3091），
    // 那样悬浮栏就跑到屏幕外，用户以为"没了"。
    RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = posX_, y = posY_;
    if (x >= 0 && y >= 0) {
        const POINT pt = { x, y };
        MONITORINFO mi = { sizeof(mi) };
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
        if (mon && GetMonitorInfoW(mon, &mi)) {
            work = mi.rcWork;
        } else {
            x = -1; y = -1;          // 不在任何显示器上 -> 回默认
        }
    }
    if (x < 0 || y < 0) {
        x = work.right - width_ - S(12, dpi_);
        y = work.bottom - height_ - S(12, dpi_);
        posX_ = x; posY_ = y;
    }
    // 再夹一次，防止只剩一角露在屏幕外
    if (x + width_ > work.right)  x = work.right - width_;
    if (y + height_ > work.bottom) y = work.bottom - height_;
    if (x < work.left) x = work.left;
    if (y < work.top)  y = work.top;
    posX_ = x; posY_ = y;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, SWP_NOACTIVATE);
    Reconcile();          // 本进程不在前台就先不露头，等计时器/焦点回调
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void StatusBar::HideNow() {
    // 只是临时盖住（切走），不清 want_：回到前台会自动恢复
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

// 整个桌面只留一条栏：让其它进程的栏自己隐藏（跨进程不能 DestroyWindow，那正是之前炸出幽灵栏的根因）
void StatusBar::YieldOtherBars() {
    if (!hwnd_) return;
    const UINT msg = PandaBarYieldMessage();
    const DWORD self = GetCurrentProcessId();
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, kBarClass, nullptr)) != nullptr) {
        if (w == hwnd_) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == self) continue;
        // 两步走：
        //   1) 先发消息让对方的栏自己隐藏（对方是旧版本 DLL 时不认这条消息）；
        //   2) 再直接 ShowWindow(SW_HIDE) —— 跨进程隐藏顶层窗口是允许的（**不能**用 DestroyWindow），
        //      这样连"对方的计时器已经停了、永远不会自己收"的僵尸栏也能压下去。
        PostMessageW(w, msg, 0, 0);
        ShowWindow(w, SW_HIDE);
    }
}

void StatusBar::Reconcile() {
    if (!hwnd_) return;
    const bool visible = IsWindowVisible(hwnd_) != FALSE;
    // 三个条件同时满足才显示：本进程被要求显示 / 前台窗口属于本进程 / 焦点确实落在能输入文字的控件上
    const bool wantShow = want_ && ForegroundIsSelf() && docFocus_;
    if (wantShow && !visible) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        InvalidateRect(hwnd_, nullptr, TRUE);
        YieldOtherBars();
        yieldTick_ = 0;
    } else if (wantShow && visible) {
        // 每 ~2.5 秒再喊一次：别的进程的栏要是又冒出来（或者它那条是旧版本、不认这条消息之前的），
        // 能把它们重新压下去；本进程是唯一前台，自己不会被别人压。
        if (++yieldTick_ >= 5) { yieldTick_ = 0; YieldOtherBars(); }
    } else if (!wantShow && visible) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void StatusBar::SetDocFocus(bool on) {
    if (docFocus_ == on) return;
    docFocus_ = on;
    Reconcile();          // 焦点一离开输入框就立刻收起，别等计时器
}

void StatusBar::SetIconFile(const std::wstring& name) {
    if (name == iconName_ && icon_) return;
    if (name != iconName_) {
        iconName_ = name;
        if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
        // inst_ 还是空的时候（Create 之前）不能加载：GetModuleFileName(nullptr) 会拿到宿主 exe 的目录，
        // 那里没有我们的 ico。等 Create() 里再用正确的 DLL 句柄加载。
        if (inst_) LoadIcon_();
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void StatusBar::SetThemeIndex(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    if (themeIdx_ == idx) return;
    themeIdx_ = idx;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);   // 菜单每次弹出时重建，这里只是保持一致
}

void StatusBar::SetState(bool english, bool trad) {
    english_ = english;
    trad_ = trad;
    if (hwnd_ && IsWindowVisible(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
}

int StatusBar::HitTest(int x) const {
    for (size_t i = 0; i < zones_.size(); ++i) {
        if (x >= zones_[i].left && x < zones_[i].right) return (int)i;
    }
    return -1;
}

void StatusBar::Paint(HDC dc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);

    HBRUSH bg = CreateSolidBrush(RGB(246, 247, 249));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    HBRUSH bd = CreateSolidBrush(RGB(200, 204, 210));
    FrameRect(dc, &rc, bd);
    DeleteObject(bd);

    // 最左边：产品 Logo（与输入法指示器同一个 panda.ico），这一块也是拖动区
    {
        const int size = S(18, dpi_);
        const int x = S(5, dpi_) + (S(24, dpi_) - size) / 2;
        const int y = (height_ - size) / 2;
        if (icon_) {
            DrawIconEx(dc, x, y, icon_, size, size, 0, nullptr, DI_NORMAL);
        } else {
            // 图标没加载出来就退化成一个色块，至少能看出来是产品标识
            HBRUSH b = CreateSolidBrush(RGB(210, 214, 220));
            RECT r = { x, y, x + size, y + size };
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
    }

    SetBkMode(dc, TRANSPARENT);

    HFONT font = CreateFontW(-MulDiv(13, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(dc, font);

    const wchar_t* texts[3] = {
        english_ ? L"英" : L"中",
        trad_ ? L"繁" : L"简",
        L"\u2699",
    };
    const bool active[3] = { english_, trad_, false };

    for (size_t i = 0; i < zones_.size() && i < 3; ++i) {
        RECT r = zones_[i];
        SetTextColor(dc, active[i] ? RGB(200, 70, 40) : RGB(60, 64, 70));
        DrawTextW(dc, texts[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(dc, oldFont);
    DeleteObject(font);
}

LRESULT CALLBACK StatusBar::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    StatusBar* self = (StatusBar*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    // 设置窗口保存配置后广播的"重新加载设置"消息（RegisterWindowMessage，跨进程可用）
    if (msg == PandaReloadMessage()) {
        if (self && self->onReload_) self->onReload_();
        return 0;
    }

    // 别的进程的悬浮栏刚刚显示了：本进程的栏先让位（只隐藏，不动 want_，
    // 等本进程重新成为前台时它会自己再出来）
    if (msg == PandaBarYieldMessage()) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }

    switch (msg) {
    case WM_CREATE: {
        LPCREATESTRUCT cs = (LPCREATESTRUCT)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        SetTimer(hwnd, kBarTimer, kBarTickMs, nullptr);   // 前台自检
        return 0;
    }
    case WM_TIMER: {
        if (wp != kBarTimer) break;
        if (self) self->Reconcile();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self) self->Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!self) return 0;
        SetCapture(hwnd);
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        const int hit = self->HitTest(x);
        // 左侧手柄拖动；其余段点击
        if (hit < 0) {
            self->dragging_ = true;
            POINT pt = { x, y };
            ClientToScreen(hwnd, &pt);
            RECT wr; GetWindowRect(hwnd, &wr);
            self->dragOffset_.x = pt.x - wr.left;
            self->dragOffset_.y = pt.y - wr.top;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (self && self->dragging_) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &pt);
            self->posX_ = pt.x - self->dragOffset_.x;
            self->posY_ = pt.y - self->dragOffset_.y;
            SetWindowPos(hwnd, HWND_TOPMOST, self->posX_, self->posY_, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!self) return 0;
        ReleaseCapture();
        if (self->dragging_) {
            self->dragging_ = false;
            self->SavePos();
            return 0;
        }
        const int hit = self->HitTest(GET_X_LPARAM(lp));
        if (hit >= 0 && self->onClick_) self->onClick_(hit);
        return 0;
    }
    case WM_RBUTTONUP: {
        // 右键菜单：系统指示器不给我们菜单，这里自己提供
        if (!self) return 0;
        POINT pt;
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (self->english_ ? 0 : MF_CHECKED), 1, L"中文输入");
        AppendMenuW(menu, MF_STRING | (self->trad_ ? MF_CHECKED : 0), 2, L"简繁输出");
        // 主题子菜单：命令 id 100+n 表示"切到第 n 套主题"（顺序与 TSF 里的表一致）
        HMENU themeMenu = CreatePopupMenu();
        for (int i = 0; i < 4; ++i) {
            AppendMenuW(themeMenu, MF_STRING | (self->themeIdx_ == i ? MF_CHECKED : 0),
                        100 + i, kThemeNames[i]);
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)themeMenu, L"主题");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"设置...");
        AppendMenuW(menu, MF_STRING, 4, PANDA_WUBI_VERSION_FULL);

        SetForegroundWindow(hwnd);
        const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                            pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        PostMessageW(hwnd, WM_NULL, 0, 0);

        if (cmd >= 1 && cmd <= 4 && self->onClick_) self->onClick_(cmd - 1);
        else if (cmd >= 100 && cmd <= 103 && self->onClick_) self->onClick_(cmd);   // 100+n = 切主题
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace wubi
