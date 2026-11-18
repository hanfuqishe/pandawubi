// wubi_bar.h —— 输入法状态悬浮栏（自绘，不依赖系统语言栏）
#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace wubi {

class StatusBar {
public:
    // onClick: 0=中/英 1=简/繁 2=标点 3=设置
    using ClickFn = std::function<void(int)>;
    using VoidFn  = std::function<void()>;

    StatusBar() = default;
    ~StatusBar() { Destroy(); }
    StatusBar(const StatusBar&) = delete;
    StatusBar& operator=(const StatusBar&) = delete;

    void Create(HINSTANCE inst, const std::wstring& iniPath, ClickFn onClick, VoidFn onReload = nullptr);
    void Destroy();

    // Show(true/false) 设定“本会话是否要这条栏”，并立刻尝试同步一次可见性；
    // HideNow() 只在**当前**盖住它（切到别的应用），不改变“要栏”的意愿，
    // 这样回到前台时会自动恢复。两者最终都由 Reconcile() 收敛。
    void Show(bool on);
    void HideNow();
    bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    bool Wanted()  const { return want_; }

    void SetState(bool english, bool trad);
    void Reposition(int x, int y);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void EnsureCreated();
    void Reconcile();      // 计时器：want_ && 前台 → 显示，否则隐藏
    void Layout();
    void Paint(HDC dc);
    int  HitTest(int x) const;
    void SavePos();
    void LoadPos();

    HWND        hwnd_ = nullptr;
    HINSTANCE   inst_ = nullptr;
    std::wstring iniPath_;
    ClickFn     onClick_;
    VoidFn      onReload_;

    bool english_ = false;
    bool trad_    = false;


    int  dpi_ = 96;
    int  width_ = 0;
    int  height_ = 0;
    std::vector<RECT> zones_;   // 每段的命中区域（客户坐标）

    bool  dragging_ = false;
    POINT dragOffset_ = { 0, 0 };
    int   posX_ = -1, posY_ = -1;   // -1 = 默认位置（右下角）
    bool  want_ = false;            // 本会话是否要这条栏（不因临时失焦而清除）
};

} // namespace wubi
